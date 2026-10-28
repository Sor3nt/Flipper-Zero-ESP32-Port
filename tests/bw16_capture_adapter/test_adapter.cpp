/* Executes the actual Arduino adapter with mocked radio/UART boundaries. */
#include "Arduino.h"
#include "wifi_conf.h"
#include "bw16_native.h"
#include "bw16_pcap.h"
static uint32_t fake_us;
static char output[1024][BWC_LINE_SIZE];
static unsigned output_count,tx_calls,promisc_mode;
static bool radio_ok=true,promisc_ok=true,expect_capture_tx,inject_eapol_on_tx;
static unsigned tx_cost_ms;
static uint8_t tuned_channel;
static void (*receive_callback)(unsigned char*,unsigned int,void*);
MockSerial Serial={},Serial1={};
uint32_t millis(){return fake_us/1000U;}
uint32_t micros(){return fake_us;}
void delay(unsigned value){fake_us+=value*1000U;}
void MockSerial::println(const char* text){
 assert(output_count<1024 && strlen(text)<BWC_LINE_SIZE);
 strcpy(output[output_count++],text);
 /* Model blocking serialization so deadline/STOP checks include wire time. */
 fake_us+=(uint32_t)(strlen(text)+2U)*87U;
}
int wifi_on(int mode){assert(mode==RTW_MODE_STA);return 1;}
int wifi_scan_networks(rtw_result_t (*)(rtw_scan_handler_result_t*),void*){return RTW_SUCCESS;}
int wifi_set_channel(uint8_t ch){tuned_channel=ch;return radio_ok?0:-1;}
int wifi_set_promisc(int mode,void (*cb)(unsigned char*,unsigned int,void*),int len){
 if(mode){assert(mode==RTW_PROMISC_ENABLE_2 && len==1);if(!radio_ok || !promisc_ok)return -1;}
 promisc_mode=mode;receive_callback=cb;return 0;
}
#include "../../firmware/bw16_controller/bw16_controller.ino"
static const uint8_t target[6]={2,3,4,5,6,7};
static void init(){
 fake_us=1000000U;output_count=tx_calls=0;radio_ok=true;promisc_mode=0;receive_callback=NULL;
 promisc_ok=true;expect_capture_tx=inject_eapol_on_tx=false;tx_cost_ms=0;
 Serial={};Serial1={};setup();assert(wifi_ready && Serial.baud==115200 && Serial1.baud==115200);
 controller.ap_count=1;controller.scan_valid=controller.indices_valid=true;
 BwnAp* ap=&controller.aps[0];memcpy(ap->mac,target,6);ap->channel=36;strcpy(ap->ssid,"Owned fixture");
 output_count=0;
}
static void command(const char* line){
 size_t n=strlen(line);assert(n<sizeof(Serial1.input));memcpy(Serial1.input,line,n);
 Serial1.position=0;Serial1.used=(unsigned)n;loop();assert(!Serial1.available());
}
static bool has(const char* text){for(unsigned i=0;i<output_count;++i)if(!strcmp(output[i],text))return true;return false;}
static unsigned packets(){unsigned n=0;for(unsigned i=0;i<output_count;++i)if(!strncmp(output[i],"PCAP1:",6))++n;return n;}
static void eapol(uint8_t* bytes){
 memset(bytes,0,64);bytes[0]=8;bytes[1]=1;memcpy(bytes+4,target,6);bytes[10]=2;bytes[15]=9;
 const uint8_t snap[12]={0xaa,0xaa,3,0,0,0,0x88,0x8e,2,3,0,4};memcpy(bytes+24,snap,12);
 bytes[36]=0xA5;bytes[37]=0x77;bytes[38]=0x33;bytes[39]=0x11;
}
void bwn_radio_deauth(const uint8_t source[6],const uint8_t destination[6],uint16_t reason){
 /* Any passive-mode transmission fails immediately. Capture must already be live. */
 assert(expect_capture_tx && capturing && receive_callback && promisc_mode==RTW_PROMISC_ENABLE_2);
 assert(controller.state==BwnCapture && controller.capture_with_deauth && tuned_channel==36);
 assert(!memcmp(source,target,6) && !memcmp(destination,"\xff\xff\xff\xff\xff\xff",6));
 assert(tx_calls<3 && reason==((const uint16_t[]){1,4,16})[tx_calls]);
 char start[40];snprintf(start,sizeof(start),"CAPTURE_START:%08lX",(unsigned long)controller.capture_session);
 assert(has(start));++tx_calls;
 /* A real receive callback during TX must enqueue before the sender finishes. */
 if(inject_eapol_on_tx){uint8_t bytes[64];eapol(bytes);inject_eapol_on_tx=false;receive_callback(bytes,40,NULL);}
 fake_us+=tx_cost_ms*1000U;
}
static void assisted_capture(){
 init();expect_capture_tx=inject_eapol_on_tx=true;
 command("CAPTURE_DEAUTH 0 00000010\n");
 assert(tx_calls==3 && capturing && capture_accepted==1 && capture_queued==1);
 assert(has("CAPTURE_DEAUTH:00000010:3"));
 for(unsigned i=0;i<100;++i)loop();
 assert(tx_calls==3 && packets()==1 && capture_emitted==1);
 command("STOP\n");assert(controller.state==BwnIdle && !capturing && tx_calls==3);
 assert(has("CAPTURE_STATS:00000010:1:0") && has("CAPTURE_DONE:00000010"));
 puts("adapter: assisted capture arms RX before three TX attempts, retains an immediate EAPOL, no repeated burst PASS");

 init();command("CAPTURE_DEAUTH 0 00000011\nSTOP\n");
 assert(controller.state==BwnIdle && !tx_calls && !capturing);
 init();radio_ok=false;command("CAPTURE_DEAUTH 0 00000012\n");
 assert(controller.state==BwnIdle && !tx_calls && !capturing && !receive_callback);
 init();promisc_ok=false;command("CAPTURE_DEAUTH 0 00000013\n");
 assert(controller.state==BwnIdle && !tx_calls && !capturing && !receive_callback);
 puts("adapter: STOP queued before burst, channel failure and receiver startup failure prevent all TX PASS");

 init();bwn_command(&controller,"CAPTURE_DEAUTH 0 00000014",millis());
 assert(capturing && !tx_calls);fake_us+=1000000U;loop();
 assert(controller.state==BwnCapture && !tx_calls && has("CAPTURE_DEAUTH:00000014:0"));
 for(unsigned i=0;i<100;++i) { loop(); }
 assert(!tx_calls);command("STOP\n");
 init();expect_capture_tx=true;tx_cost_ms=1000;
 command("CAPTURE_DEAUTH 0 00000015\n");
 assert(tx_calls==1 && capturing && has("CAPTURE_DEAUTH:00000015:1"));
 for(unsigned i=0;i<100;++i) { loop(); }
 assert(tx_calls==1);command("STOP\n");
 /* Switching back to passive resets assisted-mode state and never sends again. */
 expect_capture_tx=false;tx_cost_ms=0;command("CAPTURE 0 00000016\n");
 assert(capturing && !controller.capture_with_deauth && tx_calls==1);
 for(unsigned i=0;i<100;++i) { loop(); }
 assert(tx_calls==1);command("STOP\n");
 puts("adapter: independent 1-second TX cutoff checked per submission and passive restart remains zero-TX PASS");
}
int main(){
 uint8_t raw[64],large[BWC_MAX_FRAME];BwcPacket decoded;
 init();command("CAPTURE 0 1234ABCD\n");assert(has("CAPTURE_START:1234ABCD") && receive_callback && tuned_channel==36);
 for(unsigned i=0;i<9;++i){memset(large,0x55,sizeof(large));eapol(large);fake_us+=100;receive_callback(large,1578,NULL);memset(large,0xff,sizeof(large));}
 assert(capture_queued==8 && capture_dropped==1 && packets()==0);
 uint32_t stop_start=millis();command("STOP\n");assert(millis()-stop_start<5000U);
 assert(!capturing && !receive_callback && !promisc_mode && controller.state==BwnIdle && !tx_calls);
 assert(packets()==8 && has("CAPTURE_STATS:1234ABCD:8:1") && has("CAPTURE_DONE:1234ABCD"));
 unsigned sequence=0,done=0;for(unsigned i=0;i<output_count;++i){
  if(!strncmp(output[i],"PCAP1:",6)){assert(!done && bwc_decode(output[i],&decoded));assert(decoded.sequence==sequence++ && decoded.session==0x1234ABCD && decoded.data[36]==0xA5 && decoded.length==1578 && decoded.data[1577]==0x55);}
  if(!strncmp(output[i],"CAPTURE_DONE:",13))done=1;
 }
 puts("adapter: callback copies, bounded queue/drop count, ordered STOP drain and no radio TX PASS");
 init();command("CAPTURE 0 00000001\n");
 memset(raw,0,64);raw[0]=0x80;memcpy(raw+16,target,6);receive_callback(raw,40,NULL);receive_callback(raw,40,NULL);
 assert(capture_accepted==1);eapol(raw);raw[4]^=2;receive_callback(raw,40,NULL);assert(capture_accepted==1);
 fake_us=capture_started_us+BWN_CAPTURE_MS*1000U;eapol(raw);receive_callback(raw,40,NULL);assert(capture_accepted==1);
 loop();assert(controller.state==BwnIdle && packets()==1 && !tx_calls && has("CAPTURE_DONE:00000001"));
 puts("adapter: first target beacon only, wrong BSSID rejection and independent receive deadline PASS");
 init();command("CAPTURE 0 00000002\n");
 for(unsigned i=0;i<BWN_CAPTURE_PACKETS;++i){eapol(raw);assert(receive_callback);receive_callback(raw,40,NULL);loop();}
 assert(controller.state==BwnIdle && packets()==256 && has("CAPTURE_STATS:00000002:256:0") && !tx_calls);
 init();radio_ok=false;command("CAPTURE 0 00000003\n");assert(controller.state==BwnIdle && !capturing && !receive_callback && !tx_calls);
 puts("adapter: 256-packet bound, startup failure cleanup, existing Wi-Fi +1 readiness PASS");
 assisted_capture();
 puts("No real serial port opened; all Wi-Fi and transmission calls mocked.");
}
