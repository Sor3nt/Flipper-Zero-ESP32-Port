#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
typedef int FS_Error;
enum {FSE_OK,FSE_NOT_READY,FSE_INTERNAL,FSE_DENIED};
typedef struct {int unused;} Storage;
typedef struct {Storage* storage;} Desktop;
static bool s_sd_was_mounted,pending,raw;
static FS_Error status,unmount_result;
static unsigned unmount_calls,release_calls,install_calls,start_calls;
#define FURI_LOG_W(...) ((void)0)
static FS_Error storage_sd_status(Storage* s) {(void)s;return status;}
static FS_Error storage_sd_unmount(Storage* s) {
 (void)s;++unmount_calls;
 if(unmount_result==FSE_OK) {pending=false;status=FSE_NOT_READY;}
 return unmount_result;
}
static bool furi_hal_sd_release_fatfs(void) {
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
 assert(!pending);
#endif
 ++release_calls;return true;
}
static bool furi_hal_usb_composite_install(int x,int y,void* z,void* a) {
 (void)x;(void)y;(void)z;(void)a;assert(release_calls==1);++install_calls;return true;
}
static bool furi_hal_usb_msc_start(void) {
 assert(install_calls==1);
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
 assert(!pending);
#endif
 ++start_calls;raw=true;return true;
}
/* PRODUCTION */
static void reset(FS_Error initial,FS_Error detach) {
 status=initial;unmount_result=detach;pending=initial==FSE_INTERNAL;raw=false;
 s_sd_was_mounted=false;unmount_calls=release_calls=install_calls=start_calls=0;
}
int main(void) {
 Storage storage={0};Desktop d={&storage};const char* error=NULL;
 reset(FSE_OK,FSE_OK);assert(desktop_usb_storage_enter(&d,&error));
 assert(unmount_calls==1&&raw&&s_sd_was_mounted);
 reset(FSE_NOT_READY,FSE_OK);assert(desktop_usb_storage_enter(&d,&error));
 assert(!unmount_calls&&raw&&!s_sd_was_mounted);
 reset(FSE_INTERNAL,FSE_OK);assert(desktop_usb_storage_enter(&d,&error));
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
 assert(unmount_calls==1&&!pending&&raw&&s_sd_was_mounted);
 for(unsigned s=FSE_OK;s<=FSE_INTERNAL;s+=FSE_INTERNAL) {
  reset(s,FSE_DENIED);assert(!desktop_usb_storage_enter(&d,&error));
  assert(unmount_calls==1&&!release_calls&&!install_calls&&!start_calls&&!raw&&error);
 }
#else
 assert(!unmount_calls&&pending&&raw&&!s_sd_was_mounted);
 reset(FSE_OK,FSE_DENIED);assert(desktop_usb_storage_enter(&d,&error));assert(raw);
#endif
 puts("PASS production USB storage handoff");
}
