#include "bw16_selection.h"

#include <limits.h>
#include <string.h>

static bool groupable(const Bw16Ap* ap) {
    /* The v4 firmware uses this marker for hidden SSIDs. Never combine hidden
     * APs, even when several rows have the same placeholder text. */
    return ap->raw_ssid[0] && memchr(ap->raw_ssid, 0, sizeof(ap->raw_ssid)) &&
           strcmp(ap->raw_ssid, "HIDDEN NETWORK") != 0;
}

void bw16_selection_build(Bw16Selection* selection, const Bw16Ap* aps, unsigned count) {
    if(!selection) return;
    memset(selection, 0, sizeof(*selection));
    for(unsigned i = 0; i < BW16_MAX_APS; ++i) selection->group_for_ap[i] = UINT_MAX;
    if(!aps) return;
    if(count > BW16_MAX_APS) count = BW16_MAX_APS;
    selection->ap_count = count;
    for(unsigned i = 0; i < count; ++i) {
        unsigned group = selection->group_count;
        if(groupable(&aps[i])) {
            for(unsigned j = 0; j < selection->group_count; ++j) {
                const Bw16Ap* first = &aps[selection->groups[j].first];
                if(groupable(first) && strcmp(first->raw_ssid, aps[i].raw_ssid) == 0) {
                    group = j;
                    break;
                }
            }
        }
        if(group == selection->group_count) {
            selection->groups[group].first = i;
            ++selection->group_count;
        }
        selection->group_for_ap[i] = group;
        ++selection->groups[group].count;
        if(aps[i].channel >= 1 && aps[i].channel <= 14)
            selection->groups[group].bands |= BW16_BAND_24;
        else if(aps[i].channel >= 32 && aps[i].channel <= 196)
            selection->groups[group].bands |= BW16_BAND_5;
    }
}

bool bw16_selection_link_groups(Bw16Selection* selection, unsigned first, unsigned second) {
    if(!selection || first >= selection->group_count || second >= selection->group_count ||
       first == second)
        return false;
    /* Keep groups in first-appearance order, regardless of argument order. */
    unsigned keep = first < second ? first : second;
    unsigned drop = first < second ? second : first;
    Bw16Group* merged = &selection->groups[keep];
    const Bw16Group* removed = &selection->groups[drop];
    if(removed->first < merged->first) merged->first = removed->first;
    merged->count += removed->count;
    merged->bands |= removed->bands;
    for(unsigned i = 0; i < selection->ap_count; ++i) {
        unsigned group = selection->group_for_ap[i];
        if(group == drop) selection->group_for_ap[i] = keep;
        else if(group > drop) --selection->group_for_ap[i];
    }
    for(unsigned i = drop + 1; i < selection->group_count; ++i)
        selection->groups[i - 1] = selection->groups[i];
    --selection->group_count;
    memset(&selection->groups[selection->group_count], 0, sizeof(*selection->groups));
    return true;
}

unsigned bw16_selection_group_selected(const Bw16Selection* selection, unsigned group) {
    if(!selection || group >= selection->group_count) return 0;
    unsigned count = 0;
    for(unsigned i = 0; i < selection->ap_count; ++i) {
        if(selection->group_for_ap[i] == group && selection->selected[i]) ++count;
    }
    return count;
}

bool bw16_selection_toggle_group(Bw16Selection* selection, unsigned group) {
    if(!selection || group >= selection->group_count) return false;
    bool select = bw16_selection_group_selected(selection, group) != selection->groups[group].count;
    for(unsigned i = 0; i < selection->ap_count; ++i) {
        if(selection->group_for_ap[i] == group) selection->selected[i] = select;
    }
    return true;
}

bool bw16_selection_toggle_ap(Bw16Selection* selection, unsigned ap) {
    if(!selection || ap >= selection->ap_count) return false;
    selection->selected[ap] = !selection->selected[ap];
    return true;
}

unsigned bw16_selection_count(const Bw16Selection* selection) {
    if(!selection) return 0;
    unsigned count = 0;
    for(unsigned i = 0; i < selection->ap_count; ++i) {
        if(selection->selected[i]) ++count;
    }
    return count;
}

unsigned bw16_selection_indices(const Bw16Selection* selection, unsigned* out, unsigned capacity) {
    if(!selection || !out) return 0;
    unsigned count = 0;
    for(unsigned i = 0; i < selection->ap_count && count < capacity; ++i) {
        if(selection->selected[i]) out[count++] = i;
    }
    return count;
}
