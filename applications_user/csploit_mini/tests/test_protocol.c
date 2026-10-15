#include "net_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned rd(const uint8_t *p){return p[0]*256+p[1];}
static void wr(uint8_t *p,unsigned v){p[0]=v>>8;p[1]=v;}
static unsigned sum(const uint8_t *p,size_t n){unsigned s=0;for(size_t i=0;i<n;i+=2)s+=(unsigned)p[i]<<8|(i+1<n?p[i+1]:0);return s;}
static unsigned checksum(unsigned s){while(s>>16)s=(s&65535)+(s>>16);return (~s)&65535;}
int main(void){uint8_t q[600],a[600],ip[]={192,0,2,9},addresses[4][4];NsQuestion parsed;
    size_t n=ns_query("Lab-Test.example",0x1234,q,sizeof q);assert(n==34);assert(ns_question(q,n,NsDns,&parsed));assert(!strcmp(parsed.name,"Lab-Test.example"));
    size_t z=ns_answer(q,n,NsDns,"lab-test.example.",ip,a,sizeof a);assert(z==n+16&&rd(a)==0x1234&&rd(a+2)==0x8500&&rd(a+4)==1&&rd(a+6)==1&&rd(a+z-6)==4&&!memcmp(a+z-4,ip,4));assert(ns_addresses(a,z,0x1234,addresses,4)==1&&!memcmp(addresses[0],ip,4));assert(!ns_answer(q,n,NsDns,"unrelated.example",ip,a,sizeof a));
    wr(q+n-4,28);z=ns_answer(q,n,NsDns,"lab-test.example",ip,a,sizeof a);assert(z==n&&rd(a+6)==0);wr(q+n-4,1);
    assert(!ns_answer(q,n,NsDns,"lab-test.example",ip,a,12));q[12]=0xc0;q[13]=12;assert(!ns_question(q,n,NsDns,&parsed));
    n=ns_query("lab-test",0x5678,q,sizeof q);q[2]=q[3]=0;z=ns_answer(q,n,NsLlmnr,"LAB-TEST",ip,a,sizeof a);assert(z==n+16&&rd(a+2)==0x8000);q[2]=4;assert(!ns_answer(q,n,NsLlmnr,"lab-test",ip,a,sizeof a));
    /* RFC1002 first-level encoding for LAB-TEST<20>, broadcast NB query. */
    memset(q,0,sizeof q);wr(q,0x4321);wr(q+2,0x0110);wr(q+4,1);q[12]=32;const char nb[16]={'L','A','B','-','T','E','S','T',' ',' ',' ',' ',' ',' ',' ',0x20};for(unsigned i=0;i<16;i++){q[13+i*2]='A'+(((unsigned char)nb[i])>>4);q[14+i*2]='A'+(nb[i]&15);}wr(q+46,32);wr(q+48,1);n=50;z=ns_answer(q,n,NsNbns,"lab-test",ip,a,sizeof a);assert(z==62&&rd(a+2)==0x8580&&rd(a+4)==0&&rd(a+6)==1&&rd(a+54)==6&&!memcmp(a+58,ip,4));
    uint8_t own[6]={2,0,0,0,0,1},target[6]={2,0,0,0,0,2},gateway[6]={2,0,0,0,0,3},tip[4]={192,0,2,10},gip[4]={192,0,2,1};uint8_t frame[600],sm[6],si[4];
    assert(net_arp(frame,true,own,target,own,gip,target,tip)==60);assert(net_arp_read(frame,60,sm,si));assert(!memcmp(sm,own,6)&&!memcmp(si,gip,4));frame[18]=8;assert(!net_arp_read(frame,60,sm,si));
    memset(frame,0,sizeof frame);memcpy(frame,own,6);memcpy(frame+6,target,6);wr(frame+12,0x0800);frame[14]=0x45;frame[22]=64;frame[23]=17;memcpy(frame+26,tip,4);memcpy(frame+30,gip,4);n=ns_query("lab-test",0x1234,frame+42,sizeof frame-42);wr(frame+16,n+28);wr(frame+24,checksum(sum(frame+14,20)));wr(frame+34,40000);wr(frame+36,53);wr(frame+38,n+8);
    z=net_dns_redirect(frame,n+42,own,target,tip,"lab-test",ip,a,sizeof a);assert(z>42);assert(!memcmp(a,target,6)&&!memcmp(a+6,own,6));assert(!memcmp(a+26,gip,4)&&!memcmp(a+30,tip,4));assert(rd(a+36)==40000);assert(checksum(sum(a+14,20))==0);assert(checksum(sum(a+26,8)+17+rd(a+38)+sum(a+34,rd(a+38)))==0);
    assert(!net_dns_redirect(frame,n+42,own,target,tip,"other",ip,a,sizeof a));frame[24]^=1;assert(!net_dns_redirect(frame,n+42,own,target,tip,"lab-test",ip,a,sizeof a));frame[24]^=1;
    uint8_t before[600];memcpy(before,frame,sizeof before);assert(net_relay(frame,n+42,own,target,gateway,tip));assert(!memcmp(frame,gateway,6)&&!memcmp(frame+6,own,6)&&!memcmp(frame+12,before+12,n+30));assert(!net_relay(frame,n+42,own,target,gateway,tip));
    const char auth[]="GET / HTTP/1.1\r\naUtHoRiZaTiOn: SYNTHETIC-DO-NOT-STORE\r\n\r\n";assert(net_http_auth_present((const uint8_t*)auth,strlen(auth)));const char body[]="GET / HTTP/1.1\r\n\r\nAuthorization: body";assert(!net_http_auth_present((const uint8_t*)body,strlen(body)));
    uint32_t rng=0x1337abcd;for(unsigned run=0;run<50000;run++){size_t length=run%600;for(size_t i=0;i<length;i++){rng=rng*1664525+1013904223;q[i]=rng>>24;}for(unsigned proto=0;proto<3;proto++)ns_answer(q,length,proto,"lab-test",ip,a,sizeof a);net_dns_redirect(q,length,own,target,tip,"lab-test",ip,a,sizeof a);net_relay(q,length,own,target,gateway,tip);}
    puts("PASS: DNS/LLMNR/NBNS wire formats, scope filtering, malformed packets, DNS IPv4/UDP checksums, ARP/relay, exposure indicator, 50000 fuzz inputs");return 0;
}
