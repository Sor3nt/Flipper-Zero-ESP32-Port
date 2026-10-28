/* Host-only checks of the production raw 802.11 classifier and PCAP codec. */
#include "bw16_pcap.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static const uint8_t target[6]={2,1,2,3,4,5}, other[6]={2,6,7,8,9,10};
static const uint8_t snap[8]={0xaa,0xaa,3,0,0,0,0x88,0x8e};
static size_t data(uint8_t* f,unsigned ds,bool qos,bool order,size_t body) {
    memset(f,0,BWC_MAX_FRAME+1);
    f[0]=qos?0x88:0x08;f[1]=(uint8_t)(ds|(order?0x80:0));
    memcpy(f+4,other,6);memcpy(f+10,other,6);memcpy(f+16,other,6);
    memcpy(f+(ds==1?4:ds==2?10:16),target,6);
    size_t h=24+(qos?2+(order?4:0):0);
    memcpy(f+h,snap,8);f[h+8]=2;f[h+9]=3;
    f[h+10]=(uint8_t)(body>>8);f[h+11]=(uint8_t)body;
    for(size_t i=0;i<body;++i) f[h+12+i]=(uint8_t)i;
    return h+12+body;
}
static void classifier(void) {
    uint8_t f[BWC_MAX_FRAME+1];
    for(unsigned ds=0;ds<3;++ds)for(unsigned qos=0;qos<2;++qos)for(unsigned order=0;order<2;++order) {
        size_t n=data(f,ds,qos!=0,order!=0,19);
        for(size_t cut=0;cut<n;++cut)assert(bwc_classify(f,cut,target)==BwcIgnore);
        assert(bwc_classify(f,n,target)==BwcEapol);
        assert(bwc_classify(f,n,other)==BwcIgnore);
        for(unsigned cf=0;cf<4;++cf) {
            f[0]=(uint8_t)((qos?0x88:0x08)|(cf<<4));
            assert(bwc_classify(f,n,target)==BwcEapol);
        }
        f[0]|=0x40;assert(bwc_classify(f,n,target)==BwcIgnore);
    }
    size_t n=data(f,1,false,false,95);
    assert(bwc_classify(NULL,n,target)==BwcIgnore && bwc_classify(f,n,NULL)==BwcIgnore);
    f[0]|=1;assert(bwc_classify(f,n,target)==BwcIgnore);f[0]&=0xfc;
    f[1]|=0x40;assert(bwc_classify(f,n,target)==BwcIgnore);f[1]&=0xbf;
    f[1]|=4;assert(bwc_classify(f,n,target)==BwcIgnore);f[1]&=0xfb;
    f[22]=1;assert(bwc_classify(f,n,target)==BwcIgnore);f[22]=0;
    f[1]|=8;assert(bwc_classify(f,n,target)==BwcEapol);
    f[1]=3;assert(bwc_classify(f,n,target)==BwcIgnore);
    f[0]=0xd4;assert(bwc_classify(f,n,target)==BwcIgnore);
    n=data(f,1,true,true,95);f[24]=0x80;assert(bwc_classify(f,n,target)==BwcIgnore);
    n=data(f,0,true,false,95);f[25]=1;assert(bwc_classify(f,n,target)==BwcIgnore);
    n=data(f,2,true,false,95);f[25]=1;assert(bwc_classify(f,n,target)==BwcEapol);
    n=data(f,2,false,false,0);
    for(unsigned type=0;type<=8;++type){f[33]=(uint8_t)type;assert(bwc_classify(f,n,target)==BwcEapol);}
    f[32]=0;assert(bwc_classify(f,n,target)==BwcIgnore);f[32]=2;
    f[34]=f[35]=255;assert(bwc_classify(f,n,target)==BwcIgnore);
    n=data(f,2,false,false,95);
    for(unsigned i=0;i<8;++i){f[24+i]^=1;assert(bwc_classify(f,n,target)==BwcIgnore);f[24+i]^=1;}
    f[n]=0x11;f[n+1]=0x22;assert(bwc_classify(f,n+2,target)==BwcEapol);
    n=data(f,1,false,false,BWC_MAX_FRAME-36);
    assert(n==BWC_MAX_FRAME && bwc_classify(f,n,target)==BwcEapol);
    assert(bwc_classify(f,n+1,target)==BwcIgnore);
    memset(f,0,sizeof(f));f[0]=0x80;memcpy(f+16,target,6);
    for(size_t cut=0;cut<36;++cut)assert(bwc_classify(f,cut,target)==BwcIgnore);
    assert(bwc_classify(f,36,target)==BwcBeacon && bwc_classify(f,BWC_MAX_FRAME,target)==BwcBeacon);
    assert(bwc_classify(f,36,other)==BwcIgnore);
    f[1]=1;assert(bwc_classify(f,36,target)==BwcIgnore);
    f[1]=0;f[0]=0x50;assert(bwc_classify(f,36,target)==BwcIgnore);
    puts("pcap: classifier boundary, DS/QoS/HT, beacon and EAPOL checks PASS");
}
static void codec(void) {
    BwcPacket p={.session=0x12345678,.sequence=0x9abcdef0,.usec=0x01020304,
        .length=4,.kind=BwcEapol,.data={0,127,128,255}}, got, before;
    /* Independent CRC from Python zlib.crc32(struct.pack('<IIIHB',...) + payload). */
    const char* golden="PCAP1:12345678:9ABCDEF0:01020304:0004:02:F820FD83:007F80FF";
    char line[BWC_LINE_SIZE+4],bad[BWC_LINE_SIZE+4];
    size_t n=bwc_encode(line,sizeof(line),&p);
    assert(n==strlen(golden) && !strcmp(line,golden) && bwc_decode(line,&got));
    assert(got.session==p.session && got.sequence==p.sequence && got.usec==p.usec &&
        got.length==p.length && got.kind==p.kind && !memcmp(got.data,p.data,p.length));
    memset(&got,0xa5,sizeof(got));before=got;
    for(size_t cut=0;cut<n;++cut) {
        memcpy(bad,golden,cut);bad[cut]=0;assert(!bwc_decode(bad,&got));
        assert(!memcmp(&got,&before,sizeof(got)));
    }
    for(size_t i=0;i<n;++i) {
        strcpy(bad,golden);bad[i]=bad[i]=='0'?'1':'0';assert(!bwc_decode(bad,&got));
        assert(!memcmp(&got,&before,sizeof(got)));
    }
    const char* suffix[]={"\n","\r"," debug","00"};
    for(unsigned i=0;i<4;++i){strcpy(bad,golden);strcat(bad,suffix[i]);assert(!bwc_decode(bad,&got));}
    strcpy(bad,"debug ");strcat(bad,golden);assert(!bwc_decode(bad,&got));
    strcpy(bad,golden);bad[50]='g';assert(!bwc_decode(bad,&got));
    strcpy(bad,golden);memcpy(bad+33,"FFFF",4);assert(!bwc_decode(bad,&got));
    memset(bad,'A',sizeof(bad));bad[sizeof(bad)-1]=0;assert(!bwc_decode(bad,&got));
    assert(!bwc_decode(NULL,&got) && !bwc_decode(golden,NULL));
    strcpy(bad,golden);
    for(size_t i=6;i<n;++i)if(bad[i]>='A' && bad[i]<='F')bad[i]+='a'-'A';
    assert(bwc_decode(bad,&got));
    for(size_t cap=0;cap<=n;++cap)assert(!bwc_encode(line,cap,&p));
    assert(bwc_encode(line,n+1,&p)==n);
    assert(!bwc_encode(NULL,sizeof(line),&p) && !bwc_encode(line,sizeof(line),NULL));
    p.length=0;assert(!bwc_encode(line,sizeof(line),&p));
    p.length=BWC_MAX_FRAME+1;assert(!bwc_encode(line,sizeof(line),&p));
    p.length=BWC_MAX_FRAME;p.kind=BwcIgnore;assert(!bwc_encode(line,sizeof(line),&p));
    p.kind=BwcBeacon;p.session=UINT32_MAX;p.sequence=0;p.usec=UINT32_MAX;
    for(size_t i=0;i<p.length;++i)p.data[i]=(uint8_t)i;
    n=bwc_encode(line,BWC_LINE_SIZE,&p);
    assert(n==3250 && !line[n] && bwc_decode(line,&got));
    assert(got.session==UINT32_MAX && !got.sequence && got.usec==UINT32_MAX &&
        got.length==BWC_MAX_FRAME && got.kind==BwcBeacon && !memcmp(got.data,p.data,p.length));
    puts("pcap: independent CRC, corruption, exact framing and maximum packet roundtrip PASS");
}
static void headers(void) {
    const uint8_t global[24]={0xd4,0xc3,0xb2,0xa1,2,0,4,0,0,0,0,0,0,0,0,0,0x40,6,0,0,105,0,0,0};
    const uint8_t record[16]={0x78,0x56,0x34,0x12,0x3f,0x42,0x0f,0,0x40,6,0,0,0x40,6,0,0};
    uint8_t out[24],before[24];
    bwc_pcap_header(out);assert(!memcmp(out,global,24));bwc_pcap_header(NULL);
    assert(bwc_pcap_record_header(out,0x12345678,999999,BWC_MAX_FRAME));
    assert(!memcmp(out,record,16));memcpy(before,out,24);
    assert(!bwc_pcap_record_header(out,0,1000000,1));
    assert(!bwc_pcap_record_header(out,0,0,0));
    assert(!bwc_pcap_record_header(out,0,0,BWC_MAX_FRAME+1));
    assert(!bwc_pcap_record_header(out,0,0,UINT32_MAX));
    assert(!bwc_pcap_record_header(NULL,0,0,1));
    assert(!memcmp(before,out,24));
    assert(bwc_pcap_record_header(out,UINT32_MAX,0,1));
    puts("pcap: independent global/record header bytes and timestamp/length bounds PASS");
}
int main(void){classifier();codec();headers();return 0;}
