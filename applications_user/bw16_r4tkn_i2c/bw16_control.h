#pragma once
#include "bw16_protocol.h"

#define BW16_MAX_CLIENTS 32
#define BW16_COMMAND_SIZE 48
#define BW16_OPERATION_MS 30000U
#define BW16_CAPTURE_MS 120000U
#define BW16_START_TIMEOUT_MS 5000U
#define BW16_STOP_TIMEOUT_MS 20000U
#define BW16_CAPS_TIMEOUT_MS 1000U

typedef enum {
    Bw16Idle, Bw16Scan, Bw16Stations, Bw16Clients, Bw16Capabilities,
    Bw16Starting, Bw16Active, Bw16Stopping, Bw16Stopped, Bw16Fault
} Bw16State;
typedef enum { Bw16TargetStation, Bw16TargetClient, Bw16TargetAll, Bw16TargetSelected, Bw16TargetCapture } Bw16Target;
typedef bool (*Bw16Send)(void* context, const char* command);
typedef struct {
    Bw16State state;
    Bw16Target target;
    Bw16Ap aps[BW16_MAX_APS];
    char clients[BW16_MAX_CLIENTS][13];
    unsigned ap_count, ap_total, client_count, client_total, client_ap;
    bool aps_valid, indices_valid, clients_valid, station_started, remote_unknown, restart_required;
    bool native_selected, selected_ap_valid, start_confirmed;
    bool capture_supported, capture_deauth_supported;
    bool capture_with_deauth, capture_deauth_reported;
    uint32_t capture_tx_attempts;
    uint32_t capture_session, capture_emitted, capture_dropped;
    uint64_t selected_mask;
    unsigned selected_ap;
    uint32_t started, operation_started, counter;
    char error[48];
    Bw16Send send;
    void* context;
} Bw16Control;

void bw16_control_init(Bw16Control* control, Bw16Send send, void* context);
/* An old firmware may ignore this read-only request; timeout returns to Idle. */
bool bw16_control_capabilities(Bw16Control* control, uint32_t now);
bool bw16_control_scan(Bw16Control* control, bool stations, uint32_t now);
bool bw16_control_clients(Bw16Control* control, unsigned ap, uint32_t now);
/* The UI must obtain explicit confirmation before calling this function. */
bool bw16_control_start(Bw16Control* control, Bw16Target target, unsigned ap, unsigned client, uint32_t now);
bool bw16_control_start_selected(Bw16Control* control, const unsigned* indices, unsigned count, uint32_t now);
bool bw16_control_start_capture(Bw16Control* control, unsigned ap, uint32_t session, uint32_t now);
bool bw16_control_start_capture_deauth(Bw16Control* control, unsigned ap, uint32_t session, uint32_t now);
bool bw16_control_stop(Bw16Control* control, uint32_t now);
void bw16_control_line(Bw16Control* control, const char* line, uint32_t now);
void bw16_control_tick(Bw16Control* control, uint32_t now);
void bw16_control_error(Bw16Control* control, const char* reason, uint32_t now);
bool bw16_control_busy(const Bw16Control* control);
