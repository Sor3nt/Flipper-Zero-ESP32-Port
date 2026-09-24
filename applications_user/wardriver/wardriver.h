#pragma once
#include "wardriver_types.h"
#include "wardriver_config.h"
#include "wardriver_wigle.h"
#include <furi.h>
#include <gui/gui.h>
#include <gui/view_port.h>
typedef enum { WardPageHome,WardPageMenu,WardPageList,WardPageDetail,WardPageSettings,WardPageAbout,WardPageStats,WardPageWigle,WardPageUploadConfirm,WardPageUploadStatus } WardPage;
typedef enum { WardWigleNone,WardWigleSave,WardWigleReload,WardWigleClear,WardWigleSend } WardWigleCommand;
typedef enum { WardOuiNotStarted,WardOuiLoading,WardOuiReady,WardOuiUnavailable } WardOuiState;
typedef struct {
    WardPage page;
    WardriverConfig config;
    size_t count,capacity,selected,wifi_count,ble_count,notable_count,list_count;
    uint64_t detections;
    uint32_t dropped,overflow;
    WardriverNetwork last,rows[3];
    unsigned row_count,menu,setting,detail;
    bool active,editing,log_ok,ready,transitioning,radio_transition;
    WardOuiState oui_state;
    bool notable_only;
    WardriverGpsData gps;
    char gps_status[24],status[32],notice[32],wifi_status[12],ble_status[12];
    uint32_t phase_started;
    bool wigle_busy,wigle_uploading,wigle_name_set,wigle_token_set,wifi_online;
    unsigned wigle_menu,upload_percent;
    char wigle_status[48],wigle_path[WARD_WIGLE_PATH_SIZE];
} WardriverModel;
typedef struct {
    FuriMutex* lock;
    FuriMessageQueue* inputs;
    FuriMessageQueue* observations;
    FuriThread* worker;
    Gui* gui;
    ViewPort* view;
    WardriverModel model;
    uint32_t quit,want_running,save_config,finished;
    uint32_t wigle_command,wigle_action,cancel_upload;
    WardWigleCredentials wigle_credentials;
} WardriverApp;
void wardriver_ui_draw(Canvas* canvas,void* context);
void wardriver_ui_input(InputEvent* event,void* context);
void wardriver_ui_handle(WardriverApp* app,const InputEvent* event);
void wardriver_wigle_dialog(WardriverApp* app,unsigned action);
