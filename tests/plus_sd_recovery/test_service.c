#include "service_stubs.h"
#include "service_production.inc"
FATFS fatfs_object;
const FS_Api fs_api=0;
static uint32_t now_ticks;
static bool present,msc,rtc,drop_on_init;
static int hal_failures,hal_calls,reset_calls,mount_result,free_result;
static int unmount_result;
static int mount_calls,unmount_calls,delay_calls,notify_calls,formats,deletions;
static int timestamps,handles,events[3],redraws,processed,messages_left;
static Storage app;
static jmp_buf loop_end;

uint8_t furi_hal_sd_max_mount_retry_count(void){return 3;}
bool furi_hal_sd_is_present(void){return present;}
int furi_hal_sd_init(bool reset){
    ++hal_calls;if(reset)++reset_calls;
    if(drop_on_init){present=false;drop_on_init=false;}
    if(hal_failures){if(hal_failures>0)--hal_failures;return 1;}
    return 0;
}
bool furi_hal_usb_msc_is_active(void){return msc;}
void furi_hal_sd_presence_init(void){}
void fatfs_init(void){}
int f_mount(FATFS* fs,const char* path,int immediate){
    UNUSED(path);UNUSED(immediate);
    if(!fs){++unmount_calls;return unmount_result;}
    ++mount_calls;return mount_result;
}
int f_getfree(const char* path,uint32_t* clusters,FATFS** fs){
    UNUSED(path);*clusters=1;*fs=&fatfs_object;return free_result;
}
int f_mkfs(const char* path,int fmt,int cluster,void* work,UINT size){
    UNUSED(path);UNUSED(fmt);UNUSED(cluster);UNUSED(work);UNUSED(size);++formats;return FR_OK;
}
int f_setlabel(const char* label){UNUSED(label);return FR_OK;}
void* furi_record_open(const char* name){UNUSED(name);return NULL;}
void furi_record_close(const char* name){UNUSED(name);}
void furi_record_create(const char* name,void* value){UNUSED(name);UNUSED(value);}
void sd_notify_wait(NotificationApp* n){UNUSED(n);++notify_calls;}
void sd_notify_wait_off(NotificationApp* n){UNUSED(n);++notify_calls;}
void sd_notify_error(NotificationApp* n){UNUSED(n);++notify_calls;}
void sd_notify_success(NotificationApp* n){UNUSED(n);++notify_calls;}
void sd_notify_eject(NotificationApp* n){UNUSED(n);++notify_calls;}
uint32_t furi_get_tick(void){return now_ticks;}
uint32_t furi_ms_to_ticks(uint32_t ms){return (ms+9)/10;}
void furi_delay_ms(uint32_t ms){++delay_calls;now_ticks+=furi_ms_to_ticks(ms);}
StorageStatus storage_data_status(StorageData* s){return s->status;}
const char* storage_data_status_text(StorageData* s){UNUSED(s);return "status";}
void storage_data_timestamp(StorageData* s){UNUSED(s);++timestamps;}
size_t storage_open_files_count(StorageData* s){UNUSED(s);return (size_t)handles;}
bool furi_hal_rtc_is_flag_set(int flag){UNUSED(flag);return rtc;}
void furi_hal_rtc_reset_flag(int flag){UNUSED(flag);rtc=false;}
bool sd_remove_recursive(const char* path){UNUSED(path);++deletions;return true;}
void view_port_enabled_set(void* vp,bool enabled){UNUSED(vp);UNUSED(enabled);}
void view_port_update(void* vp){UNUSED(vp);++redraws;}
void furi_pubsub_publish(void* pubsub,const void* event){UNUSED(pubsub);++events[((const StorageEvent*)event)->type];}
Storage* storage_app_alloc(void){return &app;}
void storage_process_message(Storage* a,StorageMessage* msg){UNUSED(a);UNUSED(msg);++processed;}
int furi_message_queue_get(void* queue,StorageMessage* msg,uint32_t timeout){
    UNUSED(queue);UNUSED(msg);UNUSED(timeout);
    if(!messages_left--)longjmp(loop_end,1);
    now_ticks+=furi_ms_to_ticks(100);return FuriStatusOk;
}
static void start(int failures,int mount_status,int free_status){
    free(app.storage[ST_EXT].data);
    app=(Storage){0};present=true;msc=rtc=drop_on_init=false;now_ticks=0;
    hal_failures=failures;mount_result=mount_status;free_result=free_status;unmount_result=FR_OK;
    hal_calls=reset_calls=mount_calls=unmount_calls=delay_calls=notify_calls=0;
    formats=deletions=timestamps=handles=redraws=processed=0;
    for(unsigned i=0;i<3;++i)events[i]=0;
    app.storage[ST_EXT].status=StorageStatusNotReady;
    storage_ext_init(&app.storage[ST_EXT]);
    app.sd_gui.enabled=app.storage[ST_EXT].status!=StorageStatusNotReady;
}
static void advance(unsigned ms){now_ticks+=furi_ms_to_ticks(ms);storage_tick(&app);}
static void baseline(void){
    start(0,FR_OK,FR_OK);assert(hal_calls==1);assert(app.storage[0].status==StorageStatusOK);
    advance(30000);assert(hal_calls==1);assert(events[StorageEventTypeCardMount]==0);
    handles=1;assert(storage_process_sd_unmount(&app)==FSE_DENIED);
    assert(unmount_calls==0&&app.storage[0].status==StorageStatusOK);handles=0;
    assert(storage_process_sd_unmount(&app)==FSE_OK);advance(30000);assert(hal_calls==1);
    assert(storage_process_sd_unmount(&app)==FSE_NOT_READY);
    assert(events[StorageEventTypeCardUnmount]==1);
    assert(sd_mount_card(&app.storage[0],false)==FSE_OK);storage_tick(&app);
    assert(hal_calls==2&&events[StorageEventTypeCardMount]==1);
    start(0,FR_NO_FILESYSTEM,FR_NO_FILESYSTEM);advance(30000);
    assert(app.storage[0].status==StorageStatusNoFS&&hal_calls==1&&formats==0);
    start(-1,FR_OK,FR_OK);assert(hal_calls==3&&reset_calls==1&&delay_calls==3);
    assert(app.storage[0].status==StorageStatusErrorInternal);
}
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
static void recovery(void){
    start(3,FR_OK,FR_OK);advance(4990);assert(hal_calls==3);
    rtc=true;advance(10);assert(hal_calls==4&&reset_calls==2);
    assert(app.storage[0].status==StorageStatusOK);
    assert(events[StorageEventTypeCardMount]==1&&redraws==1);
    assert(delay_calls==3&&notify_calls==0&&formats==0&&deletions==0);
    advance(30000);assert(hal_calls==4&&events[StorageEventTypeCardMount]==1);
    start(-1,FR_OK,FR_OK);
    for(int i=1;i<=6;++i){advance(5000);assert(hal_calls==3+i);}
    advance(60000);assert(hal_calls==9&&reset_calls==7&&delay_calls==3);
    assert(((SDData*)app.storage[0].data)->recovery_left==0);
    start(3,FR_OK,FR_OK);msc=true;advance(60000);assert(hal_calls==3);
    assert(((SDData*)app.storage[0].data)->recovery_left==6);
    msc=false;handles=1;advance(60000);assert(hal_calls==3);
    handles=0;storage_tick(&app);assert(hal_calls==4&&app.storage[0].status==StorageStatusOK);
    start(3,FR_OK,FR_OK);assert(sd_unmount_card(&app.storage[0])==FSE_OK);advance(60000);
    assert(hal_calls==3&&app.storage[0].status==StorageStatusNotReady);
    start(3,FR_OK,FR_OK);unmount_result=FR_DISK_ERR;
    assert(storage_process_sd_unmount(&app)==FSE_INTERNAL);
    assert(app.storage[0].status==StorageStatusErrorInternal);
    assert(((SDData*)app.storage[0].data)->recovery_left==0);
    advance(60000);assert(hal_calls==3);
    start(0,FR_OK,FR_OK);unmount_result=FR_DISK_ERR;
    assert(storage_process_sd_unmount(&app)==FSE_INTERNAL);
    assert(app.storage[0].status==StorageStatusOK);
    start(3,FR_OK,FR_OK);present=false;advance(5000);assert(hal_calls==3);
    assert(app.storage[0].status==StorageStatusNotReady);
    present=true;storage_tick(&app);assert(hal_calls==4&&app.storage[0].status==StorageStatusOK);
    start(3,FR_OK,FR_OK);drop_on_init=true;advance(5000);
    assert(app.storage[0].status==StorageStatusNotReady&&events[StorageEventTypeCardMount]==0);
    assert(((SDData*)app.storage[0].data)->recovery_left==0);
    start(0,FR_DISK_ERR,FR_OK);assert(hal_calls==3);mount_result=FR_OK;advance(5000);
    assert(hal_calls==4&&app.storage[0].status==StorageStatusOK);
    start(0,FR_OK,FR_DISK_ERR);assert(hal_calls==3);free_result=FR_OK;advance(5000);
    assert(hal_calls==4&&app.storage[0].status==StorageStatusOK);
    start(0,FR_NO_FILESYSTEM,FR_DISK_ERR);advance(60000);
    assert(app.storage[0].status==StorageStatusNoFS&&hal_calls==1&&formats==0);
    start(3,FR_OK,FR_OK);mount_result=free_result=FR_NO_FILESYSTEM;advance(5000);
    assert(app.storage[0].status==StorageStatusNoFS);advance(60000);assert(hal_calls==4&&formats==0);
    start(3,FR_OK,FR_OK);SDData* d=app.storage[0].data;
    now_ticks=UINT32_MAX-200;d->recovery_at=now_ticks;advance(4990);assert(hal_calls==3);
    advance(10);assert(hal_calls==4);
    start(3,FR_OK,FR_OK);assert(sd_format_card(&app.storage[0])==FSE_OK);
    advance(60000);assert(formats==1&&hal_calls==3);
    start(-1,FR_OK,FR_OK);sd_mount_card(&app.storage[0],false);assert(hal_calls==6);
    advance(60000);assert(hal_calls==6);
    start(3,FR_OK,FR_OK);messages_left=60;
    if(!setjmp(loop_end))storage_srv(NULL);
    assert(processed==60&&hal_calls==4&&events[StorageEventTypeCardMount]==1);
}
#else
static void original(void){
    start(3,FR_OK,FR_OK);advance(60000);assert(hal_calls==3);
    assert(events[StorageEventTypeCardMount]==0);
    messages_left=60;if(!setjmp(loop_end))storage_srv(NULL);
    assert(processed==60&&hal_calls==3);
}
#endif
int main(void){
    baseline();
#if CONFIG_T_EMBED_PLUS_SD_RECOVERY
    recovery();
    puts("PASS: 5s/6-attempt retry cap; single reset; elapsed wraparound; busy queue; USB/handle/eject/removal guards; detach errors preserve status and cancel retry; NoFS/no format/no deletion; error-to-OK mount event exactly once");
#else
    original();puts("PASS: disabled profile retains mount/retry/eject and idle-tick behavior");
#endif
    free(app.storage[ST_EXT].data);return 0;
}
