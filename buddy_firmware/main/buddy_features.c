#include "buddy_features.h"

/* Each feature lives in its own file under features/ and exports a const
 * BuddyFeature. Add new ones here and to main/CMakeLists.txt. */
extern const BuddyFeature buddy_feature_identify;
extern const BuddyFeature buddy_feature_capture_hs;

static const BuddyFeature* const s_features[] = {
    &buddy_feature_identify,
    &buddy_feature_capture_hs,
};

#define FEATURE_COUNT (sizeof(s_features) / sizeof(s_features[0]))

uint32_t buddy_features_caps(void) {
    uint32_t mask = 0;
    for(size_t i = 0; i < FEATURE_COUNT; ++i) {
        if(s_features[i]->id < 32) mask |= (1u << s_features[i]->id);
    }
    return mask;
}

uint32_t buddy_features_running_mask(void) {
    uint32_t mask = 0;
    for(size_t i = 0; i < FEATURE_COUNT; ++i) {
        if(s_features[i]->id < 32 && s_features[i]->is_running && s_features[i]->is_running()) {
            mask |= (1u << s_features[i]->id);
        }
    }
    return mask;
}

const BuddyFeature* buddy_features_get(uint8_t id) {
    for(size_t i = 0; i < FEATURE_COUNT; ++i) {
        if(s_features[i]->id == id) return s_features[i];
    }
    return NULL;
}

const BuddyFeature* const* buddy_features_list(size_t* out_count) {
    if(out_count) *out_count = FEATURE_COUNT;
    return s_features;
}

void buddy_features_stop_all(void) {
    for(size_t i = 0; i < FEATURE_COUNT; ++i) {
        if(s_features[i]->is_running && s_features[i]->is_running() && s_features[i]->stop) {
            s_features[i]->stop();
        }
    }
}
