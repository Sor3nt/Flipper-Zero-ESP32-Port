# Wardriver WDGWars upload

Wardriver 1.5.2 provides one **Uploads** entry for WiGLE and WDGWars together.
The working GPS mode/UART/pins are preserved. Logging now produces only the
GPS-valid Wi-Fi upload CSV shared by both destinations.

## Setup

Generate your own API key in your [WDGWars profile](https://wdgwars.pl/profile).
On the SD card create `apps_data/wardriver/wdgwars.conf` containing:

```text
API Key: YOUR_64_CHARACTER_HEX_KEY
```

Replace the placeholder with the actual 64 hexadecimal characters. Do not
put the WiGLE token here or add this key to the GPS `config`.
The **WDGWars API Key** editor on the Uploads screen can also save it. It opens blank deliberately;
the menu shows SET rather than revealing the saved key. The SD file is plaintext,
so protect your card. Keys are not included in release files or diagnostics.

1. Connect using the existing firmware Wi-Fi settings.
2. Stop the Wardriver survey; uploads cannot run while scanning.
3. Wardriver menu -> Uploads -> Reload keys from SD if you edited the files.
4. Choose GPS CSV -> select a saved `wardrive_*_wigle.csv`.
5. Upload to both -> confirm sharing networks and GPS with both services.

Back cancels an upload. After a timeout, interruption or uncertain result, check
your account before retrying; there is no automatic retry. "Accepted" means the
server reported acceptance, not that every row was new or earned credit.
The native app reports authentication/rate/file-limit/network failures.

## One action for both services

The single Uploads screen contains: Choose GPS CSV, Upload to both, WiGLE API
Name, WiGLE API Token, WDGWars API Key, Reload keys from SD, and Clear both keys.
There are no separate WiGLE/WDGWars upload menus. Existing credential files are
preserved; entering a key saves only that service, while reload reads both files.
WiGLE is attempted first, then WDGWars; a failure of one does not hide the
other's result. Turn the encoder on the result screen to see WiGLE 1/2 and
WDGWars 2/2. The two requests run sequentially to avoid simultaneous TLS buffers.
Missing credentials are caught before either request. Back cancels outstanding
work, but cannot undo an upload already accepted by a server. No automatic retry
occurs. Check both accounts before repeating a partial or uncertain upload:
the combined action attempts both destinations each time.

## Protocol and limits

Uses the platform's documented [CSV endpoint](https://www.wdgwars.pl/press):
HTTPS POST `https://wdgwars.pl/api/upload-csv`, `X-API-Key`,
multipart field `file`, WiGLE-1.6 CSV. Certificates/hostname are verified;
redirects are not followed, so keys cannot be forwarded to another host.
Only a strict top-level boolean `ok: true` is treated as accepted.

The existing WiGLE GPS-valid Wi-Fi CSV is streamed in 2 KiB chunks after
incremental validation. Local/BLE-only CSV files are not accepted. The logger
creates no file until an actual Wi-Fi observation has a valid fix and GPS UTC.
Fix loss pauses eligible rows; stale last-fix data is never substituted. Live
BLE/RID detection remains available but is not saved to an extra CSV format.
The upload CSV is buffered (4 KiB), flushed every five seconds and on session
close. Existing SD files are left untouched.
Local file cap 40 MiB; server limits may be lower. Response storage is limited
to 4 KiB, network-call timeout 10 seconds, overall deadline 10 minutes.
Cancellation is checked between blocking operations. No entire file is loaded
into RAM. HTTP runs through the existing WLAN worker.

Credentials are separate from WiGLE, with staged SD writes/backup recovery.
Clear both keys removes both services' config files and temporary/backup copies.
The temporary transfer and credential copies are erased on exit.

WiGLE HTTP 429 means too many uploads within its rate-limit window; its own
[Android uploader](https://github.com/wiglenet/wigle-wifi-wardriving/blob/main/wiglewifiwardriving/src/main/java/net/wigle/wigleandroid/background/ObservationUploader.java)
reports this condition as an upload-rate limit. No verified fixed quota or reset
interval is assumed here. Pause uploads and retry later; there is no retry loop.
The owner confirmed WDGWars uploads work with the correct CSV. The 1.5.2 menu
cleanup does not change either HTTPS endpoint, CSV format or response handling.

## Tests

The actual shared uploader/config/parser runs against fault-injecting SD/HTTP
mocks: multipart parsing, partial writes, API-key isolation, malformed CSV/JSON,
duplicate response fields, rejected auth, redirects, 413/429/5xx, cancellation,
deadline, no retries and complete cleanup. Production UI tests check both
destinations and require explicit confirmation. No real scan or key was uploaded
during development; live authenticated acceptance needs the owner's device/key.
