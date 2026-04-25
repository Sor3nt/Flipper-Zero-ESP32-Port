#include "../infrared_app_i.h"
#include "../infrared_tvbgone_codes.h"

typedef enum {
    TVBGoneRegionNA,
    TVBGoneRegionEU,
    TVBGoneRegionCount,
} TVBGoneRegion;

static void infrared_scene_tvbgone_transmit_signal(InfraredApp* infrared, const IrCode* code) {
    if(!code) return;
    
    infrared_signal_set_raw_signal(
        infrared->current_signal,
        code->times,
        code->numpairs,
        code->timer_val * 1000,
        50);
    
    infrared_tx_send_once(infrared);
    furi_delay_ms(150);
}

void infrared_scene_tvbgone_on_enter(void* context) {
    InfraredApp* infrared = context;
    
    popup_set_icon(infrared->popup, 0, 32, &I_InfraredLearnShort_128x31);
    popup_set_header(infrared->popup, "TV-B-Gone", 95, 10, AlignCenter, AlignCenter);
    popup_set_text(infrared->popup, "Select Region:\nUP/DOWN to choose\nOK to send", 10, 35, AlignLeft, AlignCenter);
    popup_set_callback(infrared->popup, NULL);
    
    view_dispatcher_switch_to_view(infrared->view_dispatcher, InfraredViewPopup);
}

bool infrared_scene_tvbgone_on_event(void* context, SceneManagerEvent event) {
    InfraredApp* infrared = context;
    bool consumed = false;
    
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InfraredCustomEventTypeSignalReceived) {
            scene_manager_previous_scene(infrared->scene_manager);
            consumed = true;
        }
    }
    
    return consumed;
}

void infrared_scene_tvbgone_on_exit(void* context) {
    InfraredApp* infrared = context;
    popup_set_icon(infrared->popup, 0, 0, NULL);
    popup_set_text(infrared->popup, NULL, 0, 0, AlignCenter, AlignCenter);
}

static void infrared_scene_tvbgone_region_callback(VariableItem* item) {
    InfraredApp* infrared = variable_item_get_context(item);
    uint8_t region = variable_item_get_current_value_index(item);
    
    if(region == TVBGoneRegionNA) {
        for(uint8_t i = 0; i < NA_CODES_COUNT; i++) {
            infrared_scene_tvbgone_transmit_signal(infrared, NApowerCodes[i]);
        }
    } else if(region == TVBGoneRegionEU) {
        for(uint8_t i = 0; i < EU_CODES_COUNT; i++) {
            infrared_scene_tvbgone_transmit_signal(infrared, EUpowerCodes[i]);
        }
    }
}
