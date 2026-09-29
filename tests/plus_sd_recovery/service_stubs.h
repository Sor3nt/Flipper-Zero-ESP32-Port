#pragma once
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>
#define UNUSED(x) ((void)(x))
#define FURI_LOG_I(...) ((void)0)
#define FURI_LOG_E(...) ((void)0)
#define FURI_LOG_W(...) ((void)0)
#define TAG "StorageTest"
#define STORAGE_TICK 1000
#define RECORD_NOTIFICATION "notification"
#define RECORD_STORAGE "storage"
#define STORAGE_INTERNAL_DIR_NAME "0:/int"
#define FuriHalRtcFlagStorageFormatInternal 1
#define _MAX_SS 512
#define FM_FAT32 1
#define FM_FAT 2
#define FuriStatusOk 0
#define ST_EXT 0
#define STORAGE_COUNT 2

typedef unsigned UINT;
typedef struct {unsigned value;} FATFS;
typedef int NotificationApp;
typedef int SDError;
enum {FR_OK, FR_DISK_ERR, FR_NOT_READY, FR_NO_FILE, FR_NO_PATH, FR_NO_FILESYSTEM,
      FR_EXIST, FR_INVALID_NAME, FR_INVALID_OBJECT, FR_INVALID_PARAMETER, FR_DENIED,
      FR_MKFS_ABORTED};
typedef enum {FSE_OK,FSE_NOT_READY,FSE_NOT_EXIST,FSE_EXIST,FSE_INVALID_NAME,
              FSE_INVALID_PARAMETER,FSE_DENIED,FSE_INTERNAL} FS_Error;
typedef enum {StorageStatusOK,StorageStatusNotReady,StorageStatusNotMounted,
              StorageStatusNoFS,StorageStatusNotAccessible,StorageStatusErrorInternal} StorageStatus;
typedef struct StorageData StorageData;
typedef struct {void (*tick)(StorageData*);} StorageApi;
typedef int FS_Api;
struct StorageData {void* data; StorageApi api; const FS_Api* fs_api; StorageStatus status;};
typedef struct {void* view_port;bool enabled;} StorageSDGui;
typedef struct {void* message_queue;StorageData storage[STORAGE_COUNT];StorageSDGui sd_gui;void* pubsub;} Storage;
typedef struct {int value;} StorageMessage;
typedef enum {StorageEventTypeCardMount,StorageEventTypeCardUnmount,StorageEventTypeCardMountError} StorageEventType;
typedef struct {StorageEventType type;} StorageEvent;
extern FATFS fatfs_object;
extern const FS_Api fs_api;
uint8_t furi_hal_sd_max_mount_retry_count(void);
bool furi_hal_sd_is_present(void);
int furi_hal_sd_init(bool reset);
bool furi_hal_usb_msc_is_active(void);
void furi_hal_sd_presence_init(void);
void fatfs_init(void);
int f_mount(FATFS* fs,const char* path,int immediate);
int f_getfree(const char* path,uint32_t* clusters,FATFS** fs);
int f_mkfs(const char* path,int fmt,int cluster,void* work,UINT size);
int f_setlabel(const char* label);
void* furi_record_open(const char* name);
void furi_record_close(const char* name);
void furi_record_create(const char* name,void* value);
void sd_notify_wait(NotificationApp* n);
void sd_notify_wait_off(NotificationApp* n);
void sd_notify_error(NotificationApp* n);
void sd_notify_success(NotificationApp* n);
void sd_notify_eject(NotificationApp* n);
uint32_t furi_get_tick(void);
uint32_t furi_ms_to_ticks(uint32_t ms);
void furi_delay_ms(uint32_t ms);
StorageStatus storage_data_status(StorageData* s);
const char* storage_data_status_text(StorageData* s);
void storage_data_timestamp(StorageData* s);
size_t storage_open_files_count(StorageData* s);
bool furi_hal_rtc_is_flag_set(int flag);
void furi_hal_rtc_reset_flag(int flag);
bool sd_remove_recursive(const char* path);
void view_port_enabled_set(void* vp,bool enabled);
void view_port_update(void* vp);
void furi_pubsub_publish(void* pubsub,const void* event);
Storage* storage_app_alloc(void);
void storage_process_message(Storage* app,StorageMessage* msg);
int furi_message_queue_get(void* queue,StorageMessage* msg,uint32_t timeout);
