#include "hid_helper.h"

// ESP32 port notes
//  - furi_hal_usb_set_config(&usb_hid) can legitimately fail here (USB composite active for
//    qFlipper/USB storage, U2F running, TinyUSB could not be installed) and
//    bt_profile_start() can fail when the BLE stack is busy. Upstream used furi_check() and
//    would reset the whole device; both now end in the "HID error" screen instead.
//  - The playback timer ticks every 10 ms and BLE needs the radio awake: light sleep is
//    inhibited for as long as HID is active.

static void bt_status_changed_callback(BtStatus status, void *context)
{
    App *app = context;
    bool connected = status == BtStatusConnected;
    atomic_store_explicit(&app->ble_connected, connected, memory_order_release);
    if (app->notifications)
    {
        notification_internal_message(
            app->notifications,
            connected ? &sequence_set_blue_255 : &sequence_reset_blue);
    }
    if (app->view_port)
    {
        view_port_update(app->view_port);
    }
}

static void hid_sleep_inhibit_enter(App *app)
{
    if (!app->sleep_inhibited)
    {
        furi_hal_power_light_sleep_inhibit_enter();
        app->sleep_inhibited = true;
    }
}

static void hid_sleep_inhibit_exit(App *app)
{
    if (app->sleep_inhibited)
    {
        furi_hal_power_light_sleep_inhibit_exit();
        app->sleep_inhibited = false;
    }
}

bool hid_init(App *app)
{
    if (app->hid_active)
        return true;
    app->hid_error = NULL;

    if (app->transport == TransportUsb)
    {
        FuriHalUsbInterface *previous = furi_hal_usb_get_config();
        furi_hal_usb_unlock();
        if (!furi_hal_usb_set_config(&usb_hid, NULL))
        {
            app->hid_error = "USB is busy";
            return false;
        }
        app->usb_mode_prev = previous;
    }
    else
    {
        bt_disconnect(app->bt);
        furi_delay_ms(200);
        bt_keys_storage_set_storage_path(app->bt, APP_DATA_PATH("bt_hid.keys"));
        BleProfileHidParams ble_params = {.device_name_prefix = "ClickRec", .mac_xor = 0};
        app->ble_profile =
            bt_profile_start(app->bt, ble_profile_hid, (FuriHalBleProfileParams)&ble_params);
        if (!app->ble_profile)
        {
            bt_keys_storage_set_default_path(app->bt);
            app->hid_error = "Bluetooth unavailable";
            return false;
        }
        if (bt_is_enabled(app->bt))
        {
            furi_hal_bt_start_advertising();
        }
        bt_set_status_changed_callback(app->bt, bt_status_changed_callback, app);
        atomic_store_explicit(&app->ble_connected, false, memory_order_release);
    }
    hid_sleep_inhibit_enter(app);
    app->hid_active = true;
    return true;
}

void hid_deinit(App *app)
{
    if (!app->hid_active)
        return;
    // Stop acting on the HID first: nothing may press a button while the transport goes away.
    // A playback timer callback that is already running gets a moment to leave.
    app->hid_active = false;
    furi_delay_ms(30);
    if (app->transport == TransportUsb)
    {
        furi_hal_usb_set_config(app->usb_mode_prev, NULL);
        app->usb_mode_prev = NULL;
    }
    else
    {
        bt_set_status_changed_callback(app->bt, NULL, NULL);
        bt_disconnect(app->bt);
        furi_delay_ms(200);
        bt_keys_storage_set_default_path(app->bt);
        bt_profile_restore_default(app->bt);
        app->ble_profile = NULL;
        atomic_store_explicit(&app->ble_connected, false, memory_order_release);
        if (app->notifications)
        {
            notification_internal_message(app->notifications, &sequence_reset_blue);
        }
    }
    hid_sleep_inhibit_exit(app);
}

bool hid_ensure_init(App *app)
{
    return app->hid_active || hid_init(app);
}

void hid_mouse_move(App *app, int8_t dx, int8_t dy)
{
    if (!app->hid_active)
        return;
    if (app->transport == TransportUsb)
    {
        furi_hal_hid_mouse_move(dx, dy);
    }
    else if (app->ble_profile)
    {
        ble_profile_hid_mouse_move(app->ble_profile, dx, dy);
    }
}

void hid_mouse_press(App *app)
{
    hid_mouse_press_btn(app, HID_MOUSE_BTN_LEFT);
}

void hid_mouse_release(App *app)
{
    hid_mouse_release_btn(app, HID_MOUSE_BTN_LEFT);
}

void hid_mouse_click(App *app)
{
    hid_mouse_press(app);
    furi_delay_ms(CLICK_HOLD_MS);
    hid_mouse_release(app);
}

bool hid_is_connected(App *app)
{
    if (!app->hid_active)
        return false;
    if (app->transport == TransportUsb)
    {
        return furi_hal_hid_is_connected();
    }
    else
    {
        return app->ble_profile != NULL;
    }
}

void hid_mouse_press_btn(App *app, uint8_t btn)
{
    if (!app->hid_active)
        return;
    if (app->transport == TransportUsb)
    {
        furi_hal_hid_mouse_press(btn);
    }
    else if (app->ble_profile)
    {
        ble_profile_hid_mouse_press(app->ble_profile, btn);
    }
}

void hid_mouse_release_btn(App *app, uint8_t btn)
{
    if (!app->hid_active)
        return;
    if (app->transport == TransportUsb)
    {
        furi_hal_hid_mouse_release(btn);
    }
    else if (app->ble_profile)
    {
        ble_profile_hid_mouse_release(app->ble_profile, btn);
    }
}

void hid_mouse_release_all(App *app)
{
    if (!app->hid_active)
        return;
    uint8_t all = HID_MOUSE_BTN_LEFT | HID_MOUSE_BTN_RIGHT | HID_MOUSE_BTN_WHEEL;
    hid_mouse_release_btn(app, all);
}
