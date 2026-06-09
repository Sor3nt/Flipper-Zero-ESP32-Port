#include "../nrf24_app.h"
#include "../helpers/nrf24_channel_source.h"
#include "../helpers/nrf24_jam_config.h"

#include <furi.h>
#include <stdio.h>

#define DWELL_VALUES_COUNT \
    ((NRF24_JAM_DWELL_MAX_US - NRF24_JAM_DWELL_MIN_US) / NRF24_JAM_DWELL_STEP_US + 1)
#define BURST_VALUES_COUNT (NRF24_JAM_BURST_MAX - NRF24_JAM_BURST_MIN + 1)

static void jam_config_set_pa(VariableItem* item) {
    Nrf24App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    nrf24_jam_config_get(app->jam.source, app->jam.protocol)->pa_level = idx;
    variable_item_set_current_value_text(item, nrf24_jam_pa_label_long(idx));
    nrf24_jam_config_touch();
}

static void jam_config_set_rate(VariableItem* item) {
    Nrf24App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    nrf24_jam_config_get(app->jam.source, app->jam.protocol)->data_rate = idx;
    variable_item_set_current_value_text(item, nrf24_jam_rate_label(idx));
    nrf24_jam_config_touch();
}

static void jam_config_set_strategy(VariableItem* item) {
    Nrf24App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    nrf24_jam_config_get(app->jam.source, app->jam.protocol)->strategy = idx;
    variable_item_set_current_value_text(item, nrf24_jam_strategy_label_long(idx));
    nrf24_jam_config_touch();
}

static void jam_config_set_dwell(VariableItem* item) {
    Nrf24App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    uint16_t dwell = (uint16_t)(NRF24_JAM_DWELL_MIN_US + (uint32_t)idx * NRF24_JAM_DWELL_STEP_US);
    Nrf24JamConfig* cfg = nrf24_jam_config_get(app->jam.source, app->jam.protocol);
    cfg->dwell_us = dwell;
    char buf[12];
    /* AFH interprets the dwell value as milliseconds, not microseconds. */
    if(cfg->strategy == Nrf24StrategyAfh) {
        snprintf(buf, sizeof(buf), "%ums", dwell);
    } else {
        snprintf(buf, sizeof(buf), "%uus", dwell);
    }
    variable_item_set_current_value_text(item, buf);
    nrf24_jam_config_touch();
}

static void jam_config_set_burst(VariableItem* item) {
    Nrf24App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    uint8_t burst = (uint8_t)(idx + NRF24_JAM_BURST_MIN);
    nrf24_jam_config_get(app->jam.source, app->jam.protocol)->burst_count = burst;
    char buf[8];
    snprintf(buf, sizeof(buf), "%u", burst);
    variable_item_set_current_value_text(item, buf);
    nrf24_jam_config_touch();
}

static void jam_config_set_hop(VariableItem* item) {
    Nrf24App* app = variable_item_get_context(item);
    uint8_t idx = variable_item_get_current_value_index(item);
    nrf24_jam_config_get(app->jam.source, app->jam.protocol)->random_hop = idx ? 1 : 0;
    variable_item_set_current_value_text(item, nrf24_jam_hop_label(idx));
    nrf24_jam_config_touch();
}

void nrf24_app_scene_jam_config_on_enter(void* context) {
    Nrf24App* app = context;
    VariableItemList* vil = app->var_item_list;
    Nrf24JamConfig* cfg = nrf24_jam_config_get(app->jam.source, app->jam.protocol);
    VariableItem* item;
    char buf[12];

    variable_item_list_reset(vil);
    /* Protocol config is per-preset, so name the preset in the header (e.g.
     * "Protocol: BLE Adv") to make clear which slot is being edited. */
    if(app->jam.source == Nrf24SourceProtocol) {
        char header[24];
        snprintf(
            header,
            sizeof(header),
            "Protocol: %s",
            nrf24_jam_preset_short((Nrf24JamPreset)app->jam.protocol));
        variable_item_list_set_header(vil, header);
    } else {
        variable_item_list_set_header(vil, nrf24_source_type_label(app->jam.source));
    }

    item = variable_item_list_add(vil, "PA Level", Nrf24Pa_Count, jam_config_set_pa, app);
    variable_item_set_current_value_index(item, cfg->pa_level);
    variable_item_set_current_value_text(item, nrf24_jam_pa_label_long(cfg->pa_level));

    item = variable_item_list_add(vil, "Data Rate", Nrf24Rate_Count, jam_config_set_rate, app);
    variable_item_set_current_value_index(item, cfg->data_rate);
    variable_item_set_current_value_text(item, nrf24_jam_rate_label(cfg->data_rate));

    item =
        variable_item_list_add(vil, "Strategy", Nrf24StrategyCount, jam_config_set_strategy, app);
    variable_item_set_current_value_index(item, cfg->strategy);
    variable_item_set_current_value_text(item, nrf24_jam_strategy_label_long(cfg->strategy));

    item = variable_item_list_add(vil, "Dwell", DWELL_VALUES_COUNT, jam_config_set_dwell, app);
    variable_item_set_current_value_index(
        item, (uint8_t)((cfg->dwell_us - NRF24_JAM_DWELL_MIN_US) / NRF24_JAM_DWELL_STEP_US));
    if(cfg->strategy == Nrf24StrategyAfh) {
        snprintf(buf, sizeof(buf), "%ums", cfg->dwell_us); /* AFH dwell is in ms */
    } else {
        snprintf(buf, sizeof(buf), "%uus", cfg->dwell_us);
    }
    variable_item_set_current_value_text(item, buf);

    item = variable_item_list_add(vil, "Burst Pkts", BURST_VALUES_COUNT, jam_config_set_burst, app);
    variable_item_set_current_value_index(item, (uint8_t)(cfg->burst_count - NRF24_JAM_BURST_MIN));
    snprintf(buf, sizeof(buf), "%u", cfg->burst_count);
    variable_item_set_current_value_text(item, buf);

    item = variable_item_list_add(vil, "Hop Order", 2, jam_config_set_hop, app);
    variable_item_set_current_value_index(item, cfg->random_hop ? 1 : 0);
    variable_item_set_current_value_text(item, nrf24_jam_hop_label(cfg->random_hop));

    view_dispatcher_switch_to_view(app->view_dispatcher, Nrf24ViewJamConfig);
}

bool nrf24_app_scene_jam_config_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void nrf24_app_scene_jam_config_on_exit(void* context) {
    Nrf24App* app = context;
    nrf24_jam_config_save();
    variable_item_list_reset(app->var_item_list);
}
