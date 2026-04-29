#pragma once

#include <gui/view.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef enum {
    MjAttackPhaseIdle,
    MjAttackPhaseScanning,
    MjAttackPhaseRunning,
    MjAttackPhaseDone,
    MjAttackPhaseError,
} MjAttackPhase;

typedef struct {
    MjAttackPhase phase;
    bool hardware_ok;
    char target_label[24];
    char script_name[32];
    size_t line_cur;
    size_t line_total;
    uint8_t current_channel; /* used in scanning sub-phase of Auto mode */
    char last_warning[64];
    char error_text[40];
} Nrf24MjAttackModel;

typedef enum {
    Nrf24MjAttackEventStop = 1,
} Nrf24MjAttackEvent;

View* nrf24_mj_attack_view_alloc(void);
void nrf24_mj_attack_view_free(View* view);
