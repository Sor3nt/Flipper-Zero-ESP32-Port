/* Host-only tests of the production I2C codec and server. No I2C or radio I/O. */
#include "bw16_i2c_link.h"
#include "bw16_pcap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static BwiServer server;
static uint8_t bytes[BWI_TX_CAPACITY + 1U], reconstructed[BWI_TX_CAPACITY + 1U];
static const uint32_t session = 0x12345678U;
static void fix_crc(uint8_t* raw) {
    uint32_t crc = bwi_crc32(raw, 60);
    for(unsigned i=0;i<4;++i){raw[60+i]=(uint8_t)crc;crc>>=8;}
}
static bool request(uint8_t op,uint32_t id,uint32_t seq,uint32_t ack,const char* command) {
    BwiFrame frame={.op=op,.session=id,.sequence=seq,.ack=ack};
    uint8_t raw[BWI_FRAME_SIZE];
    if(command){size_t n=strlen(command);assert(n<=BWI_PAYLOAD_SIZE);frame.length=(uint8_t)n;memcpy(frame.payload,command,n);}
    assert(bwi_encode(raw,&frame));
    return bwi_server_receive(&server,raw,sizeof(raw));
}
static BwiFrame response(void) {
    uint8_t raw[BWI_FRAME_SIZE];BwiFrame frame;
    assert(bwi_server_response(&server,raw) && bwi_decode(raw,sizeof(raw),&frame));
    return frame;
}
static void opened(void) {
    bwi_server_init(&server);
    assert(request(BWI_OP_OPEN,session,0,0,NULL));
    assert(server.open_pending && !server.ready && !server.fault);
    bwi_server_set_ready(&server,true);assert(!server.ready);
    assert(bwi_server_take_open(&server) && !bwi_server_take_open(&server));
    bwi_server_set_ready(&server,true);
    assert(response().op==BWI_FLAG_READY);
}
static size_t drain(uint8_t* out,size_t capacity,bool retries) {
    size_t used=0;uint32_t ack=server.response_acked;
    while(bwi_server_pending(&server)) {
        BwiFrame frame=response();
        assert(frame.session==session && frame.sequence==ack+1 && frame.length && frame.length<=40);
        assert(used+frame.length<=capacity);
        if(retries){
            BwiFrame repeat=response();
            assert(repeat.sequence==frame.sequence && repeat.length==frame.length &&
                   !memcmp(repeat.payload,frame.payload,frame.length));
        }
        memcpy(out+used,frame.payload,frame.length);used+=frame.length;ack=frame.sequence;
        assert(request(BWI_OP_POLL,session,0,ack,NULL));
        if(retries)assert(request(BWI_OP_POLL,session,0,ack,NULL));
    }
    BwiFrame empty=response();assert(!empty.sequence && !empty.length);
    return used;
}
static void codec(void) {
    assert(bwi_crc32((const uint8_t*)"123456789",9)==0xcbf43926U);
    BwiFrame input={.op=BWI_OP_COMMAND,.session=0x12345678,.sequence=1,.length=5}, decoded,before;
    memcpy(input.payload,"SCAN\n",5);
    uint8_t raw[65],mutated[65];assert(bwi_encode(raw,&input));
    assert(raw[0]=='B' && raw[1]=='I' && raw[2]==1 && raw[3]==3);
    assert(raw[4]==0x78 && raw[5]==0x56 && raw[6]==0x34 && raw[7]==0x12);
    assert(raw[8]==1 && raw[16]==5 && !memcmp(raw+20,"SCAN\n",5));
    /* Independent Python zlib.crc32 vector for these exact first sixty bytes. */
    assert(raw[60]==0x97 && raw[61]==0x0b && raw[62]==0xf3 && raw[63]==0x43);
    assert(bwi_decode(raw,64,&decoded) && decoded.session==input.session && decoded.sequence==1);
    memset(&decoded,0xa5,sizeof(decoded));before=decoded;
    for(size_t n=0;n<64;++n){assert(!bwi_decode(raw,n,&decoded));assert(!memcmp(&decoded,&before,sizeof(decoded)));}
    assert(!bwi_decode(raw,65,&decoded));
    for(unsigned i=0;i<64;++i)for(unsigned bit=0;bit<8;++bit) {
        memcpy(mutated,raw,64);mutated[i]^=(uint8_t)(1U<<bit);
        assert(!bwi_decode(mutated,64,&decoded));assert(!memcmp(&decoded,&before,sizeof(decoded)));
    }
    const unsigned positions[]={0,1,2,16,17,18,19,25,59};
    for(unsigned i=0;i<sizeof(positions)/sizeof(*positions);++i) {
        memcpy(mutated,raw,64);mutated[positions[i]]=positions[i]==16?41:0x7f;fix_crc(mutated);
        assert(!bwi_decode(mutated,64,&decoded));
    }
    input.length=41;assert(!bwi_encode(raw,&input));
    input.length=0;input.op=0;input.session=input.sequence=input.ack=0;
    assert(bwi_encode(raw,&input) && bwi_decode(raw,64,&decoded) && !decoded.op && !decoded.session);
    assert(!bwi_encode(NULL,&input) && !bwi_encode(raw,NULL));
    assert(!bwi_decode(NULL,64,&decoded) && !bwi_decode(raw,64,NULL));
    puts("i2c: independent CRC/wire bytes, every truncated length/bit corruption, reserved and padding checks PASS");
}
static void sessions(void) {
    bwi_server_init(&server);BwiFrame frame=response();
    assert(!frame.session && !frame.op && !frame.length && !frame.sequence && !frame.ack);
    assert(!request(BWI_OP_OPEN,0,0,0,NULL));
    assert(!request(BWI_OP_OPEN,session,1,0,NULL));
    assert(!request(BWI_OP_OPEN,session,0,1,NULL));
    assert(!request(BWI_OP_OPEN,session,0,0,"X"));
    assert(request(BWI_OP_OPEN,session,0,0,NULL));
    assert(!request(BWI_OP_COMMAND,session,1,0,"SCAN\n"));
    assert(bwi_server_enqueue_line(&server,"READY",5));
    assert(request(BWI_OP_OPEN,session,0,0,NULL) && server.open_pending);
    assert(bwi_server_pending(&server)==6);
    assert(bwi_server_take_open(&server));bwi_server_set_ready(&server,true);
    assert(request(BWI_OP_COMMAND,session,1,0,"SCAN\n"));
    frame=response();assert(frame.sequence==1 && frame.ack==1 && frame.length==6);
    assert(request(BWI_OP_OPEN,session,0,0,NULL));
    assert(!server.open_pending && server.ready && server.command_count==1 && server.command_ack==1);
    BwiFrame retry=response();assert(retry.sequence==1 && retry.length==6 && !memcmp(retry.payload,frame.payload,6));
    assert(request(BWI_OP_OPEN,session+1,0,0,NULL));
    assert(server.open_pending && !server.ready && !server.command_count && !server.command_ack &&
           !server.tx_used && !server.response_acked && server.response_sequence==1);
    frame=response();assert(frame.session==session+1 && !frame.length && !frame.ack && !frame.op);
    assert(!request(BWI_OP_COMMAND,session,2,0,"SCAN\n"));
    assert(!request(BWI_OP_STOP,session,0,0,NULL));
    assert(!request(BWI_OP_POLL,session,0,1,NULL));
    opened();bwi_server_set_fault(&server,true);
    assert(server.fault && server.stop_pending && !server.ready);
    assert(response().op==BWI_FLAG_FAULT);
    assert(!request(BWI_OP_COMMAND,session,1,0,"SCAN\n"));
    assert(request(BWI_OP_STOP,session,0,0,NULL) && bwi_server_take_stop(&server));
    bwi_server_set_ready(&server,true);assert(!server.ready);
    assert(request(BWI_OP_OPEN,session,0,0,NULL) && server.fault);
    assert(request(BWI_OP_OPEN,session+1,0,0,NULL) && !server.fault && server.open_pending);
    puts("i2c: OPEN idempotence/new-session reset, readiness gating, stale-session rejection and latched fault PASS");
}
static void commands_and_stop(void) {
    opened();BwiCommand command;
    assert(!request(BWI_OP_COMMAND,session,0,0,"SCAN\n"));
    assert(!request(BWI_OP_COMMAND,session,2,0,"SCAN\n"));
    assert(!request(BWI_OP_COMMAND,session,1,0,"SCAN"));
    assert(!request(BWI_OP_COMMAND,session,1,0,"\n"));
    assert(!request(BWI_OP_COMMAND,session,1,0,"SC\nAN\n"));
    assert(!request(BWI_OP_COMMAND,session,1,0,"SCAN\r\n"));
    assert(!request(BWI_OP_COMMAND,session,1,0,"SC\tAN\n"));
    assert(!request(BWI_OP_COMMAND,session,1,0,"\x80\n"));
    assert(!request(99,session,0,0,NULL));
    assert(!request(BWI_OP_POLL,session,1,0,NULL));
    assert(!request(BWI_OP_POLL,session,0,0,"X"));
    assert(!request(BWI_OP_COMMAND,session,1,1,"SCAN\n"));
    for(unsigned i=1;i<=8;++i)assert(request(BWI_OP_COMMAND,session,i,0,"SCAN\n"));
    assert(server.command_count==8 && response().ack==8);
    assert(!request(BWI_OP_COMMAND,session,9,0,"SCAN\n"));
    assert(request(BWI_OP_COMMAND,session,1,0,"CHANGED\n") && server.command_count==8);
    for(unsigned i=0;i<8;++i) {
        assert(bwi_server_take_command(&server,&command));
        assert(command.length==5 && !strcmp(command.data,"SCAN\n"));
    }
    assert(!bwi_server_take_command(&server,&command));
    char longest[41];memset(longest,'A',39);longest[39]='\n';longest[40]=0;
    assert(request(BWI_OP_COMMAND,session,9,0,longest));
    assert(bwi_server_take_command(&server,&command) && command.length==40 && !strcmp(command.data,longest));
    assert(request(BWI_OP_COMMAND,session,9,0,longest) && !server.command_count);
    opened();
    for(unsigned i=1;i<=8;++i)assert(request(BWI_OP_COMMAND,session,i,0,"SCAN\n"));
    assert(bwi_server_enqueue_line(&server,"PCAP buffered",13));size_t pending=bwi_server_pending(&server);
    assert(!request(BWI_OP_STOP,session,1,0,NULL));
    assert(!request(BWI_OP_STOP,session,0,1,NULL));
    assert(!request(BWI_OP_STOP,session,0,0,"X"));
    assert(server.command_count==8 && request(BWI_OP_STOP,session,0,0,NULL));
    assert(server.stop_pending && !server.ready && !server.command_count && server.command_ack==8);
    assert(bwi_server_pending(&server)==pending && !bwi_server_take_command(&server,&command));
    assert(!request(BWI_OP_COMMAND,session,9,0,"SCAN\n"));
    assert(bwi_server_take_stop(&server) && !bwi_server_take_stop(&server));
    bwi_server_set_ready(&server,true);
    assert(request(BWI_OP_COMMAND,session,8,0,"SCAN\n") && !server.command_count);
    assert(request(BWI_OP_COMMAND,session,9,0,"SCAN\n") && server.command_count==1);
    puts("i2c: strict commands, bounded queue, accepted-ACK/duplicate behavior and out-of-band STOP while full PASS");
}
static void fragments_and_capacity(void) {
    opened();assert(bwi_server_enqueue_line(&server,"A",1));
    assert(!request(BWI_OP_POLL,session,0,1,NULL)); /* ACK an unexposed sequence. */
    BwiFrame first=response();assert(first.sequence==1 && first.length==2);
    assert(bwi_server_enqueue_line(&server,"BC",2));
    BwiFrame retry=response();assert(retry.length==2 && !memcmp(retry.payload,"A\n",2));
    assert(!request(BWI_OP_POLL,session,0,2,NULL) && bwi_server_pending(&server)==5);
    assert(request(BWI_OP_COMMAND,session,1,1,"SCAN\n")); /* Piggyback ACK. */
    assert(server.command_ack==1 && bwi_server_pending(&server)==3);
    assert(request(BWI_OP_COMMAND,session,1,0,"SCAN\n")); /* Frozen retry ACK is older. */
    assert(server.command_count==1 && bwi_server_pending(&server)==3);
    assert(drain(reconstructed,sizeof(reconstructed),true)==3 && !memcmp(reconstructed,"BC\n",3));
    opened();memset(bytes,'A',BWI_TX_CAPACITY);
    assert(bwi_server_enqueue_line(&server,(const char*)bytes,BWI_TX_CAPACITY-1U));
    assert(!bwi_server_free(&server) && bwi_server_pending(&server)==BWI_TX_CAPACITY);
    assert(!bwi_server_enqueue_line(&server,"X",1) && !bwi_server_enqueue_line(&server,"",0));
    assert(drain(reconstructed,sizeof(reconstructed),true)==BWI_TX_CAPACITY);
    for(unsigned i=0;i<BWI_TX_CAPACITY-1U;++i)assert(reconstructed[i]=='A');
    assert(reconstructed[BWI_TX_CAPACITY-1U]=='\n' && bwi_server_free(&server)==BWI_TX_CAPACITY);
    assert(!bwi_server_enqueue_line(&server,"A\nB",3));
    assert(!bwi_server_enqueue_line(&server,"A\rB",3));
    assert(!bwi_server_enqueue_line(&server,"A\0B",3));
    assert(!bwi_server_enqueue_line(&server,(const char*)bytes,BWI_TX_CAPACITY));
    assert(!bwi_server_pending(&server));
    /* Move the write/read cursor near the end; the following PCAP line wraps. */
    assert(bwi_server_enqueue_line(&server,(const char*)bytes,BWI_TX_CAPACITY-100U));
    assert(drain(reconstructed,sizeof(reconstructed),false)==BWI_TX_CAPACITY-99U);
    BwcPacket packet={.session=session,.sequence=1,.usec=123456,.length=BWC_MAX_FRAME,.kind=BwcEapol};
    for(unsigned i=0;i<BWC_MAX_FRAME;++i)packet.data[i]=(uint8_t)i;
    char line[BWC_LINE_SIZE];size_t n=bwc_encode(line,sizeof(line),&packet);
    assert(n==3250 && bwi_server_enqueue_line(&server,line,n));
    assert(drain(reconstructed,sizeof(reconstructed),true)==n+1);
    assert(!memcmp(reconstructed,line,n) && reconstructed[n]=='\n');
    puts("i2c: stable fragments, lost/repeated ACKs, atomic full-ring rejection and exact long-PCAP wrap/reassembly PASS");
}
int main(void) {
    codec();sessions();commands_and_stop();fragments_and_capacity();
    return 0;
}
