#include "screen.h"
#include "font.h"
#include <stdio.h>
#include <string.h>
static uint32_t rows(unsigned y, unsigned height) {
  uint32_t mask = 0;
  for (unsigned s = y / 8; s <= (y + height - 1) / 8; s++) mask |= 1u << s;
  return mask;
}
uint32_t rtl_screen_changes(const RtlScreen *a, const RtlScreen *b, bool valid) {
  if (!valid) return (1u << 22) - 1;
  if (a->live.enabled != b->live.enabled) return (1u << 22) - 1;
  if (b->live.enabled) {
    uint32_t changed = 0;
    const RtlDashboard *x=&a->live,*y=&b->live;
    if (x->receiving!=y->receiving || x->busy!=y->busy || strcmp(a->title,b->title)) changed|=rows(0,18);
    if (x->frequency!=y->frequency || strcmp(x->profile,y->profile) || strcmp(x->mode,y->mode)) changed|=rows(20,34);
    if (x->rssi!=y->rssi || x->signal_count!=y->signal_count || memcmp(x->signal,y->signal,sizeof x->signal)) changed|=rows(20,34);
    if (x->messages!=y->messages || x->drops!=y->drops || x->decode_ms!=y->decode_ms || x->devices!=y->devices) changed|=rows(56,17);
    if(x->selected!=y->selected || x->devices!=y->devices) changed|=rows(75,8);
    for(unsigned i=0;i<3;i++)
      if(x->cards!=y->cards || memcmp(&x->card[i],&y->card[i],sizeof x->card[i])) changed|=rows(86+i*19,18);
    if(strcmp(a->status,b->status))changed|=rows(146,7);
    if(strcmp(a->foot,b->foot))changed|=rows(160,7);
    return changed;
  }
  uint32_t changed = 0;
  if (strcmp(a->title, b->title) || strcmp(a->badge, b->badge)) changed |= rows(5, 7);
  for (unsigned row = 0; row < NUI_ROWS; row++) {
    if (strcmp(a->body[row], b->body[row]) ||
        ((a->highlight == (int)row) != (b->highlight == (int)row)))
      changed |= rows(23 + row * 9, 9);
  }
  if (strcmp(a->status, b->status)) changed |= rows(146, 7);
  if (strcmp(a->foot, b->foot)) changed |= rows(160, 7);
  return changed;
}
static void rect(uint16_t *s, unsigned sy, int x, int y, int w, int h, uint16_t c) {
  int top=y>(int)sy?y:(int)sy, bottom=y+h<(int)sy+8?y+h:(int)sy+8;
  for(int yy=top;yy<bottom;yy++)for(int xx=x;xx<x+w;xx++)pi_pixel(s,sy,xx,yy,c);
}
static void compact_count(char *out, size_t n, unsigned value) {
  if(value<10000)snprintf(out,n,"%u",value);
  else if(value<1000000)snprintf(out,n,"%uk",value/1000);
  else if(value<1000000000)snprintf(out,n,"%uM",value/1000000);
  else snprintf(out,n,"%uG",value/1000000000);
}
static void dashboard(const RtlScreen *s, uint16_t *stripe, unsigned sy) {
  const RtlDashboard *d=&s->live;
  uint16_t bg=pi_rgb(0x071119), card=pi_rgb(0x0e202b), border=pi_rgb(0x1b3441),
      bright=pi_rgb(0xe5f4f6), muted=pi_rgb(0x819ca9), cyan=pi_rgb(0x47dae8),
      green=pi_rgb(0x75e2ad), amber=pi_rgb(0xffc575);
  rect(stripe,sy,0,0,320,176,bg);
  rect(stripe,sy,0,0,320,18,card);
  for(unsigned i=0;i<3;i++)rect(stripe,sy,7+i*4,11-i*3,2,3+i*3,cyan);
  pi_text(stripe,sy,24,5,"RTL433",1,bright);
  pi_text(stripe,sy,70,5,"/ SIGNAL DESK",1,muted);
  rect(stripe,sy,254,3,60,12,d->receiving?pi_rgb(0x15372f):border);
  pi_text(stripe,sy,261,5,d->receiving?(d->busy?"DECODE":"LIVE RX"):"PAUSED",1,d->receiving?green:muted);
  char text[64];
  snprintf(text,sizeof text,"%lu.%03lu",(unsigned long)(d->frequency/1000000),
           (unsigned long)(d->frequency%1000000/1000));
  pi_text(stripe,sy,8,24,text,2,bright);
  pi_text(stripe,sy,106,31,"MHz",1,muted);
  pi_text(stripe,sy,8,45,d->profile,1,cyan);
  pi_text(stripe,sy,130,45,d->mode,1,muted);
  snprintf(text,sizeof text,"RSSI %d dBm",d->rssi);
  pi_text(stripe,sy,218,22,text,1,muted);
  rect(stripe,sy,218,51,94,1,border);
  for(unsigned i=0;i<d->signal_count && i<48;i++) {
    int level=(d->signal[i]+120)*19/90; if(level<0)level=0;if(level>19)level=19;
    int x=218+(47-(int)d->signal_count+1+(int)i)*2;
    rect(stripe,sy,x,51-level,1,level,pi_rgb(0x16414c));
    pi_pixel(stripe,sy,x,51-level,cyan);
    if(i) {
      int prev=(d->signal[i-1]+120)*19/90;if(prev<0)prev=0;if(prev>19)prev=19;
      rect(stripe,sy,x-1,51-(prev>level?prev:level),1,(prev>level?prev-level:level-prev)+1,cyan);
    }
  }
  const char *labels[]={"DEV","MSG","DEC","LOSS"};
  unsigned vals[]={d->devices,d->messages,d->decode_ms,d->drops};
  for(unsigned i=0;i<4;i++) {
    int x=6+i*78;rect(stripe,sy,x,56,75,16,card);
    char count[12];compact_count(count,sizeof count,vals[i]);
    snprintf(text,sizeof text,"%s %s%s",labels[i],count,i==2?"ms":"");
    pi_text(stripe,sy,x+4,61,text,1,i==3&&d->drops?amber:bright);
  }
  pi_text(stripe,sy,8,76,"DECODED DEVICES",1,muted);
  snprintf(text,sizeof text,"%u / %u",d->devices?d->selected+1:0,d->devices);
  pi_text(stripe,sy,260,76,text,1,muted);
  for(unsigned i=0;i<d->cards && i<3;i++) {
    const RtlDeviceCard *c=&d->card[i];int y=86+i*19;
    rect(stripe,sy,6,y,308,17,c->selected?pi_rgb(0x12353e):card);
    if(c->selected)rect(stripe,sy,6,y,2,17,cyan);
    pi_text(stripe,sy,14,y+1,c->name,1,bright);
    pi_text(stripe,sy,14,y+9,c->detail,1,muted);
    char count[12];compact_count(count,sizeof count,c->hits);
    snprintf(text,sizeof text,"x%s",count);pi_text(stripe,sy,246,y+1,text,1,green);
    int bars=(c->rssi+110)/15;if(bars<0)bars=0;if(bars>4)bars=4;
    for(int b=0;b<4;b++)rect(stripe,sy,292+b*5,y+13-b*3,3,3+b*3,b<bars?cyan:border);
  }
  if(!d->cards) {
    pi_text(stripe,sy,18,96,"LISTENING FOR SENSOR DATA",1,bright);
    pi_text(stripe,sy,18,112,"Decoded devices appear here.",1,muted);
    pi_text(stripe,sy,18,126,"Match frequency; HOLD OK: lock/resume.",1,muted);
  }
  pi_text(stripe,sy,6,146,s->status,1,amber);
  rect(stripe,sy,0,157,320,13,card);
  pi_text(stripe,sy,6,160,s->foot,1,muted);
}
void rtl_screen_stripe(const RtlScreen *s, uint16_t *stripe, unsigned sy) {
  if (s->live.enabled) { dashboard(s,stripe,sy); return; }
  memset(stripe, 0, 320 * 8 * 2);
  uint16_t white = pi_rgb(0xe4f5f2), cyan = pi_rgb(0x5ed9ed),
           dim = pi_rgb(0x69978d), amber = pi_rgb(0xffce69);
  rect(stripe,sy,0,0,320,176,pi_rgb(0x071119));
  rect(stripe,sy,0,0,320,18,pi_rgb(0x0e202b));
  rect(stripe,sy,0,157,320,13,pi_rgb(0x0e202b));
  pi_text(stripe, sy, 5, 5, s->title, 1, cyan);
  pi_text(stripe, sy, 242, 5, s->badge, 1, amber);
  if (sy <= 17 && sy + 8 > 17)
    for (unsigned x = 4; x < 316; x++) pi_pixel(stripe, sy, x, 17, dim);
  for (unsigned row = 0; row < NUI_ROWS; row++) {
    if((int)row==s->highlight) {
      rect(stripe,sy,0,23+row*9,320,9,pi_rgb(0x12353e));
      rect(stripe,sy,0,23+row*9,2,9,cyan);
    }
    pi_text(stripe, sy, 4, 24 + row * 9, s->body[row], 1,
            (int)row == s->highlight ? cyan : white);
  }
  pi_text(stripe, sy, 4, 146, s->status, 1, amber);
  pi_text(stripe, sy, 4, 160, s->foot, 1, dim);
}
