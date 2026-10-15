#include "ui/screen.h"
#include "ui/font.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Unclipped 1.1.0 rasterizer, kept as an independent pixel reference. */
static void reference_text(uint16_t *stripe, unsigned sy, int x, int y,
                           const char *text, unsigned scale, uint16_t color) {
  for(unsigned i=0;text[i];i++) {
    unsigned c=(unsigned char)text[i]; if(c<32||c>126)c='?';
    for(unsigned gx=0;gx<5;gx++) for(unsigned gy=0;gy<7;gy++) if(pi_glyphs[c-32][gx]&(1<<gy))
      for(unsigned dy=0;dy<scale;dy++) for(unsigned dx=0;dx<scale;dx++)
        pi_pixel(stripe,sy,x+i*6*scale+gx*scale+dx,y+gy*scale+dy,color);
  }
}
int main(void) {
  uint16_t a[320*8], b[320*8]; char glyphs[96];
  for (unsigned i=0;i<95;i++)glyphs[i]=i+32; glyphs[95]=0;
  for (unsigned sy=0;sy<170;sy+=8)
    for (int y=-8;y<175;y+=7)
      for (unsigned scale=1;scale<=3;scale++) {
        memset(a,0,sizeof a);memset(b,0,sizeof b);
        reference_text(a,sy,-9,y,glyphs,scale,0xffff);
        pi_text(b,sy,-9,y,glyphs,scale,0xffff);
        assert(!memcmp(a,b,sizeof a));
      }
  uint16_t *screen=calloc(320*176,sizeof(*screen)), *reference=calloc(320*176,sizeof(*reference));
  assert(screen && reference);
  RtlScreen before={.highlight=-1}, after={.highlight=-1};
  unsigned transmitted=0, full=0;
  for (unsigned n=0;n<100;n++) {
    if (n==0) {strcpy(after.title,"RTL433");strcpy(after.foot,"TURN SELECT OK DETAILS");}
    else if (n%9==0) memset(after.body,0,sizeof after.body);
    else snprintf(after.body[n%13],53,"reading %u pressure 35.5 PSI",n);
    if(n%3==0)after.highlight=(n%14)-1;
    if(n%5==0)snprintf(after.status,53,"status %u",n);
    if(n%7==0)snprintf(after.badge,16,"%u",n);
    uint32_t changed=rtl_screen_changes(&before,&after,n!=0);
    for(unsigned sy=0;sy<170;sy+=8) {
      rtl_screen_stripe(&after,reference+sy*320,sy); full++;
      if(changed&(1u<<(sy/8))) {rtl_screen_stripe(&after,screen+sy*320,sy);transmitted++;}
    }
    assert(!memcmp(screen,reference,320*176*sizeof(*screen)));
    before=after;
    assert(!rtl_screen_changes(&before,&after,true));
  }
  assert(transmitted < full/3);
  unsigned live_transmitted=0,live_full=0;
  for(unsigned n=0;n<100;n++) {
    memset(&after.live,0,sizeof after.live);
    RtlDashboard *d=&after.live;
    d->enabled=true;d->receiving=n%17!=0;d->busy=n%3==0;
    d->frequency=433920000;d->rssi=-100+n%60;
    d->devices=7;d->selected=n%7;d->messages=n;d->decode_ms=5;
    strcpy(d->profile,"FSK dev 47.6k");strcpy(d->mode,"SWEEP 4/6");
    d->signal_count=48;
    for(unsigned j=0;j<48;j++)d->signal[j]=-120+(n+j)%90;
    d->cards=n%13?3:0;
    for(unsigned j=0;j<3;j++) {
      snprintf(d->card[j].name,31,"Sensor %u",j);
      snprintf(d->card[j].detail,42,"ID 123456789ABCDE CH 12 -75dBm");
      d->card[j].rssi=-75;d->card[j].hits=12;d->card[j].selected=j==n%3;
    }
    uint32_t changed=rtl_screen_changes(&before,&after,true);
    for(unsigned sy=0;sy<176;sy+=8) {
      rtl_screen_stripe(&after,reference+sy*320,sy);live_full++;
      if(changed&(1u<<(sy/8))){rtl_screen_stripe(&after,screen+sy*320,sy);live_transmitted++;}
    }
    assert(!memcmp(screen,reference,320*176*sizeof(*screen)));
    before=after;assert(!rtl_screen_changes(&before,&after,true));
  }
  assert(live_transmitted<live_full);
  assert(rtl_screen_changes(&before,&after,false)==(1u<<22)-1); /* dialogs force repaint */
  free(screen);free(reference);
  printf("PASS: clipped font matches all glyphs; incremental screen is pixel-identical (%u vs %u stripe transfers), clears and highlights included\n",transmitted,full);
  printf("PASS: dashboard full/partial rendering identical across 100 updates, empty states, cards, signal graph and mode changes (%u/%u stripes)\n",live_transmitted,live_full);
}
