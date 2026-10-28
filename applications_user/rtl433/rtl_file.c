#include "rtl_file.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
char *rtl_pulse_text(const RtlFrame *f) {
  if (f->pulses.num_pulses < 1 || f->pulses.num_pulses > PD_MAX_PULSES)
    return NULL;
  size_t cap = 128 + (size_t)f->pulses.num_pulses * 24;
  char *s = malloc(cap);
  if (!s)
    return NULL;
  size_t n = snprintf(s, cap, "RTL433-PULSE 2\n%lu %u %d %u %d %ld\n",
                      (unsigned long)f->meta.frequency, f->meta.fsk,
                      f->meta.rssi, f->pulses.num_pulses,
                      f->meta.profile_known ? (int)f->meta.profile : -1,
                      f->tail_known ? (long)f->tail_gap_us : -1L);
  for (unsigned i = 0; i < f->pulses.num_pulses; i++)
    n += snprintf(s + n, cap - n, "%d %d\n", f->pulses.pulse[i],
                  f->pulses.gap[i]);
  return s;
}
static bool number(const char **s, long *n) {
  while (**s == ' ' || **s == '\n' || **s == '\r' || **s == '\t')
    (*s)++;
  const char *p = *s;
  bool neg = false;
  if (*p == '-') {
    neg = true;
    p++;
  }
  if (*p < '0' || *p > '9')
    return false;
  unsigned long v = 0;
  while (*p >= '0' && *p <= '9') {
    if (v > (INT_MAX - (unsigned)(*p - '0')) / 10u)
      return false;
    v = v * 10 + *p++ - '0';
  }
  if (*p && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t')
    return false;
  *n = neg ? -(long)v : (long)v;
  *s = p;
  return true;
}
bool rtl_pulse_parse(const char *s, RtlFrame *f) {
  const char *header = "RTL433-PULSE 1\n";
  bool v2 = !strncmp(s, "RTL433-PULSE 2\n", strlen(header));
  if (!v2 && strncmp(s, header, strlen(header)))
    return false;
  s += strlen(header);
  long hz, fsk, rssi, count;
  if (!number(&s, &hz) || !number(&s, &fsk) || !number(&s, &rssi) ||
      !number(&s, &count) || hz < 100000000 || hz > 1000000000 || fsk < 0 ||
      fsk > 1 || rssi < -127 || rssi > 20 || count < 1 || count > PD_MAX_PULSES)
    return false;
  long profile = -1, tail = -1;
  if (v2 && (!number(&s, &profile) || !number(&s, &tail) || profile < -1 ||
             profile > 5 || tail < -1 || tail > 10000000 ||
             (profile >= 0 && ((profile >= 2) != (fsk != 0)))))
    return false;
  memset(f, 0, sizeof(*f));
  f->pulses.sample_rate = 1000000;
  f->pulses.num_pulses = count;
  f->meta =
      (RtlMeta){.frequency = hz, .fsk = fsk, .rssi = rssi, .offline = true};
  f->meta.profile_known = profile >= 0;
  f->meta.profile = profile >= 0 ? profile : 0;
  f->tail_known = tail >= 0;
  f->tail_gap_us = tail >= 0 ? tail : 0;
  for (long i = 0; i < count; i++) {
    long p, g;
    if (!number(&s, &p) || !number(&s, &g) || p < 1 || p > 10000000 || g < 1 ||
        g > 10000000)
      return false;
    f->pulses.pulse[i] = p;
    f->pulses.gap[i] = g;
  }
  if (f->tail_known && f->tail_gap_us > (uint32_t)f->pulses.gap[count - 1])
    return false;
  while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')
    s++;
  return !*s;
}
