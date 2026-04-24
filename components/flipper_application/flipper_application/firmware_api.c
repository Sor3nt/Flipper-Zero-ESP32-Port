/**
 * @brief Firmware API interface for ESP32 port.
 * Exposes firmware functions to dynamically loaded FAP applications.
 *
 * To regenerate after adding new APIs:
 *   xtensa-esp32s3-elf-nm -u app.fap | grep "^         U " | sed 's/^         U //' | \
 *     grep -v '^__\|^I_' | sort > /tmp/syms.txt
 *   python3 tools/gen_api_table.py -f /tmp/syms.txt
 *
 * The table MUST be sorted by hash value.
 */
#include "api_hashtable/api_hashtable.h"

#include <furi.h>
#include <furi_hal_power.h>
#include <gui/gui.h>
#include <gui/view_port.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/modules/loading.h>
#include <gui/modules/popup.h>
#include <gui/modules/text_box.h>
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <bt/bt_service/bt.h>
#include <ble_profile/extra_profiles/serial_profile.h>
#include <dialogs/dialogs.h>
#include <dolphin/dolphin.h>
#include <flipper_format/flipper_format.h>
#include <flipper_format/flipper_format_i.h>
#include <toolbox/stream/stream.h>
#include <toolbox/manchester_decoder.h>
#include <toolbox/path.h>
#include <furi_hal_gpio.h>
/* subghz headers - resolved via subghz component INCLUDE_DIRS */
#include "devices.h"
#include "receiver.h"
#include "transmitter.h"
#include "environment.h"
#include "subghz_setting.h"
#include "subghz_worker.h"
#include "subghz_keystore.h"
#include "base.h"
#include "generic.h"
#include "protocol_items.h"
#include "subghz_protocol_registry.h"

#include <mjs_core_public.h>
#include <mjs_exec_public.h>
#include <mjs_object_public.h>
#include <mjs_string_public.h>
#include <mjs_array_public.h>
#include <mjs_primitive_public.h>
#include <mjs_util_public.h>
#include <mjs_array_buf_public.h>
#include <mjs_ffi_public.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* math functions needed by FAPs but not declared in ESP-IDF newlib math.h with -std=gnu17 */
extern float cosf(float);
extern float sinf(float);
extern float log10f(float);
extern float log2f(float);

/* subghz functions that may have different names in this firmware version */
extern void subghz_protocol_blocks_add_bit(void*, uint8_t);
extern uint32_t subghz_protocol_blocks_get_hash_data(void*, size_t);

/* GCC runtime helpers (from libgcc, linked into firmware) */
extern long long __udivdi3(long long, long long);
extern long long __divdi3(long long, long long);
extern long long __moddi3(long long, long long);
extern double __divdf3(double, double);
extern float __divsf3(float, float);
extern double __muldf3(double, double);
extern long long __ashldi3(long long, int);
extern long long __lshrdi3(long long, int);
extern double __floatsidf(int);
extern float __truncdfsf2(double);
extern unsigned int __bswapsi2(unsigned int);
extern unsigned long long __bswapdi2(unsigned long long);

/* Newlib runtime */
#include <sys/reent.h>

/* ROM functions */
extern int esp_rom_printf(const char* fmt, ...);

/* Icon assets (global variables in firmware) */
#include <assets_icons.h>

/* clang-format off */
static const struct sym_entry firmware_api_table[] = {
    { .hash = 0x00454575, .address = (uint32_t)variable_item_list_set_selected_item }, /* variable_item_list_set_selected_item */
    { .hash = 0x0052f904, .address = (uint32_t)scene_manager_search_and_switch_to_another_scene }, /* scene_manager_search_and_switch_to_another_scene */
    { .hash = 0x00cba46b, .address = (uint32_t)mjs_prepend_errorf }, /* mjs_prepend_errorf */
    { .hash = 0x0221ca54, .address = (uint32_t)view_allocate_model }, /* view_allocate_model */
    { .hash = 0x028d40d4, .address = (uint32_t)mjs_sprintf }, /* mjs_sprintf */
    { .hash = 0x02f86a97, .address = (uint32_t)mjs_set_exec_flags_poller }, /* mjs_set_exec_flags_poller */
    { .hash = 0x0300bcd5, .address = (uint32_t)subghz_transmitter_free }, /* subghz_transmitter_free */
    { .hash = 0x0307e799, .address = (uint32_t)subghz_transmitter_stop }, /* subghz_transmitter_stop */
    { .hash = 0x031b8511, .address = (uint32_t)subghz_block_generic_deserialize_check_count_bit }, /* subghz_block_generic_deserialize_check_count_bit */
    { .hash = 0x0406a3f5, .address = (uint32_t)gui_remove_view_port }, /* gui_remove_view_port */
    { .hash = 0x04a1a19a, .address = (uint32_t)furi_string_cat_printf }, /* furi_string_cat_printf */
    { .hash = 0x04af1dec, .address = (uint32_t)subghz_protocol_blocks_add_bit }, /* subghz_protocol_blocks_add_bit */
    { .hash = 0x04ff5882, .address = (uint32_t)canvas_set_font }, /* canvas_set_font */
    { .hash = 0x0549bd0a, .address = (uint32_t)furi_record_open }, /* furi_record_open */
    { .hash = 0x05b9c541, .address = (uint32_t)view_free }, /* view_free */
    { .hash = 0x06716444, .address = (uint32_t)mjs_is_null }, /* mjs_is_null */
    { .hash = 0x0674f232, .address = (uint32_t)&sequence_blink_stop }, /* sequence_blink_stop */
    { .hash = 0x0688626a, .address = (uint32_t)mjs_call }, /* mjs_call */
    { .hash = 0x0689dc13, .address = (uint32_t)mjs_exec }, /* mjs_exec */
    { .hash = 0x0689dca8, .address = (uint32_t)mjs_exit }, /* mjs_exit */
    { .hash = 0x068e7d2d, .address = (uint32_t)mjs_next }, /* mjs_next */
    { .hash = 0x0726391d, .address = (uint32_t)elements_button_center }, /* elements_button_center */
    { .hash = 0x0781e22e, .address = (uint32_t)furi_hal_infrared_async_tx_start }, /* furi_hal_infrared_async_tx_start */
    { .hash = 0x08d63d78, .address = (uint32_t)flipper_format_file_open_new }, /* flipper_format_file_open_new */
    { .hash = 0x0922fe88, .address = (uint32_t)view_dispatcher_add_view }, /* view_dispatcher_add_view */
    { .hash = 0x093be3cf, .address = (uint32_t)storage_common_remove }, /* storage_common_remove */
    { .hash = 0x093c3379, .address = (uint32_t)storage_common_rename }, /* storage_common_rename */
    { .hash = 0x099ec15c, .address = (uint32_t)&I_Lock_7x8 }, /* I_Lock_7x8 */
    { .hash = 0x09b86ac7, .address = (uint32_t)flipper_format_rewind }, /* flipper_format_rewind */
    { .hash = 0x0a55a9de, .address = (uint32_t)furi_hal_infrared_async_tx_set_data_isr_callback }, /* furi_hal_infrared_async_tx_set_data_isr_callback */
    { .hash = 0x0b2dd07d, .address = (uint32_t)mjs_array_buf_get_ptr }, /* mjs_array_buf_get_ptr */
    { .hash = 0x0b885c9b, .address = (uint32_t)abs }, /* abs */
    { .hash = 0x0bde1aae, .address = (uint32_t)log10f }, /* log10f */
    { .hash = 0x0c3ff887, .address = (uint32_t)elements_button_left }, /* elements_button_left */
    { .hash = 0x0cb05ef6, .address = (uint32_t)&message_note_c5 }, /* message_note_c5 */
    { .hash = 0x0cb05ef7, .address = (uint32_t)&message_note_c6 }, /* message_note_c6 */
    { .hash = 0x0d7c905a, .address = (uint32_t)view_port_set_orientation }, /* view_port_set_orientation */
    { .hash = 0x0d827590, .address = (uint32_t)memcpy }, /* memcpy */
    { .hash = 0x0d82b830, .address = (uint32_t)memset }, /* memset */
    { .hash = 0x0ddbbb90, .address = (uint32_t)scene_manager_search_and_switch_to_previous_scene }, /* scene_manager_search_and_switch_to_previous_scene */
    { .hash = 0x0de49867, .address = (uint32_t)&message_vibro_on }, /* message_vibro_on */
    { .hash = 0x0e060515, .address = (uint32_t)&message_green_255 }, /* message_green_255 */
    { .hash = 0x0e4dce53, .address = (uint32_t)view_set_previous_callback }, /* view_set_previous_callback */
    { .hash = 0x0eab4808, .address = (uint32_t)furi_string_cat }, /* furi_string_cat */
    { .hash = 0x0eab8c9c, .address = (uint32_t)furi_string_set }, /* furi_string_set */
    { .hash = 0x0f11ed7d, .address = (uint32_t)abort }, /* abort */
    { .hash = 0x0fa010db, .address = (uint32_t)subghz_worker_alloc }, /* subghz_worker_alloc */
    { .hash = 0x0fc57c2c, .address = (uint32_t)furi_string_reserve }, /* furi_string_reserve */
    { .hash = 0x0fdff19f, .address = (uint32_t)log2f }, /* log2f */
    { .hash = 0x1039c338, .address = (uint32_t)bt_profile_start }, /* bt_profile_start */
    { .hash = 0x1060307d, .address = (uint32_t)srand }, /* srand */
    { .hash = 0x10e9fe9e, .address = (uint32_t)subghz_worker_start }, /* subghz_worker_start */
    { .hash = 0x10ec782d, .address = (uint32_t)&I_DolphinDone_80x58 }, /* I_DolphinDone_80x58 */
    { .hash = 0x11115707, .address = (uint32_t)widget_add_rect_element }, /* widget_add_rect_element */
    { .hash = 0x1214bfff, .address = (uint32_t)__bswapdi2 }, /* __bswapdi2 */
    { .hash = 0x1214ffce, .address = (uint32_t)__bswapsi2 }, /* __bswapsi2 */
    { .hash = 0x135baa10, .address = (uint32_t)notification_message }, /* notification_message */
    { .hash = 0x13a0031a, .address = (uint32_t)popup_free }, /* popup_free */
    { .hash = 0x152fa781, .address = (uint32_t)ble_profile_serial_notify_buffer_is_empty }, /* ble_profile_serial_notify_buffer_is_empty */
    { .hash = 0x1761a36b, .address = (uint32_t)__ashldi3 }, /* __ashldi3 */
    { .hash = 0x177a11ff, .address = (uint32_t)subghz_protocol_decoder_base_serialize }, /* subghz_protocol_decoder_base_serialize */
    { .hash = 0x182e12dc, .address = (uint32_t)text_box_alloc }, /* text_box_alloc */
    { .hash = 0x191d9eb3, .address = (uint32_t)scene_manager_handle_custom_event }, /* scene_manager_handle_custom_event */
    { .hash = 0x195df954, .address = (uint32_t)text_box_reset }, /* text_box_reset */
    { .hash = 0x19770b31, .address = (uint32_t)mjs_get_stack_trace }, /* mjs_get_stack_trace */
    { .hash = 0x19d82b07, .address = (uint32_t)mjs_fprintf }, /* mjs_fprintf */
    { .hash = 0x1a12d6a6, .address = (uint32_t)variable_item_set_current_value_text }, /* variable_item_set_current_value_text */
    { .hash = 0x1c270cad, .address = (uint32_t)subghz_receiver_free }, /* subghz_receiver_free */
    { .hash = 0x1c793bc3, .address = (uint32_t)sscanf }, /* sscanf */
    { .hash = 0x1c9395bb, .address = (uint32_t)strchr }, /* strchr */
    { .hash = 0x1c93965e, .address = (uint32_t)strcmp }, /* strcmp */
    { .hash = 0x1c9396ca, .address = (uint32_t)strcpy }, /* strcpy */
    { .hash = 0x1c93bb9d, .address = (uint32_t)strlen }, /* strlen */
    { .hash = 0x1c93db57, .address = (uint32_t)strstr }, /* strstr */
    { .hash = 0x1ce68f24, .address = (uint32_t)mjs_mk_array }, /* mjs_mk_array */
    { .hash = 0x1daf9a69, .address = (uint32_t)furi_thread_set_callback }, /* furi_thread_set_callback */
    { .hash = 0x1fdf4f77, .address = (uint32_t)&I_WarningDolphin_45x42 }, /* I_WarningDolphin_45x42 */
    { .hash = 0x21ce0da1, .address = (uint32_t)&message_display_backlight_off }, /* message_display_backlight_off */
    { .hash = 0x23d393d7, .address = (uint32_t)storage_dir_exists }, /* storage_dir_exists */
    { .hash = 0x245caa0e, .address = (uint32_t)subghz_block_generic_deserialize }, /* subghz_block_generic_deserialize */
    { .hash = 0x24a9aef1, .address = (uint32_t)canvas_current_font_height }, /* canvas_current_font_height */
    { .hash = 0x24c4efe5, .address = (uint32_t)popup_enable_timeout }, /* popup_enable_timeout */
    { .hash = 0x25ca24fe, .address = (uint32_t)flipper_format_read_hex }, /* flipper_format_read_hex */
    { .hash = 0x25f7575f, .address = (uint32_t)ble_profile_serial_set_event_callback }, /* ble_profile_serial_set_event_callback */
    { .hash = 0x2649f22e, .address = (uint32_t)furi_mutex_free }, /* furi_mutex_free */
    { .hash = 0x2688dd53, .address = (uint32_t)furi_delay_tick }, /* furi_delay_tick */
    { .hash = 0x26f1264d, .address = (uint32_t)furi_message_queue_alloc }, /* furi_message_queue_alloc */
    { .hash = 0x2704b2a5, .address = (uint32_t)flipper_format_update_uint32 }, /* flipper_format_update_uint32 */
    { .hash = 0x27b11240, .address = (uint32_t)mjs_mk_null }, /* mjs_mk_null */
    { .hash = 0x27b685e7, .address = (uint32_t)&ble_profile_serial }, /* ble_profile_serial */
    { .hash = 0x282023ff, .address = (uint32_t)&message_red_255 }, /* message_red_255 */
    { .hash = 0x28721e1e, .address = (uint32_t)furi_timer_set_thread_priority }, /* furi_timer_set_thread_priority */
    { .hash = 0x2a5091ac, .address = (uint32_t)subghz_receiver_set_rx_callback }, /* subghz_receiver_set_rx_callback */
    { .hash = 0x2aaa426e, .address = (uint32_t)text_input_set_result_callback }, /* text_input_set_result_callback */
    { .hash = 0x2ad352a8, .address = (uint32_t)variable_item_list_set_enter_callback }, /* variable_item_list_set_enter_callback */
    { .hash = 0x2b03fe22, .address = (uint32_t)view_dispatcher_set_event_callback_context }, /* view_dispatcher_set_event_callback_context */
    { .hash = 0x2b64931f, .address = (uint32_t)&I_Unlock_7x8 }, /* I_Unlock_7x8 */
    { .hash = 0x2be2a572, .address = (uint32_t)flipper_format_get_raw_stream }, /* flipper_format_get_raw_stream */
    { .hash = 0x2c940f5a, .address = (uint32_t)text_input_free }, /* text_input_free */
    { .hash = 0x2e42fe69, .address = (uint32_t)furi_hal_power_is_otg_enabled }, /* furi_hal_power_is_otg_enabled */
    { .hash = 0x2e73b6a8, .address = (uint32_t)popup_set_context }, /* popup_set_context */
    { .hash = 0x30aa559e, .address = (uint32_t)furi_string_alloc_set_str }, /* furi_string_alloc_set_str */
    { .hash = 0x3177bc39, .address = (uint32_t)scene_manager_get_scene_state }, /* scene_manager_get_scene_state */
    { .hash = 0x32e52c9e, .address = (uint32_t)furi_thread_set_priority }, /* furi_thread_set_priority */
    { .hash = 0x33ea609e, .address = (uint32_t)bt_set_status_changed_callback }, /* bt_set_status_changed_callback */
    { .hash = 0x34a48004, .address = (uint32_t)furi_get_tick }, /* furi_get_tick */
    { .hash = 0x35257655, .address = (uint32_t)&sequence_display_backlight_off }, /* sequence_display_backlight_off */
    { .hash = 0x358068e2, .address = (uint32_t)widget_add_string_multiline_element }, /* widget_add_string_multiline_element */
    { .hash = 0x358932d0, .address = (uint32_t)furi_log_print_format }, /* furi_log_print_format */
    { .hash = 0x3595bd57, .address = (uint32_t)furi_hal_power_disable_otg }, /* furi_hal_power_disable_otg */
    { .hash = 0x36912185, .address = (uint32_t)mjs_mk_boolean }, /* mjs_mk_boolean */
    { .hash = 0x3702827f, .address = (uint32_t)canvas_draw_circle }, /* canvas_draw_circle */
    { .hash = 0x370bfdd3, .address = (uint32_t)text_box_free }, /* text_box_free */
    { .hash = 0x3753d1e3, .address = (uint32_t)&message_display_backlight_on }, /* message_display_backlight_on */
    { .hash = 0x37e825d8, .address = (uint32_t)subghz_worker_set_overrun_callback }, /* subghz_worker_set_overrun_callback */
    { .hash = 0x3af87667, .address = (uint32_t)canvas_clear }, /* canvas_clear */
    { .hash = 0x3b0aa073, .address = (uint32_t)widget_alloc }, /* widget_alloc */
    { .hash = 0x3b2ba133, .address = (uint32_t)subghz_devices_get_by_name }, /* subghz_devices_get_by_name */
    { .hash = 0x3c3a86eb, .address = (uint32_t)widget_reset }, /* widget_reset */
    { .hash = 0x3cf8a3aa, .address = (uint32_t)popup_set_timeout }, /* popup_set_timeout */
    { .hash = 0x3d3f3fbd, .address = (uint32_t)furi_thread_set_name }, /* furi_thread_set_name */
    { .hash = 0x3d8f9026, .address = (uint32_t)subghz_worker_rx_callback }, /* subghz_worker_rx_callback */
    { .hash = 0x3dc68bc3, .address = (uint32_t)scene_manager_handle_tick_event }, /* scene_manager_handle_tick_event */
    { .hash = 0x3de00ec7, .address = (uint32_t)realloc }, /* realloc */
    { .hash = 0x3e1255ee, .address = (uint32_t)__furi_crash_implementation }, /* __furi_crash_implementation */
    { .hash = 0x3e574980, .address = (uint32_t)mjs_mk_array_buf }, /* mjs_mk_array_buf */
    { .hash = 0x3e789d89, .address = (uint32_t)&sequence_blink_magenta_10 }, /* sequence_blink_magenta_10 */
    { .hash = 0x42200e72, .address = (uint32_t)popup_get_view }, /* popup_get_view */
    { .hash = 0x42943d48, .address = (uint32_t)view_port_draw_callback_set }, /* view_port_draw_callback_set */
    { .hash = 0x42d0c164, .address = (uint32_t)view_set_draw_callback }, /* view_set_draw_callback */
    { .hash = 0x42d9075a, .address = (uint32_t)flipper_format_write_string_cstr }, /* flipper_format_write_string_cstr */
    { .hash = 0x43e3a6c1, .address = (uint32_t)subghz_protocol_blocks_get_hash_data }, /* subghz_protocol_blocks_get_hash_data */
    { .hash = 0x48f2d81c, .address = (uint32_t)furi_timer_free }, /* furi_timer_free */
    { .hash = 0x48fa02e0, .address = (uint32_t)furi_timer_stop }, /* furi_timer_stop */
    { .hash = 0x49506e3f, .address = (uint32_t)__floatsidf }, /* __floatsidf */
    { .hash = 0x4b45a060, .address = (uint32_t)__assert_func }, /* __assert_func */
    { .hash = 0x4b64bd4d, .address = (uint32_t)view_dispatcher_remove_view }, /* view_dispatcher_remove_view */
    { .hash = 0x4d09b22a, .address = (uint32_t)stream_eof }, /* stream_eof */
    { .hash = 0x4d484818, .address = (uint32_t)mjs_get_int }, /* mjs_get_int */
    { .hash = 0x4d4866a3, .address = (uint32_t)mjs_get_ptr }, /* mjs_get_ptr */
    { .hash = 0x4f791416, .address = (uint32_t)furi_mutex_acquire }, /* furi_mutex_acquire */
    { .hash = 0x5124cf69, .address = (uint32_t)scene_manager_handle_back_event }, /* scene_manager_handle_back_event */
    { .hash = 0x51dc1d30, .address = (uint32_t)canvas_draw_disc }, /* canvas_draw_disc */
    { .hash = 0x51dec116, .address = (uint32_t)canvas_draw_icon }, /* canvas_draw_icon */
    { .hash = 0x51e07f95, .address = (uint32_t)canvas_draw_line }, /* canvas_draw_line */
    { .hash = 0x51e3ac48, .address = (uint32_t)canvas_draw_rbox }, /* canvas_draw_rbox */
    { .hash = 0x538016fe, .address = (uint32_t)&message_delay_25 }, /* message_delay_25 */
    { .hash = 0x5380175c, .address = (uint32_t)&message_delay_50 }, /* message_delay_50 */
    { .hash = 0x54803eb8, .address = (uint32_t)view_commit_model }, /* view_commit_model */
    { .hash = 0x5534e1a8, .address = (uint32_t)popup_set_text }, /* popup_set_text */
    { .hash = 0x568eaf4c, .address = (uint32_t)flipper_format_write_header_cstr }, /* flipper_format_write_header_cstr */
    { .hash = 0x5838cdf7, .address = (uint32_t)submenu_set_header }, /* submenu_set_header */
    { .hash = 0x5949547e, .address = (uint32_t)subghz_devices_begin }, /* subghz_devices_begin */
    { .hash = 0x59c9920a, .address = (uint32_t)canvas_draw_rframe }, /* canvas_draw_rframe */
    { .hash = 0x59cd36e3, .address = (uint32_t)furi_string_printf }, /* furi_string_printf */
    { .hash = 0x59e930cc, .address = (uint32_t)popup_set_header }, /* popup_set_header */
    { .hash = 0x5a6b0f1c, .address = (uint32_t)subghz_devices_reset }, /* subghz_devices_reset */
    { .hash = 0x5a74b905, .address = (uint32_t)furi_hal_power_suppress_charge_enter }, /* furi_hal_power_suppress_charge_enter */
    { .hash = 0x5a80c2b2, .address = (uint32_t)subghz_devices_sleep }, /* subghz_devices_sleep */
    { .hash = 0x5bab36b9, .address = (uint32_t)variable_item_set_current_value_index }, /* variable_item_set_current_value_index */
    { .hash = 0x5c8b27e3, .address = (uint32_t)dialog_message_alloc }, /* dialog_message_alloc */
    { .hash = 0x5cce8b34, .address = (uint32_t)furi_string_set_str }, /* furi_string_set_str */
    { .hash = 0x5d415ec6, .address = (uint32_t)&message_blink_start_10 }, /* message_blink_start_10 */
    { .hash = 0x5d5c8208, .address = (uint32_t)furi_delay_ms }, /* furi_delay_ms */
    { .hash = 0x5dd98f20, .address = (uint32_t)subghz_environment_load_keystore }, /* subghz_environment_load_keystore */
    { .hash = 0x5f1f6d6d, .address = (uint32_t)variable_item_list_get_view }, /* variable_item_list_get_view */
    { .hash = 0x609d84a3, .address = (uint32_t)storage_file_alloc }, /* storage_file_alloc */
    { .hash = 0x60c1c2ee, .address = (uint32_t)storage_file_close }, /* storage_file_close */
    { .hash = 0x613f9d29, .address = (uint32_t)subghz_protocol_blocks_reverse_key }, /* subghz_protocol_blocks_reverse_key */
    { .hash = 0x622edde3, .address = (uint32_t)storage_file_write }, /* storage_file_write */
    { .hash = 0x646b3aea, .address = (uint32_t)subghz_transmitter_yield }, /* subghz_transmitter_yield */
    { .hash = 0x64f8327f, .address = (uint32_t)flipper_format_file_open_always }, /* flipper_format_file_open_always */
    { .hash = 0x662589fc, .address = (uint32_t)__lshrdi3 }, /* __lshrdi3 */
    { .hash = 0x665323b8, .address = (uint32_t)mjs_destroy }, /* mjs_destroy */
    { .hash = 0x667f2ddb, .address = (uint32_t)furi_string_alloc }, /* furi_string_alloc */
    { .hash = 0x66c82dff, .address = (uint32_t)furi_string_empty }, /* furi_string_empty */
    { .hash = 0x66f03645, .address = (uint32_t)furi_timer_alloc }, /* furi_timer_alloc */
    { .hash = 0x67280fac, .address = (uint32_t)furi_thread_set_stack_size }, /* furi_thread_set_stack_size */
    { .hash = 0x6728399c, .address = (uint32_t)subghz_receiver_set_filter }, /* subghz_receiver_set_filter */
    { .hash = 0x678ee68d, .address = (uint32_t)loading_alloc }, /* loading_alloc */
    { .hash = 0x67af1453, .address = (uint32_t)furi_string_reset }, /* furi_string_reset */
    { .hash = 0x683a2408, .address = (uint32_t)furi_timer_start }, /* furi_timer_start */
    { .hash = 0x685bf492, .address = (uint32_t)mjs_is_number }, /* mjs_is_number */
    { .hash = 0x689b9fa6, .address = (uint32_t)view_set_input_callback }, /* view_set_input_callback */
    { .hash = 0x6957b300, .address = (uint32_t)mjs_is_object }, /* mjs_is_object */
    { .hash = 0x69996716, .address = (uint32_t)elements_scrollbar_pos }, /* elements_scrollbar_pos */
    { .hash = 0x6aa6f432, .address = (uint32_t)subghz_setting_get_hopper_frequency }, /* subghz_setting_get_hopper_frequency */
    { .hash = 0x6aac992f, .address = (uint32_t)mjs_mk_foreign }, /* mjs_mk_foreign */
    { .hash = 0x6af67b59, .address = (uint32_t)view_port_enabled_set }, /* view_port_enabled_set */
    { .hash = 0x6c831a1c, .address = (uint32_t)furi_string_replace_all_str }, /* furi_string_replace_all_str */
    { .hash = 0x6c93a048, .address = (uint32_t)view_dispatcher_set_tick_event_callback }, /* view_dispatcher_set_tick_event_callback */
    { .hash = 0x6e8609dc, .address = (uint32_t)variable_item_get_context }, /* variable_item_get_context */
    { .hash = 0x6f2176a9, .address = (uint32_t)subghz_protocol_registry_count }, /* subghz_protocol_registry_count */
    { .hash = 0x705e0bad, .address = (uint32_t)furi_mutex_release }, /* furi_mutex_release */
    { .hash = 0x70b6c701, .address = (uint32_t)mjs_array_del }, /* mjs_array_del */
    { .hash = 0x70b6d3cc, .address = (uint32_t)mjs_array_get }, /* mjs_array_get */
    { .hash = 0x70b706d8, .address = (uint32_t)mjs_array_set }, /* mjs_array_set */
    { .hash = 0x73b3d3aa, .address = (uint32_t)view_port_input_callback_set }, /* view_port_input_callback_set */
    { .hash = 0x73dcb981, .address = (uint32_t)__getreent }, /* __getreent */
    { .hash = 0x73f68400, .address = (uint32_t)mjs_is_string }, /* mjs_is_string */
    { .hash = 0x7434bbb5, .address = (uint32_t)widget_add_button_element }, /* widget_add_button_element */
    { .hash = 0x76b92106, .address = (uint32_t)view_port_update }, /* view_port_update */
    { .hash = 0x77202198, .address = (uint32_t)elements_multiline_text }, /* elements_multiline_text */
    { .hash = 0x795bf537, .address = (uint32_t)mjs_mk_undefined }, /* mjs_mk_undefined */
    { .hash = 0x79c45736, .address = (uint32_t)subghz_environment_alloc }, /* subghz_environment_alloc */
    { .hash = 0x7a3f8f9d, .address = (uint32_t)&sequence_blink_green_10 }, /* sequence_blink_green_10 */
    { .hash = 0x7ac1a9a0, .address = (uint32_t)subghz_environment_set_protocol_registry }, /* subghz_environment_set_protocol_registry */
    { .hash = 0x7bf69165, .address = (uint32_t)view_port_free }, /* view_port_free */
    { .hash = 0x7c943c72, .address = (uint32_t)atoi }, /* atoi */
    { .hash = 0x7c954070, .address = (uint32_t)cosf }, /* cosf */
    { .hash = 0x7c96f087, .address = (uint32_t)free }, /* free */
    { .hash = 0x7c9d3dea, .address = (uint32_t)rand }, /* rand */
    { .hash = 0x7c9dec55, .address = (uint32_t)sinf }, /* sinf */
    { .hash = 0x7d429ba6, .address = (uint32_t)widget_add_text_box_element }, /* widget_add_text_box_element */
    { .hash = 0x7e7a5018, .address = (uint32_t)storage_file_exists }, /* storage_file_exists */
    { .hash = 0x7e99681e, .address = (uint32_t)&I_Pin_back_arrow_10x8 }, /* I_Pin_back_arrow_10x8 */
    { .hash = 0x83d1579a, .address = (uint32_t)view_dispatcher_run }, /* view_dispatcher_run */
    { .hash = 0x83d61ca0, .address = (uint32_t)furi_string_cat_str }, /* furi_string_cat_str */
    { .hash = 0x84de816c, .address = (uint32_t)widget_add_text_scroll_element }, /* widget_add_text_scroll_element */
    { .hash = 0x8511e724, .address = (uint32_t)furi_message_queue_free }, /* furi_message_queue_free */
    { .hash = 0x8658fbb8, .address = (uint32_t)&I_Message_8x7 }, /* I_Message_8x7 */
    { .hash = 0x870064dd, .address = (uint32_t)submenu_get_view }, /* submenu_get_view */
    { .hash = 0x871f6356, .address = (uint32_t)subghz_devices_deinit }, /* subghz_devices_deinit */
    { .hash = 0x8742c103, .address = (uint32_t)popup_alloc }, /* popup_alloc */
    { .hash = 0x8796810c, .address = (uint32_t)mjs_array_push }, /* mjs_array_push */
    { .hash = 0x87d816f1, .address = (uint32_t)mjs_strerror }, /* mjs_strerror */
    { .hash = 0x881717a9, .address = (uint32_t)&I_Fishing_123x52 }, /* I_Fishing_123x52 */
    { .hash = 0x8872a77b, .address = (uint32_t)popup_reset }, /* popup_reset */
    { .hash = 0x891c181f, .address = (uint32_t)subghz_setting_get_preset_data }, /* subghz_setting_get_preset_data */
    { .hash = 0x89219306, .address = (uint32_t)subghz_setting_get_preset_name }, /* subghz_setting_get_preset_name */
    { .hash = 0x8ab50484, .address = (uint32_t)dolphin_deed }, /* dolphin_deed */
    { .hash = 0x8b21ae9a, .address = (uint32_t)submenu_add_item }, /* submenu_add_item */
    { .hash = 0x8ba7e5f0, .address = (uint32_t)text_input_set_header_text }, /* text_input_set_header_text */
    { .hash = 0x8bc6a6ef, .address = (uint32_t)view_set_context }, /* view_set_context */
    { .hash = 0x8d8898b8, .address = (uint32_t)canvas_draw_frame }, /* canvas_draw_frame */
    { .hash = 0x8e57da4c, .address = (uint32_t)subghz_worker_is_running }, /* subghz_worker_is_running */
    { .hash = 0x8ea562b6, .address = (uint32_t)&sequence_success }, /* sequence_success */
    { .hash = 0x8ec45c0d, .address = (uint32_t)furi_string_alloc_printf }, /* furi_string_alloc_printf */
    { .hash = 0x8f07d1ff, .address = (uint32_t)subghz_setting_alloc }, /* subghz_setting_alloc */
    { .hash = 0x8f8c1d4f, .address = (uint32_t)mjs_is_function }, /* mjs_is_function */
    { .hash = 0x8fadabe5, .address = (uint32_t)subghz_devices_stop_async_rx }, /* subghz_devices_stop_async_rx */
    { .hash = 0x8fadac27, .address = (uint32_t)subghz_devices_stop_async_tx }, /* subghz_devices_stop_async_tx */
    { .hash = 0x91ae8039, .address = (uint32_t)flipper_format_insert_or_update_uint32 }, /* flipper_format_insert_or_update_uint32 */
    { .hash = 0x9206d98e, .address = (uint32_t)elements_text_box }, /* elements_text_box */
    { .hash = 0x92c03ab2, .address = (uint32_t)text_input_get_view }, /* text_input_get_view */
    { .hash = 0x92fe76cf, .address = (uint32_t)view_get_model }, /* view_get_model */
    { .hash = 0x9424ab13, .address = (uint32_t)subghz_protocol_decoder_base_get_hash_data }, /* subghz_protocol_decoder_base_get_hash_data */
    { .hash = 0x949adeb3, .address = (uint32_t)subghz_worker_set_pair_callback }, /* subghz_worker_set_pair_callback */
    { .hash = 0x94add11a, .address = (uint32_t)elements_button_right }, /* elements_button_right */
    { .hash = 0x9531b48a, .address = (uint32_t)widget_free }, /* widget_free */
    { .hash = 0x98383973, .address = (uint32_t)furi_thread_free }, /* furi_thread_free */
    { .hash = 0x983a5ec1, .address = (uint32_t)furi_thread_join }, /* furi_thread_join */
    { .hash = 0x985ace0d, .address = (uint32_t)storage_simply_mkdir }, /* storage_simply_mkdir */
    { .hash = 0x9c136b60, .address = (uint32_t)flipper_format_free }, /* flipper_format_free */
    { .hash = 0x9dd62ee1, .address = (uint32_t)furi_thread_set_context }, /* furi_thread_set_context */
    { .hash = 0x9ee1c27c, .address = (uint32_t)furi_thread_alloc }, /* furi_thread_alloc */
    { .hash = 0x9f74bed6, .address = (uint32_t)subghz_devices_set_frequency }, /* subghz_devices_set_frequency */
    { .hash = 0x9f8ba228, .address = (uint32_t)furi_string_cmp_str }, /* furi_string_cmp_str */
    { .hash = 0xa02bb03f, .address = (uint32_t)furi_thread_start }, /* furi_thread_start */
    { .hash = 0xa033707f, .address = (uint32_t)flipper_format_string_alloc }, /* flipper_format_string_alloc */
    { .hash = 0xa0924b48, .address = (uint32_t)furi_thread_yield }, /* furi_thread_yield */
    { .hash = 0xa1cc46d7, .address = (uint32_t)&sequence_blink_cyan_10 }, /* sequence_blink_cyan_10 */
    { .hash = 0xa1dae36e, .address = (uint32_t)subghz_receiver_reset }, /* subghz_receiver_reset */
    { .hash = 0xa21e3cd1, .address = (uint32_t)ble_profile_serial_set_rpc_active }, /* ble_profile_serial_set_rpc_active */
    { .hash = 0xa2445322, .address = (uint32_t)flipper_format_get_value_count }, /* flipper_format_get_value_count */
    { .hash = 0xa2f07b5f, .address = (uint32_t)mjs_set_ffi_resolver }, /* mjs_set_ffi_resolver */
    { .hash = 0xa304212c, .address = (uint32_t)&message_sound_off }, /* message_sound_off */
    { .hash = 0xa4287b78, .address = (uint32_t)view_dispatcher_set_custom_event_callback }, /* view_dispatcher_set_custom_event_callback */
    { .hash = 0xa44210ee, .address = (uint32_t)subghz_keystore_raw_get_data }, /* subghz_keystore_raw_get_data */
    { .hash = 0xa4b4168a, .address = (uint32_t)canvas_set_color }, /* canvas_set_color */
    { .hash = 0xa5a69b61, .address = (uint32_t)furi_hal_power_suppress_charge_exit }, /* furi_hal_power_suppress_charge_exit */
    { .hash = 0xa666cb6b, .address = (uint32_t)elements_multiline_text_aligned }, /* elements_multiline_text_aligned */
    { .hash = 0xa7e32bd6, .address = (uint32_t)canvas_invert_color }, /* canvas_invert_color */
    { .hash = 0xa8a3ba4e, .address = (uint32_t)scene_manager_free }, /* scene_manager_free */
    { .hash = 0xa8aae512, .address = (uint32_t)scene_manager_stop }, /* scene_manager_stop */
    { .hash = 0xa9e82298, .address = (uint32_t)furi_thread_alloc_ex }, /* furi_thread_alloc_ex */
    { .hash = 0xa9e95939, .address = (uint32_t)subghz_devices_get_rssi }, /* subghz_devices_get_rssi */
    { .hash = 0xa9f02c63, .address = (uint32_t)__divdf3 }, /* __divdf3 */
    { .hash = 0xa9f02cc6, .address = (uint32_t)__divdi3 }, /* __divdi3 */
    { .hash = 0xa9f06c32, .address = (uint32_t)__divsf3 }, /* __divsf3 */
    { .hash = 0xaa228e4e, .address = (uint32_t)subghz_devices_set_rx }, /* subghz_devices_set_rx */
    { .hash = 0xaa228e90, .address = (uint32_t)subghz_devices_set_tx }, /* subghz_devices_set_tx */
    { .hash = 0xaa274271, .address = (uint32_t)subghz_transmitter_alloc_init }, /* subghz_transmitter_alloc_init */
    { .hash = 0xaa5738d2, .address = (uint32_t)dialog_message_set_buttons }, /* dialog_message_set_buttons */
    { .hash = 0xaa99e8c9, .address = (uint32_t)subghz_setting_get_default_frequency }, /* subghz_setting_get_default_frequency */
    { .hash = 0xab0be54c, .address = (uint32_t)&message_blue_255 }, /* message_blue_255 */
    { .hash = 0xab26b7f2, .address = (uint32_t)subghz_worker_free }, /* subghz_worker_free */
    { .hash = 0xab2de2b6, .address = (uint32_t)subghz_worker_stop }, /* subghz_worker_stop */
    { .hash = 0xac94b72e, .address = (uint32_t)subghz_setting_get_preset_count }, /* subghz_setting_get_preset_count */
    { .hash = 0xada6324e, .address = (uint32_t)furi_record_close }, /* furi_record_close */
    { .hash = 0xae5e1eed, .address = (uint32_t)subghz_environment_free }, /* subghz_environment_free */
    { .hash = 0xaef6d1a4, .address = (uint32_t)storage_simply_remove }, /* storage_simply_remove */
    { .hash = 0xaf0c3fcc, .address = (uint32_t)strncmp }, /* strncmp */
    { .hash = 0xaf0c4038, .address = (uint32_t)strncpy }, /* strncpy */
    { .hash = 0xaf0cf7e2, .address = (uint32_t)widget_get_view }, /* widget_get_view */
    { .hash = 0xaf0e70ad, .address = (uint32_t)strrchr }, /* strrchr */
    { .hash = 0xaf0fbe22, .address = (uint32_t)strtoul }, /* strtoul */
    { .hash = 0xafb971d9, .address = (uint32_t)&sequence_double_vibro }, /* sequence_double_vibro */
    { .hash = 0xaffa14ed, .address = (uint32_t)subghz_setting_get_frequency_count }, /* subghz_setting_get_frequency_count */
    { .hash = 0xb02115ff, .address = (uint32_t)flipper_format_write_string }, /* flipper_format_write_string */
    { .hash = 0xb06457a4, .address = (uint32_t)__truncdfsf2 }, /* __truncdfsf2 */
    { .hash = 0xb12ba139, .address = (uint32_t)flipper_format_file_open_existing }, /* flipper_format_file_open_existing */
    { .hash = 0xb15957f1, .address = (uint32_t)subghz_protocol_registry_get_by_index }, /* subghz_protocol_registry_get_by_index */
    { .hash = 0xb260e7ad, .address = (uint32_t)flipper_format_write_hex }, /* flipper_format_write_hex */
    { .hash = 0xb3b01449, .address = (uint32_t)subghz_receiver_alloc_init }, /* subghz_receiver_alloc_init */
    { .hash = 0xb4024f2d, .address = (uint32_t)flipper_format_write_uint32 }, /* flipper_format_write_uint32 */
    { .hash = 0xb4e4e3ac, .address = (uint32_t)dialog_message_set_header }, /* dialog_message_set_header */
    { .hash = 0xb6191733, .address = (uint32_t)stream_clean }, /* stream_clean */
    { .hash = 0xb7a22125, .address = (uint32_t)subghz_setting_get_frequency }, /* subghz_setting_get_frequency */
    { .hash = 0xb7cf09de, .address = (uint32_t)variable_item_list_alloc }, /* variable_item_list_alloc */
    { .hash = 0xb8fef056, .address = (uint32_t)variable_item_list_reset }, /* variable_item_list_reset */
    { .hash = 0xbb9ec365, .address = (uint32_t)&I_Quest_7x8 }, /* I_Quest_7x8 */
    { .hash = 0xbc4d1c50, .address = (uint32_t)view_dispatcher_alloc }, /* view_dispatcher_alloc */
    { .hash = 0xbc867b2f, .address = (uint32_t)subghz_receiver_decode }, /* subghz_receiver_decode */
    { .hash = 0xbc94c80a, .address = (uint32_t)view_alloc }, /* view_alloc */
    { .hash = 0xbcbd5eb7, .address = (uint32_t)scene_manager_alloc }, /* scene_manager_alloc */
    { .hash = 0xbcc216ae, .address = (uint32_t)mjs_array_length }, /* mjs_array_length */
    { .hash = 0xbcfff93e, .address = (uint32_t)fprintf }, /* fprintf */
    { .hash = 0xbdd69f1b, .address = (uint32_t)memmove }, /* memmove */
    { .hash = 0xbde04aac, .address = (uint32_t)dialog_message_set_icon }, /* dialog_message_set_icon */
    { .hash = 0xbde65c88, .address = (uint32_t)dialog_message_set_text }, /* dialog_message_set_text */
    { .hash = 0xbe582cf9, .address = (uint32_t)snprintf }, /* snprintf */
    { .hash = 0xbeb85543, .address = (uint32_t)text_input_alloc }, /* text_input_alloc */
    { .hash = 0xbec63dba, .address = (uint32_t)subghz_setting_get_hopper_frequency_count }, /* subghz_setting_get_hopper_frequency_count */
    { .hash = 0xbf514ea3, .address = (uint32_t)__moddi3 }, /* __moddi3 */
    { .hash = 0xbf85a879, .address = (uint32_t)elements_string_fit_width }, /* elements_string_fit_width */
    { .hash = 0xbfc2444e, .address = (uint32_t)__muldf3 }, /* __muldf3 */
    { .hash = 0xbfe83bbb, .address = (uint32_t)text_input_reset }, /* text_input_reset */
    { .hash = 0xc0fe5a29, .address = (uint32_t)flipper_format_read_int32 }, /* flipper_format_read_int32 */
    { .hash = 0xc1e0c6d8, .address = (uint32_t)storage_common_mkdir }, /* storage_common_mkdir */
    { .hash = 0xc1fe1b10, .address = (uint32_t)popup_set_callback }, /* popup_set_callback */
    { .hash = 0xc2c80038, .address = (uint32_t)scene_manager_next_scene }, /* scene_manager_next_scene */
    { .hash = 0xc35dcba2, .address = (uint32_t)widget_add_icon_element }, /* widget_add_icon_element */
    { .hash = 0xc383030c, .address = (uint32_t)&message_delay_500 }, /* message_delay_500 */
    { .hash = 0xc3c55930, .address = (uint32_t)widget_add_string_element }, /* widget_add_string_element */
    { .hash = 0xc43afa08, .address = (uint32_t)mjs_get_double }, /* mjs_get_double */
    { .hash = 0xc4480b87, .address = (uint32_t)mjs_to_string }, /* mjs_to_string */
    { .hash = 0xc4e0d2ba, .address = (uint32_t)storage_file_free }, /* storage_file_free */
    { .hash = 0xc4e5b9aa, .address = (uint32_t)storage_file_open }, /* storage_file_open */
    { .hash = 0xc4e72f74, .address = (uint32_t)storage_file_read }, /* storage_file_read */
    { .hash = 0xc4e7bc60, .address = (uint32_t)storage_file_seek }, /* storage_file_seek */
    { .hash = 0xc4e7d013, .address = (uint32_t)storage_file_size }, /* storage_file_size */
    { .hash = 0xc648e496, .address = (uint32_t)subghz_setting_free }, /* subghz_setting_free */
    { .hash = 0xc64c2194, .address = (uint32_t)subghz_setting_load }, /* subghz_setting_load */
    { .hash = 0xc655b74e, .address = (uint32_t)submenu_alloc }, /* submenu_alloc */
    { .hash = 0xc6a36b60, .address = (uint32_t)subghz_environment_get_keystore }, /* subghz_environment_get_keystore */
    { .hash = 0xc77ee764, .address = (uint32_t)bt_disconnect }, /* bt_disconnect */
    { .hash = 0xc7859dc6, .address = (uint32_t)submenu_reset }, /* submenu_reset */
    { .hash = 0xc8a8c265, .address = (uint32_t)subghz_keystore_get_data }, /* subghz_keystore_get_data */
    { .hash = 0xc9b48880, .address = (uint32_t)furi_thread_set_current_priority }, /* furi_thread_set_current_priority */
    { .hash = 0xc9e513e8, .address = (uint32_t)mjs_arg }, /* mjs_arg */
    { .hash = 0xc9e51f03, .address = (uint32_t)mjs_del }, /* mjs_del */
    { .hash = 0xc9e52bce, .address = (uint32_t)mjs_get }, /* mjs_get */
    { .hash = 0xc9e55022, .address = (uint32_t)mjs_own }, /* mjs_own */
    { .hash = 0xc9e55eda, .address = (uint32_t)mjs_set }, /* mjs_set */
    { .hash = 0xca4a6509, .address = (uint32_t)mjs_is_boolean }, /* mjs_is_boolean */
    { .hash = 0xca77a4a5, .address = (uint32_t)&message_vibro_off }, /* message_vibro_off */
    { .hash = 0xcb00debe, .address = (uint32_t)mjs_get_global }, /* mjs_get_global */
    { .hash = 0xcb5a4f62, .address = (uint32_t)mjs_create }, /* mjs_create */
    { .hash = 0xcbb8d3d7, .address = (uint32_t)dialog_file_browser_show }, /* dialog_file_browser_show */
    { .hash = 0xcbfeb206, .address = (uint32_t)furi_string_alloc_set }, /* furi_string_alloc_set */
    { .hash = 0xcc6ae517, .address = (uint32_t)subghz_devices_idle }, /* subghz_devices_idle */
    { .hash = 0xcc6b0f4d, .address = (uint32_t)subghz_devices_init }, /* subghz_devices_init */
    { .hash = 0xccd89f64, .address = (uint32_t)loading_free }, /* loading_free */
    { .hash = 0xcd1484c2, .address = (uint32_t)mjs_disown }, /* mjs_disown */
    { .hash = 0xce3c78d1, .address = (uint32_t)scene_manager_has_previous_scene }, /* scene_manager_has_previous_scene */
    { .hash = 0xcfb7dc05, .address = (uint32_t)submenu_free }, /* submenu_free */
    { .hash = 0xcfbd5603, .address = (uint32_t)&sequence_semi_success }, /* sequence_semi_success */
    { .hash = 0xd00066e0, .address = (uint32_t)furi_string_equal_str }, /* furi_string_equal_str */
    { .hash = 0xd05afca7, .address = (uint32_t)&sequence_error }, /* sequence_error */
    { .hash = 0xd25b78fc, .address = (uint32_t)text_box_set_focus }, /* text_box_set_focus */
    { .hash = 0xd3b121a8, .address = (uint32_t)mjs_is_array }, /* mjs_is_array */
    { .hash = 0xd3ee8433, .address = (uint32_t)view_dispatcher_switch_to_view }, /* view_dispatcher_switch_to_view */
    { .hash = 0xd4aa49a0, .address = (uint32_t)manchester_advance }, /* manchester_advance */
    { .hash = 0xd5adbc28, .address = (uint32_t)flipper_format_file_alloc }, /* flipper_format_file_alloc */
    { .hash = 0xd6a9f1d0, .address = (uint32_t)view_set_exit_callback }, /* view_set_exit_callback */
    { .hash = 0xd7091c95, .address = (uint32_t)variable_item_list_free }, /* variable_item_list_free */
    { .hash = 0xd78d168d, .address = (uint32_t)subghz_protocol_decoder_base_get_string }, /* subghz_protocol_decoder_base_get_string */
    { .hash = 0xd83f118e, .address = (uint32_t)mjs_mk_number }, /* mjs_mk_number */
    { .hash = 0xd85bd689, .address = (uint32_t)mjs_nargs }, /* mjs_nargs */
    { .hash = 0xd8b8ff20, .address = (uint32_t)view_dispatcher_attach_to_gui }, /* view_dispatcher_attach_to_gui */
    { .hash = 0xd8baf456, .address = (uint32_t)scene_manager_previous_scene }, /* scene_manager_previous_scene */
    { .hash = 0xd8d0e145, .address = (uint32_t)scene_manager_set_scene_state }, /* scene_manager_set_scene_state */
    { .hash = 0xd91f014d, .address = (uint32_t)view_dispatcher_set_navigation_event_callback }, /* view_dispatcher_set_navigation_event_callback */
    { .hash = 0xd93acffc, .address = (uint32_t)mjs_mk_object }, /* mjs_mk_object */
    { .hash = 0xdb02a5b6, .address = (uint32_t)canvas_string_width }, /* canvas_string_width */
    { .hash = 0xdc070dfa, .address = (uint32_t)dialog_message_free }, /* dialog_message_free */
    { .hash = 0xdc0cbdab, .address = (uint32_t)text_box_get_view }, /* text_box_get_view */
    { .hash = 0xdc0e05b9, .address = (uint32_t)dialog_message_show }, /* dialog_message_show */
    { .hash = 0xdc386f64, .address = (uint32_t)subghz_devices_flush_rx }, /* subghz_devices_flush_rx */
    { .hash = 0xdc6ddfaa, .address = (uint32_t)view_dispatcher_send_custom_event }, /* view_dispatcher_send_custom_event */
    { .hash = 0xdcf93e25, .address = (uint32_t)flipper_format_update_hex }, /* flipper_format_update_hex */
    { .hash = 0xdd41e5e1, .address = (uint32_t)&subghz_protocol_registry }, /* subghz_protocol_registry */
    { .hash = 0xdd64b412, .address = (uint32_t)ble_profile_serial_tx }, /* ble_profile_serial_tx */
    { .hash = 0xddc80662, .address = (uint32_t)flipper_format_read_header }, /* flipper_format_read_header */
    { .hash = 0xdf6806b0, .address = (uint32_t)subghz_devices_end }, /* subghz_devices_end */
    { .hash = 0xe07429bb, .address = (uint32_t)mjs_is_undefined }, /* mjs_is_undefined */
    { .hash = 0xe187b854, .address = (uint32_t)view_set_enter_callback }, /* view_set_enter_callback */
    { .hash = 0xe1d208c0, .address = (uint32_t)subghz_setting_get_frequency_default_index }, /* subghz_setting_get_frequency_default_index */
    { .hash = 0xe380fc4d, .address = (uint32_t)furi_string_get_char }, /* furi_string_get_char */
    { .hash = 0xe3812d8b, .address = (uint32_t)furi_string_get_cstr }, /* furi_string_get_cstr */
    { .hash = 0xe381bfbc, .address = (uint32_t)loading_get_view }, /* loading_get_view */
    { .hash = 0xe3d9a0fc, .address = (uint32_t)mjs_mk_string }, /* mjs_mk_string */
    { .hash = 0xe4161c5e, .address = (uint32_t)&I_DolphinWait_59x54 }, /* I_DolphinWait_59x54 */
    { .hash = 0xe41634f2, .address = (uint32_t)furi_string_free }, /* furi_string_free */
    { .hash = 0xe41d324b, .address = (uint32_t)furi_string_size }, /* furi_string_size */
    { .hash = 0xe5008d82, .address = (uint32_t)furi_message_queue_get }, /* furi_message_queue_get */
    { .hash = 0xe500b5db, .address = (uint32_t)furi_message_queue_put }, /* furi_message_queue_put */
    { .hash = 0xe5b27f65, .address = (uint32_t)subghz_block_generic_serialize }, /* subghz_block_generic_serialize */
    { .hash = 0xe5b2f9b9, .address = (uint32_t)subghz_setting_get_preset_data_size }, /* subghz_setting_get_preset_data_size */
    { .hash = 0xe6563372, .address = (uint32_t)mjs_exec_file }, /* mjs_exec_file */
    { .hash = 0xe7914ee4, .address = (uint32_t)mjs_get_string }, /* mjs_get_string */
    { .hash = 0xe860249c, .address = (uint32_t)elements_bold_rounded_frame }, /* elements_bold_rounded_frame */
    { .hash = 0xe88d2352, .address = (uint32_t)bt_profile_restore_default }, /* bt_profile_restore_default */
    { .hash = 0xe9951039, .address = (uint32_t)canvas_draw_str_aligned }, /* canvas_draw_str_aligned */
    { .hash = 0xeb352f76, .address = (uint32_t)canvas_draw_box }, /* canvas_draw_box */
    { .hash = 0xeb3537f4, .address = (uint32_t)canvas_draw_dot }, /* canvas_draw_dot */
    { .hash = 0xeb357866, .address = (uint32_t)canvas_draw_str }, /* canvas_draw_str */
    { .hash = 0xec3e8481, .address = (uint32_t)storage_common_exists }, /* storage_common_exists */
    { .hash = 0xed39c32e, .address = (uint32_t)dialog_file_browser_set_basic_options }, /* dialog_file_browser_set_basic_options */
    { .hash = 0xed7500ce, .address = (uint32_t)mjs_return }, /* mjs_return */
    { .hash = 0xed9a2eb2, .address = (uint32_t)&I_Scanning_123x52 }, /* I_Scanning_123x52 */
    { .hash = 0xee46ed8c, .address = (uint32_t)stream_read }, /* stream_read */
    { .hash = 0xee477a78, .address = (uint32_t)stream_seek }, /* stream_seek */
    { .hash = 0xef1a0cd3, .address = (uint32_t)text_box_set_font }, /* text_box_set_font */
    { .hash = 0xef2190e1, .address = (uint32_t)text_box_set_text }, /* text_box_set_text */
    { .hash = 0xef2a9297, .address = (uint32_t)furi_mutex_alloc }, /* furi_mutex_alloc */
    { .hash = 0xef3e3f1c, .address = (uint32_t)variable_item_list_add }, /* variable_item_list_add */
    { .hash = 0xf0345139, .address = (uint32_t)subghz_setting_get_preset_data_by_name }, /* subghz_setting_get_preset_data_by_name */
    { .hash = 0xf0d83307, .address = (uint32_t)mjs_strcmp }, /* mjs_strcmp */
    { .hash = 0xf11bc8a4, .address = (uint32_t)subghz_transmitter_deserialize }, /* subghz_transmitter_deserialize */
    { .hash = 0xf4296c90, .address = (uint32_t)gui_add_view_port }, /* gui_add_view_port */
    { .hash = 0xf4738880, .address = (uint32_t)subghz_worker_set_context }, /* subghz_worker_set_context */
    { .hash = 0xf4c9bfe6, .address = (uint32_t)flipper_format_insert_or_update_string_cstr }, /* flipper_format_insert_or_update_string_cstr */
    { .hash = 0xf5e616f3, .address = (uint32_t)calloc }, /* calloc */
    { .hash = 0xf64d7879, .address = (uint32_t)mjs_get_bool }, /* mjs_get_bool */
    { .hash = 0xf69ff222, .address = (uint32_t)furi_string_set_strn }, /* furi_string_set_strn */
    { .hash = 0xf743cca9, .address = (uint32_t)mjs_set_errorf }, /* mjs_set_errorf */
    { .hash = 0xf7d65020, .address = (uint32_t)furi_string_push_back }, /* furi_string_push_back */
    { .hash = 0xf808955b, .address = (uint32_t)__udivdi3 }, /* __udivdi3 */
    { .hash = 0xf8382627, .address = (uint32_t)memmgr_get_free_heap }, /* memmgr_get_free_heap */
    { .hash = 0xf8527efe, .address = (uint32_t)furi_hal_infrared_async_tx_wait_termination }, /* furi_hal_infrared_async_tx_wait_termination */
    { .hash = 0xf8899db0, .address = (uint32_t)flipper_format_read_string }, /* flipper_format_read_string */
    { .hash = 0xf927c5ad, .address = (uint32_t)variable_item_get_current_value_index }, /* variable_item_get_current_value_index */
    { .hash = 0xf95f7860, .address = (uint32_t)subghz_setting_load_custom_preset }, /* subghz_setting_load_custom_preset */
    { .hash = 0xfa6b18ae, .address = (uint32_t)view_port_alloc }, /* view_port_alloc */
    { .hash = 0xfacb9ae5, .address = (uint32_t)submenu_set_selected_item }, /* submenu_set_selected_item */
    { .hash = 0xfb46fcd5, .address = (uint32_t)subghz_devices_is_frequency_valid }, /* subghz_devices_is_frequency_valid */
    { .hash = 0xfb769098, .address = (uint32_t)path_extract_filename_no_ext }, /* path_extract_filename_no_ext */
    { .hash = 0xfbada23e, .address = (uint32_t)subghz_devices_is_connect }, /* subghz_devices_is_connect */
    { .hash = 0xfbd7a5eb, .address = (uint32_t)subghz_devices_load_preset }, /* subghz_devices_load_preset */
    { .hash = 0xfbfdcf2a, .address = (uint32_t)furi_hal_power_enable_otg }, /* furi_hal_power_enable_otg */
    { .hash = 0xfc6ad6de, .address = (uint32_t)flipper_format_read_uint32 }, /* flipper_format_read_uint32 */
    { .hash = 0xfdf5a8c7, .address = (uint32_t)view_dispatcher_free }, /* view_dispatcher_free */
    { .hash = 0xfdfcd38b, .address = (uint32_t)view_dispatcher_stop }, /* view_dispatcher_stop */
    { .hash = 0xfe2b16b2, .address = (uint32_t)mjs_get_context }, /* mjs_get_context */
    { .hash = 0xfe65dcb3, .address = (uint32_t)mjs_is_foreign }, /* mjs_is_foreign */
    { .hash = 0xfea5c5ed, .address = (uint32_t)subghz_devices_start_async_rx }, /* subghz_devices_start_async_rx */
    { .hash = 0xfea5c62f, .address = (uint32_t)subghz_devices_start_async_tx }, /* subghz_devices_start_async_tx */
};
/* clang-format on */

static const HashtableApiInterface firmware_api_impl = {
    .base =
        {
            .api_version_major = 1,
            .api_version_minor = 0,
            .resolver_callback = elf_resolve_from_hashtable,
        },
    .table_begin = firmware_api_table,
    .table_end = firmware_api_table + (sizeof(firmware_api_table) / sizeof(firmware_api_table[0])),
};

const ElfApiInterface* const firmware_api_interface = &firmware_api_impl.base;
