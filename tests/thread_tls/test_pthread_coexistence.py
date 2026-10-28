"""Exercise production Furi TLS helpers against ESP-IDF's actual pthread TLS code."""
from pathlib import Path
import os,re,subprocess,tempfile
root=Path(__file__).resolve().parents[2]
idf=Path(os.environ.get('IDF_PATH',root.parent/'esp-idf'))
source=(idf/'components/pthread/pthread_local_storage.c').read_text()
assert '#define PTHREAD_TLS_INDEX 0' in source
thread=(root/'components/furi/core/thread.c').read_text()
assert 'pvTaskGetThreadLocalStoragePointer' not in thread
assert 'vTaskSetThreadLocalStoragePointer' not in thread
assert thread.count('furi_thread_local_get(')==5
assert thread.count('furi_thread_local_set(')==2
getter=thread[thread.index('FuriThread* furi_thread_get_current(void)'):thread.index('void furi_thread_yield(void)')]
for p in root.glob('sdkconfig*'):
 if p.is_file() and 'CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=' in p.read_text():
  assert 'CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=2' in p.read_text(),p
for p in (root/'applications/main/wlan_app/scenes').glob('*.c'):
 assert 'vTaskSetThreadLocalStoragePointer(NULL, 0' not in p.read_text(),p
with tempfile.TemporaryDirectory() as directory:
 d=Path(directory);(d/'freertos').mkdir();(d/'sys').mkdir()
 (d/'freertos/FreeRTOS.h').write_text('''#pragma once
#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#ifndef CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS
#define CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS 2
#endif
#define CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS 1
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
''')
 (d/'freertos/task.h').write_text('''#pragma once
#include "FreeRTOS.h"
typedef struct Task {void* slots[2];void (*destroy[2])(int,void*);} Task;
typedef Task* TaskHandle_t;
static Task tasks[2];static Task* current=&tasks[0];
static void* pvTaskGetThreadLocalStoragePointer(TaskHandle_t task,int index){return (task?task:current)->slots[index];}
static void vTaskSetThreadLocalStoragePointer(TaskHandle_t task,int index,void* p){(task?task:current)->slots[index]=p;}
static void vTaskSetThreadLocalStoragePointerAndDelCallback(TaskHandle_t task,int index,void* p,void(*cb)(int,void*)){Task*t=task?task:current;t->slots[index]=p;t->destroy[index]=cb;}
''')
 for header in ['esp_err.h','esp_log.h','sys/lock.h']:(d/header).write_text('')
 preamble='''#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>
#define pthread_key_create idf_pthread_key_create
#define pthread_key_delete idf_pthread_key_delete
#define pthread_getspecific idf_pthread_getspecific
#define pthread_setspecific idf_pthread_setspecific
#include "thread_tls.h"
typedef struct {uintptr_t tcb_first;const char* appid;} FuriThread;
'''
 main=r'''
static int destructed;
static void destructor(void* p){assert(p==(void*)0x1234);destructed++;}
int main(void){
 pthread_key_t key,other;assert(pthread_key_create(&key,destructor)==0);assert(pthread_key_create(&other,NULL)==0);
 FuriThread furi={.tcb_first=0x1400,.appid="flip_pi"};
 // Reproduce the crash: pthread interprets Furi's TCB first word as a list link.
 pid_t child=fork();assert(child>=0);
 if(child==0){freopen("/dev/null","w",stderr);vTaskSetThreadLocalStoragePointer(NULL,0,&furi);pthread_getspecific(key);_exit(0);}
 int status;waitpid(child,&status,0);assert((WIFSIGNALED(status))||(WIFEXITED(status)&&WEXITSTATUS(status)!=0));
 // With production helpers, network/pthread storage and Furi identity coexist.
 furi_thread_local_set(NULL,&furi);assert(furi_thread_get_current()==&furi);assert(pthread_getspecific(key)==NULL);
 assert(pthread_setspecific(key,(void*)0x1234)==0);assert(pthread_setspecific(other,(void*)0x4567)==0);
 assert(pthread_getspecific(key)==(void*)0x1234);assert(pthread_getspecific(other)==(void*)0x4567);
 assert(furi_thread_get_current()==&furi);assert(pvTaskGetThreadLocalStoragePointer(NULL,0)!=&furi);
 // A foreign pthread task must not be misidentified as a Furi thread/parent.
 current=&tasks[1];assert(furi_thread_get_current()==NULL);assert(pthread_getspecific(key)==NULL);
 assert(pthread_setspecific(other,(void*)0x9876)==0);assert(furi_thread_get_current()==NULL);
 pthread_internal_local_storage_destructor_callback(current);
 current=&tasks[0];assert(pthread_getspecific(other)==(void*)0x4567);
 // IDF's deletion callback frees pthread state without touching Furi identity.
 pthread_internal_local_storage_destructor_callback(current);assert(destructed==1);assert(furi_thread_get_current()==&furi);
 assert(pthread_getspecific(key)==NULL);furi_thread_local_set(NULL,NULL);assert(furi_thread_get_current()==NULL);
 pthread_key_delete(key);pthread_key_delete(other);
 puts("PASS: old slot-0 collision reproduced; isolated slots preserve Furi identity, pthread values, foreign tasks and destructors");
}
'''
 (d/'test.c').write_text(preamble+source+'\n'+getter+main)
 cmd=['cc','-std=gnu17','-fsanitize=address,undefined','-g','-I'+str(d),'-I'+str(root/'components/furi/core'),'-I'+str(idf/'components/pthread'),str(d/'test.c'),'-o',str(d/'test')]
 subprocess.run(cmd,check=True);subprocess.run([str(d/'test')],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
 rejected=subprocess.run(cmd+['-DCONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS=1'],capture_output=True)
 assert rejected.returncode and b'Furi requires at least two' in rejected.stderr
 print('PASS: undersized SDK configuration rejected at compile time; all Furi accesses and profiles migrated')
