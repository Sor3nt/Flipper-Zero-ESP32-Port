#include "rtl_scan.h"
#include <assert.h>
#include <stdio.h>

int main(void) {
  RtlScan s;
  uint32_t now = UINT32_MAX - 100000u; /* first dwell crosses timer wrap */
  rtl_scan_init(&s, now, RTL_PROFILE_ALL);
  unsigned seen = 0, variants = 0;
  uint32_t cycle = 0, first_probe = s.probe_us;
  for (unsigned i = 0; i < 72; i++) {
    unsigned p = rtl_scan_profile(s.mode, s.step);
    assert(p < 6 && !(seen & (1u << p)));
    seen |= 1u << p;
    assert(s.probe_us >= 250000 && s.probe_us <= 310000);
    assert(!rtl_scan_due(&s, now + s.probe_us - 1, false, true));
    assert(rtl_scan_due(&s, now + s.probe_us, false, true));
    /* Pending capture/queue must survive the fast dwell; sustained noise must
     * yield even with a queue that never drains. Same for a valid long burst. */
    assert(!rtl_scan_due(&s, now + s.probe_us, true, true));
    assert(!rtl_scan_due(&s, now + s.probe_us, false, false));
    assert(rtl_scan_holding(&s, now + s.probe_us, true));
    assert(!rtl_scan_due(&s, now + 2999999, true, false));
    assert(rtl_scan_due(&s, now + 3000000, true, false));
    assert(!rtl_scan_holding(&s, now + 3000000, true));
    cycle += s.probe_us;
    variants += s.probe_us != first_probe;
    now += s.probe_us + 10000; /* radio setup time belongs outside the dwell */
    bool boundary = rtl_scan_advance(&s);
    assert(boundary == (i % 6 == 5));
    if (boundary) {
      assert(seen == 63 && cycle >= 1500000 && cycle <= 1860000);
      seen = 0; cycle = 0;
    }
    rtl_scan_enter(&s, now);
  }
  assert(variants > 60);
  puts("PASS: 12 all-profile cycles, 1.50-1.86 s quiet airtime per cycle, phase variation, burst/queue hold, 3 s noise ceiling, timer wrap and no profile starvation");
  rtl_scan_init(&s, UINT32_MAX - 500000u, RTL_PROFILE_AUTO);
  for(unsigned i=0;i<12;i++) {
    assert(rtl_scan_count(s.mode)==2);
    assert(rtl_scan_profile(s.mode,s.step)==(i%2?5:1));
    assert(s.probe_us==2000000);
    assert(!rtl_scan_due(&s,s.entered_us+1999999,false,true));
    assert(rtl_scan_due(&s,s.entered_us+2000000,false,true));
    assert(!rtl_scan_due(&s,s.entered_us+2500000,true,true));
    assert(rtl_scan_due(&s,s.entered_us+3000000,true,false));
    assert(rtl_scan_advance(&s)==(i%2==1));
    rtl_scan_enter(&s,s.entered_us+2000000);
  }
  puts("PASS: original AUTO matches first-build OOK650/FSK40 order, 2-second dwells, 3-second ceiling and two-profile band boundary including timer wrap");
}
