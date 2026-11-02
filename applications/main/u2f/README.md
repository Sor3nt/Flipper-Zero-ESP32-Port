# U2F

U2F is included in the Apps launcher for ESP32-S3 boards supported by this
port. It uses a dedicated USB HID FIDO interface; it cannot run at the same
time as the qFlipper/USB-Storage composite device. Close those USB modes before
opening U2F.

## Provisioning the attestation certificate

The Flipper factory attestation private key is not available on ESP32. The
bundled factory-key file is therefore intentionally rejected. Generate a
device-specific self-signed P-256 certificate and key file:

```sh
bash tools/gen_u2f_cert.sh /path/to/sd-card/u2f/assets
```

The output directory must map to `/ext/u2f/assets/` on the device and contain:

- `cert.der`
- `cert_key.u2f`

The firmware imports the plaintext key file on first start, authenticates and
encrypts it with AES-GCM, and then rewrites `cert_key.u2f`. The storage key is
derived from the chip's eFuse unique ID and MAC (HMAC-SHA256) and is also cached
in the dedicated `u2fkeys` NVS partition. Because it can be recomputed, erasing or
relocating `u2fkeys` or reflashing the device does not invalidate U2F data created
by this firmware. Older installations that stored a random key keep using that key
while it exists in `u2fkeys` (or in default NVS, which is migrated at boot). The boot
log prints `U2F storage key source=... id=XXXXXXXX`; the id must stay the same
across reboots. Remove the generated plaintext key from the computer
after provisioning. The self-signed certificate supports the U2F attestation
signature format, but relying parties that require a trusted attestation root
may reject it.

If the app reports that the factory certificate key is unsupported, replace
both files on the SD card at `/ext/u2f/assets/` with a matching generated pair.
The bundled Type-0 Flipper key cannot be decrypted by this ESP32 port.

## Security and compatibility

- The app implements U2F over CTAPHID, including INIT, PING, LOCK, WINK, MSG,
  physical user confirmation, and a persistent authentication counter. It does
  not implement CTAP2/WebAuthn resident credentials.
- Encrypted key and counter files are authenticated. Missing key/counter state
  is initialized on first use, but malformed, unauthenticated, or
  version-mismatched existing key state is rejected rather than silently
  replaced. Device-key and certificate-key writes use verified temporary files
  before promotion.
- The signature counter is stored in two alternating authenticated slots:
  `cnt.u2f` and `cnt.u2f.b`. Each new value is synced and read back before an
  authentication response is sent. Startup selects the highest valid slot;
  it initializes zero only when neither slot exists. If neither slot is valid
  but at least one exists, U2F refuses to start rather than resetting the
  counter. Keep both files when backing up or recovering U2F data.
- First-time key/counter creation is tracked by `/ext/u2f/.provisioning`.
  Initialization can resume when the existing key and counter artifacts are
  valid and the counter is zero. A partially written or invalid artifact is
  rejected even if the marker remains; recovery then requires backing up and
  re-provisioning the authenticator state. Do not remove this marker manually
  while setup is in progress.
- The storage key (derived, or an older random key in `u2fkeys`) is not protected
  from a physical flash dump unless ESP32 flash encryption is enabled. If an
  older *random* key was lost (full flash erase, changed partition layout), the
  log shows `U2F data authentication failed ... wrong storage key` and the files
  encrypted with it cannot be recovered. Remove `key.u2f`, `key.u2f.tmp`,
  `cnt.u2f`, and `cnt.u2f.b`, restore the original plaintext `cert_key.u2f`,
  regenerate the matching certificate/key pair if necessary, and provision and
  enroll again. Removing only the data files is insufficient because the
  certificate private key was encrypted with the lost key. After this one-time
  re-provisioning the key is derived and cannot be lost this way again.
- Randomness uses the ESP-IDF RNG API. It is used not only for first-time
  secrets but also for registration key-handle nonces and AES-GCM IVs during
  persistent counter writes. Its entropy/nonce uniqueness with Wi-Fi/BLE
  disabled has not been verified on these boards; this port does not claim
  high-assurance entropy in that condition. Do not enable SAR-ADC entropy
  without coordinating with the battery ADC consumer. Stack high-water use
  also remains to be measured on each supported board.
- The loader inhibits automatic light sleep for the full lifetime of a
  launched application, including U2F. This prevents the app's USB worker and
  active transfer from being suspended by the firmware's automatic light-sleep
  policy; it does not prevent a user-requested reset or power-off.
- Do not rely on this port for high-assurance authentication until USB
  enumeration and registration/authentication have been verified on hardware
  with the intended relying party.
