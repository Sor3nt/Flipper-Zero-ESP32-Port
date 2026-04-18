#pragma once
#include <gui/view.h>

typedef struct BeaconView BeaconView;

BeaconView* beacon_view_alloc(void);
void beacon_view_free(BeaconView* view);
View* beacon_view_get_view(BeaconView* view);
void beacon_view_set_frame_count(BeaconView* view, uint32_t count);
void beacon_view_set_status(BeaconView* view, const char* status);