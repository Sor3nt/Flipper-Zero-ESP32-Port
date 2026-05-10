import re

api_file = "/home/adam/Flipper-Zero-ESP32-Port/components/flipper_application/flipper_application/firmware_api.c"

new_entries = [
    '    { .hash = 0x00eaa80e, .address = (uint32_t)furi_hal_bt_extra_beacon_set_config }, /* furi_hal_bt_extra_beacon_set_config */',
    '    { .hash = 0x0fe7c75a, .address = (uint32_t)file_stream_alloc }, /* file_stream_alloc */',
    '    { .hash = 0x10070181, .address = (uint32_t)file_stream_open }, /* file_stream_open */',
    '    { .hash = 0x100c05a5, .address = (uint32_t)file_stream_close }, /* file_stream_close */',
    '    { .hash = 0x10fdc972, .address = (uint32_t)byte_input_alloc }, /* byte_input_alloc */',
    '    { .hash = 0x25dec19e, .address = (uint32_t)furi_string_search_str }, /* furi_string_search_str */',
    '    { .hash = 0x36d439a9, .address = (uint32_t)byte_input_free }, /* byte_input_free */',
    '    { .hash = 0x39b40924, .address = (uint32_t)furi_hal_bt_extra_beacon_is_active }, /* furi_hal_bt_extra_beacon_is_active */',
    '    { .hash = 0x42714df3, .address = (uint32_t)furi_hal_power_get_battery_full_capacity }, /* furi_hal_power_get_battery_full_capacity */',
    '    { .hash = 0x4a721d93, .address = (uint32_t)furi_hal_bt_extra_beacon_stop }, /* furi_hal_bt_extra_beacon_stop */',
    '    { .hash = 0x552ecfcc, .address = (uint32_t)popup_set_icon }, /* popup_set_icon */',
    '    { .hash = 0x67b1132e, .address = (uint32_t)furi_string_right }, /* furi_string_right */',
    '    { .hash = 0x776c253a, .address = (uint32_t)furi_hal_power_get_battery_remaining_capacity }, /* furi_hal_power_get_battery_remaining_capacity */',
    '    { .hash = 0x792d988c, .address = (uint32_t)canvas_set_bitmap_mode }, /* canvas_set_bitmap_mode */',
    '    { .hash = 0x8b4e4a71, .address = (uint32_t)furi_string_start_with_str }, /* furi_string_start_with_str */',
    '    { .hash = 0x962c4710, .address = (uint32_t)view_dispatcher_enable_queue }, /* view_dispatcher_enable_queue */',
    '    { .hash = 0x98b5951b, .address = (uint32_t)furi_hal_bt_extra_beacon_start }, /* furi_hal_bt_extra_beacon_start */',
    '    { .hash = 0xb653a0df, .address = (uint32_t)byte_input_set_header_text }, /* byte_input_set_header_text */',
    '    { .hash = 0xba9af481, .address = (uint32_t)byte_input_get_view }, /* byte_input_get_view */',
    '    { .hash = 0xd2a1cc32, .address = (uint32_t)furi_hal_bt_extra_beacon_set_data }, /* furi_hal_bt_extra_beacon_set_data */',
    '    { .hash = 0xdbbbe852, .address = (uint32_t)popup_disable_timeout }, /* popup_disable_timeout */',
    '    { .hash = 0xdf0ba445, .address = (uint32_t)flipper_format_read_bool }, /* flipper_format_read_bool */',
    '    { .hash = 0xe1adfa83, .address = (uint32_t)furi_string_search_char }, /* furi_string_search_char */',
    '    { .hash = 0xe41de2cc, .address = (uint32_t)furi_string_trim }, /* furi_string_trim */',
    '    { .hash = 0xe987dcdd, .address = (uint32_t)byte_input_set_result_callback }, /* byte_input_set_result_callback */',
    '    { .hash = 0xee296313, .address = (uint32_t)stream_read_line }, /* stream_read_line */',
    '    { .hash = 0xee4090d2, .address = (uint32_t)stream_free }, /* stream_free */',
    '    { .hash = 0xef195c40, .address = (uint32_t)hex_char_to_uint8 }, /* hex_char_to_uint8 */',
    '    { .hash = 0xfe7abcd4, .address = (uint32_t)flipper_format_write_bool }, /* flipper_format_write_bool */',
    '    { .hash = 0xa5a73022, .address = (uint32_t)I_WarningDolphinFlip_45x42 }, /* I_WarningDolphinFlip_45x42 */',
    '    { .hash = 0xf580f8d3, .address = (uint32_t)I_Ok_btn_9x9 }, /* I_Ok_btn_9x9 */',
]

def get_hash(line):
    m = re.search(r'\.hash = (0x[0-9a-f]+)', line)
    return int(m.group(1), 16) if m else 0

with open(api_file, 'r') as f:
    lines = f.readlines()

all_entries = []
before = []
after = []
in_table = False
table_done = False

for line in lines:
    if '{ .hash = ' in line:
        in_table = True
        all_entries.append(line.rstrip())
    elif in_table and not table_done:
        if '};' in line:
            table_done = True
            after.append(line.rstrip())
        else:
            after.append(line.rstrip())
    elif not in_table:
        before.append(line.rstrip())
    else:
        after.append(line.rstrip())

for entry in new_entries:
    if entry not in all_entries:
        all_entries.append(entry)

all_entries.sort(key=get_hash)

with open(api_file, 'w') as f:
    f.write('\n'.join(before) + '\n')
    f.write('\n'.join(all_entries) + '\n')
    f.write('\n'.join(after) + '\n')

print(f"Done! Total entries: {len(all_entries)}")