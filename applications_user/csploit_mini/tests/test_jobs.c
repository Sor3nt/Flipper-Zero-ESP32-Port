#include "net_job.h"
#include <assert.h>
#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
struct FuriThread{pthread_t thread;int32_t(*fn)(void*);void *ctx;};
struct FuriMessageQueue{pthread_mutex_t lock;size_t cap,size,count,head;unsigned char data[];};
uint32_t furi_get_tick(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000+t.tv_nsec/1000000;}
uint32_t furi_kernel_get_tick_frequency(void){return 1000;}void furi_delay_ms(uint32_t n){usleep(n*1000);}
static void *entry(void *p){FuriThread *t=p;t->fn(t->ctx);return NULL;}
FuriThread *furi_thread_alloc_ex(const char *n,uint32_t size,int32_t(*fn)(void*),void *ctx){(void)n;(void)size;FuriThread *t=calloc(1,sizeof(*t));t->fn=fn;t->ctx=ctx;return t;}
void furi_thread_start(FuriThread *t){assert(!pthread_create(&t->thread,NULL,entry,t));}void furi_thread_join(FuriThread *t){pthread_join(t->thread,NULL);}void furi_thread_free(FuriThread *t){free(t);}
FuriMessageQueue *furi_message_queue_alloc(uint32_t n,uint32_t size){FuriMessageQueue *q=calloc(1,sizeof(*q)+n*size);q->cap=n;q->size=size;pthread_mutex_init(&q->lock,NULL);return q;}
FuriStatus furi_message_queue_put(FuriMessageQueue *q,const void *p,uint32_t timeout){(void)timeout;pthread_mutex_lock(&q->lock);bool ok=q->count<q->cap;if(ok){memcpy(q->data+((q->head+q->count)%q->cap)*q->size,p,q->size);q->count++;}pthread_mutex_unlock(&q->lock);return ok?FuriStatusOk:FuriStatusError;}
FuriStatus furi_message_queue_get(FuriMessageQueue *q,void *p,uint32_t timeout){(void)timeout;pthread_mutex_lock(&q->lock);bool ok=q->count!=0;if(ok){memcpy(p,q->data+q->head*q->size,q->size);q->head=(q->head+1)%q->cap;q->count--;}pthread_mutex_unlock(&q->lock);return ok?FuriStatusOk:FuriStatusError;}
void furi_message_queue_free(FuriMessageQueue *q){pthread_mutex_destroy(&q->lock);free(q);}
bool wlan_hal_is_connected(void){return true;}
/* All server binds are forced to loopback; no test sends ARP on a real interface. */
int test_bind(int fd,const struct sockaddr *p,socklen_t n){struct sockaddr_in a=*(const struct sockaddr_in*)p;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);unsigned port=ntohs(a.sin_port);if(port==53)port=15353;if(port==5355)port=15355;if(port==137)port=15137;a.sin_port=htons(port);return bind(fd,(struct sockaddr*)&a,n);}
int test_setsockopt(int fd,int level,int option,const void *p,socklen_t n){if(level==IPPROTO_IP&&(option==IP_ADD_MEMBERSHIP||option==IP_DROP_MEMBERSHIP)){struct ip_mreq m=*(const struct ip_mreq*)p;m.imr_interface.s_addr=htonl(INADDR_LOOPBACK);return setsockopt(fd,level,option,&m,sizeof m);}return setsockopt(fd,level,option,p,n);}
static int status(int fd){fd_set w,e;FD_ZERO(&w);FD_ZERO(&e);FD_SET(fd,&w);FD_SET(fd,&e);struct timeval t={0};int r=select(fd+1,NULL,&w,&e,&t);if(r<=0)return r;int err=0;socklen_t n=sizeof err;if(getsockopt(fd,SOL_SOCKET,SO_ERROR,&err,&n))return -1;return err?-err:1;}
static uint8_t own[]={2,0,0,0,0,1},target[]={2,0,0,0,0,2},gw[]={2,0,0,0,0,3};static uint8_t tip[]={127,0,0,2},gip[]={127,0,0,254},oip[]={127,0,0,1};
static FuriMessageQueue *packets;static atomic_uint opened,closed,poisoned,restored,forwarded;
static bool info(CsNetInfo *i){memset(i,0,sizeof(*i));memcpy(i->local_ip,oip,4);memcpy(i->gateway_ip,gip,4);i->mask[0]=i->mask[1]=i->mask[2]=255;memcpy(i->local_mac,own,6);return true;}
static bool packet_open(const CsScope *s){(void)s;assert(!packets);packets=furi_message_queue_alloc(16,sizeof(CsFrame));atomic_fetch_add(&opened,1);return true;}
static bool packet_scope(const CsScope *s){if(s->relay){CsFrame f={.length=54};memcpy(f.bytes,own,6);memcpy(f.bytes+6,target,6);f.bytes[12]=8;f.bytes[14]=0x45;f.bytes[17]=40;f.bytes[22]=64;f.bytes[23]=6;memcpy(f.bytes+26,tip,4);memcpy(f.bytes+30,gip,4);assert(furi_message_queue_put(packets,&f,0)==FuriStatusOk);}return true;}
static bool packet_send(const uint8_t *p,size_t n){assert(n>=14);if(p[12]==8&&p[13]==6){if(p[21]==1){CsFrame r={.length=60};bool is_target=!memcmp(p+38,tip,4);const uint8_t *mac=is_target?target:gw;net_arp(r.bytes,true,mac,own,mac,p+38,own,oip);assert(furi_message_queue_put(packets,&r,0)==FuriStatusOk);}else if(!memcmp(p+22,own,6))atomic_fetch_add(&poisoned,1);else atomic_fetch_add(&restored,1);}else if(p[12]==8&&p[13]==0)atomic_fetch_add(&forwarded,1);return true;}
static bool packet_poll(CsFrame *f,uint32_t wait){if(furi_message_queue_get(packets,f,0)==FuriStatusOk)return true;furi_delay_ms(wait);return false;}
static void packet_stats(CsNetStats *s){memset(s,0,sizeof(*s));}
static void packet_close(void){assert(packets);furi_message_queue_free(packets);packets=NULL;atomic_fetch_add(&closed,1);}
const CsNetApi *cs_backend(void){static const CsNetApi a={CS_NET_ABI,sizeof a,info,packet_open,packet_scope,packet_poll,packet_send,packet_stats,packet_close,status};return &a;}
static void finish(NetJob *j,char *log,size_t cap){net_cancel(j);uint32_t start=furi_get_tick();size_t used=0;for(;;){NetEvent e;while(net_poll(j,&e)){size_t n=strlen(e.text);if(used+n+2<cap){memcpy(log+used,e.text,n);used+=n;log[used++]='\n';log[used]=0;}}if(atomic_load(&j->done))break;assert(furi_get_tick()-start<5000);furi_delay_ms(10);}net_free(j);}
static int client(unsigned port,int type){int fd=socket(AF_INET,type,0);assert(fd>=0);struct timeval timeout={2,0};setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof timeout);struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};assert(!connect(fd,(struct sockaddr*)&a,sizeof a));return fd;}
int main(void){NetConfig c={.first_port=18081,.last_port=18081,.port=18080,.timeout_ms=200};strcpy(c.target,"127.0.0.1");strcpy(c.name,"lab-test");strcpy(c.reply,"192.0.2.9");char log[16384]={0};
    for(unsigned round=0;round<2;round++){NetJob *j=net_start(JobDnsServer,&c);assert(j);furi_delay_ms(60);int fd=client(15353,SOCK_DGRAM);uint8_t q[512],a[512],ips[4][4];size_t n=ns_query("lab-test",123,q,sizeof q);assert(send(fd,q,n,0)==(int)n);int z=recv(fd,a,sizeof a,0);assert(z>0&&ns_addresses(a,z,123,ips,4)==1&&ips[0][0]==192&&ips[0][3]==9);close(fd);finish(j,log,sizeof log);assert(strstr(log,"1 queries, 1 answers"));}
    for(unsigned mode=0;mode<2;mode++){NetJob *j=net_start(mode?JobNbns:JobLlmnr,&c);furi_delay_ms(60);int fd=client(mode?15137:15355,SOCK_DGRAM);uint8_t q[512],a[512];size_t n;if(!mode){n=ns_query("lab-test",125,q,sizeof q);q[2]=0;}else{memset(q,0,sizeof q);q[1]=125;q[3]=0x10;q[5]=1;q[12]=32;const unsigned char name[16]={'L','A','B','-','T','E','S','T',' ',' ',' ',' ',' ',' ',' ',0x20};for(unsigned i=0;i<16;i++){q[13+2*i]='A'+(name[i]>>4);q[14+2*i]='A'+(name[i]&15);}q[47]=32;q[49]=1;n=50;}assert(send(fd,q,n,0)==(int)n);int z=recv(fd,a,sizeof a,0);assert(z>0&&a[1]==125&&a[7]==1);close(fd);finish(j,log,sizeof log);assert(strstr(log,"1 queries, 1 answers"));}
    NetJob *j=net_start(JobExposure,&c);furi_delay_ms(60);int fd=client(18080,SOCK_STREAM);const char request[]="GET / HTTP/1.0\r\nAuthorization: SYNTHETIC-SECRET-NEVER-LOG\r\n\r\n";send(fd,request,sizeof request-1,0);char response[512];assert(recv(fd,response,sizeof response,0)>0);close(fd);finish(j,log,sizeof log);assert(strstr(log,"auth-header=present; discarded")&&!strstr(log,"SYNTHETIC-SECRET-NEVER-LOG"));
    int server=socket(AF_INET,SOCK_STREAM,0);struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(18081),.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};assert(!bind(server,(struct sockaddr*)&addr,sizeof addr));assert(!listen(server,2));j=net_start(JobScan,&c);furi_delay_ms(100);finish(j,log,sizeof log);assert(strstr(log,"[OPEN] TCP 18081"));close(server);
    strcpy(c.target,"127.0.0.2");j=net_start(JobArp,&c);furi_delay_ms(150);finish(j,log,sizeof log);assert(atomic_load(&opened)==1&&atomic_load(&closed)==1);assert(atomic_load(&poisoned)>=2&&atomic_load(&restored)==6&&atomic_load(&forwarded)>=1);assert(strstr(log,"[RESTORE] 3/3 reply pairs sent"));
    puts("PASS: actual worker loopback DNS/LLMNR/NBNS replies, repeated start/stop, TCP scan, secret-discard exposure indicator, simulated ARP resolution/forward/restore");return 0;}
