[← Back to index](README.md)

# Claims That Keep Getting Re-Reported — Already Investigated, Not Bugs

To save a future reviewer's time, these have each been checked against the
actual source and are **not** bugs on this port:

- **"`malloc()` doesn't zero memory" / "uninitialized struct field used as a
  pointer."** `components/furi/core/memmgr.h` unconditionally `#define`s
  `malloc(size)` to `calloc(1, size)` on this port (STM32's heap starts
  zeroed; ESP32's doesn't, so this is the compensating shim). Every `malloc`
  call site is effectively zero-initialized already.
- **"SPI driver bounces buffers through a stack copy — that's garbage/UB."**
  No: when a buffer isn't DMA-capable (e.g. it lives in PSRAM), ESP-IDF's SPI
  master driver automatically allocates an internal `MALLOC_CAP_DMA` bounce
  buffer and `memcpy`s into/out of it. This is correct, documented IDF
  behavior. The *actual* failure mode is that this per-transaction bounce
  allocation can fail under internal-RAM pressure (see
  [01 — internal RAM](01-internal-ram.md)) — that's the real bug to watch
  for, not the bounce mechanism itself.
- **"Register-polarity bug in NRF24's `read_reg`."** There is deliberately
  **no single global "SPI failed" sentinel value** in
  `nrf24_hw_read_reg()`. Different status bits have opposite
  active-high/active-low meaning (e.g. `RX_EMPTY` vs `TX_DS`), so a single
  safe-default return value is wrong for one or the other. Fail-safety is
  handled per call site on purpose.
- **"Mifare/KeeLoq/etc. keystore decoding is broken."** AES-encrypted
  manufacturer keystores (`keeloq_mfcodes`, `nice_flor_s`, `alutech_at_4n`)
  are genuinely **not decryptable** on this port — `furi_hal_crypto_*` is an
  identity pass-through stub (no Flipper Secure Enclave hardware exists
  here). This is a documented limitation, not a regression to chase.
- **"U2F Register should validate P1."** It deliberately doesn't — the U2F
  spec leaves P1 on Register undefined, and real browsers (Chrome sends
  `P1=0x03`/`0x83`) rely on that laxness. Don't add P1/P2 validation to U2F
  REGISTER.
