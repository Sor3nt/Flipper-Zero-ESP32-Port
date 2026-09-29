#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* The browser appends its own slash when joining a directory and filename. */
#define WARD_WIGLE_BROWSER_DIR "/ext/apps_data/wardriver"
#define WARD_WIGLE_DIR WARD_WIGLE_BROWSER_DIR "/"
#define WARD_WIGLE_CONFIG WARD_WIGLE_DIR "wigle.conf"
#define WARD_WIGLE_NAME_SIZE 96
#define WARD_WIGLE_TOKEN_SIZE 128
#define WARD_WIGLE_PATH_SIZE 192
#define WARD_WIGLE_MAX_FILE (16U*1024U*1024U)
#define WARD_WDG_CONFIG WARD_WIGLE_DIR "wdgwars.conf"
#define WARD_WDG_MAX_FILE (40U*1024U*1024U)
typedef enum { WardUploadWigle, WardUploadWdgWars } WardUploadService;
typedef struct { char name[WARD_WIGLE_NAME_SIZE],token[WARD_WIGLE_TOKEN_SIZE]; } WardWigleCredentials;
typedef bool (*WardWigleCancel)(void* context);
typedef void (*WardWigleProgress)(void* context,const char* stage,unsigned percent);
typedef struct {
    WardUploadService service;
    WardWigleCredentials credentials;
    char path[WARD_WIGLE_PATH_SIZE];
    WardWigleCancel cancelled;
    WardWigleProgress progress;
    void* context;
    char result[48];
    bool accepted,uncertain;
} WardWigleUpload;
bool wardriver_wigle_value_valid(const char* text,bool name);
bool wardriver_wigle_path_valid(const char* path);
bool wardriver_wigle_credentials_load(WardWigleCredentials* credentials);
bool wardriver_wigle_credentials_save(const WardWigleCredentials* credentials);
bool wardriver_wigle_credentials_clear(void);
bool wardriver_wdg_key_valid(const char* key);
bool wardriver_wdg_credentials_load(WardWigleCredentials* credentials);
bool wardriver_wdg_credentials_save(const WardWigleCredentials* credentials);
bool wardriver_wdg_credentials_clear(void);
void wardriver_wigle_upload(WardWigleUpload* upload);
/* Same file, two independent results; sequential TLS transfers, no retries. */
void wardriver_upload_both(WardWigleUpload* wigle,WardWigleUpload* wdg);
void wardriver_wigle_erase(void* data,size_t length);
/* Portable incremental CSV and bounded JSON validators, also used by host tests. */
typedef struct {
    char cell[512]; size_t length;
    unsigned header,column; uint32_t rows;
    bool quoted,after_quote,cr,error;
} WardWigleCsv;
bool wardriver_wigle_csv_feed(WardWigleCsv* csv,const char* data,size_t length);
bool wardriver_wigle_csv_finish(WardWigleCsv* csv);
bool wardriver_wigle_response(const char* data,size_t length,bool* success,bool* warning);
bool wardriver_wdg_response(const char* data,size_t length,bool* success,bool* warning);
