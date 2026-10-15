#include "net_protocol.h"
#include <stdio.h>
#include <string.h>
static uint16_t rd16(const uint8_t *p) { return (uint16_t)p[0] << 8 | p[1]; }
static void wr16(uint8_t *p, unsigned x) {
  p[0] = x >> 8;
  p[1] = x;
}
static unsigned lower(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
bool ns_name_equal(const char *a, const char *b) {
  size_t x = strlen(a), y = strlen(b);
  if (x && a[x - 1] == '.')
    x--;
  if (y && b[y - 1] == '.')
    y--;
  if (x != y || !x)
    return false;
  for (size_t i = 0; i < x; i++)
    if (lower((unsigned char)a[i]) != lower((unsigned char)b[i]))
      return false;
  return true;
}
bool net_ip_parse(const char *s, uint8_t ip[4]) {
  for (unsigned i = 0; i < 4; i++) {
    unsigned x = 0, n = 0;
    while (*s >= '0' && *s <= '9') {
      x = x * 10 + *s++ - '0';
      if (++n > 3 || x > 255)
        return false;
    }
    if (!n)
      return false;
    ip[i] = x;
    if (i < 3) {
      if (*s++ != '.')
        return false;
    } else if (*s)
      return false;
  }
  return true;
}
void net_ip_text(const uint8_t ip[4], char out[16]) {
  snprintf(out, 16, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}
static int hex(unsigned c) {
  c = lower(c);
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                : -1;
}
bool net_mac_parse(const char *s, uint8_t mac[6]) {
  if (strlen(s) != 17)
    return false;
  for (unsigned i = 0; i < 6; i++) {
    int h = hex(s[i * 3]), l = hex(s[i * 3 + 1]);
    if (h < 0 || l < 0 || (i < 5 && s[i * 3 + 2] != ':'))
      return false;
    mac[i] = (h << 4) | l;
  }
  return !(mac[0] & 1) && memcmp(mac, "\0\0\0\0\0\0", 6);
}
static bool dns_name(const uint8_t *p, size_t n, size_t *end, char *out,
                     size_t cap) {
  size_t at = 12, w = 0; /* Queries with compression are rejected, not followed
                            into untrusted loops. */
  while (at < n) {
    unsigned len = p[at++];
    if (!len) {
      if (!w)
        return false;
      out[w] = 0;
      *end = at;
      return true;
    }
    if (len > 63 || len > n - at)
      return false;
    if (w) {
      if (w + 1 >= cap)
        return false;
      out[w++] = '.';
    }
    if (w + len >= cap)
      return false;
    for (unsigned i = 0; i < len; i++) {
      unsigned c = p[at++];
      if (c < 33 || c > 126 || c == '.' || c == '\\')
        return false;
      out[w++] = c;
    }
  }
  return false;
}
bool ns_question(const uint8_t *p, size_t n, NsProtocol proto, NsQuestion *q) {
  if (!p || !q || n < 17 || n > 512)
    return false;
  unsigned flags = rd16(p + 2);
  if (flags & 0xf800 || rd16(p + 4) != 1 || rd16(p + 6) || rd16(p + 8))
    return false;
  if (proto == NsLlmnr && (flags & 0x0600))
    return false; /* no conflict or truncated query */
  if (proto != NsDns && rd16(p + 10))
    return false;
  size_t end;
  memset(q, 0, sizeof(*q));
  if (!dns_name(p, n, &end, q->name, sizeof q->name) || n - end < 4)
    return false;
  q->type = rd16(p + end);
  q->klass = rd16(p + end + 2);
  q->id = rd16(p);
  q->question_end = end + 4;
  if (q->klass != 1)
    return false;
  if (proto == NsNbns) { /* First-level NetBIOS encoding, 15 name bytes +
                            suffix. No scopes. */
    if (strlen(q->name) != 32)
      return false;
    char plain[16];
    for (unsigned i = 0; i < 16; i++) {
      unsigned h = q->name[2 * i], l = q->name[2 * i + 1];
      if (h < 'A' || h > 'P' || l < 'A' || l > 'P')
        return false;
      unsigned c = ((h - 'A') << 4) | (l - 'A');
      if (i < 15) {
        if (c < 32 || c > 126)
          return false;
        plain[i] = c;
      } else if (c != 0 && c != 0x20)
        return false;
    }
    plain[15] = 0;
    unsigned len = 15;
    while (len && plain[len - 1] == ' ')
      plain[--len] = 0;
    if (!len)
      return false;
    strcpy(q->name, plain);
  }
  return true;
}
size_t ns_answer(const uint8_t *query, size_t n, NsProtocol proto,
                 const char *name, const uint8_t ip[4], uint8_t *out,
                 size_t cap) {
  NsQuestion q;
  if (!ns_question(query, n, proto, &q) || !ns_name_equal(q.name, name))
    return 0;
  bool address = q.type == (proto == NsNbns ? 0x20 : 1) || q.type == 255;
  size_t qlen = q.question_end - 12,
         base = proto == NsNbns ? 12 : q.question_end;
  size_t rr_name = proto == NsNbns ? qlen - 4 : 2,
         rdlen = proto == NsNbns ? 6 : 4;
  size_t total = base + (address ? rr_name + 10 + rdlen : 0);
  if (total > cap)
    return 0;
  memset(out, 0, total);
  wr16(out, q.id);
  unsigned flags = proto == NsDns     ? 0x8400 | (rd16(query + 2) & 0x0100)
                   : proto == NsLlmnr ? 0x8000
                                      : 0x8580;
  wr16(out + 2, flags);
  wr16(out + 4, proto == NsNbns ? 0 : 1);
  wr16(out + 6, address ? 1 : 0);
  if (proto != NsNbns)
    memcpy(out + 12, query + 12, qlen);
  if (address) {
    size_t at = base;
    if (proto == NsNbns) {
      memcpy(out + at, query + 12, rr_name);
      at += rr_name;
    } else {
      out[at++] = 0xc0;
      out[at++] = 0x0c;
    }
    wr16(out + at, proto == NsNbns ? 0x20 : 1);
    wr16(out + at + 2, 1);
    out[at + 7] = 30;
    wr16(out + at + 8, rdlen);
    at += 10;
    if (proto == NsNbns) {
      out[at++] = 0;
      out[at++] = 0;
    }
    memcpy(out + at, ip, 4);
  }
  return total;
}
size_t net_arp(uint8_t out[60], bool reply, const uint8_t src[6],
               const uint8_t dst[6], const uint8_t mac[6], const uint8_t ip[4],
               const uint8_t target_mac[6], const uint8_t target_ip[4]) {
  memset(out, 0, 60);
  memcpy(out, dst, 6);
  memcpy(out + 6, src, 6);
  wr16(out + 12, 0x0806);
  wr16(out + 14, 1);
  wr16(out + 16, 0x0800);
  out[18] = 6;
  out[19] = 4;
  wr16(out + 20, reply ? 2 : 1);
  memcpy(out + 22, mac, 6);
  memcpy(out + 28, ip, 4);
  memcpy(out + 32, target_mac, 6);
  memcpy(out + 38, target_ip, 4);
  return 60;
}
bool net_arp_read(const uint8_t *p, size_t n, uint8_t mac[6], uint8_t ip[4]) {
  if (n < 42 || rd16(p + 12) != 0x0806 || rd16(p + 14) != 1 ||
      rd16(p + 16) != 0x0800 || p[18] != 6 || p[19] != 4 ||
      (rd16(p + 20) != 1 && rd16(p + 20) != 2) || (p[22] & 1) ||
      memcmp(p + 6, p + 22, 6))
    return false;
  memcpy(mac, p + 22, 6);
  memcpy(ip, p + 28, 4);
  return true;
}
bool net_relay(uint8_t *p, size_t n, const uint8_t own[6],
               const uint8_t target[6], const uint8_t gateway[6],
               const uint8_t target_ip[4]) {
  if (n < 34 || memcmp(p, own, 6) || rd16(p + 12) != 0x0800 ||
      (p[14] >> 4) != 4)
    return false;
  unsigned ihl = (p[14] & 15) * 4, total = rd16(p + 16);
  if (ihl < 20 || total < ihl || 14u + total > n)
    return false;
  if (!memcmp(p + 6, target, 6) && !memcmp(p + 26, target_ip, 4))
    memcpy(p, gateway, 6);
  else if (!memcmp(p + 6, gateway, 6) && !memcmp(p + 30, target_ip, 4))
    memcpy(p, target, 6);
  else
    return false;
  /* Same-segment L2 relay: TTL and IP checksums remain unchanged. */
  memcpy(p + 6, own, 6);
  return true;
}
bool net_http_auth_present(const uint8_t *p, size_t n) {
  size_t at = 0;
  while (at < n) {
    size_t start = at;
    while (at < n && p[at] != '\n')
      at++;
    size_t len = at - start;
    if (at < n)
      at++;
    if (!len || (len == 1 && p[start] == '\r'))
      break;
    const char key[] = "authorization:";
    if (len >= sizeof key - 1) {
      bool same = true;
      for (size_t i = 0; i < sizeof key - 1; i++)
        if (lower(p[start + i]) != key[i])
          same = false;
      if (same)
        return true;
    }
  }
  return false;
}
size_t ns_query(const char *name, uint16_t id, uint8_t *out, size_t cap) {
  size_t len = strlen(name);
  if (!len || len > 253 || cap < len + 18)
    return 0;
  memset(out, 0, 12);
  wr16(out, id);
  wr16(out + 2, 0x0100);
  wr16(out + 4, 1);
  size_t at = 12, start = 0;
  for (size_t i = 0; i <= len; i++)
    if (i == len || name[i] == '.') {
      size_t n = i - start;
      if (!n || n > 63)
        return 0;
      out[at++] = n;
      memcpy(out + at, name + start, n);
      at += n;
      start = i + 1;
    }
  out[at++] = 0;
  wr16(out + at, 1);
  wr16(out + at + 2, 1);
  return at + 4;
}
static bool skip_name(const uint8_t *p, size_t n, size_t *pos) {
  size_t at = *pos;
  for (unsigned labels = 0; labels < 128 && at < n; labels++) {
    unsigned z = p[at++];
    if ((z & 0xc0) == 0xc0) {
      if (at >= n || (((z & 63) << 8) | p[at]) >= n)
        return false;
      *pos = at + 1;
      return true;
    }
    if (z > 63 || z > n - at)
      return false;
    if (!z) {
      *pos = at;
      return true;
    }
    at += z;
  }
  return false;
}
unsigned ns_addresses(const uint8_t *p, size_t n, uint16_t id,
                      uint8_t addresses[][4], unsigned cap) {
  if (n < 12 || rd16(p) != id || (rd16(p + 2) & 0x820f) != 0x8000 ||
      rd16(p + 4) > 1)
    return 0;
  size_t at = 12;
  for (unsigned i = 0; i < rd16(p + 4); i++) {
    if (!skip_name(p, n, &at) || n - at < 4)
      return 0;
    at += 4;
  }
  unsigned count = 0;
  for (unsigned i = 0; i < rd16(p + 6); i++) {
    if (!skip_name(p, n, &at) || n - at < 10)
      return 0;
    unsigned type = rd16(p + at), klass = rd16(p + at + 2),
             len = rd16(p + at + 8);
    at += 10;
    if (len > n - at)
      return 0;
    if (type == 1 && klass == 1 && len == 4 && count < cap)
      memcpy(addresses[count++], p + at, 4);
    at += len;
  }
  return count;
}
static unsigned sum16(const uint8_t *p, size_t n, unsigned sum) {
  while (n >= 2) {
    sum += rd16(p);
    p += 2;
    n -= 2;
  }
  if (n)
    sum += (unsigned)p[0] << 8;
  return sum;
}
static uint16_t checksum(unsigned sum) {
  while (sum >> 16)
    sum = (sum & 65535) + (sum >> 16);
  return ~sum;
}
size_t net_dns_redirect(const uint8_t *p, size_t n, const uint8_t own[6],
                        const uint8_t target[6], const uint8_t target_ip[4],
                        const char *name, const uint8_t reply[4], uint8_t *out,
                        size_t cap) {
  if (n < 42 || cap < 42 || memcmp(p, own, 6) || memcmp(p + 6, target, 6) ||
      rd16(p + 12) != 0x0800 || (p[14] >> 4) != 4 || p[23] != 17 ||
      memcmp(p + 26, target_ip, 4))
    return 0;
  unsigned ihl = (p[14] & 15) * 4, total = rd16(p + 16);
  if (ihl < 20 || total < ihl + 8 || 14u + total > n ||
      (rd16(p + 20) & 0x3fff) || checksum(sum16(p + 14, ihl, 0)))
    return 0;
  const uint8_t *udp = p + 14 + ihl;
  unsigned ul = rd16(udp + 4);
  if (rd16(udp + 2) != 53 || ul < 8 || ul > total - ihl)
    return 0;
  if (rd16(udp + 6) && checksum(sum16(udp, ul, sum16(p + 26, 8, 17 + ul))))
    return 0;
  size_t answer =
      ns_answer(udp + 8, ul - 8, NsDns, name, reply, out + 42, cap - 42);
  if (!answer)
    return 0;
  memset(out, 0, 42);
  memcpy(out, target, 6);
  memcpy(out + 6, own, 6);
  wr16(out + 12, 0x0800);
  out[14] = 0x45;
  wr16(out + 16, 28 + answer);
  out[22] = 64;
  out[23] = 17;
  memcpy(out + 26, p + 30, 4);
  memcpy(out + 30, p + 26, 4);
  wr16(out + 24, checksum(sum16(out + 14, 20, 0)));
  wr16(out + 34, 53);
  memcpy(out + 36, udp, 2);
  wr16(out + 38, 8 + answer);
  uint16_t check = checksum(
      sum16(out + 34, 8 + answer, sum16(out + 26, 8, 17 + 8 + answer)));
  wr16(out + 40, check ? check : 0xffff);
  return 42 + answer;
}
