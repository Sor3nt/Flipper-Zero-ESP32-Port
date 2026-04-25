#include "../infrared_app_i.h"

void infrared_scene_bruteforce_on_enter(void* context) {
    InfraredApp* infrared = context;
    
    popup_set_icon(infrared->popup, 0, 32, &I_InfraredLearnShort_128x31);
    popup_set_header(infrared->popup, "IR Bruteforce", 95, 10, AlignCenter, AlignCenter);
    popup_set_text(infrared->popup, "Testing IR signals...\nBack to stop", 10, 35, AlignLeft, AlignCenter);
    popup_set_callback(infrared->popup, NULL);
    
    view_dispatcher_switch_to_view(infrared->view_dispatcher, InfraredViewPopup);
}

bool infrared_scene_bruteforce_on_event(void* context, SceneManagerEvent event) {
    InfraredApp* infrared = context;
    static uint32_t bruteforce_index = 0;
    bool consumed = false;
    
    if(event.type == SceneManagerEventTypeTick) {
        if(infrared->app_state.is_transmitting == false) {
            uint16_t frequencies[] = {38000, 38462, 40000, 56250};
            uint8_t freq_index = bruteforce_index / 10;
            
            if(freq_index >= 4) {
                bruteforce_index = 0;
                freq_index = 0;
            }
            
            uint16_t timings[] = {100, 200, 300, 500};
            uint8_t timing_idx = (bruteforce_index % 10) % 4;
            
            uint32_t raw_timings[] = {
                timings[timing_idx],
                timings[timing_idx],
                timings[timing_idx],
                timings[timing_idx] * 5
            };
            
            infrared_signal_set_raw_signal(
                infrared->current_signal,
                raw_timings,
                4,
                frequencies[freq_index],
                50);
            
            infrared_tx_send_once(infrared);
            bruteforce_index++;
        }
        consumed = true;
    } else if(event.type == SceneManagerEventTypeBack) {
        bruteforce_index = 0;
        scene_manager_previous_scene(infrared->scene_manager);
        consumed = true;
    }
    
    return consumed;
}

void infrared_scene_bruteforce_on_exit(void* context) {
    InfraredApp* infrared = context;
    popup_set_icon(infrared->popup, 0, 0, NULL);
    popup_set_text(infrared->popup, NULL, 0, 0, AlignCenter, AlignCenter);
}
