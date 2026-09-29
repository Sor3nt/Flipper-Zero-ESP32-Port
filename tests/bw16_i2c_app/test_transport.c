#include "bw16_i2c.h"
#include "bw16_i2c_hal.h"
#include "bw16_control.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static BwiServer server;
static uint32_t milliseconds;
static bool available, pause_service, never_ready, radio_active;
static unsigned writes, reads, executed, stops, opens, write_fail, read_fail;
static unsigned bad_crc, old_session, bad_sequence, hide_ack;
static bool need_read, rearm_mode;
static uint32_t rearm_until;
static unsigned rearm_nacks;
static uint8_t first_command[BWI_FRAME_SIZE];
static unsigned command_requests, identical_retries;
static char last_command[41];
int64_t esp_timer_get_time(void) { return (int64_t)milliseconds * 1000; }
uint32_t furi_hal_random_get(void) { return 0x98765432U; }
void furi_delay_ms(uint32_t delay) { milliseconds += delay; }
bool furi_hal_bw16_i2c_available(void) { return available; }

static void enqueue(const char* line) { assert(bwi_server_enqueue_line(&server,line,strlen(line))); }
static void service(void) {
    if(bwi_server_take_open(&server)) {
        ++opens;radio_active=false;
        if(!never_ready)bwi_server_set_ready(&server,true);
    }
    if(bwi_server_take_stop(&server)) {
        ++stops;radio_active=false;
        enqueue("CAPTURE_DONE:1234abcd");
        if(!never_ready)bwi_server_set_ready(&server,true);
    }
    if(pause_service)return;
    BwiCommand command;
    while(bwi_server_take_command(&server,&command)) {
        ++executed;memcpy(last_command,command.data,command.length+1);
        if(!strncmp(command.data,"CAPTURE",7)) {
            radio_active=true;enqueue("CAPTURE_START:1234abcd");
        } else if(!strcmp(command.data,"BW16_CAPS\n")) {
            enqueue("BW16_PCAP:1");enqueue("BW16_PCAP_DEAUTH:1");enqueue("BW16_CAPS:1");
        } else enqueue("SCAN_DONE");
    }
}
int furi_hal_bw16_i2c_write(const uint8_t* data,size_t size) {
    assert(size==BWI_FRAME_SIZE);++writes;
    if(rearm_mode && milliseconds<rearm_until) {++rearm_nacks;return ESP_FAIL;}
    milliseconds+=2;
    /* Every write is a complete transaction, before its separate read. */
    need_read=false;
    if(write_fail) { --write_fail;return ESP_FAIL; }
    BwiFrame frame;assert(bwi_decode(data,size,&frame));
    if(frame.op==BWI_OP_COMMAND) {
        if(!command_requests)memcpy(first_command,data,size);
        else if(!memcmp(first_command,data,size))++identical_retries;
        ++command_requests;
    }
    (void)bwi_server_receive(&server,data,size); /* A bus ACK is not a command ACK. */
    service();need_read=true;return ESP_OK;
}
int furi_hal_bw16_i2c_read(uint8_t* data,size_t size) {
    assert(need_read && size==BWI_FRAME_SIZE);need_read=false;++reads;milliseconds+=2;
    if(read_fail) { --read_fail;return ESP_FAIL; }
    assert(bwi_server_response(&server,data));
    if(old_session || bad_sequence || hide_ack) {
        BwiFrame frame;assert(bwi_decode(data,size,&frame));
        if(old_session) {--old_session;frame.session^=1U;}
        if(bad_sequence) {--bad_sequence;if(frame.length)frame.sequence+=1;}
        if(hide_ack) {--hide_ack;frame.ack=0;}
        assert(bwi_encode(data,&frame));
    }
    if(bad_crc) {--bad_crc;data[63]^=0x80;}
    if(rearm_mode)rearm_until=milliseconds+1;
    return ESP_OK;
}
static void setup(Bw16I2c* io) {
    memset(io,0,sizeof(*io));bwi_server_init(&server);milliseconds=1000;
    available=true;pause_service=never_ready=radio_active=need_read=rearm_mode=false;
    rearm_until=rearm_nacks=0;
    writes=reads=executed=stops=opens=write_fail=read_fail=bad_crc=old_session=bad_sequence=hide_ack=0;
    command_requests=identical_retries=0;last_command[0]=0;
}
static void connected(Bw16I2c* io) {
    setup(io);assert(bw16_i2c_open(io));
    assert(io->installed && io->ready && io->session && opens==1 && !executed);
}
static size_t drain(Bw16I2c* io,char* output,size_t capacity) {
    size_t used=0;
    for(unsigned i=0;i<100;++i) {
        int got=bw16_i2c_read(io,output+used,capacity-used-1);assert(got>=0);
        used+=(size_t)got;assert(used+1<capacity);
        size_t buffered;assert(bw16_i2c_buffered(io,&buffered)==ESP_OK);
        if(!got && !buffered)break;
    }
    output[used]=0;return used;
}
static void open_tests(void) {
    Bw16I2c io;setup(&io);available=false;
    assert(!bw16_i2c_open(&io) && !writes && !io.installed);
    setup(&io);never_ready=true;
    assert(!bw16_i2c_open(&io) && !io.installed && io.error==ESP_ERR_TIMEOUT);
    assert(writes<=64 && milliseconds<1000+BW16_I2C_OPEN_MS+40 && opens==1);
    setup(&io);write_fail=2;read_fail=2;assert(bw16_i2c_open(&io));
    assert(opens==1 && io.retries==4 && io.bus_errors==4);
    uint32_t previous=io.session;unsigned calls=writes;
    assert(bw16_i2c_close(&io)==ESP_OK && writes==calls);
    assert(bw16_i2c_open(&io) && io.session!=previous && opens==2);
    assert(!io.tx_sequence && !io.rx_sequence && !io.rx_count);
    puts("I2C OPEN: profile gate, bounded readiness, identical retries, fresh epochs and bus-preserving close PASS");
}
static void reliable_stream(void) {
    Bw16I2c io;connected(&io);read_fail=2;
    assert(bw16_i2c_send(&io,"CAPTURE 0 1234abcd\n",19)==ESP_OK);
    assert(executed==1 && command_requests==3 && identical_retries==2 && radio_active);
    char text[4096];drain(&io,text,sizeof(text));
    assert(!strcmp(text,"CAPTURE_START:1234abcd\n"));
    assert(io.bytes_received==strlen(text) && io.tx_sequence==1);

    connected(&io);hide_ack=2;
    assert(bw16_i2c_send(&io,"CAPTURE 0 1234abcd\n",19)==ESP_OK);
    assert(executed==1 && identical_retries==2 && io.duplicates==2);
    drain(&io,text,sizeof(text));assert(!strcmp(text,"CAPTURE_START:1234abcd\n"));

    connected(&io);char long_line[1001];for(unsigned i=0;i<1000;++i)long_line[i]=(char)('a'+i%26);long_line[1000]=0;
    enqueue(long_line);bad_crc=1;
    assert(bw16_i2c_read(&io,text,sizeof(text))==0 && !io.rx_sequence && io.protocol_errors==1);
    size_t count=drain(&io,text,sizeof(text));
    assert(count==1001 && !memcmp(text,long_line,1000) && text[1000]=='\n');
    assert(io.bytes_received==1001 && io.rx_sequence==26);
    puts("I2C stream: lost ACK retries one command, frozen duplicate chunks once, CRC retry and 1001-byte reconstruction PASS");
}
static void failures_and_stop(void) {
    Bw16I2c io;char text[4096];connected(&io);read_fail=BW16_I2C_RETRIES;
    assert(bw16_i2c_send(&io,"CAPTURE 0 1234abcd\n",19)==ESP_ERR_TIMEOUT);
    assert(executed==1 && stops==1 && !radio_active && io.tx_failed);
    unsigned before=executed;
    assert(bw16_i2c_send(&io,"SCAN\n",5)==ESP_ERR_INVALID_STATE && executed==before);
    assert(bw16_i2c_send(&io,"STOP\n",5)==ESP_OK && stops==2);
    drain(&io,text,sizeof(text));assert(strstr(text,"CAPTURE_DONE:1234abcd"));

    connected(&io);enqueue("fragment");old_session=3;
    assert(bw16_i2c_read(&io,text,sizeof(text))==0);
    assert(bw16_i2c_read(&io,text,sizeof(text))==0);
    assert(bw16_i2c_read(&io,text,sizeof(text))==-1 && !io.bytes_received && io.tx_failed);
    assert(bw16_i2c_send(&io,"STOP\n",5)==ESP_OK);
    uint32_t previous=io.session;bw16_i2c_close(&io);assert(bw16_i2c_open(&io));
    assert(io.session!=previous && !bwi_server_pending(&server));

    connected(&io);enqueue("fragment");bad_sequence=3;
    for(unsigned i=0;i<3;++i)assert(bw16_i2c_read(&io,text,sizeof(text))==(i==2?-1:0));
    assert(!io.rx_sequence && !io.rx_count && io.protocol_errors==3);

    connected(&io);write_fail=3;
    for(unsigned i=0;i<3;++i)assert(bw16_i2c_read(&io,text,sizeof(text))==(i==2?-1:0));
    assert(io.bus_errors==3 && io.tx_failed);

    connected(&io);enqueue("forty byte response requiring buffered space");
    io.rx_count=BW16_I2C_RX_SIZE-1;
    assert(bw16_i2c_send(&io,"SCAN\n",5)!=ESP_OK);
    assert(io.rx_count==BW16_I2C_RX_SIZE-1 && !io.rx_sequence && io.protocol_errors && stops==1);
    puts("I2C faults: uncertain command emergency STOP, STOP despite failure, wrong epoch/sequence, bus failure and RX-capacity refusal PASS");
}
static void slave_rearm(void) {
    Bw16I2c io;char text[128];connected(&io);rearm_mode=true;
    enqueue("data");
    assert(bw16_i2c_read(&io,text,sizeof(text))==5);
    uint32_t before=milliseconds;
    assert(bw16_i2c_read(&io,text,sizeof(text))==0 && !io.tx_failed);
    assert(milliseconds-before==5 && rearm_nacks==1 && io.failures==1);
    assert(bw16_i2c_read(&io,text,sizeof(text))==0 && !io.failures && !io.tx_failed);
    /* STOP immediately after a successful read must also tolerate the rearm gap. */
    before=milliseconds;
    assert(bw16_i2c_send(&io,"STOP\n",5)==ESP_OK && stops==1 && !io.tx_failed);
    assert(milliseconds-before>=5 && rearm_nacks==2);
    for(unsigned i=0;i<8;++i) {
        int got=bw16_i2c_read(&io,text,sizeof(text));
        assert(got>=0 && !io.tx_failed);
    }
    assert(io.bus_errors>=2);
    puts("I2C slave rearm: 1 ms fast-NACK gap survives bounded 5 ms poll/STOP backoff without false link fault PASS");
}

static bool send_control(void* context,const char* command) {
    return bw16_i2c_send(context,command,strlen(command))==ESP_OK;
}
static void control_flow(void) {
    Bw16I2c io;connected(&io);Bw16Control c;bw16_control_init(&c,send_control,&io);
    c.capture_supported=c.capture_deauth_supported=c.aps_valid=c.indices_valid=true;c.ap_count=1;
    strcpy(c.aps[0].mac,"001122334455");
    assert(bw16_control_start_capture_deauth(&c,0,0x1234abcd,100));
    char text[1024];drain(&io,text,sizeof(text));assert(!strcmp(text,"CAPTURE_START:1234abcd\n"));
    text[strlen(text)-1]=0;bw16_control_line(&c,text,200);assert(c.state==Bw16Active);
    assert(bw16_control_stop(&c,1000));assert(stops==1 && !radio_active);
    bw16_control_tick(&c,20999);assert(c.state==Bw16Stopping);
    bw16_control_tick(&c,21000);assert(c.state==Bw16Fault && BW16_STOP_TIMEOUT_MS==20000);
    puts("I2C real control integration: capture command/START, out-of-band STOP and isolated 20-second stop deadline PASS");
}
int main(void) {
    open_tests();reliable_stream();failures_and_stop();slave_rearm();control_flow();
    return 0;
}
