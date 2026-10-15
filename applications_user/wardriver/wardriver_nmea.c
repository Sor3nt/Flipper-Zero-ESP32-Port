#include "wardriver_nmea.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Gregorian date conversion, with explicit UTC and no dependency on firmware TZ. */
static bool leap(unsigned y) { return !(y % 4) && ((y % 100) || !(y % 400)); }
static unsigned days_month(unsigned y, unsigned m) {
    static const uint8_t days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    return days[m - 1] + (m == 2 && leap(y));
}
static uint64_t date_epoch(unsigned y, unsigned m, unsigned d) {
    uint64_t days = 0;
    for(unsigned i = 1970; i < y; ++i) days += leap(i) ? 366 : 365;
    for(unsigned i = 1; i < m; ++i) days += days_month(y, i);
    return (days + d - 1) * 86400;
}
void wardriver_format_time(uint64_t t, char out[20]) {
    if(t < 315532800ULL || t >= 4102444800ULL) { out[0] = 0; return; }
    unsigned y = 1970, m = 1;
    uint32_t days = (uint32_t)(t / 86400);
    while(days >= (unsigned)(leap(y) ? 366 : 365)) days -= leap(y++) ? 366 : 365;
    while(days >= days_month(y, m)) { days -= days_month(y, m); ++m; }
    unsigned sec = (unsigned)(t % 86400);
    snprintf(out, 20, "%04u-%02u-%02u %02u:%02u:%02u", y % 10000, m % 100, (unsigned)(days + 1) % 100,
             sec / 3600, (sec / 60) % 60, sec % 60);
}
static int hex(char c) {
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}
static bool number(const char* s, double* out) {
    if(!s || !*s) return false;
    bool digit = false, dot = false;
    const char* p = s;
    if(*p == '-' || *p == '+') ++p;
    for(; *p; ++p) {
        if(*p == '.' && !dot) dot = true;
        else if(*p >= '0' && *p <= '9') digit = true;
        else return false;
    }
    if(!digit || strlen(s) > 18) return false;
    *out = strtod(s, NULL);
    return *out > -1.0e10 && *out < 1.0e10;
}
static bool clock_value(const char* s, uint32_t* value) {
    if(strlen(s) < 6) return false;
    if(s[6] && (s[6] != '.' || !s[7])) return false;
    for(unsigned i = 0; i < 6; ++i) if(s[i] < '0' || s[i] > '9') return false;
    double n;
    if(!number(s, &n)) return false;
    unsigned h = (s[0]-'0')*10+s[1]-'0', m = (s[2]-'0')*10+s[3]-'0';
    unsigned sec = (s[4]-'0')*10+s[5]-'0';
    if(h > 23 || m > 59 || sec > 59) return false;
    *value = h*3600 + m*60 + sec;
    return true;
}
static bool coord(const char* s, const char* hemi, bool latitude, double* out) {
    double n;
    const char* dot=strchr(s,'.');
    size_t digits=dot?(size_t)(dot-s):strlen(s);
    if(digits!=(latitude?4U:5U)) return false;
    for(size_t i=0;i<digits;++i) if(s[i]<'0' || s[i]>'9') return false;
    if(!number(s, &n) || n < 0 || strlen(hemi) != 1) return false;
    char positive = latitude ? 'N' : 'E', negative = latitude ? 'S' : 'W';
    if(*hemi != positive && *hemi != negative) return false;
    unsigned degrees = (unsigned)(n / 100);
    double minutes = n - degrees*100;
    if(minutes >= 60 || degrees > (latitude ? 90U : 180U)) return false;
    *out = degrees + minutes / 60;
    if(*out > (latitude ? 90 : 180)) return false;
    if(*hemi == negative) *out = -*out;
    return true;
}
static bool date_value(const char* s, uint64_t* epoch) {
    if(strlen(s) != 6) return false;
    for(unsigned i = 0; i < 6; ++i) if(s[i] < '0' || s[i] > '9') return false;
    unsigned d=(s[0]-'0')*10+s[1]-'0', m=(s[2]-'0')*10+s[3]-'0';
    unsigned yy=(s[4]-'0')*10+s[5]-'0';
    unsigned y=(yy>=80?1900:2000)+yy;
    if(m < 1 || m > 12 || d < 1 || d > days_month(y,m)) return false;
    *epoch = date_epoch(y,m,d); return true;
}
static void sentence(WardriverNmea* p, uint32_t now) {
    char* line = p->line;
    char* star = strchr(line, '*');
    if(star) {
        if(strlen(star) != 3 || hex(star[1]) < 0 || hex(star[2]) < 0) return;
        uint8_t sum = 0;
        for(char* c=line; c < star; ++c) sum ^= (uint8_t)*c;
        if(sum != (hex(star[1])*16 + hex(star[2]))) return;
        *star=0;
    }
    char* f[24]; size_t count=1; f[0]=line;
    for(char* c=line; *c; ++c) if(*c==',') {
        *c=0; if(count==24) return; f[count++]=c+1;
    }
    if(strlen(f[0]) != 5 || f[0][0] != 'G' ||
       !(f[0][1]=='P' || f[0][1]=='N' || f[0][1]=='L' || f[0][1]=='A' || f[0][1]=='B')) return;
    bool rmc = !strcmp(f[0]+2,"RMC"), gga = !strcmp(f[0]+2,"GGA");
    if((!rmc && !gga) || count < (rmc ? 10U : 11U)) return;
    uint32_t second=0;
    bool time_valid=clock_value(f[1], &second);
    if(!time_valid && *f[1]) return;
    double lat=0, lon=0, value;
    bool fix;
    if(rmc) {
        if(strcmp(f[2],"A") && strcmp(f[2],"V")) return;
        fix = !strcmp(f[2],"A");
        if(fix && !time_valid) return;
        if(fix && (!coord(f[3],f[4],true,&lat) || !coord(f[5],f[6],false,&lon))) return;
        /* NMEA 2.3 mode N (not valid) and simulator mode are not real fixes. */
        if(count > 12 && (*f[12]=='N' || *f[12]=='S')) fix=false;
        uint64_t day;
        if(date_value(f[9],&day)) { p->day_epoch=day; p->date_valid=true; }
        else p->date_valid=false;
        p->seen_rmc=true; p->rmc_fix=fix; p->last_rmc=now; p->rmc_second=second;
    } else {
        if(!number(f[6],&value) || value < 0 || value > 8 || value != (unsigned)value) return;
        /* Autonomous, differential, PPS, fixed/float RTK; reject estimated/simulated. */
        fix=value >= 1 && value <= 5;
        if(fix && !time_valid) return;
        if(fix && (!coord(f[2],f[3],true,&lat) || !coord(f[4],f[5],false,&lon))) return;
        p->seen_gga=true; p->gga_fix=fix; p->last_gga=now; p->gga_second=second;
        p->data.satellites_valid = number(f[7],&value) && value>=0 && value<=255 && value==(unsigned)value;
        if(p->data.satellites_valid) p->data.satellites=(uint8_t)value;
        p->data.hdop_valid = number(f[8],&value) && value>0 && value<=100;
        if(p->data.hdop_valid) p->data.hdop=(float)value;
        p->data.altitude_valid=number(f[9],&value) && value>=-1000 && value<=100000 && !strcmp(f[10],"M");
        if(p->data.altitude_valid) p->data.altitude=value;
    }
    p->last_sentence=now; p->data.connected=true;
    p->data.has_fix=fix;
    if(fix) { p->data.latitude=lat; p->data.longitude=lon; }
    /* An RMC/GGA disagreement in the same measurement epoch invalidates the fix. */
    if(rmc && p->seen_gga && now-p->last_gga<2000 && p->gga_second==second && !p->gga_fix)
        p->data.has_fix=false;
    if(gga && p->seen_rmc && now-p->last_rmc<2000 && p->rmc_second==second && !p->rmc_fix)
        p->data.has_fix=false;
    p->data.timestamp_valid=time_valid && p->date_valid && (rmc || now-p->last_rmc < 3000);
    if(p->data.timestamp_valid) {
        uint64_t day=p->day_epoch;
        if(gga && p->rmc_second>86300 && second<100) day+=86400;
        p->data.timestamp=day+second;
    }
}
void wardriver_nmea_init(WardriverNmea* p) { memset(p,0,sizeof(*p)); p->data.enabled=true; }
void wardriver_nmea_feed(WardriverNmea* p,const void* bytes,size_t length,uint32_t now) {
    const uint8_t* data=bytes;
    for(size_t i=0;i<length;++i) {
        char c=(char)data[i];
        if(c=='$') { p->collecting=true; p->overflow=false; p->length=0; }
        else if(p->collecting && (c=='\r' || c=='\n')) {
            p->line[p->length]=0;
            if(!p->overflow) sentence(p,now);
            p->collecting=false;
        } else if(p->collecting) {
            if(c<32 || c>126 || p->length+1>=sizeof(p->line)) p->overflow=true;
            else if(!p->overflow) p->line[p->length++]=c;
        }
    }
}
WardriverGpsData wardriver_nmea_snapshot(const WardriverNmea* p,uint32_t now) {
    WardriverGpsData d=p->data;
    if(!d.connected || now-p->last_sentence>3000) { d.connected=false; d.has_fix=false; d.timestamp_valid=false; }
    if(now-p->last_gga>3000) d.altitude_valid=d.hdop_valid=d.satellites_valid=false;
    if(now-p->last_rmc>3000) d.timestamp_valid=false;
    return d;
}
