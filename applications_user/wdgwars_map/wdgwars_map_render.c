#include "wdgwars_map_render.h"
#include "wdgwars_map_basemap.h"
#include "wdgwars_map_world.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Native map rasterizer. The small column-major font follows the existing
 * Spectrum renderer's 5x7 bitmap convention; missing ASCII glyphs are defined
 * here. UTF-8 codepoints outside ASCII consume one visible '?' each. */
static const uint8_t glyphs[95][5] = {
 {0,0,0,0,0},{0,0,0x5f,0,0},{0,7,0,7,0},{0x14,0x7f,0x14,0x7f,0x14},
 {0x24,0x2a,0x7f,0x2a,0x12},{0x23,0x13,8,0x64,0x62},{0x36,0x49,0x55,0x22,0x50},{0,5,3,0,0},
 {0,0x1c,0x22,0x41,0},{0,0x41,0x22,0x1c,0},{0x14,8,0x3e,8,0x14},{8,8,0x3e,8,8},
 {0,0x50,0x30,0,0},{8,8,8,8,8},{0,0x60,0x60,0,0},{0x20,0x10,8,4,2},
 {0x3e,0x51,0x49,0x45,0x3e},{0,0x42,0x7f,0x40,0},{0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4b,0x31},
 {0x18,0x14,0x12,0x7f,0x10},{0x27,0x45,0x45,0x45,0x39},{0x3c,0x4a,0x49,0x49,0x30},{1,0x71,9,5,3},
 {0x36,0x49,0x49,0x49,0x36},{6,0x49,0x49,0x29,0x1e},{0,0x36,0x36,0,0},{0,0x56,0x36,0,0},
 {8,0x14,0x22,0x41,0},{0x14,0x14,0x14,0x14,0x14},{0,0x41,0x22,0x14,8},{2,1,0x51,9,6},
 {0x32,0x49,0x79,0x41,0x3e},{0x7e,0x11,0x11,0x11,0x7e},{0x7f,0x49,0x49,0x49,0x36},{0x3e,0x41,0x41,0x41,0x22},
 {0x7f,0x41,0x41,0x22,0x1c},{0x7f,0x49,0x49,0x49,0x41},{0x7f,9,9,9,1},{0x3e,0x41,0x49,0x49,0x7a},
 {0x7f,8,8,8,0x7f},{0,0x41,0x7f,0x41,0},{0x20,0x40,0x41,0x3f,1},{0x7f,8,0x14,0x22,0x41},
 {0x7f,0x40,0x40,0x40,0x40},{0x7f,2,0x0c,2,0x7f},{0x7f,4,8,0x10,0x7f},{0x3e,0x41,0x41,0x41,0x3e},
 {0x7f,9,9,9,6},{0x3e,0x41,0x51,0x21,0x5e},{0x7f,9,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
 {1,1,0x7f,1,1},{0x3f,0x40,0x40,0x40,0x3f},{0x1f,0x20,0x40,0x20,0x1f},{0x3f,0x40,0x38,0x40,0x3f},
 {0x63,0x14,8,0x14,0x63},{7,8,0x70,8,7},{0x61,0x51,0x49,0x45,0x43},{0,0x7f,0x41,0x41,0},
 {2,4,8,0x10,0x20},{0,0x41,0x41,0x7f,0},{4,2,1,2,4},{0x40,0x40,0x40,0x40,0x40},
 {0,1,2,4,0},{0x20,0x54,0x54,0x54,0x78},{0x7f,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
 {0x38,0x44,0x44,0x48,0x7f},{0x38,0x54,0x54,0x54,0x18},{8,0x7e,9,1,2},{0x0c,0x52,0x52,0x52,0x3e},
 {0x7f,8,4,4,0x78},{0,0x44,0x7d,0x40,0},{0x20,0x40,0x44,0x3d,0},{0x7f,0x10,0x28,0x44,0},
 {0,0x41,0x7f,0x40,0},{0x7c,4,0x18,4,0x78},{0x7c,8,4,4,0x78},{0x38,0x44,0x44,0x44,0x38},
 {0x7c,0x14,0x14,0x14,8},{8,0x14,0x14,0x18,0x7c},{0x7c,8,4,4,8},{0x48,0x54,0x54,0x54,0x20},
 {4,0x3f,0x44,0x40,0x20},{0x3c,0x40,0x40,0x20,0x7c},{0x1c,0x20,0x40,0x20,0x1c},{0x3c,0x40,0x30,0x40,0x3c},
 {0x44,0x28,0x10,0x28,0x44},{0x0c,0x50,0x50,0x50,0x3c},{0x44,0x64,0x54,0x4c,0x44},{0,8,0x36,0x41,0},
 {0,0,0x7f,0,0},{0,0x41,0x36,8,0},{8,4,8,0x10,8}
};
#define W WDG_MAP_DISPLAY_WIDTH
#define H WDG_MAP_DISPLAY_HEIGHT
#define PI 3.14159265358979323846
#define MAX_LAT 85.0511287798066
#define POLY_MAX 256
#define INK 0x203447U
#define MUTED 0x617586U
#define BLUE 0x2475EDU
#define PAPER 0xFDFEFEU

typedef struct {float x,y;} XY;
typedef struct {double cx,cy,world;bool globe;} Projection;
static double clampd(double x,double a,double b) {return x<a?a:x>b?b:x;}
static int mini(int a,int b) {return a<b?a:b;}
static int maxi(int a,int b) {return a>b?a:b;}
static double wrap_lon(double lon) {return lon-360.0*floor((lon+180.0)/360.0);}
static double mercator(double lat) {
    lat=clampd(lat,-MAX_LAT,MAX_LAT);
    return (1.0-log(tan(PI/4.0+lat*PI/360.0))/PI)/2.0;
}
static bool projection(const WdgMapRenderState* s,Projection* p) {
    if(!s || !isfinite(s->center_lat) || !isfinite(s->center_lon) || !isfinite(s->zoom)) return false;
    p->globe=s->zoom<0.5;
    p->world=p->globe?272.0:256.0*pow(2.0,clampd(s->zoom,1.0,20.0));
    p->cx=(wrap_lon(s->center_lon)+180.0)/360.0;
    p->cy=mercator(s->center_lat); return true;
}
static bool project(const Projection* p,double lat,double lon,XY* q) {
    if(!isfinite(lat)||!isfinite(lon)||lat < -90.0||lat > 90.0) return false;
    if(p->globe) {
        if(lon < -180.0||lon > 180.0)lon=wrap_lon(lon);
        q->x=(float)(24.0+(lon+180.0)*272.0/360.0);
        q->y=(float)(17.0+(90.0-lat)*136.0/180.0);return true;
    }
    double dx=(wrap_lon(lon)+180.0)/360.0-p->cx;
    dx-=floor(dx+0.5);
    q->x=(float)(W/2.0+dx*p->world);
    q->y=(float)(H/2.0+(mercator(lat)-p->cy)*p->world); return true;
}
bool wdgwars_map_geo_to_screen(const WdgMapRenderState* s,double lat,double lon,float* x,float* y) {
    Projection p; XY q; if(!x||!y||!projection(s,&p)||!project(&p,lat,lon,&q)) return false;
    *x=q.x; *y=q.y; return true;
}
bool wdgwars_map_screen_to_geo(const WdgMapRenderState* s,float x,float y,double* lat,double* lon) {
    Projection p; if(!lat||!lon||!isfinite(x)||!isfinite(y)||!projection(s,&p)) return false;
    if(p.globe) {
        *lat=clampd(90.0-(y-17.0)*180.0/136.0,-90.0,90.0);
        *lon=clampd((x-24.0)*360.0/272.0-180.0,-180.0,180.0);return true;
    }
    double ny=clampd(p.cy+(y-H/2.0)/p.world,0.0,1.0);
    *lat=(2.0*atan(pow(2.71828182845904523536,PI*(1.0-2.0*ny)))-PI/2.0)*180.0/PI;
    *lon=wrap_lon((p.cx+(x-W/2.0)/p.world)*360.0-180.0); return true;
}
bool wdgwars_map_viewport_bounds(const WdgMapRenderState* s,WdgMapBounds* b) {
    Projection p;if(!b||!projection(s,&p))return false;
    if(p.globe){*b=(WdgMapBounds){-90.0,-180.0,90.0,180.0};return true;}
    return b && wdgwars_map_screen_to_geo(s,0,0,&b->north,&b->west) &&
        wdgwars_map_screen_to_geo(s,W,H,&b->south,&b->east);
}
uint16_t wdgwars_map_rgb565(uint32_t rgb) {
    uint16_t c=(uint16_t)(((rgb>>8)&0xf800)|((rgb>>5)&0x07e0)|((rgb>>3)&0x001f));
    return (uint16_t)((c<<8)|(c>>8));
}
static void dot(uint16_t* p,int x,int y,uint32_t rgb,unsigned alpha) {
    if(x<0||x>=W||y<0||y>=H) return;
    if(alpha>=255) {p[y*W+x]=wdgwars_map_rgb565(rgb);return;}
    uint16_t c=p[y*W+x]; c=(uint16_t)((c<<8)|(c>>8));
    unsigned r=((c>>11)&31)*255/31,g=((c>>5)&63)*255/63,b=(c&31)*255/31;
    r=(r*(255-alpha)+((rgb>>16)&255)*alpha+127)/255;
    g=(g*(255-alpha)+((rgb>>8)&255)*alpha+127)/255;
    b=(b*(255-alpha)+(rgb&255)*alpha+127)/255;
    p[y*W+x]=wdgwars_map_rgb565((r<<16)|(g<<8)|b);
}
static void rect(uint16_t* p,int x,int y,int w,int h,uint32_t c,unsigned a) {
    int x1=mini(W,x+w),y1=mini(H,y+h); x=maxi(0,x);y=maxi(0,y);
    for(int j=y;j<y1;j++) for(int i=x;i<x1;i++) dot(p,i,j,c,a);
}
static void roundrect(uint16_t* p,int x,int y,int w,int h,int r,uint32_t c,unsigned a) {
    if(w<=0||h<=0) { return; }
    r=mini(r,mini(w,h)/2);
    int x1=mini(W,x+w),y1=mini(H,y+h);
    for(int j=maxi(0,y);j<y1;j++) for(int i=maxi(0,x);i<x1;i++) {
        int dx=i<x+r?x+r-1-i:i>=x+w-r?i-(x+w-r):0;
        int dy=j<y+r?y+r-1-j:j>=y+h-r?j-(y+h-r):0;
        if(dx*dx+dy*dy<=r*r) dot(p,i,j,c,a);
    }
}
static void circle(uint16_t* p,int x,int y,int r,uint32_t c,unsigned a) {
    r=mini(r,500); if(r<0) return;
    for(int j=maxi(0,y-r);j<mini(H,y+r+1);j++) for(int i=maxi(0,x-r);i<mini(W,x+r+1);i++)
        if((i-x)*(i-x)+(j-y)*(j-y)<=r*r) dot(p,i,j,c,a);
}
/* Liang-Barsky clips floating projected coordinates before integer conversion;
 * lines thousands of km outside the viewport never create an unbounded loop. */
static bool clip(XY* a,XY* b) {
    float dx=b->x-a->x,dy=b->y-a->y,lo=0,hi=1;
    float pp[4]={-dx,dx,-dy,dy},qq[4]={a->x,(W-1)-a->x,a->y,(H-1)-a->y};
    for(unsigned i=0;i<4;i++) {
        if(pp[i]==0) {if(qq[i]<0) return false; continue;}
        float v=qq[i]/pp[i];
        if(pp[i]<0) {if(v>hi)return false;if(v>lo)lo=v;}
        else {if(v<lo)return false;if(v<hi)hi=v;}
    }
    XY start=*a; a->x=start.x+lo*dx;a->y=start.y+lo*dy;b->x=start.x+hi*dx;b->y=start.y+hi*dy;
    return true;
}
static void line(uint16_t* p,XY a,XY b,int width,uint32_t c,unsigned alpha) {
    if(!clip(&a,&b)) return;
    int x=(int)(a.x+0.5f),y=(int)(a.y+0.5f),x1=(int)(b.x+0.5f),y1=(int)(b.y+0.5f);
    int dx=x1>x?x1-x:x-x1,dy=y1>y?y-y1:y1-y,sx=x<x1?1:-1,sy=y<y1?1:-1,err=dx+dy;
    for(;;) {
        if(width<=1)dot(p,x,y,c,alpha);else circle(p,x,y,width/2,c,alpha);
        if(x==x1&&y==y1)break;
        int e2=2*err;if(e2>=dy){err+=dy;x+=sx;}if(e2<=dx){err+=dx;y+=sy;}
    }
}
static void polygon(uint16_t* p,const XY* q,size_t n,uint32_t fill,unsigned alpha) {
    if(n<3||n>POLY_MAX)return;
    float low=q[0].y,high=q[0].y;
    for(size_t i=1;i<n;i++){if(q[i].y<low)low=q[i].y;if(q[i].y>high)high=q[i].y;}
    if(high<0||low>=H)return;
    int y0=(int)clampd(low,0,H-1),y1=(int)clampd(high,0,H-1);
    for(int y=y0;y<=y1;y++) {
        float crossings[POLY_MAX],yy=y+0.5f;size_t count=0;
        for(size_t i=0,j=n-1;i<n;j=i++) {
            if((q[i].y<=yy&&q[j].y>yy)||(q[j].y<=yy&&q[i].y>yy)) {
                float x=q[i].x+(yy-q[i].y)*(q[j].x-q[i].x)/(q[j].y-q[i].y);
                size_t k=count++;while(k&&crossings[k-1]>x){crossings[k]=crossings[k-1];--k;}crossings[k]=x;
            }
        }
        for(size_t i=0;i+1<count;i+=2) {
            int a=(int)clampd(floor(crossings[i]+0.5),0,W),b=(int)clampd(floor(crossings[i+1]+0.5),0,W);
            rect(p,a,y,b-a,1,fill,alpha);
        }
    }
}
static unsigned char next_char(const char** text) {
    const unsigned char* s=(const unsigned char*)*text;unsigned char c=*s++;
    if(c>=128){while((*s&0xc0)==0x80)++s;c='?';}
    *text=(const char*)s;return c<32||c>126?'?':c;
}
static int text_width(const char* s,int scale,int limit) {
    int width=0; if(!s)return 0;while(*s&&width+5*scale<=limit){next_char(&s);width+=6*scale;}return width?width-scale:0;
}
static void text(uint16_t* p,int x,int y,const char* s,int scale,uint32_t color,int limit) {
    if(!s||scale<1) { return; }
    int start=x;
    while(*s&&x-start+5*scale<=limit){unsigned char c=next_char(&s);const uint8_t* g=glyphs[c-32];
        for(int a=0;a<5;a++)for(int b=0;b<7;b++)if(g[a]&(1U<<b))rect(p,x+a*scale,y+b*scale,scale,scale,color,255);
        x+=6*scale;
    }
}
static void label(uint16_t* p,int x,int y,const char* s,uint32_t c,int maxwidth) {
    int width=text_width(s,1,maxwidth-15)+15;
    roundrect(p,x+1,y+2,width,15,5,0x213743,24);
    roundrect(p,x,y,width,15,5,PAPER,235);circle(p,x+6,y+7,2,c,255);text(p,x+12,y+4,s,1,INK,width-14);
}
static const WdgMapGang* gang(const WdgMapSnapshot* map,uint32_t id) {
    if(!map||!id) { return NULL; }
    size_t n=mini(map->gang_count,WDG_MAP_MAX_GANGS);
    for(size_t i=0;i<n;i++) { if(map->gangs[i].id==id)return &map->gangs[i]; }
    return NULL;
}
static uint32_t team_color(const WdgMapSnapshot* map,uint32_t id,uint32_t fallback) {
    const WdgMapGang* g=gang(map,id);return g?g->color_rgb:fallback;
}
static size_t projected_points(const Projection* p,const WdgMapPoint* points,size_t n,XY* q) {
    if(n>POLY_MAX)return 0;
    for(size_t i=0;i<n;i++)if(!project(p,points[i].lat_e6/1e6,points[i].lon_e6/1e6,&q[i]))return 0;
    return n;
}
static void path(uint16_t* pixels,const Projection* p,const WdgMapPoint* points,size_t n,
    bool closed,uint32_t fill,unsigned alpha,uint32_t stroke,int width,unsigned stroke_alpha) {
    if(n<2)return;
    XY q[POLY_MAX];size_t projected=projected_points(p,points,n,q);
    if(closed&&projected&&alpha)polygon(pixels,q,n,fill,alpha);
    size_t segments=closed?n:n-1;
    for(size_t i=0;i<segments;i++) {
        XY a,b;size_t j=(i+1)%n;
        if(projected){a=q[i];b=q[j];}
        else if(!project(p,points[i].lat_e6/1e6,points[i].lon_e6/1e6,&a)||
            !project(p,points[j].lat_e6/1e6,points[j].lon_e6/1e6,&b))continue;
        line(pixels,a,b,width,stroke,stroke_alpha);
    }
}
/* Natural Earth provides the actual coarse geography in the offline world
 * overview. Source longitudes retain both dateline edges; regional views draw
 * periodic copies rather than connecting +180 to -180 across the map. */
static void world_layer(uint16_t* pixels,const Projection* p) {
    const uint32_t water=0xC1DEEB,land=0xF6F3E9,coast=0x89AFBD;
    if(p->globe)rect(pixels,24,17,272,136,water,255);
    else rect(pixels,0,0,W,H,water,255);
    for(size_t i=0;i<wdg_world_ring_count;i++) {
        const WdgWorldRing* ring=&wdg_world_rings[i];
        if(ring->point_count<3||ring->point_count>POLY_MAX||
           (size_t)ring->first_point+ring->point_count>wdg_world_point_count)continue;
        const WdgMapPoint* points=wdg_world_points+ring->first_point;
        int first=p->globe?0:-1,last=p->globe?0:1;
        for(int copy=first;copy<=last;copy++) {
            XY q[POLY_MAX];float xmin=1e20f,xmax=-1e20f,ymin=1e20f,ymax=-1e20f;
            for(unsigned j=0;j<ring->point_count;j++) {
                double lat=points[j].lat_e6/1e6,lon=points[j].lon_e6/1e6;
                if(p->globe)project(p,lat,lon,&q[j]);
                else {
                    q[j].x=(float)(W/2.0+((lon+180.0)/360.0-p->cx+copy)*p->world);
                    q[j].y=(float)(H/2.0+(mercator(lat)-p->cy)*p->world);
                }
                if(q[j].x<xmin) { xmin=q[j].x; }
                if(q[j].x>xmax)xmax=q[j].x;
                if(q[j].y<ymin) { ymin=q[j].y; }
                if(q[j].y>ymax)ymax=q[j].y;
            }
            if(xmax<0||xmin>=W||ymax<0||ymin>=H)continue;
            polygon(pixels,q,ring->point_count,ring->hole?water:land,255);
            for(unsigned j=0;j<ring->point_count;j++)line(pixels,q[j],q[(j+1)%ring->point_count],1,coast,190);
        }
    }
}
static void world_labels(uint16_t* pixels,const Projection* p) {
    static const struct {float lat,lon;const char* name;} labels[]={
        {43,-106,"N AMERICA"},{-17,-60,"S AMERICA"},{54,22,"EUROPE"},
        {7,20,"AFRICA"},{39,100,"ASIA"},{-26,134,"AUSTRALIA"},{-79,15,"ANTARCTICA"}};
    for(unsigned i=0;i<sizeof(labels)/sizeof(labels[0]);i++) {
        XY q;if(!project(p,labels[i].lat,labels[i].lon,&q)||q.x<15||q.x>W-15||q.y<23||q.y>H-23)continue;
        int width=text_width(labels[i].name,1,80);
        text(pixels,(int)q.x-width/2,(int)q.y-3,labels[i].name,1,0x65828C,width);
    }
}
static void basemap_layer(uint16_t* pixels,const Projection* p,const WdgMapBasemap* base) {
    if(!base)return;
    unsigned count=mini(base->feature_count,256),point_count=mini(base->point_count,4096);
    for(unsigned pass=0;pass<3;pass++)for(unsigned i=0;i<count;i++) {
        const WdgBaseFeature* f=&base->features[i];
        if(f->point_count<2||(unsigned)f->first_point+f->point_count>point_count)continue;
        bool area=f->kind==WdgBaseWater||f->kind==WdgBasePark||f->kind==WdgBaseBuilding;
        if((pass==0)!=area)continue;
        uint32_t color=0xFFFFFF,stroke=0xD4D8CF;int width=1;unsigned alpha=255;
        if(f->kind==WdgBaseWater){color=0xACD4EB;stroke=0x9DCBE5;}
        else if(f->kind==WdgBasePark){color=0xC6DDB1;stroke=0xBED5A8;}
        else if(f->kind==WdgBaseBuilding){color=0xD9DCD4;stroke=0xCBD1C9;}
        else if(f->kind==WdgBaseRoadMajor){stroke=pass==1?0xC7AB7C:0xFFEBC0;width=pass==1?5:3;}
        else if(f->kind==WdgBaseRoadMinor){stroke=pass==1?0xB9C4C8:0xFFFFFF;width=pass==1?3:1;}
        else if(f->kind==WdgBaseRailway){stroke=0x9DA8A6;width=1;alpha=160;}
        else continue;
        path(pixels,p,base->points+f->first_point,f->point_count,f->closed&&area,
            color,f->closed&&area?255:0,stroke,width,alpha);
    }
}
static void road_labels(uint16_t* pixels,const Projection* p,const WdgMapBasemap* base) {
    if(!base) { return; }
    unsigned rendered=0,count=mini(base->feature_count,256),point_count=mini(base->point_count,4096);
    int previous_y=-30;
    for(unsigned i=0;i<count&&rendered<3;i++) {
        const WdgBaseFeature* f=&base->features[i];
        if(!f->name[0]||f->point_count<2||(unsigned)f->first_point+f->point_count>point_count||
           (f->kind!=WdgBaseRoadMajor&&f->kind!=WdgBaseWater&&f->kind!=WdgBasePark&&
            !(f->kind==WdgBaseRoadMinor&&p->world>=8388608.0)))continue;
        const WdgMapPoint* point=&base->points[f->first_point+f->point_count/2];XY q;
        if(!project(p,point->lat_e6/1e6,point->lon_e6/1e6,&q)||q.x<22||q.x>255||q.y<43||q.y>112)continue;
        int y=(int)q.y;if(y-previous_y<14&&previous_y-y<14)continue;
        int width=text_width(f->name,1,108),x=(int)q.x-width/2;
        roundrect(pixels,x-3,y-2,width+6,11,3,PAPER,170);text(pixels,x,y,f->name,1,MUTED,108);
        previous_y=y;++rendered;
    }
}
static void territories(uint16_t* pixels,const Projection* p,const WdgMapSnapshot* map,const WdgMapRenderState* s) {
    if(!map) { return; }
    unsigned points=mini(map->point_count,WDG_MAP_MAX_POINTS);
    unsigned hulls=mini(map->hull_count,WDG_MAP_MAX_HULLS),cells=mini(map->cell_count,WDG_MAP_MAX_CELLS);
    bool known=map->cell_anchor!=WdgMapCellAnchorUnknown && map->grid_lat_e6>0&&map->grid_lon_e6>0;
    if(!known||s->zoom<14.0||cells==0)for(unsigned i=0;i<hulls;i++) {
        const WdgMapHull* h=&map->hulls[i];if(h->point_count<3||(unsigned)h->first_point+h->point_count>points)continue;
        bool selected=h->gang_id&&h->gang_id==s->selected_gang_id;
        uint32_t c=team_color(map,h->gang_id,h->color_rgb);
        path(pixels,p,map->points+h->first_point,h->point_count,true,c,selected?75:45,c,selected?2:1,210);
    }
    for(unsigned i=0;i<cells;i++) {
        const WdgMapCell* c=&map->cells[i];uint32_t color=team_color(map,c->gang_id,c->color_rgb);XY q;
        if(!project(p,c->lat_e6/1e6,c->lon_e6/1e6,&q))continue;
        if(known&&(s->zoom>=14.0||hulls==0)) {
            double lat=c->lat_e6/1e6,lon=c->lon_e6/1e6,dlat=map->grid_lat_e6/1e6,dlon=map->grid_lon_e6/1e6;
            if(map->cell_anchor==WdgMapCellAnchorCenter){lat-=dlat/2;lon-=dlon/2;}
            XY corners[4];if(!project(p,lat,lon,&corners[0])||!project(p,lat+dlat,lon,&corners[1])||
              !project(p,lat+dlat,lon+dlon,&corners[2])||!project(p,lat,lon+dlon,&corners[3]))continue;
            bool selected=c->gang_id&&c->gang_id==s->selected_gang_id;
            polygon(pixels,corners,4,color,selected?82:51);
            for(unsigned j=0;j<4;j++)line(pixels,corners[j],corners[(j+1)%4],selected?2:1,color,selected?235:170);
        } else if(!known&&q.x>-5&&q.x<W+5&&q.y>-5&&q.y<H+5) {
            circle(pixels,(int)q.x,(int)q.y,3,PAPER,230);circle(pixels,(int)q.x,(int)q.y,2,color,240);
        }
    }
}
typedef struct {int x,y,w,h;} LabelBox;
static bool label_free(LabelBox* boxes,unsigned count,LabelBox box) {
    for(unsigned i=0;i<count;i++) {
        if(box.x<boxes[i].x+boxes[i].w+5&&box.x+box.w+5>boxes[i].x&&
           box.y<boxes[i].y+boxes[i].h+4&&box.y+box.h+4>boxes[i].y) {
            return false;
        }
    }
    return true;
}
static bool position_label(LabelBox* boxes,unsigned count,LabelBox* box,int top,int bottom) {
    if(label_free(boxes,count,*box))return true;
    static const int offsets[]={20,-20,36,-36};
    for(unsigned i=0;i<sizeof(offsets)/sizeof(offsets[0]);i++) {
        LabelBox candidate=*box;candidate.y+=offsets[i];
        if(candidate.y>=top&&candidate.y<=bottom&&label_free(boxes,count,candidate)){*box=candidate;return true;}
    }
    return false;
}
static void label_leader(uint16_t* pixels,float x,float y,LabelBox box,uint32_t color) {
    if(x>=box.x&&x<=box.x+box.w&&y>=box.y&&y<=box.y+box.h)return;
    XY edge={(float)clampd(x,box.x+3,box.x+box.w-3),(float)clampd(y,box.y+3,box.y+box.h-3)};
    line(pixels,(XY){x,y},edge,1,color,185);
}
static void territory_labels(uint16_t* pixels,const Projection* p,const WdgMapSnapshot* map,const WdgMapRenderState* state) {
    if(!map) { return; }
    LabelBox boxes[6];unsigned used=0,gangs=mini(map->gang_count,WDG_MAP_MAX_GANGS);
    XY gps={0};bool protect_gps=state->has_fix&&project(p,state->gps_lat,state->gps_lon,&gps)&&
        gps.x>=0&&gps.x<W&&gps.y>=16&&gps.y<H-16;
    if(protect_gps)boxes[used++]=(LabelBox){(int)gps.x-9,(int)gps.y-9,18,18};
    unsigned maximum=protect_gps?6:5;
    int label_top=p->globe?23:42,label_bottom=p->globe?135:105;
    for(unsigned g=0;g<gangs&&used<maximum;g++) {
        const WdgMapGang* team=&map->gangs[g];if(!team->id||!team->name[0])continue;
        float x=0,y=0;unsigned n=0;
        for(unsigned i=0;i<(unsigned)mini(map->cell_count,WDG_MAP_MAX_CELLS);i++) {
            const WdgMapCell* c=&map->cells[i];if(c->gang_id!=team->id)continue;XY q;
            double lat=c->lat_e6/1e6,lon=c->lon_e6/1e6;
            if(map->cell_anchor==WdgMapCellAnchorSouthWest){lat+=map->grid_lat_e6/2e6;lon+=map->grid_lon_e6/2e6;}
            if(project(p,lat,lon,&q)&&q.x>15&&q.x<280&&q.y>label_top&&q.y<label_bottom+10){x+=q.x;y+=q.y;++n;}
        }
        if(!n)for(unsigned i=0;i<(unsigned)mini(map->hull_count,WDG_MAP_MAX_HULLS);i++) {
            const WdgMapHull* h=&map->hulls[i];if(h->gang_id!=team->id||(unsigned)h->first_point+h->point_count>(unsigned)mini(map->point_count,WDG_MAP_MAX_POINTS))continue;
            for(unsigned j=0;j<h->point_count;j++){const WdgMapPoint* point=&map->points[h->first_point+j];XY q;
                if(project(p,point->lat_e6/1e6,point->lon_e6/1e6,&q)&&q.x>15&&q.x<280&&q.y>label_top&&q.y<label_bottom+10){x+=q.x;y+=q.y;++n;}}
        }
        if(!n) { continue; }
        int width=text_width(team->name,1,107)+15;
        LabelBox box={(int)(x/n)-width/2,(int)(y/n)-7,width,15};
        if(box.x<8) { box.x=8; }
        if(box.x+box.w>282)box.x=282-box.w;
        if(box.y<label_top) { box.y=label_top; }
        if(box.y>label_bottom)box.y=label_bottom;
        if(!position_label(boxes,used,&box,label_top,label_bottom))continue;
        label_leader(pixels,x/n,y/n,box,team->color_rgb);
        label(pixels,box.x,box.y,team->name,team->color_rgb,width);boxes[used++]=box;
    }
    /* Expanded hull names are authoritative even when no gang ID is provided. */
    for(unsigned i=0;i<(unsigned)mini(map->hull_count,WDG_MAP_MAX_HULLS)&&used<maximum;i++) {
        const WdgMapHull* h=&map->hulls[i];
        if(gang(map,h->gang_id)||!h->name[0]||h->point_count<3||
           (unsigned)h->first_point+h->point_count>(unsigned)mini(map->point_count,WDG_MAP_MAX_POINTS))continue;
        float x=0,y=0;unsigned n=0;
        for(unsigned j=0;j<h->point_count;j++) {
            const WdgMapPoint* point=&map->points[h->first_point+j];XY q;
            if(project(p,point->lat_e6/1e6,point->lon_e6/1e6,&q)&&q.x>15&&q.x<280&&q.y>label_top&&q.y<label_bottom+10){x+=q.x;y+=q.y;++n;}
        }
        if(!n) { continue; }
        int width=text_width(h->name,1,107)+15;
        LabelBox box={(int)(x/n)-width/2,(int)(y/n)-7,width,15};
        if(box.x<8) { box.x=8; }
        if(box.x+box.w>282)box.x=282-box.w;
        if(box.y<label_top) { box.y=label_top; }
        if(box.y>label_bottom)box.y=label_bottom;
        if(!position_label(boxes,used,&box,label_top,label_bottom))continue;
        label_leader(pixels,x/n,y/n,box,h->color_rgb);
        label(pixels,box.x,box.y,h->name,h->color_rgb,width);boxes[used++]=box;
    }
}
static bool saved_bounds(const WdgMapBounds* b) {
    return b&&isfinite(b->south)&&isfinite(b->north)&&isfinite(b->west)&&isfinite(b->east)&&
        b->south>=-90&&b->north<=90&&b->south<b->north&&b->west>=-180&&b->west<=180&&
        b->east>=-180&&b->east<=180&&b->west!=b->east;
}
static bool inside_bounds(const WdgMapBounds* b,const WdgMapRenderState* s) {
    if(!saved_bounds(b))return false;
    double lon=wrap_lon(s->center_lon);
    return s->center_lat>=b->south&&s->center_lat<=b->north&&
        (b->west<=b->east?(lon>=b->west&&lon<=b->east):(lon>=b->west||lon<=b->east));
}
static void coverage_edge(uint16_t* pixels,XY a,XY b) {
    if(!clip(&a,&b))return;
    float dx=b.x-a.x,dy=b.y-a.y;float ax=dx<0?-dx:dx,ay=dy<0?-dy:dy;
    int steps=(int)(ax>ay?ax:ay)+1;
    for(int i=0;i<=steps;i++)if((i/4)%2==0)dot(pixels,(int)(a.x+dx*i/steps),(int)(a.y+dy*i/steps),0x607582,160);
}
static void coverage(uint16_t* pixels,const Projection* p,const WdgMapSnapshot* map,const WdgMapBasemap* base,const WdgMapRenderState* s) {
    const WdgMapBounds* b=map&&saved_bounds(&map->bounds)?&map->bounds:base&&saved_bounds(&base->bounds)?&base->bounds:NULL;
    if(!b)return;
    XY q[4];if(project(p,b->south,b->west,&q[0])&&project(p,b->north,b->west,&q[1])&&
        project(p,b->north,b->east,&q[2])&&project(p,b->south,b->east,&q[3]))
        for(unsigned i=0;i<4;i++)coverage_edge(pixels,q[i],q[(i+1)%4]);
    bool no_territories=map&&saved_bounds(&map->bounds)&&!inside_bounds(&map->bounds,s);
    bool no_streets=base&&saved_bounds(&base->bounds)&&!inside_bounds(&base->bounds,s);
    const char* notice=no_territories?(no_streets?"Outside saved map area":"Outside saved territories"):no_streets?"Outside saved streets":NULL;
    if(notice){int width=text_width(notice,1,253)+14;roundrect(pixels,11,41,width,16,5,PAPER,247);text(pixels,18,46,notice,1,0x8C672B,width-12);}
}
static void gps_marker(uint16_t* pixels,const Projection* p,const WdgMapRenderState* s) {
    if(!s->has_fix) { return; }
    XY q;if(!project(p,s->gps_lat,s->gps_lon,&q)||q.x<-20||q.x>W+20||q.y<-20||q.y>H+20)return;
    int x=(int)(q.x+0.5f),y=(int)(q.y+0.5f);
    if(p->globe){circle(pixels,x,y,6,PAPER,255);circle(pixels,x,y,4,BLUE,255);return;}
    if(s->accuracy_known&&isfinite(s->accuracy_m)&&s->accuracy_m>0){
        double meters=40075016.686*cosf((float)(clampd(s->gps_lat,-MAX_LAT,MAX_LAT)*PI/180.0))/p->world;
        int r=(int)clampd(s->accuracy_m/meters,8,100);circle(pixels,x,y,r,BLUE,35);
    }
    circle(pixels,x,y,10,BLUE,22);circle(pixels,x+1,y+2,7,0x1A405C,45);
    circle(pixels,x,y,7,PAPER,255);circle(pixels,x,y,5,BLUE,255);circle(pixels,x-1,y-1,1,0x79B1FF,255);
}
static void compass_scale(uint16_t* pixels,const Projection* p,const WdgMapRenderState* s) {
    circle(pixels,297,53,12,0x243C50,24);circle(pixels,296,51,12,PAPER,245);
    XY north[3]={{296,43},{292,53},{300,53}},south[3]={{296,60},{292,53},{300,53}};
    polygon(pixels,north,3,0xDF5760,255);polygon(pixels,south,3,0xAAB7C2,255);
    double mpp=40075016.686*cosf((float)(clampd(s->center_lat,-MAX_LAT,MAX_LAT)*PI/180.0))/p->world;
    double target=mpp*57.0,meters=1.0;
    while(meters*10<=target)meters*=10;
    if(meters*5<=target)meters*=5;else if(meters*2<=target)meters*=2;
    int length=(int)(meters/mpp+0.5);if(length<5||length>70)return;
    int x=W-14-length,y=121;
    roundrect(pixels,x-4,y-13,length+8,20,4,PAPER,225);
    rect(pixels,x,y,length,2,INK,255);rect(pixels,x,y-3,1,4,INK,255);rect(pixels,x+length-1,y-3,1,4,INK,255);
    char caption[24];if(meters>=1000)snprintf(caption,sizeof(caption),"%.0f km",meters/1000);
    else snprintf(caption,sizeof(caption),"%.0f m",meters);
    text(pixels,x+length/2-text_width(caption,1,65)/2,y-10,caption,1,INK,65);
}
static void spinner(uint16_t* pixels,int cx,int cy,uint32_t now) {
    static const int8_t dx[8]={0,4,6,4,0,-4,-6,-4},dy[8]={-6,-4,0,4,6,4,0,-4};
    unsigned phase=(now/100)%8;
    for(unsigned i=0;i<8;i++)circle(pixels,cx+dx[i],cy+dy[i],1,BLUE,55+((i+8-phase)%8)*28);
}
/* A downloaded bounding box alone is not visible street geometry. Recognize
 * clipped segments and filled areas enclosing the map center, including areas
 * whose boundary is entirely outside the viewport. No synthetic roads exist. */
static bool visible_path(const Projection* p,const WdgMapPoint* points,size_t count,bool closed) {
    if(count<2)return false;
    XY first,previous;if(!project(p,points[0].lat_e6/1e6,points[0].lon_e6/1e6,&first))return false;
    previous=first;bool center_inside=false;
    size_t segments=closed?count:count-1;
    for(size_t i=0;i<segments;i++) {
        XY next;size_t j=(i+1)%count;
        if(j==0)next=first;
        else if(!project(p,points[j].lat_e6/1e6,points[j].lon_e6/1e6,&next))return false;
        XY a=previous,b=next;
        if(clip(&a,&b)&&((a.y>=36&&a.y<=130)||(b.y>=36&&b.y<=130)||
            (a.y<36&&b.y>130)||(b.y<36&&a.y>130)))return true;
        if(closed&&((previous.y>H/2)!=(next.y>H/2))&&
            W/2<(next.x-previous.x)*(H/2-previous.y)/(next.y-previous.y)+previous.x)center_inside=!center_inside;
        previous=next;
    }
    return center_inside;
}
static bool visible_basemap(const Projection* p,const WdgMapBasemap* base) {
    if(!base)return false;
    unsigned count=mini(base->feature_count,256),points=mini(base->point_count,4096);
    for(unsigned i=0;i<count;i++) {
        const WdgBaseFeature* f=&base->features[i];
        if(f->point_count<2||(unsigned)f->first_point+f->point_count>points)continue;
        if(visible_path(p,base->points+f->first_point,f->point_count,f->closed))return true;
    }
    return false;
}
static bool visible_territories(const Projection* p,const WdgMapSnapshot* map) {
    if(!map)return false;
    unsigned count=mini(map->hull_count,WDG_MAP_MAX_HULLS),points=mini(map->point_count,WDG_MAP_MAX_POINTS);
    for(unsigned i=0;i<count;i++) {
        const WdgMapHull* h=&map->hulls[i];
        if(h->point_count<3||(unsigned)h->first_point+h->point_count>points)continue;
        if(visible_path(p,map->points+h->first_point,h->point_count,true))return true;
    }
    for(unsigned i=0;i<(unsigned)mini(map->cell_count,WDG_MAP_MAX_CELLS);i++) {
        const WdgMapCell* cell=&map->cells[i];XY q;
        if(project(p,cell->lat_e6/1e6,cell->lon_e6/1e6,&q)&&q.x>=0&&q.x<W&&q.y>=36&&q.y<=130)return true;
    }
    return false;
}
static const char* wrapped_line(const char* cursor,char out[48],size_t width) {
    while(*cursor==' ')++cursor;
    size_t n=0,space=0;const char* after_space=NULL;
    while(*cursor&&n<width) {
        if(*cursor=='\n'){++cursor;break;}
        unsigned char c=next_char(&cursor);
        out[n++]=(char)c;if(c==' '){space=n-1;after_space=cursor;}
    }
    if(*cursor&&n==width&&after_space){n=space;cursor=after_space;}
    while(n&&out[n-1]==' ') { --n; }
    out[n]=0;return cursor;
}
static void backdrop_card(uint16_t* pixels,const WdgMapRenderState* state,const WdgMapSnapshot* map,
    const WdgMapBasemap* base,bool visible) {
    WdgMapBackdropStatus status=state->backdrop_status;
    if(status==WdgMapBackdropAuto) {
        if(visible)return;
        if(state->loading)status=WdgMapBackdropLoading;
        else if(base&&inside_bounds(&base->bounds,state))status=WdgMapBackdropEmpty;
        else status=state->online?WdgMapBackdropMissing:WdgMapBackdropOffline;
    }
    const char* title=state->map_message&&state->map_message[0]?state->map_message:NULL;
    const char* detail=state->map_detail&&state->map_detail[0]?state->map_detail:NULL;
    uint32_t accent=BLUE;
    if(status==WdgMapBackdropLoading) {
        if(!title)title=visible?"Refreshing street map...":"Loading street map...";
        if(!detail)detail=visible?"Your saved map stays available":"Downloading real streets and landmarks";
    } else if(status==WdgMapBackdropError) {
        if(!title)title="Street map download failed";
        if(!detail)detail=visible?"Your previous map is still available":"No street data received. Try again.";
        accent=0xB9792C;
    } else if(status==WdgMapBackdropOffline) {
        if(!title)title="No offline street map here";
        if(!detail)detail="Connect Wi-Fi to download this area";
        accent=0x6B8194;
    } else if(status==WdgMapBackdropEmpty) {
        if(!title)title="No mapped streets here";
        if(!detail)detail="Download complete. Try another area or zoom.";
        accent=0x6B8194;
    } else {
        if(!title)title="Street map not downloaded";
        if(!detail)detail=map?"Territories and streets download separately":"Download this area to use it offline";
    }
    const char* action=state->backdrop_retry?"Click: retry download":"Click: options";
    if(status==WdgMapBackdropLoading)action="Click: options";
    if(visible) {
        /* A refresh or failure must not erase the last successful map. */
        int width=text_width(title,1,251)+28;
        if(width>296)width=296;
        int height=status==WdgMapBackdropLoading?20:31;
        roundrect(pixels,13,43,width,height,7,0x193249,22);
        roundrect(pixels,12,41,width,height,7,PAPER,248);
        if(status==WdgMapBackdropLoading)spinner(pixels,23,51,state->now_ms);
        else {circle(pixels,23,51,5,accent,255);text(pixels,21,48,"!",1,PAPER,5);}
        text(pixels,34,48,title,1,INK,width-38);
        if(height>20)text(pixels,23,61,action,1,MUTED,width-25);
        return;
    }
    roundrect(pixels,16,46,290,77,10,0x193249,25);
    roundrect(pixels,15,43,290,77,10,PAPER,254);
    circle(pixels,32,59,10,status==WdgMapBackdropError?0xFFF2DE:0xE7EFFB,255);
    if(status==WdgMapBackdropLoading)spinner(pixels,32,59,state->now_ms);
    else if(status==WdgMapBackdropError)text(pixels,30,56,"!",1,accent,5);
    else {
        /* Generic download symbol, not a location or invented map feature. */
        rect(pixels,31,53,2,9,accent,255);
        line(pixels,(XY){27,59},(XY){32,64},1,accent,255);
        line(pixels,(XY){32,64},(XY){37,59},1,accent,255);
        rect(pixels,26,67,12,1,accent,255);
    }
    text(pixels,48,56,title,1,INK,248);
    char line_text[48];const char* cursor=detail;
    for(unsigned i=0;i<2&&*cursor;i++) {
        cursor=wrapped_line(cursor,line_text,44);
        text(pixels,25,76+(int)i*10,line_text,1,MUTED,269);
    }
    int width=text_width(action,1,264)+12;
    roundrect(pixels,23,101,width,13,4,status==WdgMapBackdropError?0xFFF2DE:0xEAF1FB,255);
    text(pixels,29,104,action,1,accent,262);
}
static void chrome(uint16_t* pixels,const WdgMapSnapshot* map,const WdgMapRenderState* s,const WdgMapBasemap* base) {
    roundrect(pixels,9,8,302,29,9,0x173645,32);roundrect(pixels,8,6,304,29,9,PAPER,253);
    circle(pixels,21,20,5,BLUE,255);circle(pixels,21,20,2,PAPER,255);text(pixels,33,16,"WDG MAP",1,INK,85);
    roundrect(pixels,83,12,54,16,5,s->online?0xE6F4EB:0xEEF1F6,255);
    text(pixels,89,17,s->online?"ONLINE":"OFFLINE",1,s->online?0x247744:MUTED,45);
    char gps[24];if(s->has_fix)snprintf(gps,sizeof(gps),"GPS %u",s->satellites);
    else snprintf(gps,sizeof(gps),"%s",s->gps_connected?"NO FIX":"GPS --");
    if(s->loading)spinner(pixels,158,20,s->now_ms);
    roundrect(pixels,184,12,73,16,5,s->has_fix?0xE6F4EB:0xFFF4DB,255);
    circle(pixels,191,20,2,s->has_fix?0x34A467:0xD39D3D,255);text(pixels,197,17,gps,1,s->has_fix?0x247744:0x956A24,57);
    char zoom[16];snprintf(zoom,sizeof(zoom),"Z:%d",(int)(clampd(s->zoom,1,20)+0.5));text(pixels,270,17,zoom,1,INK,36);
    if(base&&base->feature_count)text(pixels,9,122,"(c) OpenStreetMap contributors",1,0x53696E,182);
    else text(pixels,9,122,"Territories: WDGWars",1,0x53696E,180);
    roundrect(pixels,9,134,302,31,9,0x173645,35);roundrect(pixels,8,132,304,31,9,PAPER,250);
    const WdgMapGang* selected=gang(map,s->selected_gang_id);
    uint32_t c=selected?selected->color_rgb:BLUE;
    roundrect(pixels,15,139,4,17,2,c,255);
    const char* title=selected?selected->name:s->area_name&&s->area_name[0]?s->area_name:s->follow?"Following your location":"Explore the map";
    text(pixels,25,139,title,1,INK,220);
    const char* notice=s->status&&s->status[0]?s->status:s->basemap_status&&s->basemap_status[0]?s->basemap_status:NULL;
    char age[32],detail[65];
    if(!map&&!base)snprintf(age,sizeof(age),"No map downloaded");
    else if(!s->cache_age_known)snprintf(age,sizeof(age),"Cache age unknown");
    else if(s->cache_age_seconds<60)snprintf(age,sizeof(age),"Updated just now");
    else if(s->cache_age_seconds<3600)snprintf(age,sizeof(age),"Cached %lum ago",(unsigned long)(s->cache_age_seconds/60));
    else if(s->cache_age_seconds<86400)snprintf(age,sizeof(age),"Cached %luh ago",(unsigned long)(s->cache_age_seconds/3600));
    else snprintf(age,sizeof(age),"Cached %lud ago",(unsigned long)(s->cache_age_seconds/86400));
    snprintf(detail,sizeof(detail),"%s%s%s",age,notice?" | ":"",notice?notice:"");
    text(pixels,25,150,detail,1,MUTED,274);
    roundrect(pixels,266,136,37,12,4,0xEEF3FA,255);text(pixels,272,139,s->follow?"LIVE":"PAN",1,BLUE,29);
}
static void globe_chrome(uint16_t* pixels,const WdgMapSnapshot* map,const WdgMapRenderState* s) {
    rect(pixels,0,0,W,16,PAPER,255);rect(pixels,0,154,W,16,PAPER,255);
    text(pixels,7,5,"WDG MAP",1,INK,48);text(pixels,68,5,"GLOBE",1,BLUE,48);
    char gps[24];if(s->has_fix)snprintf(gps,sizeof(gps),"GPS %u",s->satellites);
    else snprintf(gps,sizeof(gps),"%s",s->gps_connected?"NO FIX":"GPS --");
    text(pixels,185,5,gps,1,s->has_fix?0x287447:MUTED,63);
    text(pixels,266,5,s->online?"ONLINE":"OFFLINE",1,s->online?0x287447:MUTED,48);
    if(s->loading)spinner(pixels,151,8,s->now_ms);
    char summary[64];
    if(map&&map->cell_count)snprintf(summary,sizeof(summary),"Natural Earth | %u cells cached",map->cell_count);
    else if(map&&map->hull_count)snprintf(summary,sizeof(summary),"Natural Earth | %u zones cached",map->hull_count);
    else snprintf(summary,sizeof(summary),"Natural Earth | %s",map&&saved_bounds(&map->bounds)?"No territories returned":"Territories not loaded");
    text(pixels,6,160,summary,1,MUTED,248);
    char age[20];
    if(!map||!s->cache_age_known)snprintf(age,sizeof(age),"--");
    else if(s->cache_age_seconds<60)snprintf(age,sizeof(age),"now");
    else if(s->cache_age_seconds<3600)snprintf(age,sizeof(age),"%lum ago",(unsigned long)(s->cache_age_seconds/60));
    else if(s->cache_age_seconds<86400)snprintf(age,sizeof(age),"%luh ago",(unsigned long)(s->cache_age_seconds/3600));
    else snprintf(age,sizeof(age),"%lud ago",(unsigned long)(s->cache_age_seconds/86400));
    text(pixels,260,160,age,1,MUTED,54);
}
void wdgwars_map_render(uint16_t* pixels,const WdgMapSnapshot* map,const WdgMapRenderState* s,const WdgMapBasemap* base) {
    if(!pixels||!s) { return; }
    Projection p;if(!projection(s,&p))return;
    bool coarse=s->zoom<8.5;
    bool streets_visible=visible_basemap(&p,base),territories_visible=visible_territories(&p,map);
    bool visible=coarse||streets_visible||territories_visible;
    uint16_t background=wdgwars_map_rgb565(streets_visible?0xEDF0EB:0xEEF2F5);
    for(size_t i=0;i<W*H;i++)pixels[i]=background;
    if(coarse)world_layer(pixels,&p);
    if(!p.globe)basemap_layer(pixels,&p,base);
    territories(pixels,&p,map,s);
    if(p.globe&&(!map||(!map->cell_count&&!map->hull_count)))world_labels(pixels,&p);
    else if(!p.globe)road_labels(pixels,&p,base);
    territory_labels(pixels,&p,map,s);
    if(!s->follow&&!p.globe){line(pixels,(XY){153,85},(XY){167,85},1,INK,140);line(pixels,(XY){160,78},(XY){160,92},1,INK,140);}
    gps_marker(pixels,&p,s);coverage(pixels,&p,map,base,s);
    if(!p.globe&&visible)compass_scale(pixels,&p,s);
    if(!coarse||s->backdrop_status==WdgMapBackdropLoading||s->backdrop_status==WdgMapBackdropError)
        backdrop_card(pixels,s,map,base,visible);
    if(p.globe)globe_chrome(pixels,map,s);else chrome(pixels,map,s,base);
}
void wdgwars_map_render_menu(uint16_t* pixels,const char* title,const char* const* lines,size_t count,size_t selected,const char* footer) {
    if(!pixels) { return; }
    rect(pixels,0,0,W,H,0x182E42,100);roundrect(pixels,9,7,302,157,11,PAPER,255);
    roundrect(pixels,20,16,4,13,2,BLUE,255);text(pixels,33,19,title?title:"Map",1,INK,256);
    rect(pixels,21,34,278,1,0xE6ECF0,255);size_t first=selected<count&&selected>=6?selected-5:0;
    size_t available=count-first;if(available>6)available=6;
    for(size_t row=0;row<available;row++) {
        size_t index=first+row;int y=40+(int)row*16;bool focus=index==selected;
        if(focus)roundrect(pixels,18,y-2,283,15,5,0xE7F0FF,255);
        text(pixels,27,y+2,lines&&lines[index]?lines[index]:"",1,focus?0x165FD2:INK,263);
        if(focus){rect(pixels,20,y+2,2,7,BLUE,255);text(pixels,290,y+2,">",1,BLUE,6);}
    }
    if(count>6) {
        int rail=92,top=40;rect(pixels,305,top,2,rail,0xE0E7ED,255);
        int size=maxi(9,(int)(rail*6/count)),position=(int)((rail-size)*first/(count-6));
        roundrect(pixels,305,top+position,2,size,1,0x8CADCD,255);
    }
    rect(pixels,21,140,278,1,0xE6ECF0,255);text(pixels,22,149,footer?footer:"Turn: choose   Click: open   Back: map",1,MUTED,276);
}