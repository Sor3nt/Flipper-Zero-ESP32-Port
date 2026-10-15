/* Narrow libc/compiler compatibility for the T-Embed Native Tools FAP ABI.
 * GPL-3.0-or-later. Calendar functions use UTC, with no process-global timezone
 * dependency. strftime supports the formats actually used by this vendored
 * decoder snapshot. */
#ifdef ESP_PLATFORM
#include <datetime/datetime.h>
#include <errno.h>
#include <esp_timer.h>
#include <furi_hal_rtc.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
double __floatdidf(int64_t x) {
  if (x < 0)
    return -(double)((uint64_t)(-(x + 1)) + 1);
  return (double)(uint64_t)x;
}
float __floatdisf(int64_t x) { return (float)__floatdidf(x); }
long long llround(double x) {
  if (!isfinite(x))
    return LLONG_MIN;
  if (x >= 9223372036854775808.0)
    return LLONG_MAX;
  if (x <= -9223372036854775808.0)
    return LLONG_MIN;
  double y = x < 0 ? -x : x;
  double whole = floor(y);
  uint64_t v = (uint64_t)whole + (y - whole >= 0.5);
  return x < 0 ? -(long long)v : (long long)v;
}
float sqrtf(float x) {
  if (x < 0)
    return NAN;
  if (!isfinite(x) || x == 0)
    return x;
  bool scaled = x < 0x1p-126f;
  if (scaled)
    x *= 0x1p24f;
  union {
    float f;
    uint32_t u;
  } v = {x};
  v.u = (v.u >> 1) + 0x1fc00000;
  float y = v.f;
  for (unsigned i = 0; i < 6; i++)
    y = 0.5f * (y + x / y);
  return scaled ? y * 0x1p-12f : y;
}
int sprintf(char *dst, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(dst, INT_MAX, fmt, ap);
  va_end(ap);
  return n;
}
long strtol(const char *s, char **end, int base) {
  const char *start = s;
  while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\v' ||
         *s == '\f')
    s++;
  bool neg = *s == '-';
  if (*s == '-' || *s == '+')
    s++;
  if (*s == '-' || *s == '+' || *s == ' ' || *s == '\t' || *s == '\n') {
    if (end)
      *end = (char *)start;
    return 0;
  }
  char *p;
  unsigned long v = strtoul(s, &p, base);
  if (p == s) {
    if (end)
      *end = (char *)start;
    return 0;
  }
  if (end)
    *end = p;
  unsigned long limit =
      neg ? (unsigned long)LONG_MAX + 1 : (unsigned long)LONG_MAX;
  if (v > limit) {
    errno = ERANGE;
    return neg ? LONG_MIN : LONG_MAX;
  }
  return neg ? (v == limit ? LONG_MIN : -(long)v) : (long)v;
}
static bool leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }
static int month_days(int y, int m) {
  static const unsigned char days[] = {31, 28, 31, 30, 31, 30,
                                       31, 31, 30, 31, 30, 31};
  return days[m] + (m == 1 && leap(y));
}
struct tm *localtime_r(const time_t *clock, struct tm *out) {
  if (!clock || !out)
    return NULL;
  int64_t seconds = *clock, days = seconds / 86400, rem = seconds % 86400;
  if (rem < 0) {
    rem += 86400;
    days--;
  }
  int64_t original = days;
  int y = 1970;
  if (days > 366LL * 10000 || days < -366LL * 10000)
    return NULL;
  while (days < 0) {
    --y;
    days += 365 + leap(y);
  }
  while (days >= 365 + leap(y)) {
    days -= 365 + leap(y);
    y++;
  }
  memset(out, 0, sizeof(*out));
  out->tm_yday = days;
  out->tm_year = y - 1900;
  out->tm_wday = (original + 4) % 7;
  if (out->tm_wday < 0)
    out->tm_wday += 7;
  int m = 0;
  while (days >= month_days(y, m)) {
    days -= month_days(y, m);
    m++;
  }
  out->tm_mon = m;
  out->tm_mday = days + 1;
  out->tm_hour = rem / 3600;
  out->tm_min = (rem % 3600) / 60;
  out->tm_sec = rem % 60;
  return out;
}
time_t mktime(struct tm *t) {
  if (!t)
    return (time_t)-1;
  int64_t year = (int64_t)t->tm_year + 1900, month = t->tm_mon;
  year += month / 12;
  month %= 12;
  if (month < 0) {
    year--;
    month += 12;
  }
  if (year < -8000 || year > 12000)
    return (time_t)-1;
  int64_t days = 0;
  for (int y = 1970; y < year; y++)
    days += 365 + leap(y);
  for (int y = 1969; y >= year; y--)
    days -= 365 + leap(y);
  for (int m = 0; m < month; m++)
    days += month_days(year, m);
  days += (int64_t)t->tm_mday - 1;
  int64_t seconds = days * 86400 + (int64_t)t->tm_hour * 3600 +
                    (int64_t)t->tm_min * 60 + t->tm_sec;
  time_t result = seconds;
  if ((int64_t)result != seconds)
    return (time_t)-1;
  if (!localtime_r(&result, t))
    return (time_t)-1;
  return result;
}
int gettimeofday(struct timeval *tv, void *tz) {
  (void)tz;
  if (!tv)
    return -1;
  /* The decoder worker is the sole caller. Keep subsecond intervals monotonic
     even when the user adjusts the RTC during a receive session. */
  static bool initialized;
  static int64_t offset;
  int64_t now = esp_timer_get_time();
  if (!initialized) {
    DateTime dt;
    furi_hal_rtc_get_datetime(&dt);
    offset = (int64_t)datetime_datetime_to_timestamp(&dt) * 1000000 - now;
    initialized = true;
  }
  int64_t stamp = offset + now;
  tv->tv_sec = stamp / 1000000;
  tv->tv_usec = stamp % 1000000;
  return 0;
}
time_t time(time_t *out) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  if (out)
    *out = tv.tv_sec;
  return tv.tv_sec;
}
void perror(const char *s) { printf("%s: error %d\n", s ? s : "", errno); }
size_t strftime(char *dst, size_t cap, const char *fmt, const struct tm *t) {
  size_t used = 0;
  if (!cap)
    return 0;
  dst[0] = 0;
  for (; *fmt; fmt++) {
    char part[64];
    if (*fmt != '%') {
      part[0] = *fmt;
      part[1] = 0;
    } else {
      fmt++;
      switch (*fmt) {
      case '%':
        strcpy(part, "%");
        break;
      case 'Y':
        snprintf(part, sizeof part, "%04d", t->tm_year + 1900);
        break;
      case 'm':
        snprintf(part, sizeof part, "%02d", t->tm_mon + 1);
        break;
      case 'd':
        snprintf(part, sizeof part, "%02d", t->tm_mday);
        break;
      case 'H':
        snprintf(part, sizeof part, "%02d", t->tm_hour);
        break;
      case 'M':
        snprintf(part, sizeof part, "%02d", t->tm_min);
        break;
      case 'S':
        snprintf(part, sizeof part, "%02d", t->tm_sec);
        break;
      case 'z':
        strcpy(part, "+0000");
        break;
      case 'Z':
        strcpy(part, "UTC");
        break;
      case 'F':
        snprintf(part, sizeof part, "%04d-%02d-%02d", t->tm_year + 1900,
                 t->tm_mon + 1, t->tm_mday);
        break;
      case 'T':
        snprintf(part, sizeof part, "%02d:%02d:%02d", t->tm_hour, t->tm_min,
                 t->tm_sec);
        break;
      default:
        dst[0] = 0;
        return 0;
      }
    }
    size_t n = strlen(part);
    if (n >= cap - used) {
      dst[0] = 0;
      return 0;
    }
    memcpy(dst + used, part, n);
    used += n;
    dst[used] = 0;
  }
  return used;
}
#endif
