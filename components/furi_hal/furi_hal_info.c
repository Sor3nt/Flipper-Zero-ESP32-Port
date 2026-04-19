#include "furi_hal_info.h"

#include <furi.h>
#include <version.h>
#include <protobuf_version.h>

#include "furi_hal_version.h"
#include "furi_hal_bt.h"

void furi_hal_info_get_api_version(uint16_t* major, uint16_t* minor) {
    if(major) *major = 0;
    if(minor) *minor = 1;
}

void furi_hal_info_get(PropertyValueCallback out, char sep, void* context) {
    furi_check(out);

    FuriString* key = furi_string_alloc();
    FuriString* value = furi_string_alloc();
    PropertyValueContext property_context = {
        .key = key,
        .value = value,
        .out = out,
        .sep = sep,
        .last = false,
        .context = context,
    };

    /* Format version (qFlipper checks this) */
    property_value_out(&property_context, NULL, 2, "format", "major", "3");
    property_value_out(&property_context, NULL, 2, "format", "minor", "0");

    /* Hardware identification */
    property_value_out(
        &property_context, NULL, 2, "hardware", "model", furi_hal_version_get_model_name());

    /* UID from ESP32 MAC */
    const uint8_t* uid = furi_hal_version_uid();
    furi_string_printf(
        value,
        "%02X%02X%02X%02X%02X%02X",
        uid[0], uid[1], uid[2], uid[3], uid[4], uid[5]);
    property_value_out(
        &property_context, NULL, 2, "hardware", "uid", furi_string_get_cstr(value));

    property_value_out(&property_context, "%u", 2, "hardware", "otp.ver",
        (unsigned int)furi_hal_version_get_otp_version());
    property_value_out(&property_context, "%u", 2, "hardware", "timestamp",
        (unsigned int)furi_hal_version_get_hw_timestamp());
    property_value_out(&property_context, "%u", 2, "hardware", "ver",
        (unsigned int)furi_hal_version_get_hw_version());
    property_value_out(&property_context, "%u", 2, "hardware", "target",
        (unsigned int)furi_hal_version_get_hw_target());
    property_value_out(&property_context, "%u", 2, "hardware", "body",
        (unsigned int)furi_hal_version_get_hw_body());
    property_value_out(&property_context, "%u", 2, "hardware", "connect",
        (unsigned int)furi_hal_version_get_hw_connect());
    property_value_out(&property_context, "%u", 2, "hardware", "display",
        (unsigned int)furi_hal_version_get_hw_display());
    property_value_out(&property_context, "%u", 2, "hardware", "color",
        (unsigned int)furi_hal_version_get_hw_color());
    property_value_out(&property_context, NULL, 2, "hardware", "region.builtin",
        furi_hal_version_get_hw_region_name());
    property_value_out(&property_context, NULL, 2, "hardware", "region.provisioned",
        furi_hal_version_get_hw_region_name_otp());

    const char* name = furi_hal_version_get_name_ptr();
    if(name) {
        property_value_out(&property_context, NULL, 2, "hardware", "name", name);
    }

    /* Firmware information */
    const Version* firmware_version = furi_hal_version_get_firmware_version();
    if(firmware_version) {
        property_value_out(&property_context, NULL, 2, "firmware", "commit.hash",
            version_get_githash(firmware_version));
        property_value_out(&property_context, NULL, 2, "firmware", "commit.dirty",
            version_get_dirty_flag(firmware_version) ? "true" : "false");
        property_value_out(&property_context, NULL, 2, "firmware", "branch.name",
            version_get_gitbranch(firmware_version));
        property_value_out(&property_context, NULL, 2, "firmware", "version",
            version_get_version(firmware_version));
        property_value_out(&property_context, NULL, 2, "firmware", "build.date",
            version_get_builddate(firmware_version));
        property_value_out(&property_context, "%u", 2, "firmware", "target",
            (unsigned int)version_get_target(firmware_version));

        uint16_t api_major, api_minor;
        furi_hal_info_get_api_version(&api_major, &api_minor);
        property_value_out(&property_context, "%u", 2, "firmware", "api.major",
            (unsigned int)api_major);
        property_value_out(&property_context, "%u", 2, "firmware", "api.minor",
            (unsigned int)api_minor);
        property_value_out(&property_context, NULL, 2, "firmware", "origin.fork", "unleashed");
        property_value_out(&property_context, NULL, 2, "firmware", "origin.git", "");
    }

    /* Radio/BLE information */
    property_value_out(&property_context, NULL, 2, "radio", "alive", "true");
    property_value_out(&property_context, NULL, 2, "radio", "mode", "Stack");

    const BleGlueC2Info* c2_info = ble_glue_get_c2_info();
    property_value_out(&property_context, "%u", 2, "radio", "stack.type",
        (unsigned int)c2_info->StackType);
    property_value_out(&property_context, "%u", 2, "radio", "stack.major",
        (unsigned int)c2_info->VersionMajor);
    property_value_out(&property_context, "%u", 2, "radio", "stack.minor",
        (unsigned int)c2_info->VersionMinor);
    property_value_out(&property_context, "%u", 2, "radio", "stack.sub",
        (unsigned int)c2_info->VersionSub);

    /* BLE MAC */
    const uint8_t* ble_mac = furi_hal_version_get_ble_mac();
    furi_string_printf(
        value,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        ble_mac[0], ble_mac[1], ble_mac[2], ble_mac[3], ble_mac[4], ble_mac[5]);
    property_value_out(&property_context, NULL, 2, "radio", "ble.mac", furi_string_get_cstr(value));

    /* Protobuf version */
    property_value_out(&property_context, "%u", 2, "protobuf", "version.major",
        (unsigned int)PROTOBUF_MAJOR_VERSION);

    property_context.last = true;
    property_value_out(&property_context, "%u", 2, "protobuf", "version.minor",
        (unsigned int)PROTOBUF_MINOR_VERSION);

    furi_string_free(key);
    furi_string_free(value);
}
