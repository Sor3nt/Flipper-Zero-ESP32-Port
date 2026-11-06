#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int FS_Error;
enum {FSE_OK,FSE_NOT_READY,FSE_INTERNAL,FSE_DENIED};
typedef enum {
 DesktopUsbStorageStateInit,DesktopUsbStorageStateActive,DesktopUsbStorageStateError,
 DesktopUsbStorageStateEjectFirst,DesktopUsbStorageStateEjected,
} DesktopUsbStorageState;
typedef struct {int unused;} Storage;
typedef struct {int unused;} DesktopUsbStorageView;
typedef struct {Storage* storage;DesktopUsbStorageView* usb_storage_view;} Desktop;
#define TAG "test"
#define FURI_LOG_E(...) ((void)0)
#define FURI_LOG_W(...) (++warnings)
static bool s_sd_was_mounted,s_eject_warning,msc_active,host_locked;
static unsigned warnings,state_calls;
static DesktopUsbStorageState view_state;
/* Ordered trace of the leave sequence: stop, delay, mount, uninstall. */
static char trace[64];
static unsigned delayed_ms;
static void step(char c) {size_t n=strlen(trace);assert(n+1<sizeof(trace));trace[n]=c;trace[n+1]=0;}
static void furi_hal_usb_msc_stop(void) {step('S');msc_active=false;host_locked=false;}
static void furi_delay_ms(unsigned ms) {step('D');delayed_ms+=ms;}
static FS_Error storage_sd_mount(Storage* s) {(void)s;assert(!msc_active);step('M');return FSE_OK;}
static bool furi_hal_usb_composite_uninstall(void) {step('U');return true;}
static bool furi_hal_usb_msc_is_active(void) {return msc_active;}
static bool furi_hal_usb_msc_is_removal_locked(void) {return host_locked;}
static void desktop_usb_storage_set_state(DesktopUsbStorageView* v,DesktopUsbStorageState s,const char* e) {
 assert(v&&!e);view_state=s;++state_calls;
}
/* PRODUCTION */
static void reset(bool active,bool locked) {
 msc_active=active;host_locked=locked;s_eject_warning=false;warnings=state_calls=0;
 view_state=DesktopUsbStorageStateActive;trace[0]=0;delayed_ms=0;
}
int main(void) {
 Storage storage={0};DesktopUsbStorageView view={0};Desktop d={&storage,&view};
 /* Leave: MSC stops first, the host gets >=1.5 s, then FATFS remounts, then
  * the composite goes; no remount when the SD was not mounted before. */
 reset(true,false);s_sd_was_mounted=true;desktop_usb_storage_leave(&d);
 assert(!strcmp(trace,"SDMU")&&delayed_ms>=1500&&!s_sd_was_mounted);
 reset(true,false);s_sd_was_mounted=false;desktop_usb_storage_leave(&d);
 assert(!strcmp(trace,"SDU"));
 /* Back: no MSC (error screen) or a host that never locked (e.g. Windows
  * quick removal) leaves immediately, as before. */
 reset(false,true);assert(desktop_usb_storage_may_exit(&d,false)&&!state_calls);
 reset(true,false);assert(desktop_usb_storage_may_exit(&d,false)&&!state_calls);
 /* Host holds the medium: warn and stay; nothing is stopped or remounted. */
 reset(true,true);assert(!desktop_usb_storage_may_exit(&d,false));
 assert(view_state==DesktopUsbStorageStateEjectFirst&&s_eject_warning&&!trace[0]&&msc_active);
 /* Retry while still locked keeps warning; the tick does not claim an eject. */
 assert(!desktop_usb_storage_may_exit(&d,false));
 desktop_usb_storage_tick(&d);assert(view_state==DesktopUsbStorageStateEjectFirst&&s_eject_warning);
 /* Host ejects (ALLOW / START STOP UNIT): offered once, then Back leaves. */
 host_locked=false;desktop_usb_storage_tick(&d);
 assert(view_state==DesktopUsbStorageStateEjected&&!s_eject_warning);
 unsigned calls=state_calls;desktop_usb_storage_tick(&d);assert(state_calls==calls);
 assert(desktop_usb_storage_may_exit(&d,false));
 /* Host locks again after the offer: Back warns again. */
 host_locked=true;assert(!desktop_usb_storage_may_exit(&d,false));
 assert(view_state==DesktopUsbStorageStateEjectFirst);
 /* Long Back on the warning forces the exit and logs it. */
 assert(desktop_usb_storage_may_exit(&d,true)&&warnings==1);
 /* Never report "ejected" for a host that never locked. */
 reset(true,false);desktop_usb_storage_tick(&d);assert(!state_calls);
 puts("PASS USB storage exit: eject-first warning, retry, host-eject offer, forced exit, leave order");
}
