#include "../infrared_app_i.h"

void infrared_scene_debug_on_enter(void* context) {
    InfraredApp* infrared = context;
    InfraredWorker* worker = infrared->worker;

    infrared_worker_rx_set_received_signal_callback(
        worker, infrared_signal_received_callback, context);
    infrared_worker_rx_set_error_callback(worker, infrared_rx_error_callback, context);
    infrared_worker_rx_enable_blink_on_receiving(worker, true);
    if(!infrared_worker_rx_start(worker)) {
        infrared_show_error_message(infrared, "Infrared receiver unavailable");
        return;
    }

    infrared_debug_view_set_text(infrared->debug_view, "Received signals\nwill appear here");
    view_dispatcher_switch_to_view(infrared->view_dispatcher, InfraredViewDebugView);
}

bool infrared_scene_debug_on_event(void* context, SceneManagerEvent event) {
    InfraredApp* infrared = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InfraredCustomEventTypeSignalReceived) {
            InfraredDebugView* debug_view = infrared->debug_view;
            InfraredSignal* signal = infrared->current_signal;

            if(infrared_signal_is_raw(signal)) {
                const InfraredRawSignal* raw = infrared_signal_get_raw_signal(signal);
                infrared_debug_view_set_text(debug_view, "RAW\n%d samples\n", raw->timings_size);

                printf("RAW, %zu samples:\r\n", raw->timings_size);
                for(size_t i = 0; i < raw->timings_size; ++i) {
                    printf("%lu ", raw->timings[i]);
                }
                printf("\r\n");

            } else {
                const InfraredMessage* message = infrared_signal_get_message(signal);
                infrared_debug_view_set_text(
                    debug_view,
                    "%s\nA:0x%0*lX\nC:0x%0*lX\n%s\n",
                    infrared_get_protocol_name(message->protocol),
                    ROUND_UP_TO(infrared_get_protocol_address_length(message->protocol), 4),
                    message->address,
                    ROUND_UP_TO(infrared_get_protocol_command_length(message->protocol), 4),
                    message->command,
                    message->repeat ? " R" : "");

                printf(
                    "== %s, A:0x%0*lX, C:0x%0*lX%s ==\r\n",
                    infrared_get_protocol_name(message->protocol),
                    ROUND_UP_TO(infrared_get_protocol_address_length(message->protocol), 4),
                    message->address,
                    ROUND_UP_TO(infrared_get_protocol_command_length(message->protocol), 4),
                    message->command,
                    message->repeat ? " R" : "");
            }
            consumed = true;
        } else if(event.event == InfraredCustomEventTypeRxError) {
            /* The receiver is dead; on_exit() stops the worker. */
            infrared_show_error_message(infrared, "Infrared receiver\nstopped working");
            scene_manager_previous_scene(infrared->scene_manager);
            consumed = true;
        }
    }

    return consumed;
}

void infrared_scene_debug_on_exit(void* context) {
    InfraredApp* infrared = context;
    InfraredWorker* worker = infrared->worker;
    infrared_worker_rx_set_error_callback(worker, NULL, NULL);
    infrared_worker_rx_stop(worker);
    infrared_worker_rx_enable_blink_on_receiving(worker, false);
    infrared_worker_rx_set_received_signal_callback(worker, NULL, NULL);
}
