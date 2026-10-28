/* Target-ABI measurement: compile with the ESP32-S3 compiler and inspect nm -S.
 * These BSS symbol sizes describe allocations, not measured runtime free heap. */
#include "wardriver_types.h"
#include "wardriver_gps.h"
#include "wardriver_monitor.h"
char wardriver_observation_bytes[sizeof(WardriverObservation)];
char wardriver_network_bytes[sizeof(WardriverNetwork)];
char wardriver_nmea_bytes[sizeof(WardriverNmea)];
char wardriver_gps_bytes[sizeof(WardriverGps)];
char wardriver_monitor_bytes[sizeof(WardriverMonitor)];
char wardriver_max_table_bytes[4096 * sizeof(WardriverNetwork)];
char wardriver_queue_payload_bytes[192 * sizeof(WardriverObservation)];
