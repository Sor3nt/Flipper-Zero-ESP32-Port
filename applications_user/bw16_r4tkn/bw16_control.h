#pragma once
#include "bw16_protocol.h"

#define BW16_MAX_CLIENTS 32
#define BW16_COMMAND_SIZE 48
#define BW16_OPERATION_MS 30000U
#define BW16_START_TIMEOUT_MS 5000U
#define BW16_STOP_TIMEOUT_MS 5000U

typedef enum {
    Bw16Idle, Bw16Scan, Bw16Stations, Bw16Clients,
    Bw16Starting, Bw16Active, Bw16Stopping, Bw16Stopped, Bw16Fault
} Bw16State;
typedef enum { Bw16TargetStation, Bw16TargetClient, Bw16TargetAll } Bw16Target;
typedef bool (*Bw16Send)(void* context, const char* command);
typedef struct {
    Bw16State state;
    Bw16Target target;
    Bw16Ap aps[BW16_MAX_APS];
    char clients[BW16_MAX_CLIENTS][13];
    unsigned ap_count, ap_total, client_count, client_total, client_ap;
    bool aps_valid, indices_valid, clients_valid, station_started, remote_unknown, restart_required;
    uint32_t started, operation_started, counter;
    char error[48];
    Bw16Send send;
    void* context;
} Bw16Control;

void bw16_control_init(Bw16Control* control, Bw16Send send, void* context);
bool bw16_control_scan(Bw16Control* control, bool stations, uint32_t now);
bool bw16_control_clients(Bw16Control* control, unsigned ap, uint32_t now);
/* The UI must obtain explicit confirmation before calling this function. */
bool bw16_control_start(Bw16Control* control, Bw16Target target, unsigned ap, unsigned client, uint32_t now);
bool bw16_control_stop(Bw16Control* control, uint32_t now);
void bw16_control_line(Bw16Control* control, const char* line, uint32_t now);
void bw16_control_tick(Bw16Control* control, uint32_t now);
void bw16_control_error(Bw16Control* control, const char* reason, uint32_t now);
bool bw16_control_busy(const Bw16Control* control);
