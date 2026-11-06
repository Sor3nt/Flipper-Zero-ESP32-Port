#include "hid_helper.h"

// ESP32 port notes
//  - The USB transport takes the USB port as an exclusive LEASE (furi_hal_usb_lease_acquire):
//    a running qFlipper session is paused and resumed on release, the previous USB interface
//    is restored by furi_hal_usb_lease_release(), and the lease is refused while USB is locked
//    (an RPC session over USB) or leased to another feature (the USB-UART bridge). The USB
//    lock is never overridden: this app used to call furi_hal_usb_unlock() first, which would
//    have ripped USB away from a running RPC session.
//  - Starting HID can fail (USB busy, TinyUSB could not be installed) and bt_profile_start()
//    can fail when the BLE stack is busy. Upstream used furi_check() and would reset the whole
//    device; both end in the "HID error" screen instead.
//  - The playback timer ticks every 10 ms and BLE needs the radio awake: light sleep is
//    inhibited for as long as HID is active.

static const char hid_usb_owner[] = "Click Recorder";

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

// Give the USB lease back. True when nothing is owned any more; false keeps the lease for a
// retry. Stopping HID alone leaves the TinyUSB HID stack installed and the PHY routed to the
// OTG controller (the host would keep seeing the mouse, and the USB-Serial-JTAG console and
// flashing port stay gone), so the stack is released explicitly first.
static bool hid_usb_release(App *app)
{
    if (!app->usb_lease)
        return true;
    (void)furi_hal_usb_set_config_for_owner(hid_usb_owner, NULL, NULL); // stop HID
    if (!furi_hal_usb_hid_release_for_owner(hid_usb_owner))
        FURI_LOG_W("ClickRec", "HID stack not released; USB-Serial-JTAG may stay off until reboot");
    if (furi_hal_usb_lease_release(hid_usb_owner, app->usb_mode_prev))
    {
        app->usb_lease = false;
        app->usb_mode_prev = NULL;
        return true;
    }
    if (!furi_hal_usb_is_lease_owner(hid_usb_owner))
    {
        // The lease is gone (only resuming a paused owner failed): nothing left to keep.
        app->usb_lease = false;
        app->usb_mode_prev = NULL;
        return true;
    }
    return false;
}

bool hid_init(App *app)
{
    if (app->hid_active)
        return true;
    app->hid_error = NULL;

    if (app->transport == TransportUsb)
    {
        if (!app->usb_lease)
        {
            // Refused while USB is locked or leased to someone else; never forced.
            if (!furi_hal_usb_lease_acquire(hid_usb_owner, &app->usb_mode_prev))
            {
                app->hid_error = "USB is busy";
                return false;
            }
            app->usb_lease = true;
        }
        if (!furi_hal_usb_set_config_for_owner(hid_usb_owner, &usb_hid, NULL))
        {
            app->hid_error = "USB HID failed";
            (void)hid_usb_release(app);
            return false;
        }
    }
    else
    {
        // A USB lease left over from a failed release must not be forgotten (nor the
        // light-sleep veto that went with it).
        if (!hid_usb_release(app))
        {
            app->hid_error = "USB release pending";
            return false;
        }
        hid_sleep_inhibit_exit(app);
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
    // A held button must be let go before the transport goes away, or the host keeps seeing
    // it pressed (USB) / the BLE link drops with it held. Only then stop acting on the HID:
    // nothing may press a button while the transport goes away. A playback timer callback that
    // is already running gets a moment to leave.
    hid_mouse_release_all(app);
    app->hid_active = false;
    furi_delay_ms(30);
    if (app->transport == TransportUsb)
    {
        if (!hid_usb_release(app))
        {
            // Still ours: keep the light-sleep veto too, and try again later.
            FURI_LOG_E("ClickRec", "Could not release the USB lease; USB stays reserved");
            return;
        }
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

void hid_deinit_final(App *app)
{
    hid_deinit(app);
    for (uint8_t attempt = 0; app->usb_lease && attempt < 5; attempt++)
    {
        furi_delay_ms(100);
        if (hid_usb_release(app))
            hid_sleep_inhibit_exit(app);
    }
    if (app->usb_lease)
        FURI_LOG_E("ClickRec", "USB could not be released; it stays reserved until reboot");
    else
        hid_sleep_inhibit_exit(app); // whichever path released the lease, the veto goes with it
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
