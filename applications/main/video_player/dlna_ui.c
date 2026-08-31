#include "dlna_ui.h"
#include "dlna_wifi.h"
#include "dlna_ssdp.h"
#include "wlan_passwords.h"

#include <furi.h>
#include <gui/elements.h>
#include <gui/icon.h>
#include <assets_icons.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG "DlnaUi"
#define CONNECT_TIMEOUT_MS 15000
#define SSDP_SCAN_MS       6000
#define MAX_APS            32

typedef enum {
    DlnaUiClosed,
    DlnaUiWifiScan,
    DlnaUiWifiPassword,
    DlnaUiConnecting,
    DlnaUiDeviceScan,
    DlnaUiConnected,
} DlnaUiView;

typedef enum {
    JobNone,
    JobWifiScan,
    JobConnect,
    JobSsdpScan,
} DlnaUiJob;

struct DlnaUi {
    DlnaUiView view;

    /* async job */
    FuriThread* job_thread;
    volatile DlnaUiJob job;
    volatile bool job_done;

    /* wifi scan results */
    wifi_ap_record_t* aps;
    uint16_t ap_count;
    int32_t ap_sel;
    bool ap_has_pw[MAX_APS];
    bool pw_needs_save;

    /* chosen AP + password entry */
    char ssid[33];
    uint8_t bssid[6];
    uint8_t channel;
    char password[65];
    int pw_len;
    int kb_row;
    int kb_col;
    bool kb_shift;

    /* connect */
    uint32_t connect_deadline;
    bool connect_failed;

    /* ssdp results */
    DlnaDevice devices[DLNA_MAX_DEVICES];
    int dev_count;
    int32_t dev_sel;

    /* chosen target */
    DlnaDevice target;
    bool have_target;
};

/* ---------- keyboard layout ---------- */

static const char* const KB_ROWS[] = {
    "1234567890",
    "qwertyuiop",
    "asdfghjkl",
    "zxcvbnm.-_",
};
#define KB_CHAR_ROWS 4
#define KB_FUNC_ROW  4
#define KB_FUNC_KEYS 4 /* Aa  SP  DEL  OK */

static int kb_row_len(int row) {
    if(row < KB_CHAR_ROWS) return (int)strlen(KB_ROWS[row]);
    return KB_FUNC_KEYS;
}

/* ---------- lifecycle ---------- */

DlnaUi* dlna_ui_alloc(void) {
    DlnaUi* ui = malloc(sizeof(DlnaUi));
    memset(ui, 0, sizeof(DlnaUi));
    ui->view = DlnaUiClosed;
    ui->job = JobNone;
    return ui;
}

static void dlna_job_join(DlnaUi* ui) {
    if(ui->job_thread) {
        furi_thread_join(ui->job_thread);
        furi_thread_free(ui->job_thread);
        ui->job_thread = NULL;
    }
}

void dlna_ui_free(DlnaUi* ui) {
    if(!ui) return;
    dlna_job_join(ui);
    if(ui->aps) free(ui->aps);
    /* Always call stop — NOT just when is_started(): if esp_wifi_init failed
     * the worker task is alive but s_started stayed false, and stop() is what
     * terminates that task (and restores BT). Leaving it alive would run
     * FAP-text code after unload → StoreProhibited on the next launch. stop()
     * is idempotent when nothing was ever started. */
    dlna_wifi_stop();
    free(ui);
}

/* ---------- async jobs ---------- */

static int32_t dlna_job_fn(void* ctx) {
    DlnaUi* ui = ctx;
    switch(ui->job) {
    case JobWifiScan:
        if(!dlna_wifi_is_started()) dlna_wifi_start();
        if(ui->aps) {
            free(ui->aps);
            ui->aps = NULL;
        }
        dlna_wifi_scan(&ui->aps, &ui->ap_count, MAX_APS);
        for(int i = 0; i < ui->ap_count && i < MAX_APS; i++) {
            ui->ap_has_pw[i] = wlan_password_exists((const char*)ui->aps[i].ssid);
        }
        break;
    case JobConnect:
        if(!dlna_wifi_is_started()) dlna_wifi_start();
        dlna_wifi_connect(ui->ssid, ui->password, ui->bssid[0] ? ui->bssid : NULL, ui->channel);
        break;
    case JobSsdpScan:
        ui->dev_count = dlna_ssdp_scan(ui->devices, DLNA_MAX_DEVICES, SSDP_SCAN_MS);
        break;
    default:
        break;
    }
    ui->job_done = true;
    return 0;
}

static void dlna_start_job(DlnaUi* ui, DlnaUiJob job) {
    dlna_job_join(ui);
    ui->job = job;
    ui->job_done = false;
    ui->job_thread = furi_thread_alloc_ex("DlnaUiJob", 4096, dlna_job_fn, ui);
    furi_thread_start(ui->job_thread);
}

static bool dlna_busy(DlnaUi* ui) {
    return ui->job != JobNone && !ui->job_done;
}

/* ---------- public entry / state ---------- */

void dlna_ui_start_connect(DlnaUi* ui) {
    if(dlna_wifi_is_connected()) {
        ui->view = DlnaUiDeviceScan;
        ui->dev_count = 0;
        ui->dev_sel = 0;
        dlna_start_job(ui, JobSsdpScan);
    } else {
        ui->view = DlnaUiWifiScan;
        ui->ap_count = 0;
        dlna_start_job(ui, JobWifiScan);
    }
}

void dlna_ui_disconnect_target(DlnaUi* ui) {
    ui->have_target = false;
}

bool dlna_ui_is_connected(DlnaUi* ui) {
    return ui->have_target;
}

bool dlna_ui_is_active(DlnaUi* ui) {
    return ui->view != DlnaUiClosed;
}

uint32_t dlna_ui_own_ip(DlnaUi* ui) {
    UNUSED(ui);
    return dlna_wifi_get_own_ip();
}

const DlnaDevice* dlna_ui_target(DlnaUi* ui) {
    return ui->have_target ? &ui->target : NULL;
}

/* ---------- tick (drives async transitions) ---------- */

void dlna_ui_tick(DlnaUi* ui) {
    if(ui->view == DlnaUiConnecting) {
        if(dlna_wifi_is_connected()) {
            if(ui->pw_needs_save && ui->password[0]) {
                wlan_password_save(ui->ssid, ui->password);
                ui->pw_needs_save = false;
            }
            ui->view = DlnaUiDeviceScan;
            ui->dev_count = 0;
            ui->dev_sel = 0;
            dlna_start_job(ui, JobSsdpScan);
        } else if(furi_get_tick() > ui->connect_deadline) {
            if(dlna_wifi_last_fail_is_auth()) {
                wlan_password_delete(ui->ssid);
            }
            ui->connect_failed = true;
            ui->view = DlnaUiWifiScan;
            dlna_start_job(ui, JobWifiScan);
        }
        return;
    }

    if(ui->job == JobNone || !ui->job_done) return;

    DlnaUiJob done = ui->job;
    ui->job = JobNone;
    dlna_job_join(ui);

    /* If the user left the flow (Back during a busy job) while a job was still
     * running, just reap it — do NOT drive a view transition, which would
     * re-open the DLNA UI the user already dismissed. */
    if(ui->view == DlnaUiClosed) return;

    switch(done) {
    case JobWifiScan:
        ui->ap_sel = 0;
        break;
    case JobConnect:
        ui->view = DlnaUiConnecting;
        ui->connect_deadline = furi_get_tick() + furi_ms_to_ticks(CONNECT_TIMEOUT_MS);
        ui->connect_failed = false;
        break;
    case JobSsdpScan:
        ui->dev_sel = 0;
        break;
    default:
        break;
    }
}

/* ---------- rendering ---------- */

static void render_list(
    Canvas* canvas, DlnaUi* ui, const char* title, const char* empty, bool busy,
    int count, int32_t sel) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, title);
    canvas_draw_line(canvas, 0, 12, 127, 12);
    canvas_set_font(canvas, FontSecondary);

    if(busy) {
        canvas_draw_str_aligned(canvas, 64, 36, AlignCenter, AlignCenter, "Scanning...");
        return;
    }
    if(count == 0) {
        canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, empty);
        if(ui->view == DlnaUiDeviceScan)
            canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignCenter, "OK to rescan");
        elements_button_left(canvas, "Back");
        return;
    }

    const int rows = 3;
    int32_t top = sel - rows / 2;
    if(top < 0) top = 0;
    if(top > count - rows) top = count - rows;
    if(top < 0) top = 0;
    for(int i = top; i < top + rows && i < count; i++) {
        int y = 14 + (i - top) * 13;
        bool s = (i == sel);
        if(s) {
            canvas_draw_box(canvas, 0, y, 128, 13);
            canvas_invert_color(canvas);
        }
        char buf[40];
        if(ui->view == DlnaUiWifiScan) {
            const wifi_ap_record_t* ap = &ui->aps[i];
            strncpy(buf, (const char*)ap->ssid, sizeof(buf) - 1);
            buf[sizeof(buf) - 1] = '\0';
            if(buf[0] == '\0') strcpy(buf, "(hidden)");
            buf[18] = '\0';
            canvas_draw_str(canvas, 4, y + 10, buf);
            bool unlocked = (ap->authmode == WIFI_AUTH_OPEN) || ui->ap_has_pw[i];
            canvas_draw_icon(canvas, 128 - 7 - 3, y + 3, unlocked ? &I_Unlock_7x8 : &I_Lock_7x8);
        } else {
            const DlnaDevice* d = &ui->devices[i];
            snprintf(buf, sizeof(buf), "%s", d->name[0] ? d->name : "Renderer");
            buf[24] = '\0';
            canvas_draw_str(canvas, 4, y + 10, buf);
        }
        if(s) canvas_invert_color(canvas);
    }
    elements_button_left(canvas, "Back");
    elements_button_center(canvas, "OK");
}

static void render_password(Canvas* canvas, DlnaUi* ui) {
    canvas_set_font(canvas, FontSecondary);

    char shown[24];
    int start = ui->pw_len > 20 ? ui->pw_len - 20 : 0;
    snprintf(shown, sizeof(shown), "%s", ui->password + start);
    canvas_draw_str(canvas, 2, 9, "Pwd:");
    canvas_draw_str(canvas, 26, 9, shown);
    canvas_draw_line(canvas, 0, 11, 127, 11);

    for(int row = 0; row < KB_CHAR_ROWS; row++) {
        const char* r = KB_ROWS[row];
        int len = (int)strlen(r);
        for(int col = 0; col < len; col++) {
            int x = 3 + col * 12;
            int y = 13 + row * 10;
            char c = r[col];
            if(ui->kb_shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            bool sel = (ui->kb_row == row && ui->kb_col == col);
            if(sel) {
                canvas_draw_box(canvas, x - 1, y, 11, 10);
                canvas_invert_color(canvas);
            }
            char s[2] = {c, 0};
            canvas_draw_str(canvas, x + 1, y + 8, s);
            if(sel) canvas_invert_color(canvas);
        }
    }

    const char* fk[KB_FUNC_KEYS] = {"Aa", "SP", "DEL", "OK"};
    for(int col = 0; col < KB_FUNC_KEYS; col++) {
        int x = 3 + col * 31;
        int y = 13 + KB_FUNC_ROW * 10;
        bool sel = (ui->kb_row == KB_FUNC_ROW && ui->kb_col == col);
        if(sel) {
            canvas_draw_box(canvas, x - 1, y, 30, 10);
            canvas_invert_color(canvas);
        }
        canvas_draw_str(canvas, x + 2, y + 8, fk[col]);
        if(ui->kb_shift && col == 0) canvas_draw_frame(canvas, x - 1, y, 30, 10);
        if(sel) canvas_invert_color(canvas);
    }
}

static void render_connecting(Canvas* canvas, DlnaUi* ui) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 22, AlignCenter, AlignCenter, "Connecting...");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 38, AlignCenter, AlignCenter, ui->ssid);
    elements_button_left(canvas, "Cancel");
}

static void render_connected(Canvas* canvas, DlnaUi* ui) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "Remote Target");
    canvas_draw_line(canvas, 0, 12, 127, 12);
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 4, 26, ui->target.name[0] ? ui->target.name : "Renderer");

    char ipbuf[24];
    uint32_t ip = ui->target.ip;
    snprintf(
        ipbuf, sizeof(ipbuf), "%u.%u.%u.%u:%u",
        (unsigned)(ip & 0xff), (unsigned)((ip >> 8) & 0xff),
        (unsigned)((ip >> 16) & 0xff), (unsigned)((ip >> 24) & 0xff),
        (unsigned)ui->target.port);
    canvas_draw_str(canvas, 4, 40, ipbuf);

    elements_button_center(canvas, "Done");
}

void dlna_ui_render(Canvas* canvas, DlnaUi* ui) {
    switch(ui->view) {
    case DlnaUiWifiScan:
        render_list(canvas, ui, "Select WiFi", "No networks", dlna_busy(ui), ui->ap_count, ui->ap_sel);
        break;
    case DlnaUiWifiPassword:
        render_password(canvas, ui);
        break;
    case DlnaUiConnecting:
        render_connecting(canvas, ui);
        break;
    case DlnaUiDeviceScan:
        render_list(canvas, ui, "Remote Devices", "None found", dlna_busy(ui), ui->dev_count, ui->dev_sel);
        break;
    case DlnaUiConnected:
        render_connected(canvas, ui);
        break;
    default:
        break;
    }
}

/* ---------- input ---------- */

static void kb_commit_char(DlnaUi* ui) {
    if(ui->kb_row < KB_CHAR_ROWS) {
        char c = KB_ROWS[ui->kb_row][ui->kb_col];
        if(ui->kb_shift && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if(ui->pw_len < (int)sizeof(ui->password) - 1) {
            ui->password[ui->pw_len++] = c;
            ui->password[ui->pw_len] = '\0';
        }
        return;
    }
    switch(ui->kb_col) {
    case 0:
        ui->kb_shift = !ui->kb_shift;
        break;
    case 1:
        if(ui->pw_len < (int)sizeof(ui->password) - 1) {
            ui->password[ui->pw_len++] = ' ';
            ui->password[ui->pw_len] = '\0';
        }
        break;
    case 2:
        if(ui->pw_len > 0) {
            ui->password[--ui->pw_len] = '\0';
        }
        break;
    case 3:
        ui->pw_needs_save = true;
        dlna_start_job(ui, JobConnect);
        break;
    }
}

static bool input_wifi_scan(DlnaUi* ui, const InputEvent* in) {
    if(dlna_busy(ui)) {
        if(in->key == InputKeyBack && in->type == InputTypeShort) ui->view = DlnaUiClosed;
        return ui->view != DlnaUiClosed;
    }
    if(in->type != InputTypeShort && in->type != InputTypeRepeat) return true;
    switch(in->key) {
    case InputKeyUp:
        if(ui->ap_count) ui->ap_sel = (ui->ap_sel - 1 + ui->ap_count) % ui->ap_count;
        break;
    case InputKeyDown:
        if(ui->ap_count) ui->ap_sel = (ui->ap_sel + 1) % ui->ap_count;
        break;
    case InputKeyOk:
        if(ui->ap_count) {
            wifi_ap_record_t* ap = &ui->aps[ui->ap_sel];
            strncpy(ui->ssid, (const char*)ap->ssid, sizeof(ui->ssid) - 1);
            ui->ssid[sizeof(ui->ssid) - 1] = '\0';
            memcpy(ui->bssid, ap->bssid, 6);
            ui->channel = ap->primary;
            ui->pw_len = 0;
            ui->password[0] = '\0';
            ui->pw_needs_save = false;
            ui->kb_row = 0;
            ui->kb_col = 0;
            ui->kb_shift = false;
            if(ap->authmode == WIFI_AUTH_OPEN) {
                dlna_start_job(ui, JobConnect);
            } else if(ui->ap_has_pw[ui->ap_sel]) {
                wlan_password_read(ui->ssid, ui->password, sizeof(ui->password));
                ui->pw_len = (int)strlen(ui->password);
                dlna_start_job(ui, JobConnect);
            } else {
                ui->view = DlnaUiWifiPassword;
            }
        }
        break;
    case InputKeyBack:
        ui->view = DlnaUiClosed;
        return false;
    default:
        break;
    }
    return true;
}

static bool input_password(DlnaUi* ui, const InputEvent* in) {
    if(in->type != InputTypeShort && in->type != InputTypeRepeat) return true;
    switch(in->key) {
    case InputKeyUp:
        ui->kb_row = (ui->kb_row - 1 + (KB_CHAR_ROWS + 1)) % (KB_CHAR_ROWS + 1);
        if(ui->kb_col >= kb_row_len(ui->kb_row)) ui->kb_col = kb_row_len(ui->kb_row) - 1;
        break;
    case InputKeyDown:
        ui->kb_row = (ui->kb_row + 1) % (KB_CHAR_ROWS + 1);
        if(ui->kb_col >= kb_row_len(ui->kb_row)) ui->kb_col = kb_row_len(ui->kb_row) - 1;
        break;
    case InputKeyLeft:
        ui->kb_col = (ui->kb_col - 1 + kb_row_len(ui->kb_row)) % kb_row_len(ui->kb_row);
        break;
    case InputKeyRight:
        ui->kb_col = (ui->kb_col + 1) % kb_row_len(ui->kb_row);
        break;
    case InputKeyOk:
        kb_commit_char(ui);
        break;
    case InputKeyBack:
        ui->view = DlnaUiWifiScan;
        break;
    default:
        break;
    }
    return true;
}

static bool input_connecting(DlnaUi* ui, const InputEvent* in) {
    if(in->type == InputTypeShort && in->key == InputKeyBack) {
        dlna_wifi_disconnect();
        ui->view = DlnaUiWifiScan;
    }
    return true;
}

static bool input_device_scan(DlnaUi* ui, const InputEvent* in) {
    if(dlna_busy(ui)) {
        if(in->key == InputKeyBack && in->type == InputTypeShort) ui->view = DlnaUiClosed;
        return ui->view != DlnaUiClosed;
    }
    if(in->type != InputTypeShort && in->type != InputTypeRepeat) return true;
    switch(in->key) {
    case InputKeyUp:
        if(ui->dev_count) ui->dev_sel = (ui->dev_sel - 1 + ui->dev_count) % ui->dev_count;
        break;
    case InputKeyDown:
        if(ui->dev_count) ui->dev_sel = (ui->dev_sel + 1) % ui->dev_count;
        break;
    case InputKeyOk:
        if(ui->dev_count) {
            ui->target = ui->devices[ui->dev_sel];
            ui->have_target = true;
            ui->view = DlnaUiConnected;
        } else {
            /* empty list → rescan */
            ui->dev_count = 0;
            ui->dev_sel = 0;
            dlna_start_job(ui, JobSsdpScan);
        }
        break;
    case InputKeyBack:
        ui->view = DlnaUiClosed;
        return false;
    default:
        break;
    }
    return true;
}

static bool input_connected(DlnaUi* ui, const InputEvent* in) {
    if(in->type != InputTypeShort) return true;
    if(in->key == InputKeyOk || in->key == InputKeyBack) {
        ui->view = DlnaUiClosed;
        return false;
    }
    return true;
}

bool dlna_ui_input(DlnaUi* ui, const InputEvent* event) {
    switch(ui->view) {
    case DlnaUiWifiScan:
        return input_wifi_scan(ui, event);
    case DlnaUiWifiPassword:
        return input_password(ui, event);
    case DlnaUiConnecting:
        return input_connecting(ui, event);
    case DlnaUiDeviceScan:
        return input_device_scan(ui, event);
    case DlnaUiConnected:
        return input_connected(ui, event);
    default:
        return false;
    }
}
