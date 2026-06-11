#include "../nrf24_app.h"
#include "../nrf24_hw.h"
#include "../helpers/nrf24_channel_source.h"
#include "../helpers/nrf24_jam_config.h"

#include <furi.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
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
        } else if(!want && ctx->active) {
            jam_teardown(ctx);
        }

        if(ctx->active && cfg->strategy == Nrf24StrategyAfh) {
            /* AFH exhaustion: park a strong CW carrier on one channel long
             * enough (dwell read as MILLISECONDS) for the victim's adaptive
             * hopping to classify it bad, then step to the next — sequentially,
             * to systematically push channels onto the bad-channel map. The
             * carrier keeps transmitting via REUSE_TX_PL, so the SPI bus is
             * dropped during the dwell (LCD/SD stay responsive) and only
             * re-acquired briefly to retune. */
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
            }
        } else if(ctx->active) {
            uint16_t dwell = cfg->dwell_us;
            uint8_t burst = cfg->burst_count;
            uint8_t strategy = cfg->strategy;

            nrf24_hw_acquire();
            uint32_t batch_end = furi_get_tick() + pdMS_TO_TICKS(30);
            while(!ctx->stop && ctx->desired_running && !ctx->dirty &&
                  furi_get_tick() < batch_end) {
                cur_ch = chbuf[hop_index % chcount];
                if(strategy == Nrf24StrategyCw) {
                    nrf24_hw_jammer_set_channel(cur_ch);
                    esp_rom_delay_us(dwell);
                } else {
                    /* Flood / Turbo: keep bursting on this channel until the
                     * dwell window (µs) elapses — at least one burst. Use the
                     * microsecond timer so sub-millisecond dwells are honored. */
                    bool turbo = (strategy == Nrf24StrategyTurbo);
                    int64_t hop_end = esp_timer_get_time() + (int64_t)dwell;
                    do {
                        nrf24_hw_flood_burst(cur_ch, burst, turbo);
                    } while(esp_timer_get_time() < hop_end && !ctx->stop &&
                            ctx->desired_running && !ctx->dirty);
                }
                hop_index++;
                hops++;
                if(hop_index >= chcount) {
                    hop_index = 0;
                    sweeps++;
                    if(cfg->random_hop && chcount > 1) jam_shuffle(chbuf, chcount);
                }
            }
            nrf24_hw_release();
        }

        uint32_t now = furi_get_tick();
        if(now - last_view_tick >= pdMS_TO_TICKS(150)) {
            last_view_tick = now;
            uint32_t elapsed_ms = 0;
            uint32_t cps = 0;
            if(ctx->active) {
                uint32_t et = now - start_tick;
                elapsed_ms = (tick_hz == 1000) ? et : (uint32_t)((uint64_t)et * 1000 / tick_hz);
                if(elapsed_ms > 300) cps = (uint32_t)((uint64_t)hops * 1000 / elapsed_ms);
            }
            uint8_t ch = cur_ch;
            uint32_t s = sweeps;
            bool run = ctx->active;
            with_view_model(
                app->jam_view,
                Nrf24JamModel * model,
                {
                    model->channel = ch;
                    model->hop_count = hops;
                    model->sweeps = s;
                    model->elapsed_ms = elapsed_ms;
                    model->ch_per_sec = cps;
                    model->running = run;
                },
                true);
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
        /* Just switch source type; scanning happens on demand via the Scan
         * button (jam_should_scan), not automatically on cycle. */
        nrf24_source_cycle_type(app);
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
