/* Actual I2C IRQ driver + actual Arduino adapter, with only hardware boundaries mocked. */
#include "Arduino.h"
#include "wifi_conf.h"
extern "C" {
#include "ameba_soc.h"
}
#include "bw16_native.h"
#include "bw16_pcap.h"
#include "bw16_i2c_link.h"
static BwiServer allocated_server;
static bool fail_allocation;
static size_t available_heap;
size_t xPortGetFreeHeapSize(){return available_heap;}
static unsigned allocation_calls;
void* pvPortMalloc(size_t size){assert(size==sizeof(BwiServer));++allocation_calls;return fail_allocation?NULL:&allocated_server;}
static uint32_t fake_ms, fake_subus;
static unsigned radio_calls, promisc_mode, irq_steps;
static bool wifi_ok, rx_ok, expect_radio, disable_stuck, stop_on_tx;
static unsigned radio_cost_ms;
static uint8_t tuned_channel;
static void (*receive_callback)(unsigned char*,unsigned int,void*);
static IRQ_FUN irq_callback;
static uint8_t hw_rx[256], hw_tx[16];
static unsigned hw_rx_count, hw_rx_head, hw_tx_count, hw_tx_head;
static uint32_t hw_status;
static bool last_read_needed_recovery;
I2C_TypeDef mock_i2c = {};
const I2C_DevTable I2C_DEV_TABLE[1] = {{&mock_i2c}};
MockSerial Serial = {}, Serial1 = {};
uint32_t millis(){return fake_ms;}
uint32_t micros(){return fake_ms*1000U+fake_subus;}
void delay(unsigned value){fake_ms+=value;}
static void wire_byte(){fake_subus+=90;fake_ms+=fake_subus/1000U;fake_subus%=1000U;}
void MockSerial::println(const char*){assert(!"I2C build must not write UART responses");}
int wifi_on(int mode){assert(mode==RTW_MODE_STA);return wifi_ok?1:-1;}
int wifi_scan_networks(rtw_result_t (*)(rtw_scan_handler_result_t*),void*){return 0;}
int wifi_set_channel(uint8_t channel){tuned_channel=channel;return wifi_ok?0:-1;}
int wifi_set_promisc(int mode,void (*cb)(unsigned char*,unsigned int,void*),int len){
 if(mode){assert(mode==RTW_PROMISC_ENABLE_2 && len==1);if(!rx_ok)return -1;}
 receive_callback=cb;promisc_mode=mode;return 0;
}
uint32_t I2C_GetINT(I2C_TypeDef*){
 uint32_t pending=hw_status;
 if(hw_rx_count)pending|=BIT_IC_INTR_STAT_R_RX_FULL;
 if(!hw_tx_count)pending|=BIT_IC_INTR_STAT_R_TX_EMPTY;
 return pending&mock_i2c.IC_INTR_MASK;
}
void I2C_Cmd(I2C_TypeDef*,int enable){
 if(enable){mock_i2c.IC_ENABLE_STATUS=1;return;}
 hw_rx_count=hw_tx_count=hw_rx_head=hw_tx_head=0;
 if(!disable_stuck)mock_i2c.IC_ENABLE_STATUS=0;
}
void I2C_ClearINT(I2C_TypeDef*,uint32_t bit){hw_status&=~bit;}
void I2C_ClearAllINT(I2C_TypeDef*){hw_status=0;}
int I2C_CheckFlagState(I2C_TypeDef*,uint32_t bits){
 return ((bits&BIT_IC_STATUS_TFNF)&&hw_tx_count<sizeof(hw_tx)) ||
        ((bits&BIT_IC_STATUS_TFE)&&!hw_tx_count) || ((bits&BIT_IC_STATUS_RFNE)&&hw_rx_count);
}
uint8_t I2C_ReceiveData(I2C_TypeDef*){assert(hw_rx_count);++irq_steps;--hw_rx_count;return hw_rx[hw_rx_head++];}
void I2C_SlaveSend(I2C_TypeDef*,uint8_t value){assert(hw_tx_count<sizeof(hw_tx));++irq_steps;hw_tx[(hw_tx_head+hw_tx_count++)%sizeof(hw_tx)]=value;}
void I2C_INTConfig(I2C_TypeDef*,uint32_t bits,int enable){if(enable)mock_i2c.IC_INTR_MASK|=bits;else mock_i2c.IC_INTR_MASK&=~bits;}
void I2C_StructInit(I2C_InitTypeDef* value){memset(value,0,sizeof(*value));}
void I2C_Init(I2C_TypeDef*,I2C_InitTypeDef* c){assert(c->I2CAckAddr==0x42 && c->I2CMaster==0 && !c->I2CSlvAckGC);}
void Pinmux_Config(unsigned pin,unsigned func){assert((pin==25 || pin==26)&&func==PINMUX_FUNCTION_I2C);}
void PAD_PullCtrl(unsigned pin,unsigned pull){assert((pin==25 || pin==26)&&pull==GPIO_PuPd_UP);}
void InterruptRegister(IRQ_FUN callback,unsigned irq,uint32_t,unsigned priority){assert(irq==I2C0_IRQ_LP&&priority==7);irq_callback=callback;}
void InterruptEn(unsigned irq,unsigned priority){assert(irq==I2C0_IRQ_LP&&priority==7);}
#include "../../firmware/bw16_controller_i2c/bw16_i2c_slave.cpp"
#include "../../firmware/bw16_controller_i2c/bw16_controller_i2c.ino"
static const uint8_t target[6]={2,3,4,5,6,7};
static uint32_t host_session,host_command,host_ack;
static char output[1000000];
static size_t output_used;
static void fire(uint32_t bits){hw_status|=bits;irq_steps=0;assert(irq_callback);irq_callback(NULL);assert(irq_steps<=128);}
static void write_raw(const uint8_t* data,size_t length,bool stop=true){
 assert(!recovering && mock_i2c.IC_ENABLE_STATUS);
 for(size_t i=0;i<length;++i){assert(!hw_rx_count);hw_rx_head=0;hw_rx[0]=data[i];hw_rx_count=1;wire_byte();fire(0);}
 if(stop)fire(BIT_IC_INTR_STAT_R_STOP_DET);
}
static void read_raw(uint8_t* data,size_t length,bool finish=true){
 assert(!recovering && mock_i2c.IC_ENABLE_STATUS);
 fire(BIT_IC_INTR_STAT_R_RD_REQ);
 for(size_t i=0;i<length;++i){
  if(!hw_tx_count)fire((mock_i2c.IC_INTR_MASK&BIT_IC_INTR_MASK_M_TX_EMPTY)?0:BIT_IC_INTR_STAT_R_RD_REQ);
  assert(hw_tx_count);data[i]=hw_tx[hw_tx_head];hw_tx_head=(hw_tx_head+1U)%sizeof(hw_tx);--hw_tx_count;wire_byte();
 }
 if(finish){fire(BIT_IC_INTR_STAT_R_RX_DONE|BIT_IC_INTR_STAT_R_STOP_DET);last_read_needed_recovery=recovering;if(length==64)assert(!recovering&&mock_i2c.IC_ENABLE_STATUS);bwi_hw_poll(millis());}
}
static void request(uint8_t op,const char* command=NULL,uint32_t ack=0){
 BwiFrame f={};f.op=op;f.session=host_session;f.ack=ack;
 if(command){f.sequence=++host_command;f.length=(uint8_t)strlen(command);memcpy(f.payload,command,f.length);}
 uint8_t raw[64];assert(bwi_encode(raw,&f));write_raw(raw,sizeof(raw));
}
static BwiFrame poll(){
 request(BWI_OP_POLL,NULL,host_ack);uint8_t raw[64];read_raw(raw,64);BwiFrame f;assert(bwi_decode(raw,64,&f));
 assert(f.session==host_session);
 if(f.length && f.sequence!=host_ack){assert(f.sequence==host_ack+1);assert(output_used+f.length<sizeof(output));memcpy(output+output_used,f.payload,f.length);output_used+=f.length;output[output_used]=0;host_ack=f.sequence;}
 return f;
}
static void drain_output(){for(unsigned n=0;n<25000;++n){poll();loop();if(!bwi_server_pending(link_server))return;}assert(!"output failed to drain");}
static void fixture_ap(){controller.ap_count=1;controller.scan_valid=controller.indices_valid=true;memcpy(controller.aps[0].mac,target,6);controller.aps[0].channel=36;strcpy(controller.aps[0].ssid,"Owned fixture");}
static void init(){
 link_server=NULL;fail_allocation=false;allocation_calls=0;available_heap=256000;
 fake_ms=1000;fake_subus=0;radio_calls=radio_cost_ms=0;wifi_ok=rx_ok=true;expect_radio=disable_stuck=stop_on_tx=false;
 hw_status=hw_rx_count=hw_tx_count=hw_rx_head=hw_tx_head=0;mock_i2c={};Serial={};Serial1={};receive_callback=NULL;
 capture_queued=0;capturing=observing=scan_inflight=scan_done=false;scan_count=scan_read=client_count=client_read=0;
 host_session=0x11223344;host_command=host_ack=0;output_used=0;output[0]=0;
 setup();assert(allocation_calls==1);assert(!Serial.baud&&!Serial1.baud);assert(mock_i2c.IC_CON&BIT_CTRL_IC_CON_IC_SLAVE_DISABLE_1);
 request(BWI_OP_OPEN);assert(link_server->open_pending&&!link_server->ready);loop();assert(link_server->ready);fixture_ap();
}
static void command(const char* text){request(BWI_OP_COMMAND,text,host_ack);loop();}
static void eapol(uint8_t* bytes,size_t len){
 memset(bytes,0x55,len);memset(bytes,0,40);bytes[0]=8;bytes[1]=1;memcpy(bytes+4,target,6);bytes[10]=2;bytes[15]=9;
 const uint8_t snap[]={0xaa,0xaa,3,0,0,0,0x88,0x8e,2,3,0,4};memcpy(bytes+24,snap,sizeof(snap));bytes[36]=0xA5;
}
void bwn_radio_deauth(const uint8_t source[6],const uint8_t*,uint16_t){
 assert(expect_radio&&transport_allowed()&&!memcmp(source,target,6));
 if(controller.state==BwnCapture)assert(capturing&&receive_callback&&promisc_mode==RTW_PROMISC_ENABLE_2&&tuned_channel==36);
 ++radio_calls;if(stop_on_tx){stop_on_tx=false;request(BWI_OP_STOP);}fake_ms+=radio_cost_ms;
}
static void framing(){
 init();BwiFrame f={};f.op=BWI_OP_COMMAND;f.session=host_session;f.sequence=1;f.length=5;memcpy(f.payload,"PING\n",5);
 uint8_t raw[65],first[64],second[64];assert(bwi_encode(raw,&f));raw[64]=0;
 write_raw(raw,63);loop();assert(!link_server->command_ack);
 write_raw(raw,65);loop();assert(!link_server->command_ack);
 raw[60]^=1;write_raw(raw,64);loop();assert(!link_server->command_ack);raw[60]^=1;
 write_raw(raw,64,false);read_raw(first,64);assert(link_server->command_ack==1);host_command=1;loop();
 assert(bwi_server_pending(link_server)>0);
 read_raw(first,7);read_raw(second,64);BwiFrame response;assert(bwi_decode(second,64,&response));assert(!memcmp(first,second,7));
 /* Duplicate command after the interrupted read must not execute the command twice. */
 write_raw(raw,64);loop();drain_output();assert(!strcmp(output,"ERR UNKNOWN_COMMAND\n"));
 puts("I2C: partial/oversize/badCRC discard, repeatedSTART, short-read retry and command dedup PASS");

 init();assert(enqueue_line("FIRST RESPONSE SNAPSHOT"));read_raw(first,4,false);
 assert(enqueue_line("SECOND RESPONSE"));
 for(unsigned i=4;i<64;++i){if(!hw_tx_count)fire(0);assert(hw_tx_count);first[i]=hw_tx[hw_tx_head];hw_tx_head=(hw_tx_head+1)%16;--hw_tx_count;}
 fire(BIT_IC_INTR_STAT_R_RX_DONE|BIT_IC_INTR_STAT_R_STOP_DET);bwi_hw_poll(millis());
 assert(bwi_decode(first,64,&response)&&response.length==24&&!memcmp(response.payload,"FIRST RESPONSE SNAPSHOT\n",24));
 read_raw(second,64);assert(!memcmp(first,second,64));
 /* A hung/aborted transfer never creates a polling spin in the IRQ/main loop. */
 uint8_t byte=1;write_raw(&byte,1,false);fake_ms+=BWI_HW_TRANSACTION_MS;disable_stuck=true;bwi_hw_poll(millis());assert(recovering);
 for(unsigned i=0;i<100;++i)bwi_hw_poll(millis());assert(recovering);
 mock_i2c.IC_ENABLE_STATUS=0;disable_stuck=false;bwi_hw_poll(millis());assert(!recovering&&!rx_used);
 puts("I2C: stable per-read snapshot, held unacked response, bounded peripheral recovery PASS");
}
static void all_short_reads(){
 init();assert(enqueue_line("Every short read must retry the same held response"));
 uint8_t prefix[64],whole[64],baseline[64];read_raw(baseline,64);assert(!last_read_needed_recovery);
 for(unsigned length=1;length<64;++length){
  read_raw(prefix,length);assert(last_read_needed_recovery);
  read_raw(whole,64);assert(!last_read_needed_recovery&&!memcmp(prefix,whole,length)&&!memcmp(whole,baseline,64));
 }
 assert(allocation_calls==1);puts("I2C: all63 short-read lengths flush safely; exact64 stays enabled and identical retry PASS");
}
static void hardware_errors(){
 init();uint8_t byte=1,raw[80];write_raw(&byte,1,false);fire(BIT_IC_INTR_STAT_R_RX_OVER|BIT_IC_INTR_STAT_R_STOP_DET);assert(!link_server->command_ack);
 assert(enqueue_line("RETRY AFTER ABORT"));read_raw(raw,3,false);
 /* Hardware abort flushes stale FIFO before its next read-request IRQ. */
 hw_tx_count=hw_tx_head=0;fire(BIT_IC_INTR_STAT_R_TX_ABRT|BIT_IC_INTR_STAT_R_RD_REQ);
 for(unsigned i=0;i<64;++i){if(!hw_tx_count)fire(0);assert(hw_tx_count);raw[i]=hw_tx[hw_tx_head];hw_tx_head=(hw_tx_head+1)%16;--hw_tx_count;}
 fire(BIT_IC_INTR_STAT_R_RX_DONE|BIT_IC_INTR_STAT_R_STOP_DET);bwi_hw_poll(millis());BwiFrame response;assert(bwi_decode(raw,64,&response));
 read_raw(raw,80);assert(!memcmp(raw+64,"\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0",16));
 read_raw(raw,2,false);fire(BIT_IC_INTR_STAT_R_TX_OVER);assert(recovering);bwi_hw_poll(millis());assert(!recovering);
 strcpy(Serial1.input,"DEAUTH_ALL\n");Serial1.used=(unsigned)strlen(Serial1.input);loop();assert(Serial1.position==0&&!radio_calls);
 puts("I2C: RXoverflow discard, TXabort flush/retry, overread padding, TXoverflow recovery, no UART ingress PASS");
}
static void scan_reconnect(){
 init();command("SCAN\n");assert(scan_inflight&&controller.state==BwnScan);
 host_session++;host_command=host_ack=0;request(BWI_OP_OPEN);loop();assert(controller.state==BwnIdle&&!link_server->ready&&scan_holds_ready&&!controller.ap_count);
 rtw_scan_handler_result_t old={};old.ap_details.channel=6;memcpy(old.ap_details.BSSID.octet,target,6);old.ap_details.SSID.len=5;memcpy(old.ap_details.SSID.val,"STALE",5);scan_callback(&old);loop();assert(!controller.ap_count&&!link_server->ready);
 old.scan_complete=RTW_TRUE;scan_callback(&old);loop();assert(link_server->ready&&!scan_holds_ready&&!scan_inflight&&!controller.ap_count&&!bwi_server_pending(link_server));
 command("SCAN\n");assert(controller.state==BwnScan&&scan_inflight);request(BWI_OP_STOP);loop();assert(controller.state==BwnIdle&&!link_server->ready);scan_callback(&old);loop();assert(link_server->ready);
 puts("I2C: reconnect/STOP during async scan holds READY until stale callback drain completes PASS");
}
static void capture_stop(){
 init();command("CAPTURE 0 1234ABCD\n");assert(capturing&&!radio_calls);
 /* Fill near normal-output bound, then keep all8 driver-sized frames queued. */
 static char filler[4050];memset(filler,'X',sizeof(filler)-1);filler[sizeof(filler)-1]=0;assert(enqueue_line(filler));
 uint8_t bytes[1578];for(unsigned i=0;i<9;++i){eapol(bytes,sizeof(bytes));receive_callback(bytes,sizeof(bytes),NULL);memset(bytes,0xee,sizeof(bytes));}
 loop();assert(capture_queued==8&&capture_dropped==1);
 const uint32_t stop_time=millis();request(BWI_OP_STOP);assert(!transport_allowed());loop();
 assert(controller.state==BwnIdle&&!capturing&&!receive_callback&&!radio_calls&&!link_server->fault);
 drain_output();assert((uint32_t)(millis()-stop_time)<15000);printf("I2C STOP full drain simulated wire time: %lu ms\n",(unsigned long)(millis()-stop_time));
 assert(strstr(output,"CAPTURE_STATS:1234ABCD:8:1\n")&&strstr(output,"CAPTURE_DONE:1234ABCD\n"));
 unsigned count=0;const char* cursor=output;
 while((cursor=strstr(cursor,"PCAP1:"))){const char* end=strchr(cursor,'\n');assert(end);char line[BWC_LINE_SIZE];assert((size_t)(end-cursor)<sizeof(line));memcpy(line,cursor,(size_t)(end-cursor));line[end-cursor]=0;BwcPacket packet;assert(bwc_decode(line,&packet));assert(packet.session==0x1234ABCD&&packet.sequence==count++&&packet.length==1578&&packet.data[36]==0xA5&&packet.data[1577]==0x55);cursor=end+1;}
 assert(count==8);puts("I2C: backpressure, callback copy, exact PCAP/CRC and full8-frame STOP drain under15sec PASS");
}
static void safety(){
 init();command("CAPTURE 0 00000001\n");
 for(unsigned i=0;i<8;++i)request(BWI_OP_COMMAND,"PING\n");assert(link_server->command_count==8);
 request(BWI_OP_STOP);assert(!link_server->command_count);loop();assert(controller.state==BwnIdle&&!capturing);drain_output();assert(!strstr(output,"PONG"));
 init();bwn_command(&controller,"CAPTURE_DEAUTH 0 00000002",millis());assert(capturing&&!radio_calls);
 request(BWI_OP_STOP);loop();assert(!radio_calls&&controller.state==BwnIdle);
 init();expect_radio=true;command("CAPTURE_DEAUTH 0 00000003\n");assert(radio_calls==3);for(unsigned i=0;i<100;++i)loop();assert(radio_calls==3);
 init();rx_ok=false;expect_radio=true;command("CAPTURE_DEAUTH 0 00000004\n");assert(!radio_calls&&controller.state==BwnIdle);
 init();expect_radio=stop_on_tx=true;command("CAPTURE_DEAUTH 0 00000009\n");assert(radio_calls==1&&!capturing&&controller.state==BwnIdle);
 init();expect_radio=true;radio_cost_ms=1000;command("CAPTURE_DEAUTH 0 0000000A\n");assert(radio_calls==1);
 init();expect_radio=true;command("DEAUTH_STATION 0\n");assert(radio_calls==3);unsigned previous=radio_calls;fake_ms=last_link_ms+LINK_LOST_MS;loop();assert(radio_calls==previous&&controller.state==BwnIdle);
 init();expect_radio=true;command("DEAUTH_STATION 0\n");previous=radio_calls;fake_ms=controller.started+BWN_OPERATION_MS;request(BWI_OP_POLL,NULL,host_ack);loop();assert(radio_calls==previous&&controller.state==BwnIdle);
 puts("I2C: out-of-band STOP/full queue, per-submission STOP/deadline guard, armed single burst and legacy30s cutoff PASS");
 init();command("CAPTURE 0 00000005\n");fake_ms=last_link_ms+LINK_LOST_MS;uint8_t bytes[40];eapol(bytes,sizeof(bytes));receive_callback(bytes,sizeof(bytes),NULL);assert(capture_accepted==0);loop();assert(!capturing&&controller.state==BwnIdle&&link_server->fault&&!radio_calls);
 host_session++;host_command=host_ack=0;request(BWI_OP_OPEN);loop();assert(link_server->ready&&!link_server->fault&&!controller.ap_count&&!bwi_server_pending(link_server));
 init();command("CAPTURE 0 00000006\n");host_session++;host_command=host_ack=0;request(BWI_OP_OPEN);loop();assert(!capturing&&controller.state==BwnIdle&&!controller.ap_count&&!bwi_server_pending(link_server));
 init();command("CAPTURE 0 00000007\n");fake_ms=controller.started+BWN_CAPTURE_MS;request(BWI_OP_POLL,NULL,host_ack);loop();assert(!capturing&&controller.state==BwnIdle&&!link_server->fault);drain_output();assert(strstr(output,"CAPTURE_DONE:00000007"));
 puts("I2C: link-loss stop, fresh-session reset without stale output, independent capture deadline PASS");
 init();fake_ms=0xffffff00U;last_link_ms=fake_ms;command("CAPTURE 0 00000008\n");fake_ms=last_link_ms+LINK_LOST_MS;loop();assert(link_server->fault&&!capturing);
 init();static char full[BWI_TX_CAPACITY];memset(full,'A',sizeof(full)-1);full[sizeof(full)-1]=0;assert(enqueue_line(full));assert(!enqueue_line("OVERFLOW")&&link_server->fault);loop();assert(controller.state==BwnIdle);
 puts("I2C: millis rollover and explicit output-overflow FAULT stop PASS");
}
static void allocation_failure(){
 init();link_server=NULL;available_heap=sizeof(BwiServer)+63U;unsigned before=allocation_calls;setup();assert(!wifi_ready&&allocation_calls==before);request(BWI_OP_OPEN);
 uint8_t raw[64];read_raw(raw,64);BwiFrame response;assert(bwi_decode(raw,64,&response));
 assert(response.session==host_session&&response.op==BWI_FLAG_FAULT&&!response.length);
 for(unsigned i=0;i<10;++i)loop();assert(!radio_calls&&!receive_callback);
 puts("I2C: low-heap preflight skips the fatal SDK allocator and answers FAULT without radio startup PASS");
}
int main(){framing();all_short_reads();hardware_errors();scan_reconnect();capture_stop();safety();allocation_failure();puts("No real I2C, UART or radio operations performed.");}
