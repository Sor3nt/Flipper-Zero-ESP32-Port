#include "bw16_selection.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static Bw16Ap parse(const char* line) {
    Bw16Ap ap;
    assert(bw16_parse(line, &ap) == Bw16Record);
    return ap;
}

static void groups_and_toggles(void) {
    Bw16Ap aps[] = {
        parse("Home,6,001122334401,-40"),
        parse("Guest,1,001122334402,-50"),
        parse("Home,36,001122334403,-41"),
        parse("Home,149,001122334404,-44"),
        parse("Guest,44,001122334405,-51"),
    };
    Bw16Selection s;
    bw16_selection_build(&s, aps, 5);
    assert(s.ap_count == 5 && s.group_count == 2 && bw16_selection_count(&s) == 0);
    assert(s.groups[0].first == 0 && s.groups[0].count == 3);
    assert(s.groups[1].first == 1 && s.groups[1].count == 2);
    assert(s.groups[0].bands == (BW16_BAND_24 | BW16_BAND_5));
    assert(s.groups[1].bands == (BW16_BAND_24 | BW16_BAND_5));
    assert(s.group_for_ap[0] == 0 && s.group_for_ap[1] == 1 && s.group_for_ap[2] == 0);
    assert(s.group_for_ap[3] == 0 && s.group_for_ap[4] == 1);
    assert(s.group_for_ap[5] == UINT_MAX);
    assert(bw16_selection_toggle_group(&s, 0));
    assert(bw16_selection_group_selected(&s, 0) == 3 && bw16_selection_count(&s) == 3);
    unsigned indices[5] = {99, 99, 99, 99, 99};
    assert(bw16_selection_indices(&s, indices, 5) == 3);
    assert(indices[0] == 0 && indices[1] == 2 && indices[2] == 3 && indices[3] == 99);
    assert(bw16_selection_toggle_ap(&s, 2));
    assert(bw16_selection_group_selected(&s, 0) == 2);
    assert(bw16_selection_toggle_group(&s, 0)); /* Partial -> all. */
    assert(bw16_selection_group_selected(&s, 0) == 3);
    assert(bw16_selection_toggle_group(&s, 0)); /* All -> none. */
    assert(bw16_selection_count(&s) == 0);
    assert(bw16_selection_toggle_group(&s, 1));
    assert(bw16_selection_toggle_ap(&s, 2));
    assert(bw16_selection_count(&s) == 3);
    assert(bw16_selection_indices(&s, indices, 2) == 2);
    assert(indices[0] == 1 && indices[1] == 2); /* Scan order, not group order. */
    assert(!bw16_selection_toggle_group(&s, 2));
    assert(!bw16_selection_toggle_ap(&s, 5));
    assert(bw16_selection_group_selected(&s, 2) == 0);
    bw16_selection_build(&s, aps, 2); /* A rescan never inherits old indices. */
    assert(s.ap_count == 2 && s.group_count == 2 && bw16_selection_count(&s) == 0);
    assert(s.groups[0].bands == BW16_BAND_24 && s.groups[1].bands == BW16_BAND_24);
}

static void exact_identity(void) {
    Bw16Ap aps[] = {
        parse(",1,001122334400,-40"),
        parse(",36,001122334401,-40"),
        parse("HIDDEN NETWORK,6,001122334402,-40"),
        parse("HIDDEN NETWORK,44,001122334403,-40"),
        parse("Home,1,001122334404,-40"),
        parse("home,36,001122334405,-40"),
        parse("Home_2G,1,001122334406,-40"),
        parse("Home_5G,36,001122334407,-40"),
        parse("Caf\xc3\xa9,1,001122334408,-40"),
        parse("Caf\xc3\xb6,36,001122334409,-40"),
        parse("Caf\xc3\xa9,44,00112233440A,-40"),
    };
    assert(!strcmp(aps[8].ssid, aps[9].ssid)); /* Display is lossy ASCII. */
    assert(strcmp(aps[8].raw_ssid, aps[9].raw_ssid));
    assert(!strcmp(aps[8].raw_ssid, "Caf\xc3\xa9"));
    Bw16Selection s;
    bw16_selection_build(&s, aps, sizeof(aps) / sizeof(*aps));
    assert(s.group_count == 10);
    for(unsigned i = 0; i < 10; ++i) assert(s.group_for_ap[i] == i);
    assert(s.group_for_ap[10] == 8 && s.groups[8].count == 2);
    assert(bw16_selection_toggle_group(&s, 0) && bw16_selection_count(&s) == 1);
    assert(!s.selected[1]);
    assert(bw16_selection_toggle_group(&s, 2) && bw16_selection_count(&s) == 2);
    assert(!s.selected[3]);
    assert(bw16_selection_toggle_group(&s, 8));
    assert(s.selected[8] && s.selected[10] && !s.selected[9]);
}

static void explicit_pairing(void) {
    Bw16Ap aps[] = {
        parse("LAB_0305,6,001122334401,-40"),
        parse("Guest_24,1,001122334402,-50"),
        parse("LAB_0305_5G,36,001122334403,-41"),
        parse("Guest_5G,44,001122334404,-44"),
        parse("Unrelated,149,001122334405,-51"),
    };
    Bw16Selection s;
    bw16_selection_build(&s, aps, 5);
    assert(s.group_count == 5); /* Different names require explicit pairing. */
    assert(bw16_selection_toggle_ap(&s, 2));
    assert(bw16_selection_toggle_ap(&s, 3));
    bool selected_before[BW16_MAX_APS];
    memcpy(selected_before, s.selected, sizeof(selected_before));
    assert(bw16_selection_link_groups(&s, 0, 2));
    assert(s.group_count == 4 && s.ap_count == 5);
    assert(s.groups[0].first == 0 && s.groups[0].count == 2);
    assert(s.groups[0].bands == (BW16_BAND_24 | BW16_BAND_5));
    assert(s.groups[1].first == 1 && s.groups[2].first == 3 && s.groups[3].first == 4);
    assert(s.group_for_ap[0] == 0 && s.group_for_ap[1] == 1 && s.group_for_ap[2] == 0);
    assert(s.group_for_ap[3] == 2 && s.group_for_ap[4] == 3);
    assert(s.group_for_ap[5] == UINT_MAX);
    assert(bw16_selection_group_selected(&s, 0) == 1); /* Preserve partial selection. */
    assert(!memcmp(s.selected, selected_before, sizeof(selected_before)));

    /* Reverse argument order must retain original first-appearance ordering. */
    assert(bw16_selection_link_groups(&s, 2, 1));
    assert(s.group_count == 3);
    assert(s.groups[0].first == 0 && s.groups[1].first == 1 && s.groups[2].first == 4);
    assert(s.groups[0].count == 2 && s.groups[1].count == 2 && s.groups[2].count == 1);
    assert(s.groups[1].bands == (BW16_BAND_24 | BW16_BAND_5));
    assert(s.group_for_ap[0] == 0 && s.group_for_ap[1] == 1 && s.group_for_ap[2] == 0);
    assert(s.group_for_ap[3] == 1 && s.group_for_ap[4] == 2);
    assert(bw16_selection_group_selected(&s, 0) == 1);
    assert(bw16_selection_group_selected(&s, 1) == 1);
    assert(!memcmp(s.selected, selected_before, sizeof(selected_before)));
    unsigned indices[3] = {99, 99, 99};
    assert(bw16_selection_count(&s) == 2);
    assert(bw16_selection_indices(&s, indices, 3) == 2);
    assert(indices[0] == 2 && indices[1] == 3 && indices[2] == 99);

    Bw16Selection snapshot = s;
    assert(!bw16_selection_link_groups(&s, s.group_for_ap[0], s.group_for_ap[2]));
    assert(!bw16_selection_link_groups(&s, 1, 1));
    assert(!bw16_selection_link_groups(&s, 0, 3));
    assert(!bw16_selection_link_groups(&s, UINT_MAX, 0));
    assert(!bw16_selection_link_groups(NULL, 0, 1));
    assert(!memcmp(&s, &snapshot, sizeof(s)));
    assert(bw16_selection_toggle_group(&s, 0));
    assert(s.selected[0] && s.selected[2] && !s.selected[1]);
    assert(s.selected[3] && !s.selected[4]);
    bw16_selection_build(&s, aps, 5); /* Rebuild removes manual links and selection. */
    assert(s.group_count == 5 && bw16_selection_count(&s) == 0);
    for(unsigned i = 0; i < 5; ++i) {
        assert(s.groups[i].first == i && s.groups[i].count == 1);
        assert(s.group_for_ap[i] == i);
    }
}

static void bounds_and_limits(void) {
    Bw16Selection s;
    Bw16Ap aps[BW16_MAX_APS];
    memset(aps, 0, sizeof(aps));
    for(unsigned i = 0; i < BW16_MAX_APS; ++i) {
        snprintf(aps[i].raw_ssid, sizeof(aps[i].raw_ssid), "AP%u", i);
        aps[i].channel = 36;
    }
    bw16_selection_build(&s, aps, UINT_MAX); /* Never reads past the bounded array. */
    assert(s.ap_count == BW16_MAX_APS && s.group_count == BW16_MAX_APS);
    for(unsigned i = 0; i < BW16_MAX_APS; ++i) assert(bw16_selection_toggle_group(&s, i));
    unsigned indices[BW16_MAX_APS + 1];
    for(unsigned i = 0; i < BW16_MAX_APS + 1; ++i) indices[i] = UINT_MAX;
    assert(bw16_selection_count(&s) == BW16_MAX_APS);
    assert(bw16_selection_indices(&s, indices, UINT_MAX) == BW16_MAX_APS);
    for(unsigned i = 0; i < BW16_MAX_APS; ++i) assert(indices[i] == i);
    assert(indices[BW16_MAX_APS] == UINT_MAX);
    assert(!bw16_selection_toggle_group(&s, UINT_MAX));
    assert(!bw16_selection_toggle_ap(&s, UINT_MAX));
    assert(!bw16_selection_indices(&s, indices, 0));
    assert(!bw16_selection_indices(&s, NULL, 50));
    bw16_selection_build(&s, aps, 0);
    assert(!s.ap_count && !s.group_count && !bw16_selection_count(&s));
    assert(!bw16_selection_toggle_group(&s, 0) && !bw16_selection_toggle_ap(&s, 0));
    bw16_selection_build(&s, NULL, BW16_MAX_APS);
    assert(!s.ap_count && !s.group_count);
    bw16_selection_build(NULL, aps, 1);
    assert(!bw16_selection_toggle_group(NULL, 0));
    assert(!bw16_selection_toggle_ap(NULL, 0));
    assert(!bw16_selection_count(NULL));
    assert(!bw16_selection_group_selected(NULL, 0));
    assert(!bw16_selection_indices(NULL, indices, 1));

    /* No fallback to display strings when exact identity is unavailable. */
    strcpy(aps[0].ssid, "Same");
    strcpy(aps[1].ssid, "Same");
    memset(aps[0].raw_ssid, 0, sizeof(aps[0].raw_ssid));
    memset(aps[1].raw_ssid, 0, sizeof(aps[1].raw_ssid));
    bw16_selection_build(&s, aps, 2);
    assert(s.group_count == 2);
    memset(aps[0].raw_ssid, 'x', sizeof(aps[0].raw_ssid));
    memset(aps[1].raw_ssid, 'x', sizeof(aps[1].raw_ssid));
    bw16_selection_build(&s, aps, 2);
    assert(s.group_count == 2); /* Unterminated input is not compared as C strings. */

    Bw16Ap longest = parse("12345678901234567890123456789012,36,001122334400,-40");
    assert(strlen(longest.raw_ssid) == 32 && !strcmp(longest.raw_ssid, longest.ssid));
}

int main(void) {
    groups_and_toggles();
    exact_identity();
    explicit_pairing();
    bounds_and_limits();
    puts("selection: exact dual-band groups, partial toggles, hidden/UTF-8 identity, explicit pairing, rescan, bounds PASS");
    return 0;
}
