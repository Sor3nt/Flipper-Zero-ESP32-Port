#!/usr/bin/env python3
"""Run the production radio exchange against an independent Python WPS peer.

Uses real protocol crypto and EAP framing, with only ESP32 transport/time/random
mocked. This verifies protocol handling, not physical RF interoperability.
"""
from pathlib import Path
import hashlib, hmac, os, re, subprocess, tempfile
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "components/wifi/wifite/radio.c").read_text()
def function(name):
    m = re.search(r"static (?:bool|void|const char \*)\s*" + name + r"\([^;]*?\) \{", source)
    assert m, name
    end = source.index("{", m.start()) + 1
    depth = 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[m.start():end]

# RFC 3526 group 5 and an independently calculated enrollee transcript.
P = int("FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F406B7EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F356208552BB9ED529077096966D670C354E4ABC9804F1746C08CA237327FFFFFFFFFFFFFFFF", 16)
def tlv(t, b): return t.to_bytes(2, "big") + len(b).to_bytes(2, "big") + b
def u8(t, v): return tlv(t, bytes([v]))
def u16(t, v): return tlv(t, v.to_bytes(2, "big"))
def hm(k, b): return hmac.new(k, b, hashlib.sha256).digest()
def base(t): return u8(0x104a, 0x10) + u8(0x1022, t)
x = bytes([0x81]) + bytes(range(2, 33))
y = bytes(range(32, 64))
rn, en = bytes(range(16)), bytes(range(16, 32))
ap = bytes.fromhex("021122334455")
pke = pow(2, int.from_bytes(y, "big"), P).to_bytes(192, "big")
pkr = pow(2, int.from_bytes(x, "big"), P).to_bytes(192, "big")
shared = pow(int.from_bytes(pke, "big"), int.from_bytes(x, "big"), P).to_bytes(192, "big")
kdk = hm(hashlib.sha256(shared).digest(), en + ap + rn)
keys = b"".join(hm(kdk, i.to_bytes(4, "big") + b"Wi-Fi Easy and Secure Key Derivation" + (640).to_bytes(4, "big")) for i in range(1, 4))[:80]
auth, wrap = keys[:32], keys[32:48]
uuid = bytearray(rn); uuid[6] = (uuid[6] & 15) | 0x40; uuid[8] = (uuid[8] & 63) | 0x80
m1 = base(4) + tlv(0x101a, en) + tlv(0x1032, pke) + tlv(0x1020, ap) + u16(0x1004, 0x23) + u16(0x1010, 0x0d)
def signed(prev, m): return m + tlv(0x1005, hm(auth, prev + m)[:8])
m2 = signed(m1, base(5) + tlv(0x101a, en) + tlv(0x1039, rn) + tlv(0x1048, uuid) + tlv(0x1032, pkr) + u16(0x1004, 0x23) + u16(0x1010, 0x0d) + u8(0x100d, 1) + u16(0x1008, 0x010c) + tlv(0x1021, b"WIFITE") + tlv(0x1023, b"T-Embed") + tlv(0x1024, b"0.1") + tlv(0x1042, b"1") + tlv(0x1054, bytes.fromhex("00010050f2040001")) + tlv(0x1011, b"WIFITE") + u8(0x103c, 1) + u16(0x1002, 0) + u16(0x1009, 0) + u16(0x1012, 0) + tlv(0x102d, bytes.fromhex("80000001")))
pin, zero = b"12345670", bytes(16)
def ph(nonce, half): return hm(auth, nonce + hm(auth, half)[:16] + pke + pkr)
m3 = signed(m2, base(7) + tlv(0x1039, rn) + tlv(0x1014, ph(zero, pin[:4])) + tlv(0x1015, ph(zero, pin[4:])))
def encrypted(payload, iv):
    payload += tlv(0x101e, hm(auth, payload)[:8])
    pad = 16 - len(payload) % 16
    e = Cipher(algorithms.AES(wrap), modes.CBC(iv)).encryptor()
    return tlv(0x1018, iv + e.update(payload + bytes([pad]) * pad) + e.finalize())
random48, iv = bytes(range(48)), bytes(range(48, 64))
m4 = signed(m3, base(8) + tlv(0x101a, en) + tlv(0x103d, ph(random48[:16], pin[:4])) + tlv(0x103e, ph(random48[16:32], pin[4:])) + encrypted(tlv(0x103f, random48[:16]), random48[32:]))
m5 = signed(m4, base(9) + tlv(0x1039, rn) + encrypted(tlv(0x1016, zero), iv))
m6 = signed(m5, base(10) + tlv(0x101a, en) + encrypted(tlv(0x1040, random48[16:32]), iv))
m7 = signed(m6, base(11) + tlv(0x1039, rn) + encrypted(tlv(0x1017, zero) + tlv(0x1045, b"LAB") + tlv(0x1027, b"fixture-password"), iv))

harness = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "wifite.h"
#include "sdk_adapter.h"
#include "golden.h"
#define RX_MAX 1600
#define pdMS_TO_TICKS(n) (n)
enum { WIFI_REASON_AUTH_EXPIRE=2, WIFI_REASON_AUTH_FAIL=202,
 WIFI_REASON_ASSOC_EXPIRE=4, WIFI_REASON_ASSOC_FAIL=203,
 WIFI_REASON_NO_AP_FOUND=201, WIFI_REASON_BEACON_TIMEOUT=200,
 WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT=15 };
typedef struct {uint32_t captured_at; uint16_t len; bool eapol; uint8_t p[RX_MAX];} Frame;
struct WifiteJob {
 WfTarget target; WfSession *session; Frame *frame; int rx, error;
 unsigned outcome, cancel, link_ready, disconnect_reason, connected_channel,
 rx_auth, rx_assoc, rx_eapol, active_exchange, tx_eapol;
 uint8_t mac[6], bssid[6], tx[RX_MAX]; size_t tx_len;
};
static WifiteJob *job;
static Frame frames[128], output[256];
static unsigned count, cursor, written, tick, random_calls, closed, fail_tx_at;
static bool flood;
static char logs[65536];
static uint32_t now(void){return tick;}
static bool cancelled(WifiteJob*j){return j->cancel;}
static void logline(WifiteJob*j,const char*fmt,...){
 (void)j;size_t n=strlen(logs);va_list a;va_start(a,fmt);
 vsnprintf(logs+n,sizeof(logs)-n,fmt,a);va_end(a);
 n=strlen(logs);if(n+2<sizeof(logs)){logs[n]='\n';logs[n+1]=0;}
}
static const char *esp_err_to_name(int n){(void)n;return "mock TX error";}
static void wf_sdk_status(WfSdkStatus *s); /* declaration corrected below */
'''
# SDK declares this as external.
harness = harness.replace("static void wf_sdk_status(WfSdkStatus *s); /* declaration corrected below */", r'''
void wf_sdk_status(WfSdkStatus*s){*s=(WfSdkStatus){1,1,1,1,0};}
static void connect_worker(void*v){WifiteJob*j=v;j->link_ready=1;j->error=0;}
static void disconnect_worker(void*v){(void)v;closed++;}
static void tx_worker(void*v){
 WifiteJob*j=v;if(fail_tx_at&&written+1==fail_tx_at){j->error=-1;return;}
 assert(written<256);output[written].len=j->tx_len;
 memcpy(output[written++].p,j->tx,j->tx_len);j->error=0;
}
static bool wlan_hal_survey_run(void(*fn)(void*),void*j){fn(j);return true;}
static void xQueueReset(int q){(void)q;cursor=0;}
static bool xQueueReceive(int q,Frame*f,unsigned timeout){
 (void)q;tick+=flood?500:timeout;
 if(cursor<count){*f=frames[cursor++];job->rx_eapol++;return true;}
 if(flood){*f=frames[count-1];f->p[5]=(uint8_t)(10+tick/500);job->rx_eapol++;return true;}
 return false;
}
static void esp_fill_random(void*p,size_t n){
 switch(random_calls++){
 case 0:assert(n==32);memcpy(p,x,n);break;
 case 1:assert(n==16);memcpy(p,rn,n);break;
 case 2:assert(n==48);memcpy(p,random48,n);break;
 case 3:assert(n==16);memcpy(p,iv,n);break;
 default:assert(!"unexpected fresh keys on repeated message");
 }
}
''')
test = r'''
static void reset(void){
 count=cursor=written=tick=random_calls=closed=fail_tx_at=0;flood=false;logs[0]=0;
}
static void request(uint8_t id,const uint8_t*m,size_t n,unsigned flags,unsigned total){
 assert(count<128&&n+20<RX_MAX);Frame*f=&frames[count++];memset(f,0,sizeof(*f));f->eapol=true;
 uint8_t*p=f->p;size_t body=14+n+((flags&2)?2:0);
 p[0]=1;p[2]=body>>8;p[3]=body;p[4]=1;p[5]=id;p[6]=body>>8;p[7]=body;
 memcpy(p+8,"\xfe\x00\x37\x2a\x00\x00\x00\x01",8);p[16]=4;p[17]=flags;
 size_t off=18;if(flags&2){p[off++]=total>>8;p[off++]=total;}
 memcpy(p+off,m,n);f->len=4+body;
}
static void identity(void){
 Frame*f=&frames[count++];memset(f,0,sizeof(*f));f->eapol=true;f->len=9;
 memcpy(f->p,"\x01\x00\x00\x05\x01\x01\x00\x05\x01",9);
}
static void begin(void){reset();identity();request(2,m1,sizeof(m1),0,0);}
static bool run(const char*pin,WfSession*s,unsigned*outcome){
 WifiteJob j={0};Frame f={0};job=&j;j.frame=&f;j.session=s;j.target.channel=7;
 memcpy(j.bssid,ap,6);memcpy(j.target.bssid,ap,6);j.mac[0]=2;j.mac[5]=1;
 bool ok=exchange(&j,pin);assert(closed==1&&!j.active_exchange);
 if(outcome)*outcome=j.outcome;
 return ok;
}
static unsigned responses(uint8_t id,const uint8_t*m,size_t n){
 unsigned hits=0;for(unsigned i=0;i<written;i++){
  Frame*f=&output[i];if(f->len==32+n&&f->p[19]==id&&f->p[30]==4){
   assert(!memcmp(f->p+32,m,n));hits++;
  }
 }return hits;
}
int main(int argc,char**argv){
 WfSession s;unsigned outcome;
 begin();request(3,m3,sizeof(m3),0,0);
 assert(run(NULL,&s,NULL)&&random_calls==2&&responses(2,m2,sizeof(m2))==1);
 begin();request(2,m1,sizeof(m1),0,0);request(3,m3,sizeof(m3),0,0);
 assert(run(NULL,&s,NULL)&&random_calls==2&&responses(2,m2,sizeof(m2))==2);
 begin();request(3,m1,sizeof(m1),0,0);request(4,m3,sizeof(m3),0,0);
 bool ok=run(NULL,&s,NULL);
 if(argc>1&&!strcmp(argv[1],"--expect-old-failure")){
  assert(!ok&&strstr(logs,"Unexpected WPS message 0x04"));
  puts("REPRODUCED: v0.7 rejects identical M1 with a new EAP identifier before M3.");return 0;
 }
 assert(ok&&random_calls==2&&responses(2,m2,sizeof(m2))==1&&responses(3,m2,sizeof(m2))==1);
 /* Full real crypto transcript; duplicates must not change DH/nonce/transcript. */
 begin();request(3,m1,sizeof(m1),0,0);request(4,m3,sizeof(m3),0,0);
 request(5,m3,sizeof(m3),0,0);request(6,m5,sizeof(m5),0,0);
 request(7,m5,sizeof(m5),0,0);request(8,m7,sizeof(m7),0,0);
 assert(run("12345670",&s,NULL)&&random_calls==4&&!strcmp(s.key,"fixture-password"));
 assert(responses(4,m4,sizeof(m4))==1&&responses(5,m4,sizeof(m4))==1);
 assert(responses(6,m6,sizeof(m6))==1&&responses(7,m6,sizeof(m6))==1);
 /* A fragment ACK must not replace the cached complete WSC response. */
 begin();size_t half=sizeof(m1)/2;
 request(3,m1,half,3,sizeof(m1));request(4,m1+half,sizeof(m1)-half,0,0);
 request(5,m3,sizeof(m3),0,0);
 assert(run(NULL,&s,NULL)&&random_calls==2&&responses(4,m2,sizeof(m2))==1);
 /* A changed M1 is not an identical retry, and must not reuse old keys. */
 uint8_t changed[sizeof(m1)];memcpy(changed,m1,sizeof(m1));changed[14]^=1;
 begin();request(3,changed,sizeof(changed),0,0);
 assert(!run(NULL,&s,&outcome)&&!outcome&&random_calls==2);
 assert(strstr(logs,"M1 changed")&&responses(3,m2,sizeof(m2))==0);
 begin();request(2,changed,sizeof(changed),0,0);
 assert(!run(NULL,&s,&outcome)&&!outcome&&strstr(logs,"Changed payload"));
 uint8_t corrupt[sizeof(m3)];memcpy(corrupt,m3,sizeof(m3));corrupt[sizeof(corrupt)-1]^=1;
 begin();request(3,corrupt,sizeof(corrupt),0,0);
 assert(!run(NULL,&s,&outcome)&&!outcome&&strstr(logs,"M3 integrity"));
 begin();request(3,m1,sizeof(m1),0,0);fail_tx_at=4;
 assert(!run(NULL,&s,&outcome)&&!outcome&&strstr(logs,"TX failed"));
 begin();flood=true;
 assert(!run(NULL,&s,&outcome)&&!outcome&&tick>=45000&&tick<=45500&&random_calls==2);
 puts("PASS: actual exchange: same/new EAP-ID retransmissions, M1/M3/M5 replay, real M1-M7 crypto, fragment ACK isolation, changed-session rejection and bounded retries");
}
'''
with tempfile.TemporaryDirectory() as td:
    td = Path(td)
    def array(n, b): return "static const uint8_t " + n + "[]={" + ",".join(map(str, b)) + "};\n"
    (td/"golden.h").write_text("".join(array(n, b) for n, b in dict(x=x,rn=rn,ap=ap,random48=random48,iv=iv,m1=m1,m2=m2,m3=m3,m4=m4,m5=m5,m6=m6,m7=m7).items()))
    (td/"esp_err.h").write_text("#pragma once\ntypedef int esp_err_t;\n#define ESP_OK 0\n")
    (td/"test.c").write_text(harness + "\n".join(function(n) for n in ("transmit","send_eap","disconnect_name","exchange")) + test)
    mb = Path(os.environ["MBEDTLS_ROOT"]); lib = os.environ["MBEDTLS_LIB"]
    exe = td/"test"; src = ROOT/"components/wifi/wifite"
    subprocess.run(["gcc","-O1","-g","-Wall","-Wextra","-fsanitize=address,undefined","-fno-omit-frame-pointer","-I"+str(td),"-I"+str(src),"-I"+str(src.parent),"-I"+str(mb/"include"),str(td/"test.c"),str(src/"core.c"),lib,"-o",str(exe)],check=True)
    import sys
    subprocess.run([str(exe)]+sys.argv[1:],check=True,env=dict(os.environ,ASAN_OPTIONS="detect_leaks=0"))
