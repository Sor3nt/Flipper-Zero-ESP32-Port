/* Pure screen layout, shared by the native renderer and host tests. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define NUI_ROWS 13
#define NUI_COLS 52
typedef struct {
  char name[31], detail[42];
  unsigned hits;
  int rssi;
  bool selected;
} RtlDeviceCard;
typedef struct {
  bool enabled, receiving, busy;
  uint32_t frequency, messages, drops, decode_ms, wait_ms;
  unsigned devices, selected, cards;
  int rssi;
  char profile[24], mode[20];
  int8_t signal[48];
  unsigned signal_count;
  RtlDeviceCard card[3];
} RtlDashboard;
typedef struct {
  char title[40], badge[16], body[NUI_ROWS][NUI_COLS + 1],
      status[NUI_COLS + 1], foot[NUI_COLS + 1];
  int highlight;
  RtlDashboard live;
} RtlScreen;
uint32_t rtl_screen_changes(const RtlScreen *before, const RtlScreen *after,
                            bool valid);
void rtl_screen_stripe(const RtlScreen *screen, uint16_t *stripe, unsigned sy);
