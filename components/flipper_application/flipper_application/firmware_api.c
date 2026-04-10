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
#include <notification/notification_messages.h>
#include <storage/storage.h>
#include <dialogs/dialogs.h>
#include <flipper_format/flipper_format.h>
#include <toolbox/stream/stream.h>
#include <toolbox/manchester_decoder.h>
#include <furi_hal_gpio.h>
/* subghz headers - resolved via subghz component INCLUDE_DIRS */
#include "devices.h"
#include "receiver.h"
#include "environment.h"
#include "subghz_setting.h"
#include "subghz_worker.h"
#include "subghz_keystore.h"
#include "base.h"
#include "generic.h"
#include "protocol_items.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* math functions needed by FAPs but not declared in ESP-IDF newlib math.h with -std=gnu17 */
extern float cosf(float);
extern float sinf(float);

/* subghz functions that may have different names in this firmware version */
extern void subghz_protocol_blocks_add_bit(void*, uint8_t);
extern uint32_t subghz_protocol_blocks_get_hash_data(void*, size_t);

/* GCC runtime helpers (from libgcc, linked into firmware) */
extern long long __udivdi3(long long, long long);
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
    { .hash = 0x00454575, .address = (uint32_t)variable_item_list_set_selected_item },
    { .hash = 0x0221ca54, .address = (uint32_t)view_allocate_model },
    { .hash = 0x031b8511, .address = (uint32_t)subghz_block_generic_deserialize_check_count_bit },
    { .hash = 0x04a1a19a, .address = (uint32_t)furi_string_cat_printf },
    { .hash = 0x04af1dec, .address = (uint32_t)subghz_protocol_blocks_add_bit },
    { .hash = 0x04ff5882, .address = (uint32_t)canvas_set_font },
    { .hash = 0x0549bd0a, .address = (uint32_t)furi_record_open },
    { .hash = 0x05b9c541, .address = (uint32_t)view_free },
    { .hash = 0x08d63d78, .address = (uint32_t)flipper_format_file_open_new },
    { .hash = 0x0922fe88, .address = (uint32_t)view_dispatcher_add_view },
    { .hash = 0x099ec15c, .address = (uint32_t)&I_Lock_7x8 },
    { .hash = 0x09b86ac7, .address = (uint32_t)flipper_format_rewind },
    { .hash = 0x0c3ff887, .address = (uint32_t)elements_button_left },
    { .hash = 0x0d827590, .address = (uint32_t)memcpy },
    { .hash = 0x0d82b830, .address = (uint32_t)memset },
    { .hash = 0x0ddbbb90, .address = (uint32_t)scene_manager_search_and_switch_to_previous_scene },
    { .hash = 0x0eab8c9c, .address = (uint32_t)furi_string_set },
    { .hash = 0x0f11ed7d, .address = (uint32_t)abort },
    { .hash = 0x0fa010db, .address = (uint32_t)subghz_worker_alloc },
    { .hash = 0x1060307d, .address = (uint32_t)srand },
    { .hash = 0x10e9fe9e, .address = (uint32_t)subghz_worker_start },
    { .hash = 0x10ec782d, .address = (uint32_t)&I_DolphinDone_80x58 },
    { .hash = 0x11115707, .address = (uint32_t)widget_add_rect_element },
    { .hash = 0x1214bfff, .address = (uint32_t)__bswapdi2 },
    { .hash = 0x1214ffce, .address = (uint32_t)__bswapsi2 },
    { .hash = 0x135baa10, .address = (uint32_t)notification_message },
    { .hash = 0x1761a36b, .address = (uint32_t)__ashldi3 },
    { .hash = 0x177a11ff, .address = (uint32_t)subghz_protocol_decoder_base_serialize },
    { .hash = 0x191d9eb3, .address = (uint32_t)scene_manager_handle_custom_event },
    { .hash = 0x1a12d6a6, .address = (uint32_t)variable_item_set_current_value_text },
    { .hash = 0x1c270cad, .address = (uint32_t)subghz_receiver_free },
    { .hash = 0x1c9395bb, .address = (uint32_t)strchr },
    { .hash = 0x1c93965e, .address = (uint32_t)strcmp },
    { .hash = 0x1c93bb9d, .address = (uint32_t)strlen },
    { .hash = 0x1c93db57, .address = (uint32_t)strstr },
    { .hash = 0x1fdf4f77, .address = (uint32_t)&I_WarningDolphin_45x42 },
    { .hash = 0x23d393d7, .address = (uint32_t)storage_dir_exists },
    { .hash = 0x245caa0e, .address = (uint32_t)subghz_block_generic_deserialize },
    { .hash = 0x25ca24fe, .address = (uint32_t)flipper_format_read_hex },
    { .hash = 0x2a5091ac, .address = (uint32_t)subghz_receiver_set_rx_callback },
    { .hash = 0x2aaa426e, .address = (uint32_t)text_input_set_result_callback },
    { .hash = 0x2ad352a8, .address = (uint32_t)variable_item_list_set_enter_callback },
    { .hash = 0x2b03fe22, .address = (uint32_t)view_dispatcher_set_event_callback_context },
    { .hash = 0x2c940f5a, .address = (uint32_t)text_input_free },
    { .hash = 0x2e42fe69, .address = (uint32_t)furi_hal_power_is_otg_enabled },
    { .hash = 0x30aa559e, .address = (uint32_t)furi_string_alloc_set_str },
    { .hash = 0x3177bc39, .address = (uint32_t)scene_manager_get_scene_state },
    { .hash = 0x34a48004, .address = (uint32_t)furi_get_tick },
    { .hash = 0x358068e2, .address = (uint32_t)widget_add_string_multiline_element },
    { .hash = 0x358932d0, .address = (uint32_t)furi_log_print_format },
    { .hash = 0x3595bd57, .address = (uint32_t)furi_hal_power_disable_otg },
    { .hash = 0x3702827f, .address = (uint32_t)canvas_draw_circle },
    { .hash = 0x37e825d8, .address = (uint32_t)subghz_worker_set_overrun_callback },
    { .hash = 0x3af87667, .address = (uint32_t)canvas_clear },
    { .hash = 0x3b0aa073, .address = (uint32_t)widget_alloc },
    { .hash = 0x3b2ba133, .address = (uint32_t)subghz_devices_get_by_name },
    { .hash = 0x3c3a86eb, .address = (uint32_t)widget_reset },
    { .hash = 0x3d8f9026, .address = (uint32_t)subghz_worker_rx_callback },
    { .hash = 0x3dc68bc3, .address = (uint32_t)scene_manager_handle_tick_event },
    { .hash = 0x3de00ec7, .address = (uint32_t)realloc },
    { .hash = 0x3e1255ee, .address = (uint32_t)__furi_crash_implementation },
    { .hash = 0x42d0c164, .address = (uint32_t)view_set_draw_callback },
    { .hash = 0x42d9075a, .address = (uint32_t)flipper_format_write_string_cstr },
    { .hash = 0x43e3a6c1, .address = (uint32_t)subghz_protocol_blocks_get_hash_data },
    { .hash = 0x49506e3f, .address = (uint32_t)__floatsidf },
    { .hash = 0x4b45a060, .address = (uint32_t)__assert_func },
    { .hash = 0x4b64bd4d, .address = (uint32_t)view_dispatcher_remove_view },
    { .hash = 0x4d09b22a, .address = (uint32_t)stream_eof },
    { .hash = 0x5124cf69, .address = (uint32_t)scene_manager_handle_back_event },
    { .hash = 0x51dc1d30, .address = (uint32_t)canvas_draw_disc },
    { .hash = 0x51dec116, .address = (uint32_t)canvas_draw_icon },
    { .hash = 0x51e07f95, .address = (uint32_t)canvas_draw_line },
    { .hash = 0x54803eb8, .address = (uint32_t)view_commit_model },
    { .hash = 0x568eaf4c, .address = (uint32_t)flipper_format_write_header_cstr },
    { .hash = 0x5949547e, .address = (uint32_t)subghz_devices_begin },
    { .hash = 0x59c9920a, .address = (uint32_t)canvas_draw_rframe },
    { .hash = 0x59cd36e3, .address = (uint32_t)furi_string_printf },
    { .hash = 0x5a6b0f1c, .address = (uint32_t)subghz_devices_reset },
    { .hash = 0x5a74b905, .address = (uint32_t)furi_hal_power_suppress_charge_enter },
    { .hash = 0x5a80c2b2, .address = (uint32_t)subghz_devices_sleep },
    { .hash = 0x5bab36b9, .address = (uint32_t)variable_item_set_current_value_index },
    { .hash = 0x5c8b27e3, .address = (uint32_t)dialog_message_alloc },
    { .hash = 0x5cce8b34, .address = (uint32_t)furi_string_set_str },
    { .hash = 0x5d5c8208, .address = (uint32_t)furi_delay_ms },
    { .hash = 0x5dd98f20, .address = (uint32_t)subghz_environment_load_keystore },
    { .hash = 0x5f1f6d6d, .address = (uint32_t)variable_item_list_get_view },
    { .hash = 0x613f9d29, .address = (uint32_t)subghz_protocol_blocks_reverse_key },
    { .hash = 0x64f8327f, .address = (uint32_t)flipper_format_file_open_always },
    { .hash = 0x662589fc, .address = (uint32_t)__lshrdi3 },
    { .hash = 0x667f2ddb, .address = (uint32_t)furi_string_alloc },
    { .hash = 0x66c82dff, .address = (uint32_t)furi_string_empty },
    { .hash = 0x6728399c, .address = (uint32_t)subghz_receiver_set_filter },
    { .hash = 0x67af1453, .address = (uint32_t)furi_string_reset },
    { .hash = 0x689b9fa6, .address = (uint32_t)view_set_input_callback },
    { .hash = 0x69996716, .address = (uint32_t)elements_scrollbar_pos },
    { .hash = 0x6aa6f432, .address = (uint32_t)subghz_setting_get_hopper_frequency },
    { .hash = 0x6c831a1c, .address = (uint32_t)furi_string_replace_all_str },
    { .hash = 0x6c93a048, .address = (uint32_t)view_dispatcher_set_tick_event_callback },
    { .hash = 0x6e8609dc, .address = (uint32_t)variable_item_get_context },
    { .hash = 0x73dcb981, .address = (uint32_t)__getreent },
    { .hash = 0x7434bbb5, .address = (uint32_t)widget_add_button_element },
    { .hash = 0x77202198, .address = (uint32_t)elements_multiline_text },
    { .hash = 0x79c45736, .address = (uint32_t)subghz_environment_alloc },
    { .hash = 0x7ac1a9a0, .address = (uint32_t)subghz_environment_set_protocol_registry },
    { .hash = 0x7c954070, .address = (uint32_t)cosf },
    { .hash = 0x7c96f087, .address = (uint32_t)free },
    { .hash = 0x7c9d3dea, .address = (uint32_t)rand },
    { .hash = 0x7c9dec55, .address = (uint32_t)sinf },
    { .hash = 0x7e7a5018, .address = (uint32_t)storage_file_exists },
    { .hash = 0x7e99681e, .address = (uint32_t)&I_Pin_back_arrow_10x8 },
    { .hash = 0x81680f4c, .address = (uint32_t)esp_rom_printf },
    { .hash = 0x83d1579a, .address = (uint32_t)view_dispatcher_run },
    { .hash = 0x84de816c, .address = (uint32_t)widget_add_text_scroll_element },
    { .hash = 0x870064dd, .address = (uint32_t)submenu_get_view },
    { .hash = 0x871f6356, .address = (uint32_t)subghz_devices_deinit },
    { .hash = 0x891c181f, .address = (uint32_t)subghz_setting_get_preset_data },
    { .hash = 0x89219306, .address = (uint32_t)subghz_setting_get_preset_name },
    { .hash = 0x8b21ae9a, .address = (uint32_t)submenu_add_item },
    { .hash = 0x8ba7e5f0, .address = (uint32_t)text_input_set_header_text },
    { .hash = 0x8bc6a6ef, .address = (uint32_t)view_set_context },
    { .hash = 0x8e57da4c, .address = (uint32_t)subghz_worker_is_running },
    { .hash = 0x8ea562b6, .address = (uint32_t)sequence_success },
    { .hash = 0x8ec45c0d, .address = (uint32_t)furi_string_alloc_printf },
    { .hash = 0x8f07d1ff, .address = (uint32_t)subghz_setting_alloc },
    { .hash = 0x8fadabe5, .address = (uint32_t)subghz_devices_stop_async_rx },
    { .hash = 0x91ae8039, .address = (uint32_t)flipper_format_insert_or_update_uint32 },
    { .hash = 0x92c03ab2, .address = (uint32_t)text_input_get_view },
    { .hash = 0x92fe76cf, .address = (uint32_t)view_get_model },
    { .hash = 0x9424ab13, .address = (uint32_t)subghz_protocol_decoder_base_get_hash_data },
    { .hash = 0x949adeb3, .address = (uint32_t)subghz_worker_set_pair_callback },
    { .hash = 0x9531b48a, .address = (uint32_t)widget_free },
    { .hash = 0x98383973, .address = (uint32_t)furi_thread_free },
    { .hash = 0x983a5ec1, .address = (uint32_t)furi_thread_join },
    { .hash = 0x985ace0d, .address = (uint32_t)storage_simply_mkdir },
    { .hash = 0x9c136b60, .address = (uint32_t)flipper_format_free },
    { .hash = 0x9f74bed6, .address = (uint32_t)subghz_devices_set_frequency },
    { .hash = 0x9f8ba228, .address = (uint32_t)furi_string_cmp_str },
    { .hash = 0xa02bb03f, .address = (uint32_t)furi_thread_start },
    { .hash = 0xa033707f, .address = (uint32_t)flipper_format_string_alloc },
    { .hash = 0xa0924b48, .address = (uint32_t)furi_thread_yield },
    { .hash = 0xa1cc46d7, .address = (uint32_t)sequence_blink_cyan_10 },
    { .hash = 0xa1dae36e, .address = (uint32_t)subghz_receiver_reset },
    { .hash = 0xa2445322, .address = (uint32_t)flipper_format_get_value_count },
    { .hash = 0xa4287b78, .address = (uint32_t)view_dispatcher_set_custom_event_callback },
    { .hash = 0xa44210ee, .address = (uint32_t)subghz_keystore_raw_get_data },
    { .hash = 0xa4b4168a, .address = (uint32_t)canvas_set_color },
    { .hash = 0xa5a69b61, .address = (uint32_t)furi_hal_power_suppress_charge_exit },
    { .hash = 0xa666cb6b, .address = (uint32_t)elements_multiline_text_aligned },
    { .hash = 0xa8a3ba4e, .address = (uint32_t)scene_manager_free },
    { .hash = 0xa9e82298, .address = (uint32_t)furi_thread_alloc_ex },
    { .hash = 0xa9e95939, .address = (uint32_t)subghz_devices_get_rssi },
    { .hash = 0xa9f02c63, .address = (uint32_t)__divdf3 },
    { .hash = 0xa9f06c32, .address = (uint32_t)__divsf3 },
    { .hash = 0xaa228e4e, .address = (uint32_t)subghz_devices_set_rx },
    { .hash = 0xaa228e90, .address = (uint32_t)subghz_devices_set_tx },
    { .hash = 0xaa5738d2, .address = (uint32_t)dialog_message_set_buttons },
    { .hash = 0xaa99e8c9, .address = (uint32_t)subghz_setting_get_default_frequency },
    { .hash = 0xab26b7f2, .address = (uint32_t)subghz_worker_free },
    { .hash = 0xab2de2b6, .address = (uint32_t)subghz_worker_stop },
    { .hash = 0xac94b72e, .address = (uint32_t)subghz_setting_get_preset_count },
    { .hash = 0xada6324e, .address = (uint32_t)furi_record_close },
    { .hash = 0xae5e1eed, .address = (uint32_t)subghz_environment_free },
    { .hash = 0xaef6d1a4, .address = (uint32_t)storage_simply_remove },
    { .hash = 0xaf0c3fcc, .address = (uint32_t)strncmp },
    { .hash = 0xaf0cf7e2, .address = (uint32_t)widget_get_view },
    { .hash = 0xaf0e70ad, .address = (uint32_t)strrchr },
    { .hash = 0xafb971d9, .address = (uint32_t)sequence_double_vibro },
    { .hash = 0xaffa14ed, .address = (uint32_t)subghz_setting_get_frequency_count },
    { .hash = 0xb02115ff, .address = (uint32_t)flipper_format_write_string },
    { .hash = 0xb06457a4, .address = (uint32_t)__truncdfsf2 },
    { .hash = 0xb12ba139, .address = (uint32_t)flipper_format_file_open_existing },
    { .hash = 0xb260e7ad, .address = (uint32_t)flipper_format_write_hex },
    { .hash = 0xb3b01449, .address = (uint32_t)subghz_receiver_alloc_init },
    { .hash = 0xb4024f2d, .address = (uint32_t)flipper_format_write_uint32 },
    { .hash = 0xb4e4e3ac, .address = (uint32_t)dialog_message_set_header },
    { .hash = 0xb7a22125, .address = (uint32_t)subghz_setting_get_frequency },
    { .hash = 0xb7cf09de, .address = (uint32_t)variable_item_list_alloc },
    { .hash = 0xb8fef056, .address = (uint32_t)variable_item_list_reset },
    { .hash = 0xbc4d1c50, .address = (uint32_t)view_dispatcher_alloc },
    { .hash = 0xbc867b2f, .address = (uint32_t)subghz_receiver_decode },
    { .hash = 0xbc94c80a, .address = (uint32_t)view_alloc },
    { .hash = 0xbcbd5eb7, .address = (uint32_t)scene_manager_alloc },
    { .hash = 0xbcfff93e, .address = (uint32_t)fprintf },
    { .hash = 0xbdd69f1b, .address = (uint32_t)memmove },
    { .hash = 0xbde04aac, .address = (uint32_t)dialog_message_set_icon },
    { .hash = 0xbde65c88, .address = (uint32_t)dialog_message_set_text },
    { .hash = 0xbe582cf9, .address = (uint32_t)snprintf },
    { .hash = 0xbeb85543, .address = (uint32_t)text_input_alloc },
    { .hash = 0xbec63dba, .address = (uint32_t)subghz_setting_get_hopper_frequency_count },
    { .hash = 0xbf85a879, .address = (uint32_t)elements_string_fit_width },
    { .hash = 0xbfc2444e, .address = (uint32_t)__muldf3 },
    { .hash = 0xbfe83bbb, .address = (uint32_t)text_input_reset },
    { .hash = 0xc0fe5a29, .address = (uint32_t)flipper_format_read_int32 },
    { .hash = 0xc2c80038, .address = (uint32_t)scene_manager_next_scene },
    { .hash = 0xc35dcba2, .address = (uint32_t)widget_add_icon_element },
    { .hash = 0xc3c55930, .address = (uint32_t)widget_add_string_element },
    { .hash = 0xc648e496, .address = (uint32_t)subghz_setting_free },
    { .hash = 0xc64c2194, .address = (uint32_t)subghz_setting_load },
    { .hash = 0xc655b74e, .address = (uint32_t)submenu_alloc },
    { .hash = 0xc6a36b60, .address = (uint32_t)subghz_environment_get_keystore },
    { .hash = 0xc7859dc6, .address = (uint32_t)submenu_reset },
    { .hash = 0xc8a8c265, .address = (uint32_t)subghz_keystore_get_data },
    { .hash = 0xcbb8d3d7, .address = (uint32_t)dialog_file_browser_show },
    { .hash = 0xcbfeb206, .address = (uint32_t)furi_string_alloc_set },
    { .hash = 0xcc6ae517, .address = (uint32_t)subghz_devices_idle },
    { .hash = 0xcc6b0f4d, .address = (uint32_t)subghz_devices_init },
    { .hash = 0xcfb7dc05, .address = (uint32_t)submenu_free },
    { .hash = 0xcfbd5603, .address = (uint32_t)sequence_semi_success },
    { .hash = 0xd00066e0, .address = (uint32_t)furi_string_equal_str },
    { .hash = 0xd05afca7, .address = (uint32_t)sequence_error },
    { .hash = 0xd3ee8433, .address = (uint32_t)view_dispatcher_switch_to_view },
    { .hash = 0xd4aa49a0, .address = (uint32_t)manchester_advance },
    { .hash = 0xd5adbc28, .address = (uint32_t)flipper_format_file_alloc },
    { .hash = 0xd6a9f1d0, .address = (uint32_t)view_set_exit_callback },
    { .hash = 0xd7091c95, .address = (uint32_t)variable_item_list_free },
    { .hash = 0xd78d168d, .address = (uint32_t)subghz_protocol_decoder_base_get_string },
    { .hash = 0xd8b8ff20, .address = (uint32_t)view_dispatcher_attach_to_gui },
    { .hash = 0xd8baf456, .address = (uint32_t)scene_manager_previous_scene },
    { .hash = 0xd8d0e145, .address = (uint32_t)scene_manager_set_scene_state },
    { .hash = 0xd91f014d, .address = (uint32_t)view_dispatcher_set_navigation_event_callback },
    { .hash = 0xdb02a5b6, .address = (uint32_t)canvas_string_width },
    { .hash = 0xdc070dfa, .address = (uint32_t)dialog_message_free },
    { .hash = 0xdc0e05b9, .address = (uint32_t)dialog_message_show },
    { .hash = 0xdc386f64, .address = (uint32_t)subghz_devices_flush_rx },
    { .hash = 0xdc6ddfaa, .address = (uint32_t)view_dispatcher_send_custom_event },
    { .hash = 0xddc80662, .address = (uint32_t)flipper_format_read_header },
    { .hash = 0xdf6806b0, .address = (uint32_t)subghz_devices_end },
    { .hash = 0xe187b854, .address = (uint32_t)view_set_enter_callback },
    { .hash = 0xe1d208c0, .address = (uint32_t)subghz_setting_get_frequency_default_index },
    { .hash = 0xe3812d8b, .address = (uint32_t)furi_string_get_cstr },
    { .hash = 0xe4161c5e, .address = (uint32_t)&I_DolphinWait_59x54 },
    { .hash = 0xe41634f2, .address = (uint32_t)furi_string_free },
    { .hash = 0xe41d324b, .address = (uint32_t)furi_string_size },
    { .hash = 0xe5b27f65, .address = (uint32_t)subghz_block_generic_serialize },
    { .hash = 0xe5b2f9b9, .address = (uint32_t)subghz_setting_get_preset_data_size },
    { .hash = 0xe860249c, .address = (uint32_t)elements_bold_rounded_frame },
    { .hash = 0xe9951039, .address = (uint32_t)canvas_draw_str_aligned },
    { .hash = 0xeb352f76, .address = (uint32_t)canvas_draw_box },
    { .hash = 0xeb3537f4, .address = (uint32_t)canvas_draw_dot },
    { .hash = 0xeb357866, .address = (uint32_t)canvas_draw_str },
    { .hash = 0xed39c32e, .address = (uint32_t)dialog_file_browser_set_basic_options },
    { .hash = 0xee46ed8c, .address = (uint32_t)stream_read },
    { .hash = 0xee477a78, .address = (uint32_t)stream_seek },
    { .hash = 0xef3e3f1c, .address = (uint32_t)variable_item_list_add },
    { .hash = 0xf0345139, .address = (uint32_t)subghz_setting_get_preset_data_by_name },
    { .hash = 0xf4738880, .address = (uint32_t)subghz_worker_set_context },
    { .hash = 0xf4c9bfe6, .address = (uint32_t)flipper_format_insert_or_update_string_cstr },
    { .hash = 0xf5e616f3, .address = (uint32_t)calloc },
    { .hash = 0xf7d65020, .address = (uint32_t)furi_string_push_back },
    { .hash = 0xf808955b, .address = (uint32_t)__udivdi3 },
    { .hash = 0xf8382627, .address = (uint32_t)memmgr_get_free_heap },
    { .hash = 0xf8899db0, .address = (uint32_t)flipper_format_read_string },
    { .hash = 0xf927c5ad, .address = (uint32_t)variable_item_get_current_value_index },
    { .hash = 0xfacb9ae5, .address = (uint32_t)submenu_set_selected_item },
    { .hash = 0xfb46fcd5, .address = (uint32_t)subghz_devices_is_frequency_valid },
    { .hash = 0xfbada23e, .address = (uint32_t)subghz_devices_is_connect },
    { .hash = 0xfbd7a5eb, .address = (uint32_t)subghz_devices_load_preset },
    { .hash = 0xfbfdcf2a, .address = (uint32_t)furi_hal_power_enable_otg },
    { .hash = 0xfc6ad6de, .address = (uint32_t)flipper_format_read_uint32 },
    { .hash = 0xfdf5a8c7, .address = (uint32_t)view_dispatcher_free },
    { .hash = 0xfea5c5ed, .address = (uint32_t)subghz_devices_start_async_rx },
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
