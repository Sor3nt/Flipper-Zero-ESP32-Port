# Documentation

This folder is where this project's hardware/firmware know-how lives — the kind
of context that doesn't belong in the main [README](../README.md) or
[FAQ](../FAQ.md) but is exactly what a contributor needs before touching
certain parts of the codebase. It will keep growing; new topics get their own
file here rather than piling into one giant document.

**Start with [00 — Hardware Testing Scope](00-hardware-testing-scope.md).** It
explains what has (and hasn't) actually been run on real hardware, and how to
report a bug with enough evidence to act on.

## Index

| # | Topic | Read this before touching… |
|---|---|---|
| [00](00-hardware-testing-scope.md) | **Hardware testing scope — read first** | Reporting *any* bug, on *any* board |
| [01](01-internal-ram.md) | Internal RAM (DRAM) pressure | Any new buffer, task, or allocation |
| [02](02-shared-spi-bus.md) | Shared SPI bus (paradoxt) | CC1101 / NRF24 / SD / LCD code |
| [03](03-power-management.md) | Power management invariants | Light sleep, deep sleep, BT idle, ship mode |
| [04](04-input-quirks.md) | paradoxt input quirks | Any new view, scene, or FAP |
| [05](05-ble-pairing.md) | BLE / pairing state | `ble_core`, `ble_serial`, Bluedroid code |
| [06](06-fap-development.md) | Adding a FAP (external app) | `applications_user/`, `firmware_api.c` |
| [07](07-tooling-gotchas.md) | Tooling / environment gotchas | Build scripts, Python edit scripts |
| [08](08-known-non-issues.md) | Claims already investigated — not bugs | Before re-reporting a "finding" |
| [09](09-commit-guidelines.md) | Where fixes tend to live + commit guidance | Before committing / opening a PR |

## The one rule above all the others

**Nothing in this codebase has been tested on hardware the maintainer doesn't
own** (see [00](00-hardware-testing-scope.md)), and **a large share of the
fixes catalogued in here were written during review/debug sessions and had
not yet been reflashed and confirmed on real hardware at the time they were
documented** — each file says so explicitly where it applies. Treat every
claim below as "believed correct from code-level analysis," not "proven on a
board," until you or someone else verifies it and updates the note.
