[← Back to index](README.md)

# Where Fixes Tend to Live, and Commit Guidance

## Where to look first

If you're about to touch one of these subsystems, assume there is prior
context worth reading in this `docs/` folder before you start — the paradoxt
board file, the firmware's power/light-sleep code path, the Sub-GHz/NRF24
shared-SPI radio stack, the BLE/Bluedroid security path, and the WLAN app's
scan/deauth workers have each been through multiple rounds of real bug fixes
(crashes, freezes, silent failures), and each round left reasoning that is
easy to accidentally undo with a well-intentioned "simplification." When in
doubt, understand "why did it end up this way" before changing it — see
[00](00-hardware-testing-scope.md), [01](01-internal-ram.md),
[02](02-shared-spi-bus.md), [03](03-power-management.md) and
[05](05-ble-pairing.md).

## Before you commit

A large amount of this project's history exists only as working-tree changes
and the reasoning captured in this `docs/` folder — not as commit messages.
When committing:

- Commit in reviewable, subsystem-sized chunks with messages that capture the
  **why** (the root cause), not just the **what** — a bare "fix bug" message
  throws away exactly the context this documentation is trying to preserve.
- Say plainly in the commit message / PR description whether the change has
  been confirmed on real hardware, and on which board (see
  [00 — hardware testing scope](00-hardware-testing-scope.md)). A downstream
  contributor who assumes "merged" means "verified on a board" will waste
  time chasing a bug that was already known-and-flagged as untested.
- If you fix something documented in one of these files as "not yet
  confirmed on hardware," update that file once you've verified it, so the
  note doesn't keep telling the next person to re-check something that's
  already settled.
