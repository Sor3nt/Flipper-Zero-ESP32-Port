#include "../nrf24_app.h"
#include "../nrf24_hw.h"
#include "../helpers/nrf24_jam_config.h"

#include <furi.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdlib.h>
#include <string.h>

#define TAG "Nrf24Jam"

typedef struct {
    Nrf24App* app;
    FuriThread* worker;
    volatile bool stop;
    volatile bool desired_running;
    volatile bool dirty; /* source/selection changed → rebuild channel set + HW */
    /* worker-private */
    bool active;
    uint8_t active_strategy;
} JamCtx;

static JamCtx* g_ctx = NULL;

static void jam_shuffle(uint8_t* arr, size_t count) {
    for(size_t i = count - 1; i > 0; i--) {
        size_t j = (size_t)rand() % (i + 1);
        uint8_t t = arr[i];
        arr[i] = arr[j];
        arr[j] = t;
    }
}

/* Flood and Turbo use the TX-flooding HW path; CW and AFH use the carrier. */
static bool strategy_is_flood(uint8_t strategy) {
    return strategy == Nrf24StrategyFlood || strategy == Nrf24StrategyTurbo;
}

/* The current source still needs a scan before it can jam (WiFi/Activity not
 * scanned yet, or WiFi scanned but found no APs → offer a rescan). */
static bool jam_should_scan(Nrf24App* app) {
    if(nrf24_source_needs_scan(app)) return true;
    if(app->jam.source == Nrf24SourceWifi && app->wifi_ap_count == 0) return true;
    return false;
}

static void jam_teardown(JamCtx* ctx) {
    if(!ctx->active) return;
    nrf24_hw_acquire();
    if(strategy_is_flood(ctx->active_strategy)) {
        nrf24_hw_flood_stop();
    } else {
        nrf24_hw_jammer_stop();
    }
    nrf24_hw_release();
    ctx->active = false;
}

/* Push the static (non-runtime) fields into the view model. */
static void jam_view_static(
    Nrf24App* app,
    const Nrf24JamConfig* cfg,
    const uint8_t* chbuf,
    size_t chcount) {
    char sel[20];
    nrf24_source_selection_label(app, sel, sizeof(sel));
    const char* type = nrf24_source_type_label(app->jam.source);
    bool scan = jam_should_scan(app);

    with_view_model(
        app->jam_view,
        Nrf24JamModel * model,
        {
            strncpy(model->source_label, type, sizeof(model->source_label) - 1);
            model->source_label[sizeof(model->source_label) - 1] = '\0';
            strncpy(model->selection_label, sel, sizeof(model->selection_label) - 1);
            model->selection_label[sizeof(model->selection_label) - 1] = '\0';
            model->strategy = cfg->strategy;
            model->pa_level = cfg->pa_level;
            model->dwell_us = cfg->dwell_us;
            model->random_hop = cfg->random_hop;
            model->can_jam = (chcount > 0);
            model->show_scan = scan;
            uint8_t n = (chcount > sizeof(model->channels)) ? sizeof(model->channels) : chcount;
            memcpy(model->channels, chbuf, n);
            model->band_count = n;
        },
        true);
}

/* Shared runtime-stats push used by the main throttle below AND by the AFH
 * hold loop (which otherwise would not update the screen at all for the
 * whole per-channel hold -- see the call site in jam_worker). Takes a
 * snapshot of the few fields that change while jamming so callers don't need
 * to duplicate the with_view_model block. */
static void jam_view_runtime(
    Nrf24App* app,
    uint8_t channel,
    uint32_t hop_count,
    uint32_t sweeps,
    uint32_t elapsed_ms,
    uint32_t ch_per_sec,
    bool running) {
    with_view_model(
        app->jam_view,
        Nrf24JamModel * model,
        {
            model->channel = channel;
            model->hop_count = hop_count;
            model->sweeps = sweeps;
            model->elapsed_ms = elapsed_ms;
            model->ch_per_sec = ch_per_sec;
            model->running = running;
        },
        true);
}

static int32_t jam_worker(void* context) {
    JamCtx* ctx = context;
    Nrf24App* app = ctx->app;

    nrf24_hw_init();

    nrf24_hw_acquire();
    bool ok = nrf24_hw_probe();
    nrf24_hw_release();

    with_view_model(app->jam_view, Nrf24JamModel * model, { model->hardware_ok = ok; }, true);

    if(!ok) {
        FURI_LOG_W(TAG, "NRF24 probe failed");
        nrf24_hw_deinit();
        return 0;
    }

    const uint32_t tick_hz = furi_kernel_get_tick_frequency();

    uint8_t chbuf[128];
    size_t chcount = 0;
    uint32_t hop_index = 0;
    uint32_t hops = 0;
    uint32_t sweeps = 0;
    uint8_t cur_ch = 0;
    bool built = false;
    uint32_t start_tick = 0;
    uint32_t last_view_tick = 0;
    uint32_t last_cps_hops = 0; /* Hops beim letzten View-Update (Momentanraten-Basis) */
    uint32_t last_cps = 0; /* geglättete c/s (EWMA) */

    while(!ctx->stop) {
        bool want = ctx->desired_running;
        Nrf24JamConfig* cfg = nrf24_jam_config_get(app->jam.source, app->jam.protocol);

        if(ctx->dirty || !built) {
            /* (Re)build channel set for the current source + selection. */
            if(ctx->active) jam_teardown(ctx);
            chcount = nrf24_source_fill_channels(app, chbuf, sizeof(chbuf));
            /* AFH walks the band sequentially on purpose — never shuffle it. */
            if(cfg->random_hop && chcount > 1 && cfg->strategy != Nrf24StrategyAfh)
                jam_shuffle(chbuf, chcount);
            hop_index = 0;
            hops = 0;
            sweeps = 0;
            cur_ch = chcount > 0 ? chbuf[0] : 0;
            built = true;
            ctx->dirty = false;
            jam_view_static(app, cfg, chbuf, chcount);
        }

        if(want && !ctx->active && chcount > 0) {
            nrf24_hw_acquire();
            if(strategy_is_flood(cfg->strategy)) {
                nrf24_hw_flood_start_ex(cur_ch, cfg->data_rate, cfg->pa_level);
            } else {
                /* CW and AFH both bring up the continuous carrier. */
                nrf24_hw_jammer_start_ex(cur_ch, cfg->pa_level);
            }
            nrf24_hw_release();
            ctx->active = true;
            ctx->active_strategy = cfg->strategy;
            start_tick = furi_get_tick();
            last_view_tick = start_tick;
            last_cps_hops = hops;
            last_cps = 0;
        } else if(!want && ctx->active) {
            jam_teardown(ctx);
        }

        /* BT Classic 2 = exact replica of the nRF24_jammer FAP jam_bluetooth
         * (LIST mode): continuous carrier (brought up above) + tight loop that
         * only retunes RF_CH across the curated 21-channel list, ZERO dwell and
         * a per-write bus acquire/release (no 30 ms batch hold → no long
         * un-jammed gaps). Fewer channels + zero dwell give a per-channel
         * revisit near the ~625 us BT hop slot, unlike our wide 79-ch smear. */
        bool fap_bt = app->jam.source == Nrf24SourceProtocol &&
                      app->jam.protocol == Nrf24JamPresetBluetooth2 &&
                      cfg->strategy == Nrf24StrategyCw;

        if(ctx->active && fap_bt) {
            /* Exact replica of the FAP jam_bluetooth() loop AND its UI behaviour:
             * retune RF_CH across the curated list forever with ZERO dwell — and,
             * like the FAP (which never calls view_port_update while is_running),
             * DO NOT redraw the screen while jamming. On the T-Embed the nRF24 SCK
             * (GPIO11) is physically shared with the LCD clock, so every redraw
             * would lock the SPI bus and park the carrier on one channel for a
             * whole LCD frame. Keeping the screen static hands the entire bus to
             * the hop, matching the FAP's band coverage. The screen keeps whatever
             * it showed when "Run" was pressed until the loop exits (stop / pause /
             * config change), where the shared view-update below refreshes once. */
            while(!ctx->stop && ctx->desired_running && !ctx->dirty) {
                cur_ch = chbuf[hop_index % chcount];
                nrf24_hw_acquire();
                nrf24_hw_jammer_set_channel(cur_ch);
                nrf24_hw_release();
                hop_index++;
                hops++;
                if(hop_index >= chcount) {
                    hop_index = 0;
                    sweeps++;
                }
                /* Each hop is only a single RF_CH write (~microseconds), so unlike
                 * the dwell-bounded branch below we must NOT yield every hop -- that
                 * would be thousands of context-switch checks per second. Yield once
                 * every 32 hops instead: often enough (~every few hundred us) that an
                 * equal-priority GUI/input thread never starves, cheap enough not to
                 * dent the FAP's tight retune cadence. */
                if((hops & 0x1F) == 0) taskYIELD();
            }
        } else if(ctx->active && cfg->strategy == Nrf24StrategyAfh) {
            /* AFH exhaustion: park a strong CW carrier on one channel long
             * enough (dwell read as MILLISECONDS) for the victim's adaptive
             * hopping to classify it bad, then step to the next — sequentially,
             * to systematically push channels onto the bad-channel map. The
             * carrier keeps transmitting via REUSE_TX_PL, so the SPI bus is
             * dropped during the dwell (LCD/SD stay responsive) and only
             * re-acquired briefly to retune.
             *
             * dwell_us can be configured up to NRF24_JAM_DWELL_MAX_US (5000),
             * which here means up to a 5-SECOND hold on one channel. The
             * runtime view (channel / hop count / elapsed time / c/s) used to
             * only be refreshed by the shared block after this whole hold_ms
             * wait finished -- so during a multi-second hold the screen simply
             * stopped updating, which looked identical to a freeze even though
             * the app was fully responsive to Stop/Back the entire time (the
             * flag checks below still ran every 25ms). Refreshing periodically
             * INSIDE the hold loop fixes that: the elapsed timer keeps visibly
             * ticking so the screen never looks stuck. */
            cur_ch = chbuf[hop_index % chcount];
            nrf24_hw_acquire();
            nrf24_hw_jammer_set_channel(cur_ch);
            nrf24_hw_release();
            hop_index++;
            hops++;
            if(hop_index >= chcount) {
                hop_index = 0;
                sweeps++;
            }
            uint32_t hold_ms = cfg->dwell_us;
            uint32_t waited = 0;
            while(waited < hold_ms && !ctx->stop && ctx->desired_running && !ctx->dirty) {
                furi_delay_ms(25);
                waited += 25;

                uint32_t now2 = furi_get_tick();
                if(now2 - last_view_tick >= pdMS_TO_TICKS(150)) {
                    last_view_tick = now2;
                    uint32_t et = now2 - start_tick;
                    uint32_t elapsed_ms2 =
                        (tick_hz == 1000) ? et : (uint32_t)((uint64_t)et * 1000 / tick_hz);
                    /* Channel hasn't changed during the hold, so c/s is left
                     * at its last computed value rather than recomputed here. */
                    jam_view_runtime(app, cur_ch, hops, sweeps, elapsed_ms2, last_cps, true);
                }
            }
        } else if(ctx->active) {
            uint16_t dwell = cfg->dwell_us; /* NRF24_JAM_DWELL_MIN/MAX_US: 20..5000 */
            uint8_t burst = cfg->burst_count;
            uint8_t strategy = cfg->strategy;

            /* Per-hop acquire/release (not held for the whole 30ms batch) plus
             * taskYIELD() every hop. Flood/Turbo keeps an NRF24 operation mutex
             * while yielding the shared SPI host during FIFO airtime waits, so
             * the LCD/SD/CC1101 can still use their separate CS devices. */
            uint32_t batch_end = furi_get_tick() + pdMS_TO_TICKS(30);
            while(!ctx->stop && ctx->desired_running && !ctx->dirty &&
                  furi_get_tick() < batch_end) {

                nrf24_hw_acquire();
                cur_ch = chbuf[hop_index % chcount];
                if(strategy == Nrf24StrategyCw) {
                    nrf24_hw_jammer_set_channel(cur_ch);
                    /* The CONT_WAVE carrier keeps radiating autonomously once
                     * RF_CH is written, and the nRF24 ignores bus traffic while
                     * its own CS is deasserted. So drop the shared SPI bus BEFORE
                     * the dwell busy-wait (same pattern as the AFH branch above):
                     * the LCD/SD can use the bus during the hold instead of being
                     * locked out for up to dwell µs on every single hop. RF_CH was
                     * the only thing needing the bus, and it is already set. */
                    nrf24_hw_release();
                    if(dwell > 0) esp_rom_delay_us(dwell);
                } else {
                    /* Flood / Turbo: keep bursting on this channel until the
                     * dwell window (µs) elapses — at least one burst. Use the
                     * microsecond timer so sub-millisecond dwells are honored.
                     * nrf24_hw_flood_burst() bounds a stuck FIFO and yields the
                     * shared host between polls, so a radio burst cannot reserve
                     * SPI2 for its full on-air duration. */
                    bool turbo = (strategy == Nrf24StrategyTurbo);
                    int64_t hop_end = esp_timer_get_time() + (int64_t)dwell;
                    do {
                        nrf24_hw_flood_burst(cur_ch, burst, turbo);
                    } while(esp_timer_get_time() < hop_end && !ctx->stop &&
                            ctx->desired_running && !ctx->dirty);
                    nrf24_hw_release();
                }

                hop_index++;
                hops++;
                if(hop_index >= chcount) {
                    hop_index = 0;
                    sweeps++;
                    if(cfg->random_hop && chcount > 1) jam_shuffle(chbuf, chcount);
                }

                /* Give the scheduler a chance every hop -- cheap no-op if
                 * nothing else is runnable, but lets the GUI/display thread
                 * (and anything else at equal priority) get its slice instead
                 * of waiting out the whole batch window. */
                taskYIELD();
            }
        }

        uint32_t now = furi_get_tick();
        if(now - last_view_tick >= pdMS_TO_TICKS(150)) {
            uint32_t d_tick = now - last_view_tick;
            last_view_tick = now;
            uint32_t elapsed_ms = 0;
            uint32_t cps = 0;
            if(ctx->active) {
                uint32_t et = now - start_tick;
                elapsed_ms = (tick_hz == 1000) ? et : (uint32_t)((uint64_t)et * 1000 / tick_hz);
                /* Momentanrate: Hops seit dem letzten Update / Intervalldauer. Ein
                 * kumulativer Schnitt (hops/elapsed) würde vom überhöhten ersten
                 * Sample nach unten konvergieren statt konstant zu bleiben; leichte
                 * EWMA glättet Jitter durch Batch-Grenzen und LCD-Refresh. */
                uint32_t d_ms =
                    (tick_hz == 1000) ? d_tick : (uint32_t)((uint64_t)d_tick * 1000 / tick_hz);
                uint32_t d_hops = hops - last_cps_hops;
                uint32_t inst = d_ms > 0 ? (uint32_t)((uint64_t)d_hops * 1000 / d_ms) : 0;
                cps = last_cps ? (last_cps * 3 + inst) / 4 : inst;
                last_cps = cps;
                last_cps_hops = hops;
            }
            jam_view_runtime(app, cur_ch, hops, sweeps, elapsed_ms, cps, ctx->active);
        }

        furi_delay_ms(ctx->active ? 1 : 10);
    }

    jam_teardown(ctx);
    nrf24_hw_deinit();
    return 0;
}

void nrf24_app_scene_jam_on_enter(void* context) {
    Nrf24App* app = context;

    bool resume = app->jam.resume_running;
    app->jam.resume_running = false;

    JamCtx* ctx = malloc(sizeof(JamCtx));
    if(!ctx) return;
    ctx->app = app;
    ctx->stop = false;
    ctx->desired_running = resume;
    ctx->dirty = true; /* force initial channel-set build */
    ctx->active = false;
    ctx->active_strategy = Nrf24StrategyCw;
    g_ctx = ctx;

    char sel[20];
    nrf24_source_selection_label(app, sel, sizeof(sel));
    const char* type = nrf24_source_type_label(app->jam.source);
    const Nrf24JamConfig* cfg = nrf24_jam_config_get(app->jam.source, app->jam.protocol);

    with_view_model(
        app->jam_view,
        Nrf24JamModel * model,
        {
            strncpy(model->source_label, type, sizeof(model->source_label) - 1);
            model->source_label[sizeof(model->source_label) - 1] = '\0';
            strncpy(model->selection_label, sel, sizeof(model->selection_label) - 1);
            model->selection_label[sizeof(model->selection_label) - 1] = '\0';
            model->channel = 0;
            model->hop_count = 0;
            model->sweeps = 0;
            model->elapsed_ms = 0;
            model->ch_per_sec = 0;
            model->dwell_us = cfg->dwell_us;
            model->strategy = cfg->strategy;
            model->pa_level = cfg->pa_level;
            model->random_hop = cfg->random_hop;
            model->running = resume;
            model->hardware_ok = true;
            model->can_jam = true;
            model->show_scan = jam_should_scan(app);
            model->band_count = 0;
        },
        true);

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewJam);

    ctx->worker = furi_thread_alloc_ex("Nrf24Jam", 4096, jam_worker, ctx);
    furi_thread_start(ctx->worker);
}

bool nrf24_app_scene_jam_on_event(void* context, SceneManagerEvent event) {
    Nrf24App* app = context;
    if(event.type != SceneManagerEventTypeCustom || !g_ctx) return false;

    switch(event.event) {
    case Nrf24JamEventToggle: {
        if(jam_should_scan(app)) {
            /* OK button is "Scan" for an unready WiFi/Activity source: run the
             * scan now (don't auto-start jamming — button becomes "Run" after). */
            app->jam.resume_running = false;
            if(app->jam.source == Nrf24SourceWifi) {
                scene_manager_next_scene(app->scene_manager, Nrf24AppSceneJamWifiScan);
            } else {
                scene_manager_next_scene(app->scene_manager, Nrf24AppSceneJamActivityScan);
            }
            return true;
        }
        bool new_run = !g_ctx->desired_running;
        g_ctx->desired_running = new_run;
        with_view_model(
            app->jam_view, Nrf24JamModel * model, { model->running = new_run; }, true);
        return true;
    }
    case Nrf24JamEventSelectNext:
        nrf24_source_select(app, +1);
        g_ctx->dirty = true;
        return true;
    case Nrf24JamEventCycleSource:
        /* Rotate CW: next target in the flat preset+source list (wraps). Scanning
         * happens on demand via the Scan button (jam_should_scan), not on step. */
        nrf24_source_step(app, +1);
        g_ctx->dirty = true;
        return true;
    case Nrf24JamEventConfig:
        app->jam.resume_running = g_ctx->desired_running;
        scene_manager_next_scene(app->scene_manager, Nrf24AppSceneJamConfig);
        return true;
    case Nrf24JamEventRescan:
        /* Long-press OK: re-run the scan for scan-sources (even if already
         * cached), otherwise (Protocol) fall back to the config editor. */
        if(app->jam.source == Nrf24SourceWifi) {
            app->jam.resume_running = false;
            scene_manager_next_scene(app->scene_manager, Nrf24AppSceneJamWifiScan);
        } else if(app->jam.source == Nrf24SourceActivity) {
            app->jam.resume_running = false;
            scene_manager_next_scene(app->scene_manager, Nrf24AppSceneJamActivityScan);
        } else {
            app->jam.resume_running = g_ctx->desired_running;
            scene_manager_next_scene(app->scene_manager, Nrf24AppSceneJamConfig);
        }
        return true;
    default:
        return false;
    }
}

void nrf24_app_scene_jam_on_exit(void* context) {
    UNUSED(context);
    if(!g_ctx) return;

    g_ctx->stop = true;
    furi_thread_join(g_ctx->worker);
    furi_thread_free(g_ctx->worker);
    free(g_ctx);
    g_ctx = NULL;
}
