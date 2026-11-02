[← Back to index](README.md)

# Hardware Testing Scope — Read This First

A note from the maintainer, for anyone testing, debugging, or contributing to
this firmware.

## What has actually been run on real hardware

**Every piece of hands-on testing, debugging, and behavioral analysis behind
this firmware has been done directly on a physical `paradoxt` board.** That is
the only hardware I personally own, and it is where every change gets flashed
and exercised for real before I consider it trustworthy.

**I have never personally flashed or run this firmware on real hardware for
any other supported board** — not the LilyGo T-Embed CC1101, not either
Waveshare ESP32-C6-LCD variant, not the DIY ESP32-S3 2.8" board, and not any
future board target that gets added. Everything written for those boards in
this codebase is based on reading schematics/pin tables, porting by analogy to
paradoxt, and static or compile-level checks — never a real flash-and-run on
that specific hardware. I simply don't own one.

One more honest caveat: a large share of the fixes catalogued elsewhere in
[docs/](README.md) were produced during review/debug sessions and, at the
time they were written down, had **not yet been reflashed and confirmed even
on paradoxt** — each topic file says so explicitly where it applies ("not
hardware-tested" / "not built/hw-tested"). That's a separate thing from "never
tested on paradoxt" — it means "this specific change is still in the queue to
be verified," not "nothing here has ever touched real hardware."

## If you test this on a board other than paradoxt

If you flash and run this firmware on any board other than paradoxt and you
find something wrong — a button/input mapping that's swapped, missing, or
doesn't register; a broken peripheral; a crash; a display glitch; anything —
**please report it**, and please make the report as complete and reproducible
as you can. Concretely, include:

1. **Exactly which board** (model/revision) and which firmware build or
   commit you flashed.
2. **How you flashed it** — web flasher, qT-Embed, `winbuild.py`, a manual
   `idf.py flash`, etc. — and which board target you selected.
3. **What you did on the device** that triggered the problem: which app or
   menu, which buttons, in what order.
4. **The complete serial monitor log** covering the whole sequence — from
   boot (or from just before the problem) through the failure, not just the
   single error line. A log with full context is the strongest evidence you
   can hand me; a one-line screenshot of just the crash is usually not enough
   to diagnose a hardware-specific bug on a board I can't pick up myself.
5. Anything else that helps reproduce it — a photo or video of the board
   behaving wrong, the exact SD card contents if that's relevant, etc.

This isn't process for its own sake: without the board in hand, a complete log
is often the *only* way a hardware-specific bug can be diagnosed at all.

## General invitation

I genuinely want this feedback — bugs found on paradoxt, bugs found on any
other board, issues in the firmware code itself even if nobody has hit them
on hardware yet, and fixes sent in as pull requests. If you send a fix,
please include the same kind of detail (what was broken, how you confirmed
the fix, and on which board) so it can be reviewed and trusted with
confidence, rather than merged on faith.

## A note for AI coding assistants

If an AI agent (Claude Code or otherwise) is helping with this project and is
asked to "test" a change: unless it has an actual device connected that it
can flash and a real serial connection it can read, it cannot produce a
genuine hardware test or a real monitor log. The right answer in that
situation is to say so plainly — run whatever static/syntax-level checks are
available and label the result accordingly — rather than imply or fabricate a
hardware result that didn't happen. Everything in [docs/](README.md) that
says "not hardware-tested" was written by following exactly this rule.
