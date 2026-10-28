#pragma once

#include "bw16_protocol.h"

#define BW16_BAND_24 1U
#define BW16_BAND_5 2U

typedef struct {
    unsigned first;
    unsigned count;
    uint8_t bands;
} Bw16Group;

typedef struct {
    Bw16Group groups[BW16_MAX_APS];
    unsigned group_for_ap[BW16_MAX_APS];
    bool selected[BW16_MAX_APS];
    unsigned ap_count;
    unsigned group_count;
} Bw16Selection;

/* Rescans start with no selection. Group only exact, non-hidden raw SSIDs. */
void bw16_selection_build(Bw16Selection* selection, const Bw16Ap* aps, unsigned count);
/* Explicit pairing applies only until rebuild; per-AP selections are preserved. */
bool bw16_selection_link_groups(Bw16Selection* selection, unsigned first, unsigned second);
/* A partial group becomes fully selected; a fully selected group is cleared. */
bool bw16_selection_toggle_group(Bw16Selection* selection, unsigned group);
bool bw16_selection_toggle_ap(Bw16Selection* selection, unsigned ap);
unsigned bw16_selection_group_selected(const Bw16Selection* selection, unsigned group);
unsigned bw16_selection_count(const Bw16Selection* selection);
/* Return the number written, in original scan index order, up to capacity. */
unsigned bw16_selection_indices(const Bw16Selection* selection, unsigned* out, unsigned capacity);
