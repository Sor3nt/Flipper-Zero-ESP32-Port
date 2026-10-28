#!/usr/bin/env python3
"""Run the actual worker orchestration/scan/status functions against mock I/O.

No radio use. Covers success short-circuit, fallback ordering, lock waits,
stale frames and client data arriving after the last AP beacon.
"""
from pathlib import Path
import os, re, subprocess, tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'components/wifi/wifite/radio.c').read_text()

def function(name):
    match = re.search(r'static (?:bool|void|int32_t) ' + name + r'\([^;]*?\) \{', source)
    assert match, name
    start = source.index('{', match.start())
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]

common = r'''
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "wifite.h"
'''
frame = source[source.index('#define RX_MAX'):source.index('struct WifiteJob')]
common += frame + r'''
struct WifiteJob {
  WifiteMode mode; WfTarget target; char pin[9]; WfSession *session;
  unsigned cancel, finished, outcome, dropped, log_dropped;
  Frame *frame; int rx, events, error; uint8_t channel;
};
static unsigned tick;
static char logs[32768];
static bool cancelled(WifiteJob *j) { return j->cancel; }
static void logline(WifiteJob*j, const char *fmt, ...) {
  (void)j; size_t n=strlen(logs); va_list a; va_start(a,fmt);
  vsnprintf(logs+n,sizeof(logs)-n,fmt,a); va_end(a);
  n=strlen(logs); if(n+2<sizeof(logs)){logs[n]='\n';logs[n+1]=0;}
}
static void furi_delay_ms(unsigned n){tick+=n;}
'''

workflow = r'''
static unsigned exchanges, captures, solvers, brutes, keys, fresh_calls;
static unsigned first_outcome, capture_outcome, opens, closes;
static bool first_ok, capture_ok, verify_ok, leased;
static bool brute_ok, cancel_first, cancel_capture, fail_fresh; static int pixie_result;
static bool radio_open(WifiteJob*j){(void)j;assert(!leased);leased=true;opens++;return true;}
static void radio_close(WifiteJob*j){(void)j;if(leased)closes++;leased=false;}
static void scan(WifiteJob*j){(void)j;}
static bool fresh_target(WifiteJob*j){(void)j;fresh_calls++;return !(fail_fresh && fresh_calls==2);}
static bool exchange(WifiteJob*j,const char*pin){
  assert(leased);
  if(!pin){captures++;assert(exchanges==1&&opens==2);j->session->stage=capture_ok?3:0;
    j->outcome=capture_outcome;if(cancel_capture)j->cancel=1;return capture_ok;}
  exchanges++;
  if(exchanges==1){assert(!strcmp(pin,"12345670"));j->outcome=first_outcome;
    if(cancel_first)j->cancel=1;return first_ok;}
  assert(pixie_result>0 && !strcmp(pin,"87654325"));return verify_ok;
}
static int wf_mock_pixie(WfSession*s,bool full,uint32_t epoch,WfProgress cb,void*ctx){
  (void)epoch;(void)cb;(void)ctx;assert(!full&&captures==1);solvers++;
  if(pixie_result>0)strcpy(s->pin,"87654325");return pixie_result;
}
static bool solver_progress(void*c,const char*p,uint64_t n,uint64_t t){(void)c;(void)p;(void)n;(void)t;return true;}
static bool brute_force(WifiteJob*j){(void)j;assert(leased&&opens==3&&captures==1);
  assert((solvers==1&&pixie_result==0)||(!solvers&&!capture_ok));brutes++;return brute_ok;}
static void result(WifiteJob*j,const char*label,const char*value){(void)j;(void)value;if(!strcmp(label,"NETWORK KEY: "))keys++;}
'''
workflow_test = r'''
static void reset(void){
 exchanges=captures=solvers=brutes=keys=fresh_calls=opens=closes=0;leased=false;
 first_outcome=1;first_ok=false;capture_ok=verify_ok=brute_ok=true;
 cancel_first=cancel_capture=fail_fresh=false;capture_outcome=0;pixie_result=0;logs[0]=0;tick=0;
}
static void run(void){WfSession s={0};WifiteJob j={.mode=WifiteAuto,.session=&s};
 work(&j);assert(j.finished&&!leased&&opens==closes);}
int main(void){
 reset();first_ok=true;run();assert(exchanges==1&&!captures&&!solvers&&!brutes&&keys==1);
 reset();run();assert(exchanges==1&&captures==1&&solvers==1&&brutes==1&&keys==1);
 reset();first_outcome=2;pixie_result=4;run();assert(exchanges==2&&solvers==1&&!brutes&&keys==1);
 reset();first_outcome=3;run();assert(exchanges==1&&!captures&&!solvers&&!brutes&&!keys);
 reset();first_outcome=0;run();assert(exchanges==1&&captures==1&&solvers==1&&brutes==1&&keys==1);
 assert(strstr(logs,"First PIN unverified")&&!strstr(logs,"Default PIN rejected"));
 reset();cancel_first=true;run();assert(exchanges==1&&!captures&&!brutes&&!keys);
 reset();capture_ok=false;run();assert(captures==1&&!solvers&&brutes==1&&keys==1);
 assert(strstr(logs,"offline search not run")&&!strstr(logs,"NO MATCH"));
 reset();first_outcome=0;capture_ok=false;brute_ok=false;run();
 assert(captures==1&&!solvers&&brutes==1&&!keys&&strstr(logs,"AUTO 3/3"));
 reset();capture_ok=false;capture_outcome=3;run();assert(captures==1&&!solvers&&!brutes&&!keys);
 reset();capture_ok=false;cancel_capture=true;run();assert(captures==1&&!solvers&&!brutes&&!keys);
 reset();fail_fresh=true;run();assert(exchanges==1&&!captures&&!solvers&&!brutes);
 reset();pixie_result=-2;run();assert(solvers==1&&!brutes&&!keys);
 puts("PASS: actual Auto worker: default PIN first, timeout/capture-failure fallback, radio reacquisition, success/lock/cancel/error stops");
}
'''

passive = r'''
#define ESP_OK 0
#define FuriStatusOk 0
#define pdMS_TO_TICKS(n) (n)
typedef struct {unsigned schan,nchan;} wifi_country_t;
static Frame incoming[16]; static unsigned incoming_n, at, events;
static WfTarget observed[WF_TARGET_MAX]; static unsigned observed_n;
static unsigned now(void){return tick;}
static int esp_wifi_get_country(wifi_country_t*c){c->schan=1;c->nchan=1;return 0;}
static const char*esp_err_to_name(int n){(void)n;return "mock";}
static void listen_worker(void*c){((WifiteJob*)c)->error=0;}
static bool wlan_hal_survey_run(void(*f)(void*),void*c){f(c);return true;}
static bool channel(WifiteJob*j,unsigned ch){j->channel=ch;return true;}
static void xQueueReset(int q){(void)q;}
static bool xQueueReceive(int q,Frame*f,unsigned ms){
 (void)q;
 if(at<incoming_n && incoming[at].captured_at<=tick+ms){
   if(incoming[at].captured_at>tick)tick=incoming[at].captured_at;
   *f=incoming[at++];return true;
 }
 tick+=ms;return false;
}
static int furi_message_queue_put(int q,const WifiteEvent*e,unsigned ms){
 (void)q;(void)ms;events++;unsigned i=0;
 while(i<observed_n && memcmp(observed[i].bssid,e->target.bssid,6))i++;
 if(i==observed_n)observed_n++;observed[i]=e->target;return FuriStatusOk;
}
static const uint8_t ap[]={2,1,2,3,4,5};
static void beacon(unsigned when,unsigned seq,int locked,bool wps){
 Frame*f=&incoming[incoming_n++];memset(f,0,sizeof(*f));
 f->captured_at=when;f->rssi=-42;f->channel=1;f->p[0]=0x80;
 memcpy(f->p+10,ap,6);memcpy(f->p+16,ap,6);f->p[22]=seq<<4;f->p[23]=seq>>4;
 const uint8_t ies[]={0,3,'L','A','B',3,1,1,221,14,0,0x50,0xf2,4,0x10,0x4a,0,1,0x10,0x10,0x57,0,1,0};
 memcpy(f->p+36,ies,sizeof(ies));f->len=36+sizeof(ies);
 if(!wps)f->len=44;
 else if(locked<0){f->p[45]=9;f->len-=5;}
 else f->p[f->len-1]=locked;
}
static void client(unsigned when,unsigned id){
 Frame*f=&incoming[incoming_n++];memset(f,0,sizeof(*f));f->captured_at=when;f->len=26;
 f->p[0]=8;f->p[1]=1;memcpy(f->p+4,ap,6);f->p[10]=2;f->p[15]=id;
}
static WifiteJob job;static Frame scratch;
static void reset(void){
 tick=1000;at=incoming_n=events=observed_n=0;logs[0]=0;
 job=(WifiteJob){.frame=&scratch,.target={.channel=1,.clients=3}};
 memcpy(job.target.bssid,ap,6);
}
'''
passive_test = r'''
int main(void){
 reset();beacon(1100,1,0,true);beacon(1400,2,0,true);
 assert(fresh_target(&job)&&job.target.locked==0&&job.target.clients==3&&tick<2000);
 reset();beacon(900,1,1,true);beacon(1100,2,0,true);beacon(1400,3,0,true);
 assert(fresh_target(&job)&&job.outcome==0); /* stale queued lock ignored */
 reset();beacon(1100,1,1,true);beacon(1400,2,1,true);beacon(3000,3,0,true);beacon(3300,4,0,true);
 assert(fresh_target(&job)&&job.target.locked==0&&tick==3300);
 reset();beacon(1100,1,1,true);beacon(1400,2,1,true);
 assert(!fresh_target(&job)&&job.outcome==3&&tick>=31000&&tick<31100);
 reset();beacon(1100,1,1,true);beacon(1400,2,-1,true);beacon(1800,3,-1,true);
 assert(!fresh_target(&job)&&job.outcome==3); /* absent does not clear lock */
 reset();beacon(1100,1,-1,true);beacon(1400,2,-1,true);
 assert(fresh_target(&job)&&job.target.locked==-1);
 reset();beacon(1100,1,0,true);beacon(1400,1,0,true);
 assert(!fresh_target(&job)&&!job.outcome); /* duplicate isn't confirmation */
 reset();beacon(1100,1,0,true);beacon(1400,2,0,true);incoming[1].p[16]^=2;
 assert(!fresh_target(&job)); /* different BSSID */
 reset();beacon(1100,1,0,true);beacon(1400,2,0,true);incoming[1].channel=6;
 assert(!fresh_target(&job)); /* different channel */
 reset();job.cancel=1;assert(!fresh_target(&job)&&tick==1000);
 puts("PASS: actual fresh-status worker: stale/duplicate filtering, lock confirmation/clearance, unknown, 30s limit and cancellation");
 reset();job.mode=WifiteScan;beacon(1100,1,0,true);client(1150,7);client(1200,7);client(1250,8);
 scan(&job);assert(observed_n==1&&observed[0].wps&&observed[0].clients==2);
 reset();job.mode=WifiteScan;client(1050,7);beacon(1100,1,0,true);beacon(1200,2,0,false);
 scan(&job);assert(observed_n==1&&observed[0].wps&&observed[0].locked==0&&observed[0].clients==1);
 reset();job.mode=WifiteScan;beacon(1100,1,0,false);client(1150,7);
 scan(&job);assert(observed_n==1&&!observed[0].wps&&observed[0].locked==-1&&observed[0].clients==1);
 puts("PASS: actual scan worker: clients after final beacon, de-duplication, non-WPS APs and WPS probe/beacon preservation");
}
'''

brute = r'''
static unsigned tries, saved_count, outcomes[8], save_first;
static bool cancel_attempt, unavailable;
static char pins[8][9];
static bool fresh_target(WifiteJob*j){(void)j;return !unavailable;}
static bool exchange(WifiteJob*j,const char*pin){
 assert(tries<8);strcpy(pins[tries],pin);j->outcome=outcomes[tries++];
 if(cancel_attempt)j->cancel=1;return false;
}
static void checkpoint(WifiteJob*j,WfBrute*b,bool load,bool remove){
 (void)j;assert(!remove);if(!load){saved_count++;save_first=b->first;}
}
'''
brute_test = r'''
static void reset(void){tries=saved_count=save_first=0;cancel_attempt=unavailable=false;
 memset(outcomes,0,sizeof(outcomes));memset(pins,0,sizeof(pins));logs[0]=0;}
static void run(void){WifiteJob j={0};assert(!brute_force(&j));}
int main(void){
 reset();run();assert(tries==3&&saved_count==1&&save_first==0);
 assert(!strcmp(pins[0],pins[1])&&!strcmp(pins[1],pins[2]));
 assert(strstr(logs,"3 ambiguous attempts; PIN not skipped"));
 reset();outcomes[1]=1;run();assert(tries==5&&save_first==1);
 assert(!strcmp(pins[0],pins[1])&&strcmp(pins[1],pins[2])&&
        !strcmp(pins[2],pins[3])&&!strcmp(pins[3],pins[4]));
 reset();outcomes[0]=3;run();assert(tries==1&&save_first==0);
 reset();cancel_attempt=true;run();assert(tries==1&&save_first==0);
 reset();unavailable=true;run();assert(tries==0&&save_first==0);
 puts("PASS: actual online worker: three inconclusive attempts stop without skipping PINs, lock/cancel stop, checkpoint preserved");
}
'''

with tempfile.TemporaryDirectory() as td:
    td = Path(td)
    crypto = root / 'components/wifi/wifite/core.c'
    mb = Path(os.environ['MBEDTLS_ROOT'])
    for name, body in (
        ('workflow', common + workflow + function('next_online_stage') + function('work').replace('wf_pixie_all(', 'wf_mock_pixie(') + workflow_test),
        ('passive', common + passive + function('scan') + function('fresh_target') + passive_test),
        ('brute', common + brute + function('brute_force') + brute_test),
    ):
        c = td / (name + '.c')
        c.write_text(body)
        exe = td / name
        subprocess.run(['gcc', '-std=c11', '-O1', '-g', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-I' + str(root / 'components/wifi'),
                        '-I' + str(mb / 'include'), str(c), str(crypto),
                        os.environ['MBEDTLS_LIB'], '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True, env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
