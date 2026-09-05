/**
 * @file rpc_system.c
 * RPC System commands — ESP32 port
 *
 * Firmware-Update ueber RPC (qT-Embed / qFlipper-Protokoll):
 *   1. Host laedt die furi_esp32.bin per storage_write nach /ext/update/…
 *   2. system_update_request(update_manifest = Pfad der .bin) — validiert das
 *      Image (Magic, Chip-ID, Projektname, OTA-Slot, Loader frei) und merkt
 *      sich den Pfad; Antwort = UpdateResultCode.
 *   3. system_reboot_request(UPDATE) — startet die System-App "ota_updater"
 *      mit dem gemerkten Pfad. Die App flasht mit Vollbild-Fortschritt in die
 *      inaktive OTA-Partition, setzt das qFlipper-Resume-Flag und rebootet.
 *      Der Host wartet, bis das Geraet wieder online ist (wie beim Flipper).
 *   system_reboot_request(OS) → Neustart. system_reboot_request(DFU) → ROM-
 *   Download-Modus (fw_ota_reboot_to_download_mode_async) fuer den "Full
 *   flash" von qT-Embed (Bootloader + Partitionstabelle + App per esptool-
 *   Protokoll ueber USB-Serial-JTAG, z. B. Umstieg auf das Dual-OTA-Layout).
 *
 * Factory-Reset bleibt nicht implementiert.
 */

#include <flipper.pb.h>
#include <furi_hal.h>
#include <notification/notification_messages.h>
#include <protobuf_version.h>
#include <loader/loader.h>
#include <fw_ota/fw_ota.h>

#include "rpc_i.h"

#define TAG "RpcSystem"

/* Pfad aus system_update_request, konsumiert von system_reboot_request(UPDATE). */
#define RPC_UPDATE_PATH_MAX 200
static char s_pending_update_path[RPC_UPDATE_PATH_MAX];
/* Argument-Prefix fuer die OTA-Updater-App: "rpc:" = Update kam per USB-RPC →
 * nach dem Reboot die qFlipper-Bridge wieder hochfahren. */
#define RPC_UPDATER_APP  "ota_updater"
#define RPC_UPDATER_ARG_PREFIX "rpc:"

typedef struct {
    RpcSession* session;
    PB_Main* response;
} RpcSystemContext;

static void rpc_system_system_ping_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_ping_request_tag);

    FURI_LOG_D(TAG, "Ping");

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    if(request->has_next) {
        rpc_send_and_release_empty(
            session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
        return;
    }

    PB_Main response = PB_Main_init_default;
    response.command_status = PB_CommandStatus_OK;
    response.command_id = request->command_id;
    response.which_content = PB_Main_system_ping_response_tag;

    const PB_System_PingRequest* ping_request = &request->content.system_ping_request;
    PB_System_PingResponse* ping_response = &response.content.system_ping_response;
    if(ping_request->data && (ping_request->data->size > 0)) {
        ping_response->data = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(ping_request->data->size));
        memcpy(ping_response->data->bytes, ping_request->data->bytes, ping_request->data->size);
        ping_response->data->size = ping_request->data->size;
    }

    rpc_send_and_release(session, &response);
}

static void rpc_system_system_reboot_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_reboot_request_tag);
    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    const PB_System_RebootRequest_RebootMode mode = request->content.system_reboot_request.mode;

    if(mode == PB_System_RebootRequest_RebootMode_OS) {
        /* Wie beim Original: keine Antwort, das Geraet verschwindet einfach vom
         * Bus. Kurze Verzoegerung, damit die Session noch aufraeumen kann. */
        FURI_LOG_I(TAG, "Reboot (OS)");
        fw_ota_reboot_async(300);

    } else if(mode == PB_System_RebootRequest_RebootMode_UPDATE) {
        if(!s_pending_update_path[0]) {
            FURI_LOG_W(TAG, "Reboot (UPDATE) without prior system_update_request");
            rpc_send_and_release_empty(
                session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
            return;
        }

        char args[sizeof(RPC_UPDATER_ARG_PREFIX) + RPC_UPDATE_PATH_MAX];
        snprintf(args, sizeof(args), RPC_UPDATER_ARG_PREFIX "%s", s_pending_update_path);
        s_pending_update_path[0] = '\0';

        FURI_LOG_I(TAG, "Reboot (UPDATE): starting %s with %s", RPC_UPDATER_APP, args);
        Loader* loader = furi_record_open(RECORD_LOADER);
        const LoaderStatus status = loader_start(loader, RPC_UPDATER_APP, args, NULL);
        furi_record_close(RECORD_LOADER);

        if(status != LoaderStatusOk) {
            FURI_LOG_E(TAG, "Failed to start %s: %d", RPC_UPDATER_APP, status);
            rpc_send_and_release_empty(
                session,
                request->command_id,
                (status == LoaderStatusErrorAppStarted) ? PB_CommandStatus_ERROR_APP_SYSTEM_LOCKED :
                                                          PB_CommandStatus_ERROR_APP_CANT_START);
        }
        /* Erfolg: keine Antwort (Updater flasht und rebootet). */

    } else if(mode == PB_System_RebootRequest_RebootMode_DFU) {
        /* "Recovery" = ROM-Download-Modus: qT-Embed flasht danach Bootloader +
         * Partitionstabelle + App ueber das esptool-Protokoll (Full flash).
         * Resume-Flag setzen, damit die neue Firmware die qFlipper-Bridge nach
         * dem Flash wieder hochfaehrt (best effort — RTC-RAM ueberlebt den
         * ROM-Bootloader normalerweise; sonst zeigt der Host einen Hinweis). */
        FURI_LOG_I(TAG, "Reboot (DFU): entering ROM download mode");
        fw_ota_set_resume_qflipper(true);
        if(!fw_ota_reboot_to_download_mode_async(300)) {
            fw_ota_set_resume_qflipper(false);
            rpc_send_and_release_empty(
                session, request->command_id, PB_CommandStatus_ERROR_NOT_IMPLEMENTED);
        }

    } else {
        FURI_LOG_W(TAG, "Reboot mode %d not supported on ESP32", (int)mode);
        rpc_send_and_release_empty(
            session, request->command_id, PB_CommandStatus_ERROR_NOT_IMPLEMENTED);
    }
}

static void rpc_system_system_update_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_update_request_tag);
    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    const char* path = request->content.system_update_request.update_manifest;
    PB_System_UpdateResponse_UpdateResultCode code = PB_System_UpdateResponse_UpdateResultCode_OK;
    s_pending_update_path[0] = '\0';

    if(!path || !path[0] || path[0] != '/' || strlen(path) >= RPC_UPDATE_PATH_MAX) {
        code = PB_System_UpdateResponse_UpdateResultCode_ManifestPathInvalid;
    } else if(!fw_ota_is_supported()) {
        /* Single-App-Partitionslayout (kein ota_0/ota_1) → nur USB-/Web-Flasher. */
        code = PB_System_UpdateResponse_UpdateResultCode_StageMissing;
    } else {
        Loader* loader = furi_record_open(RECORD_LOADER);
        bool locked = loader_is_locked(loader);
        furi_record_close(RECORD_LOADER);

        if(locked) {
            /* Eine App laeuft — der Updater koennte nicht gestartet werden. */
            code = PB_System_UpdateResponse_UpdateResultCode_ManifestPointerError;
        } else {
            FwOtaImageInfo info;
            switch(fw_ota_inspect_image(path, &info)) {
            case FwOtaImageOk:
                strncpy(s_pending_update_path, path, RPC_UPDATE_PATH_MAX - 1);
                s_pending_update_path[RPC_UPDATE_PATH_MAX - 1] = '\0';
                FURI_LOG_I(
                    TAG,
                    "Update staged: %s (%s, %lu bytes, %s %s)",
                    path,
                    info.version,
                    (unsigned long)info.size,
                    info.date,
                    info.time);
                break;
            case FwOtaImageFileNotFound:
                code = PB_System_UpdateResponse_UpdateResultCode_ManifestFolderNotFound;
                break;
            case FwOtaImageNotAnImage:
                code = PB_System_UpdateResponse_UpdateResultCode_ManifestInvalid;
                break;
            case FwOtaImageChipMismatch:
                code = PB_System_UpdateResponse_UpdateResultCode_TargetMismatch;
                break;
            case FwOtaImageWrongProject:
                code = PB_System_UpdateResponse_UpdateResultCode_OutdatedManifestVersion;
                break;
            case FwOtaImageReadError:
            default:
                code = PB_System_UpdateResponse_UpdateResultCode_StageIntegrityError;
                break;
            }
        }
    }

    if(code != PB_System_UpdateResponse_UpdateResultCode_OK) {
        FURI_LOG_W(TAG, "Update request rejected (%s): code %d", path ? path : "(null)", (int)code);
    }

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->has_next = false;
    response->command_status = PB_CommandStatus_OK;
    response->which_content = PB_Main_system_update_response_tag;
    response->content.system_update_response.code = code;

    rpc_send_and_release(session, response);
    free(response);
}

static void rpc_system_system_device_info_callback(
    const char* key,
    const char* value,
    bool last,
    void* context) {
    furi_assert(key);
    furi_assert(value);
    RpcSystemContext* ctx = context;
    furi_assert(ctx);

    char* str_key = strdup(key);
    char* str_value = strdup(value);

    ctx->response->has_next = !last;
    ctx->response->content.system_device_info_response.key = str_key;
    ctx->response->content.system_device_info_response.value = str_value;

    rpc_send_and_release(ctx->session, ctx->response);
}

static void rpc_system_system_device_info_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_device_info_request_tag);

    FURI_LOG_D(TAG, "DeviceInfo");

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->which_content = PB_Main_system_device_info_response_tag;
    response->command_status = PB_CommandStatus_OK;

    RpcSystemContext device_info_context = {
        .session = session,
        .response = response,
    };
    furi_hal_info_get(rpc_system_system_device_info_callback, '_', &device_info_context);

    free(response);
}

static void rpc_system_system_get_datetime_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_get_datetime_request_tag);

    FURI_LOG_D(TAG, "GetDatetime");

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    DateTime datetime;
    furi_hal_rtc_get_datetime(&datetime);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->which_content = PB_Main_system_get_datetime_response_tag;
    response->command_status = PB_CommandStatus_OK;
    response->content.system_get_datetime_response.has_datetime = true;
    response->content.system_get_datetime_response.datetime.hour = datetime.hour;
    response->content.system_get_datetime_response.datetime.minute = datetime.minute;
    response->content.system_get_datetime_response.datetime.second = datetime.second;
    response->content.system_get_datetime_response.datetime.day = datetime.day;
    response->content.system_get_datetime_response.datetime.month = datetime.month;
    response->content.system_get_datetime_response.datetime.year = datetime.year;
    response->content.system_get_datetime_response.datetime.weekday = datetime.weekday;

    rpc_send_and_release(session, response);
    free(response);
}

static void rpc_system_system_set_datetime_process(const PB_Main* request, void* context) {
    furi_assert(request);
    RpcSession* session = (RpcSession*)context;

    if(!request->content.system_set_datetime_request.has_datetime) {
        rpc_send_and_release_empty(
            session, request->command_id, PB_CommandStatus_ERROR_INVALID_PARAMETERS);
        return;
    }

    DateTime datetime;
    datetime.hour = request->content.system_set_datetime_request.datetime.hour;
    datetime.minute = request->content.system_set_datetime_request.datetime.minute;
    datetime.second = request->content.system_set_datetime_request.datetime.second;
    datetime.day = request->content.system_set_datetime_request.datetime.day;
    datetime.month = request->content.system_set_datetime_request.datetime.month;
    datetime.year = request->content.system_set_datetime_request.datetime.year;
    datetime.weekday = request->content.system_set_datetime_request.datetime.weekday;
    furi_hal_rtc_set_datetime(&datetime);

    rpc_send_and_release_empty(session, request->command_id, PB_CommandStatus_OK);
}

static void rpc_system_system_power_info_callback(
    const char* key,
    const char* value,
    bool last,
    void* context) {
    furi_assert(key);
    furi_assert(value);
    RpcSystemContext* ctx = context;
    furi_assert(ctx);

    char* str_key = strdup(key);
    char* str_value = strdup(value);

    ctx->response->has_next = !last;
    ctx->response->content.system_device_info_response.key = str_key;
    ctx->response->content.system_device_info_response.value = str_value;

    rpc_send_and_release(ctx->session, ctx->response);
}

static void rpc_system_system_get_power_info_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_power_info_request_tag);

    FURI_LOG_D(TAG, "GetPowerInfo");

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->which_content = PB_Main_system_power_info_response_tag;
    response->command_status = PB_CommandStatus_OK;

    RpcSystemContext power_info_context = {
        .session = session,
        .response = response,
    };
    furi_hal_power_info_get(rpc_system_system_power_info_callback, '_', &power_info_context);

    free(response);
}

static void rpc_system_system_protobuf_version_process(const PB_Main* request, void* context) {
    furi_assert(request);
    furi_assert(request->which_content == PB_Main_system_protobuf_version_request_tag);

    FURI_LOG_D(TAG, "ProtobufVersion");

    RpcSession* session = (RpcSession*)context;
    furi_assert(session);

    PB_Main* response = malloc(sizeof(PB_Main));
    response->command_id = request->command_id;
    response->has_next = false;
    response->command_status = PB_CommandStatus_OK;
    response->which_content = PB_Main_system_protobuf_version_response_tag;
    response->content.system_protobuf_version_response.major = PROTOBUF_MAJOR_VERSION;
    response->content.system_protobuf_version_response.minor = PROTOBUF_MINOR_VERSION;

    rpc_send_and_release(session, response);
    free(response);
}

void* rpc_system_system_alloc(RpcSession* session) {
    furi_assert(session);

    RpcHandler rpc_handler = {
        .message_handler = NULL,
        .decode_submessage = NULL,
        .context = session,
    };

    rpc_handler.message_handler = rpc_system_system_ping_process;
    rpc_add_handler(session, PB_Main_system_ping_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_reboot_process;
    rpc_add_handler(session, PB_Main_system_reboot_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_device_info_process;
    rpc_add_handler(session, PB_Main_system_device_info_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_get_datetime_process;
    rpc_add_handler(session, PB_Main_system_get_datetime_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_set_datetime_process;
    rpc_add_handler(session, PB_Main_system_set_datetime_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_get_power_info_process;
    rpc_add_handler(session, PB_Main_system_power_info_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_protobuf_version_process;
    rpc_add_handler(session, PB_Main_system_protobuf_version_request_tag, &rpc_handler);

    rpc_handler.message_handler = rpc_system_system_update_process;
    rpc_add_handler(session, PB_Main_system_update_request_tag, &rpc_handler);

    return NULL;
}
