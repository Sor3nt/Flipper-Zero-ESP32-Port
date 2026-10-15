#include "core.h"
#include "golden.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned calls;
static bool keep(void *ctx, unsigned n, unsigned total) {
  (void)ctx;
  (void)n;
  (void)total;
  calls++;
  return true;
}
static bool stop(void *ctx, unsigned n, unsigned total) {
  (void)ctx;
  (void)n;
  (void)total;
  return false;
}
static bool progress(void *ctx, const char *phase, uint64_t n, uint64_t total) {
  (void)ctx;
  (void)phase;
  (void)n;
  (void)total;
  return true;
}
static void framing(void) {
  uint8_t p[128] = {8, 2}, ap[6] = {2, 1, 2, 3, 4, 5},
          sta[6] = {2, 6, 7, 8, 9, 10};
  memcpy(p + 4, sta, 6);
  memcpy(p + 10, ap, 6);
  memcpy(p + 16, ap, 6);
  const uint8_t llc[] = {0xaa, 0xaa, 3, 0, 0, 0, 0x88, 0x8e};
  memcpy(p + 24, llc, 8);
  p[32] = 1;
  p[35] = 21;
  p[36] = 1;
  p[37] = 7;
  p[39] = 21;
  const uint8_t wsc[] = {254, 0, 0x37, 0x2a, 0, 0, 0, 1};
  memcpy(p + 40, wsc, 8);
  p[48] = 4;
  p[49] = 3;
  p[51] = 10;
  memcpy(p + 52, "12345", 5);
  WfEap e = wf_eap(p, 57, ap, sta);
  assert(e.kind == WfWsc && e.id == 7 && e.flags == 3 && e.total == 10 &&
         e.len == 5);
  for (size_t n = 0; n < 57; n++)
    assert(wf_eap(p, n, ap, sta).kind == WfUnrelated);
  p[4] ^= 2;
  assert(wf_eap(p, 57, ap, sta).kind == WfUnrelated);
  p[4] ^= 2;
  p[49] = 4;
  assert(wf_eap(p, 57, ap, sta).kind == WfUnrelated);
  p[49] = 3;
  WfFragments f = {0};
  assert(wf_reassemble(&f, &e) == 0);
  assert(wf_reassemble(&f, &e) == 2);
  e.flags = 1;
  assert(wf_reassemble(&f, &e) == -1);
  e.flags = 3;
  e = (WfEap){.kind = WfWsc,
              .opcode = 4,
              .id = 8,
              .flags = 1,
              .data = (const uint8_t *)"678",
              .len = 3};
  assert(wf_reassemble(&f, &e) == 0);
  assert(wf_reassemble(&f, &e) == 2);
  e = (WfEap){.kind = WfWsc,
              .opcode = 4,
              .id = 9,
              .data = (const uint8_t *)"90",
              .len = 2};
  WfEap last = e;
  assert(wf_reassemble(&f, &e) == 1 && e.len == 10 &&
         !memcmp(e.data, "1234567890", 10));
  assert(wf_reassemble(&f, &last) == 2);
  e = (WfEap){.kind = WfWsc,
              .opcode = 4,
              .id = 10,
              .data = (const uint8_t *)"next",
              .len = 4};
  assert(wf_reassemble(&f, &e) == 1);
  f = (WfFragments){0};
  e.flags = 1;
  assert(wf_reassemble(&f, &e) == -1);
  e.flags = 3;
  e.total = WF_MSG_MAX + 1;
  assert(wf_reassemble(&f, &e) == -1);
  e.total = 3;
  assert(wf_reassemble(&f, &e) == -1);
  f = (WfFragments){0};
  e.total = 4;
  assert(wf_reassemble(&f, &e) == -1); /* MF requires more bytes */
  f = (WfFragments){0};
  e.total = 10;
  assert(wf_reassemble(&f, &e) == 0);
  e.id++;
  e.flags = 0;
  e.total = 0;
  assert(wf_reassemble(&f, &e) == -1); /* premature final */
  uint8_t beacon[80] = {0x80};
  memcpy(beacon + 16, ap, 6);
  const uint8_t ie[] = {0,   3,  'L',  'A',  'B',  3, 1,    6,
                        221, 14, 0,    0x50, 0xf2, 4, 0x10, 0x4a,
                        0,   1,  0x10, 0x10, 0x57, 0, 1,    1};
  memcpy(beacon + 36, ie, sizeof(ie));
  WfTarget t;
  assert(wf_beacon(beacon, 36 + sizeof(ie), &t) && t.locked == 1 &&
         t.wps == 1 && t.channel == 6 && !strcmp(t.ssid, "LAB"));
  beacon[36 + sizeof(ie) - 1] = 0;
  assert(wf_beacon(beacon, 36 + sizeof(ie), &t) && t.locked == 0);
  beacon[36 + sizeof(ie) - 1] = 2;
  assert(wf_beacon(beacon, 36 + sizeof(ie), &t) && t.locked == -1);
  /* WPS State=1 and Selected Registrar=1 are not AP PIN lock flags. */
  beacon[36 + sizeof(ie) - 4] = 0x44;
  beacon[36 + sizeof(ie) - 1] = 1;
  assert(wf_beacon(beacon, 36 + sizeof(ie), &t) && t.locked == -1);
  beacon[36 + sizeof(ie) - 4] = 0x41;
  assert(wf_beacon(beacon, 36 + sizeof(ie), &t) && t.locked == -1 && t.selected == 1);
  beacon[36 + sizeof(ie) - 4] = 0x57;
  beacon[45] = 9;
  assert(wf_beacon(beacon, 36 + sizeof(ie) - 5, &t) && t.locked == -1);
  assert(wf_scan_beacon(beacon, 44, &t) && !t.wps && t.locked == -1);
  assert(!wf_beacon(beacon, 44, &t));
  /* Missing/duplicate attributes must never produce a locked result. */
  beacon[45] = 19;
  memcpy(beacon + 36 + sizeof(ie), ie + sizeof(ie) - 5, 5);
  assert(wf_beacon(beacon, 36 + sizeof(ie) + 5, &t) && t.locked == -1);
  WfLockWatch w = {0};
  assert(wf_lock_observe(&w, 1, 1, 100) == WfLockPending);
  assert(wf_lock_observe(&w, 1, 1, 500) == WfLockPending); /* retransmission */
  assert(wf_lock_observe(&w, 1, 2, 500) == WfLockSet);
  assert(wf_lock_observe(&w, -1, 3, 600) == WfLockPending);
  assert(wf_lock_observe(&w, -1, 4, 1000) == WfLockPending); /* absence != clear */
  assert(wf_lock_observe(&w, 0, 5, 1100) == WfLockPending);
  assert(wf_lock_observe(&w, 0, 6, 1400) == WfLockClear);
  w = (WfLockWatch){0};
  assert(wf_lock_observe(&w, -1, 1, 0) == WfLockPending);
  assert(wf_lock_observe(&w, -1, 2, 300) == WfLockUnknown);
  w = (WfLockWatch){0};
  assert(wf_lock_observe(&w, 0, 1, UINT32_MAX - 100) == WfLockPending);
  assert(wf_lock_observe(&w, 1, 2, UINT32_MAX - 200) == WfLockPending); /* stale */
  assert(!w.saw_lock);
  assert(wf_lock_observe(&w, 0, 3, 200) == WfLockClear); /* timer wraps */
  puts("Selected-AP EAP framing, WSC reassembly/retransmissions and lock "
       "advertisement tests passed");
}
static void family(int mode, const uint8_t *nonce, const uint8_t *h1,
                   const uint8_t *h2) {
  WfSession s = {.stage = 3};
  memcpy(s.auth, auth, 32);
  memcpy(s.pke, pke, 192);
  memcpy(s.pkr, pkr, 192);
  memcpy(s.enonce, nonce, 16);
  memcpy(s.ehash1, h1, 32);
  memcpy(s.ehash2, h2, 32);
  int got = wf_pixie_all(&s, mode == 5, 100000, progress, NULL);
  assert(got == mode);
  assert(!strcmp(s.pin, "12345670"));
  printf("Pixie family %d independent fixture passed\n", mode);
}
int main(void) {
  framing();
  WfSession s;
  uint8_t out[WF_MSG_MAX], bad[WF_MSG_MAX];
  size_t n;
  assert(wf_m2(&s, m1, sizeof(m1), x, rn, out, &n));
  assert(n == sizeof(m2) && !memcmp(out, m2, n));
  assert(!memcmp(s.auth, auth, 32) && !memcmp(s.wrap, wrap, 16));
  memcpy(bad, m3, sizeof(m3));
  bad[sizeof(m3) - 1] ^= 1;
  assert(!wf_m3(&s, bad, sizeof(m3)));
  assert(wf_m3(&s, m3, sizeof(m3)));
  assert(wf_pixie(&s, stop, NULL) == -1);
  assert(wf_pixie(&s, keep, NULL) == 1);
  assert(!strcmp(s.pin, "12345670"));
  assert(wf_m4(&s, random48, out, &n));
  assert(n == sizeof(m4) && !memcmp(out, m4, n));
  assert(wf_m6(&s, m5, sizeof(m5), iv, out, &n));
  assert(n == sizeof(m6) && !memcmp(out, m6, n));
  memcpy(bad, m7, sizeof(m7));
  bad[sizeof(m7) - 1] ^= 1;
  assert(!wf_m7(&s, bad, sizeof(m7)));
  assert(wf_m7(&s, m7, sizeof(m7)));
  assert(!strcmp(s.key, "fixture-password"));
  for (size_t i = 0; i < sizeof(m1); i++)
    assert(!wf_m2(&s, m1, i, x, rn, out, &n));
  const uint8_t duplicate[] = {0x10, 0x22, 0, 1, 4, 0x10, 0x22, 0, 1, 4};
  assert(!wf_attr(duplicate, sizeof(duplicate), 0x1022, &n));
  WfBrute b = {0};
  char pin[9];
  assert(wf_brute_pin(&b, pin) && !strcmp(pin, "00000000"));
  assert(!wf_brute_next(&b, 0) && b.first == 0);
  assert(wf_brute_next(&b, 1) && b.first == 1);
  assert(wf_brute_next(&b, 2) && b.phase == 1 && b.second == 1);
  assert(!wf_brute_next(&b, 1));
  for (unsigned i = 0; i < 10000; i++) {
    WfBrute v = {.first = i};
    assert(wf_brute_pin(&v, pin));
    assert(wf_checksum(i * 1000) == (unsigned)(pin[7] - '0'));
  }
  b = (WfBrute){.first = 9999};
  assert(wf_brute_next(&b, 1));
  assert(!wf_brute_pin(&b, pin));
  b = (WfBrute){.first = 1234, .second = 999, .phase = 1};
  assert(wf_brute_next(&b, 2));
  assert(!wf_brute_pin(&b, pin));
  family(1, nonce1, hash1_1, hash2_1);
  family(2, nonce2, hash1_2, hash2_2);
  family(4, nonce4, hash1_4, hash2_4);
  family(5, nonce5, hash1_5, hash2_5);
  family(3, nonce3, hash1_3, hash2_3);
  /* Arbitrary RF inputs must not overread parser buffers. */
  uint32_t rng = 5;
  uint8_t raw[1700];
  WfTarget target;
  uint8_t ap[6] = {0}, sta[6] = {0};
  for (unsigned it = 0; it < 100000; it++) {
    rng = rng * 1664525 + 1013904223;
    size_t z = rng % sizeof(raw);
    for (size_t k = 0; k < z; k++) {
      rng = rng * 1664525 + 1013904223;
      raw[k] = rng >> 24;
    }
    if (it % 3 == 0 && z > 0)
      raw[0] = 0x80;
    if (it % 3 == 1 && z > 1) {
      raw[0] = 8;
      raw[1] = 2;
    }
    wf_beacon(raw, z, &target);
    wf_eap(raw, z, ap, sta);
    wf_tlv_valid(raw, z);
    wf_attr(raw, z, 0x1022, &n);
  }
  puts("WPS M1-M7, DH/KDF/HMAC/AES, rejection, brute progression and 100000 "
       "parser cases passed");
}
