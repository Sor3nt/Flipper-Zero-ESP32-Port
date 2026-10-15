#include <assert.h>
#include <errno.h>
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "stubs/datetime/datetime.h"
double rtl_floatdidf(int64_t);float rtl_floatdisf(int64_t);long long rtl_llround(double);float rtl_sqrtf(float);long rtl_strtol(const char*,char**,int);struct tm *rtl_localtime_r(const time_t*,struct tm*);time_t rtl_mktime(struct tm*);size_t rtl_strftime(char*,size_t,const char*,const struct tm*);int rtl_gettimeofday(struct timeval*,void*);
static int64_t ticks=123456;int64_t esp_timer_get_time(void){return ticks;}
void furi_hal_rtc_get_datetime(DateTime *d){memset(d,0,sizeof(*d));}uint32_t datetime_datetime_to_timestamp(DateTime *d){(void)d;return 1700000000;}
int main(void){int64_t ints[]={0,1,-1,INT64_MAX,INT64_MIN,INT64_C(1234567890123456),-INT64_C(1234567890123456)};for(unsigned i=0;i<sizeof ints/sizeof*ints;i++){assert(rtl_floatdidf(ints[i])==(double)ints[i]);assert(rtl_floatdisf(ints[i])==(float)ints[i]);}
    double values[]={0,-0.0,0.49999999999999994,0.5,-0.5,1.5,-1.5,4503599627370495.5,1e16,-1e16};for(unsigned i=0;i<sizeof values/sizeof*values;i++)assert(rtl_llround(values[i])==llround(values[i]));
    float roots[]={0,1,2,4,12345,FLT_MIN,FLT_TRUE_MIN,FLT_MAX,INFINITY};for(unsigned i=0;i<sizeof roots/sizeof*roots;i++){float a=rtl_sqrtf(roots[i]),b=sqrtf(roots[i]);assert(a==b||fabsf(a-b)<=b*2e-7f);}assert(isnan(rtl_sqrtf(-1)));
    for(int year=1960;year<2400;year+=7)for(int mon=-2;mon<14;mon++){struct tm a={.tm_year=year-1900,.tm_mon=mon,.tm_mday=43,.tm_hour=27,.tm_min=-9,.tm_sec=73},b=a;time_t want=timegm(&b),got=rtl_mktime(&a);assert(got==want&&a.tm_year==b.tm_year&&a.tm_mon==b.tm_mon&&a.tm_mday==b.tm_mday&&a.tm_yday==b.tm_yday&&a.tm_wday==b.tm_wday);}
    struct tm a={.tm_year=126,.tm_mon=9,.tm_mday=15,.tm_hour=13,.tm_min=55,.tm_sec=3};char buf[64];assert(rtl_strftime(buf,sizeof buf,"%F %T %z",&a));assert(!strcmp(buf,"2026-10-15 13:55:03 +0000"));assert(!rtl_strftime(buf,2,"%Y",&a));assert(!rtl_strftime(buf,sizeof buf,"%",&a));
    const char *cases[]={"0","-1","0xff","077"," --2","+ 2","9223372036854775808","-9223372036854775809","31z",""};for(unsigned i=0;i<sizeof cases/sizeof*cases;i++){char *x,*y;errno=0;long want=strtol(cases[i],&x,0);int err=errno;errno=0;long got=rtl_strtol(cases[i],&y,0);assert(got==want&&x==y&&errno==err);}
    struct timeval tv;assert(!rtl_gettimeofday(&tv,NULL)&&tv.tv_sec==1700000000&&tv.tv_usec==0);ticks+=234567;assert(!rtl_gettimeofday(&tv,NULL)&&tv.tv_usec==234567);ticks+=1000000;assert(!rtl_gettimeofday(&tv,NULL)&&tv.tv_sec==1700000001&&tv.tv_usec==234567);
    puts("PASS: FAP compatibility math, integer conversion, UTC calendar normalization, time intervals, bounded formatting");return 0;}
