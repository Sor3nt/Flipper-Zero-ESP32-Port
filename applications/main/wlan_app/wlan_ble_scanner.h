#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Konfigurasi
// ============================================================================

// Store internal scanner. Menyimpan hingga 64 device (semua yang terdeteksi).
#define BLE_SCAN_MAX_DEVICES    64

// View + scene hanya butuh top-N sorted by RSSI.
// 32 cukup untuk list + scroll tanpa lag. Set 16 kalau RAM ketat.
// Set 64 kalau environment padat & butuh lihat semua.
#define BLE_SCAN_VIEW_ENTRIES   32

// ============================================================================
// Tipe
// ============================================================================
typedef enum {
    BleAddrPublic = 0,
    BleAddrRandom,
    BleAddrUnknown,
} BleAddrType;

// Baris list (ramping) — hanya field yang digambar di daftar.
typedef struct {
    uint8_t     mac[6];
    char        name[24];
    int8_t      rssi;
    uint16_t    company_id;
    BleAddrType addr_type;
    bool        has_name;
    bool        has_mfr_data;
    uint32_t    adv_count;
} BleDeviceView;

// Entri penuh — disimpan di store + ditampilkan di detail.
typedef struct {
    uint8_t     mac[6];
    char        name[32];
    char        vendor[24];
    int8_t      rssi;
    int8_t      rssi_max;
    int8_t      rssi_min;
    uint16_t    company_id;
    BleAddrType addr_type;
    bool        has_name;
    bool        has_mfr_data;
    uint8_t     flags;
    bool        has_flags;
    uint32_t    first_seen_ms;
    uint32_t    last_seen_ms;
    uint32_t    adv_count;
    bool        valid;
} BleDeviceEntry;

// ============================================================================
// Lifecycle
// ============================================================================

/** Start BLE scan. Non-blocking — bring-up jalan di worker.
 *  Return true kalau task berhasil dibuat. */
bool     wlan_ble_scan_start(void);

/** Request stop (ASYNC). Hanya set flag + non-blocking.
 *  Task akan exit di loop berikutnya. Cek status via wlan_ble_scan_is_stopped(). */
void     wlan_ble_scan_stop(void);

/** true kalau scanner sedang aktif (running flag). */
bool     wlan_ble_scan_is_running(void);

/** true kalau task sudah benar-benar exit (safe untuk re-enter / free). */
bool     wlan_ble_scan_is_stopped(void);

/** Cleanup the selected BLE host. Panggil SEKALI saat app keluar total (di app_free).
 *  Blocking — tunggu worker exit (stop scan + teardown stack + restore btshim). */
void     wlan_ble_scan_deinit(void);

// ============================================================================
// Kontrol runtime
// ============================================================================

/** Pause scan sementara (task tetap hidup, tapi radio berhenti).
 *  Cocok untuk: user buka detail view → hemat baterai. */
void     wlan_ble_scan_pause(void);

/** Resume scan setelah pause. */
void     wlan_ble_scan_resume(void);

// ============================================================================
// Data access
// ============================================================================

/** Hapus semua device dari store. */
void     wlan_ble_scan_clear(void);

/** Total device yang tercatat (max BLE_SCAN_MAX_DEVICES). */
uint16_t wlan_ble_scan_get_count(void);

/** Snapshot top-N device sorted by RSSI (untuk list view).
 *  @param out  buffer minimal sebesar `max`
 *  @param max  jumlah maksimal device yang ditulis (max: BLE_SCAN_MAX_DEVICES)
 *  @return     jumlah device yang ditulis
 *  Memakai timeout internal — kalau lock tidak didapat, return 0 (bukan hang). */
uint16_t wlan_ble_scan_snapshot_view(BleDeviceView* out, uint16_t max);

/** Ambil detail penuh berdasarkan MAC.
 *  Return false kalau device sudah hilang dari store. */
bool     wlan_ble_scan_get_full_by_mac(const uint8_t mac[6], BleDeviceEntry* out);

// ============================================================================
// Export
// ============================================================================

/** Export semua device ke CSV format WiGLE v1.6 (Type=BLE).
 *  Return true kalau sukses. Jumlah ditulis ke *out_count. */
bool     wlan_ble_scan_export_csv(const char* path, uint16_t* out_count);

#ifdef __cplusplus
}
#endif
