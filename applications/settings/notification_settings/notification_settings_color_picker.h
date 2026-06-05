/**
 * @file notification_settings_color_picker.h
 * @brief Small RGB color picker view for the "UI Background → Custom" setting.
 *
 * Three channels (R/G/B), each 0-255. While open it pushes the chosen color
 * straight to the LCD background tint (furi_hal_display_set_fg_color) so the
 * whole screen acts as a live preview. Reports the result via a callback:
 * confirmed = Save pressed, !confirmed = Back/cancel.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <gui/view.h>

typedef struct NotificationColorPicker NotificationColorPicker;

/** Picker result callback.
 * @param context    user context passed to notification_color_picker_set_callback
 * @param rgb        chosen color as 0x00RRGGBB (only valid when confirmed)
 * @param confirmed  true = Save, false = cancelled (Back) */
typedef void (*NotificationColorPickerCallback)(void* context, uint32_t rgb, bool confirmed);

NotificationColorPicker* notification_color_picker_alloc(void);

void notification_color_picker_free(NotificationColorPicker* picker);

View* notification_color_picker_get_view(NotificationColorPicker* picker);

void notification_color_picker_set_callback(
    NotificationColorPicker* picker,
    NotificationColorPickerCallback callback,
    void* context);

/** Preload the picker with a starting color (0x00RRGGBB) and push it to the
 * live LCD preview. Call right before switching to the picker view. */
void notification_color_picker_set_color(NotificationColorPicker* picker, uint32_t rgb);
