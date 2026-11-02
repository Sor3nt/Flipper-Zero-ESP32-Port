[← Back to index](README.md)

# Power Management Invariants — Do Not "Simplify" These

These were set deliberately and tested against real battery/charger behavior
on paradoxt. If a change conflicts with one of the rules below, that's a sign
to stop and ask, not to proceed.

- **BQ25896 ship-mode shutdown path is final — never modify it.** Calling it
  is fine; changing *how* it's called is not, without hardware
  re-validation.
- **Light sleep must never poll VBUS/charger registers.** Use the VBUS value
  cached right before sleeping.
- **Bluetooth that is enabled but idle (advertising only, no
  connection/RPC/pairing/beacon) must be stopped while light-sleeping and
  restarted on wake.** Never touch BT that is connected or actively in use.
- **paradoxt has no battery ADC** (that's smoochie-only) — it uses the
  BQ27220 fuel gauge. Don't add ADC init on paradoxt.
- **Deep sleep is entered only when shutting down with USB connected**, and
  must leave no floating pins. Light sleep likewise must leave every
  connected pad in a defined state (WS2812 blacked out + data line held low,
  CC1101 in SLEEP with GDO pins at a defined level, PN532 in PowerDown).
- **Light sleep must never engage while an app/feature is actively running**,
  and waking must not degrade performance or stability.
- **Any new periodic timer or poll in a service must pause itself on the
  idle pubsub** (`furi_hal_power_get_idle_pubsub()`, idle=true) and resume on
  wake. A periodic FreeRTOS timer left running is a wakeup source no matter
  how "lightweight" it looks — several already-shipped services (desktop
  tick, storage tick, the Expansion listener, the USB-LED auto-off timer) had
  to be retrofitted for exactly this.
- **Never hold `light_sleep_inhibit` for a passive/background listener.**
  Only hold it while something is actually, synchronously using the
  hardware.
- **The global light-sleep inhibit counter is a plain `enter`/`exit` tally
  with no reset.** If you add a new inhibit call site, the enter and exit
  *must* be provably balanced on every code path, including error paths. A
  real crash: non-atomic guard-flag logic let two concurrent contexts (a BT
  host-stack callback thread and a service-teardown thread) both think they
  owned the "currently inhibited" flag and both call `exit()` — the counter
  underflowed and `furi_check` aborted in a completely unrelated thread
  (`ExpansionDetect`) that was just the next innocent caller to hit the
  check. If a new subsystem needs its own inhibit guard flag shared across
  threads, claim the enter/exit *transition* inside a critical section (and
  do the actual power-API call outside it, since that takes a mutex) — don't
  rely on a plain bool.
- Known, accepted limitation (not a bug to "fix" casually): a few long-lived
  inhibitors by design keep the gate closed — WiFi started (even
  disconnected), a connected Expansion module, an active RFID/USB-UART
  session, the Find My Flipper beacon. That's intentional.
