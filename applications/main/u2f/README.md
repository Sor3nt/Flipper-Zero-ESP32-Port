# U2F

U2F is included in the Apps launcher for ESP32-S3 boards supported by this
port. It uses a dedicated USB HID FIDO interface; it cannot run at the same
time as the qFlipper/USB-Storage composite device. When qFlipper is active, U2F
pauses its bridge and releases the composite USB device before taking ownership;
the bridge is restarted when U2F exits. Other composite owners, including active
USB-Storage/MSC, are not preempted, so stop/eject them before opening U2F.

## Attestation certificate

The app needs an attestation certificate and its private key to answer U2F
registration requests. The Flipper factory attestation private key is not
available on ESP32, so the bundled factory-key file (certificate `Type 0`) is
intentionally rejected and cannot be decrypted by this port.

### Generating a self-signed certificate on the device (default)

When the app opens and finds **no usable** attestation certificate, it shows a
**"No U2F key installed"** screen with a **Generate** button instead of an
error. Press OK to create a device-specific, self-signed P-256 (`secp256r1`)
certificate and key directly on the device; the app then initializes and is
ready to use. No PC-side tooling or file copying is required.

The generated pair is stored at `/ext/u2f/assets/`:

- `cert.der` — the self-signed X.509 certificate (DER)
- `cert_key.u2f` — the matching private key

The serial log shows `Generating self-signed U2F attestation certificate`. The
generated key is written as a plaintext key file and then immediately
authenticated and re-encrypted with AES-GCM (see the key-storage note below),
exactly like an imported key.

This self-signed certificate supports the U2F attestation signature format and
works with the common relying parties (e.g. GitHub, Google, WebAuthn test
pages). Relying parties that require a *trusted* attestation root may reject it;
in that case supply your own certificate (next section).

> Pressing **Generate** is an explicit action and replaces any leftover or
> broken `cert.der`/`cert_key.u2f` (including a stale `cert_key.u2f.tmp`) in
> `/ext/u2f/assets/` with a fresh pair. It is only offered when no usable
> certificate is present, so a valid, working certificate is never affected. To
> start over from a working authenticator, delete the attestation files (see the
> recovery note under "Security and compatibility") and reopen the app.

### Installing your own certificate (optional)

To use a certificate you control instead of the auto-generated one, place a
matching pair on the SD card **before** opening the app for the first time (or
after deleting the auto-generated pair). The directory must map to
`/ext/u2f/assets/` on the device and contain:

- `cert.der` — your attestation certificate, DER-encoded, with a P-256
  (`secp256r1`) public key, at most 2048 bytes
- `cert_key.u2f` — the matching private key as a Flipper U2F certificate-key
  file

`cert_key.u2f` is a Flipper-format key file. Two forms are accepted:

- **Plaintext (`Type 2`, recommended for import):**

  ```
  Filetype: Flipper U2F Certificate Key File
  Version: 1
  Type: 2
  Data: <32 bytes of the raw P-256 private scalar, hex, space-separated>
  ```

  The firmware imports this on first start, verifies that the key matches the
  public key in `cert.der`, encrypts it with AES-GCM, and rewrites
  `cert_key.u2f` as the encrypted form (`Type 1`). Remove the plaintext copy
  from your computer after provisioning.

- **Encrypted (`Type 1`):** a key already encrypted by this firmware/device.
  Only valid on the same device that produced it (the storage key is
  device-derived; see below).

The certificate's public key must correspond to the private key, otherwise the
app refuses to start (`Attestation key/certificate validation failed`).

### Certificate-key storage

The storage key that encrypts `cert_key.u2f` is derived from the chip's eFuse
unique ID and MAC (HMAC-SHA256) and is also cached in the dedicated `u2fkeys`
NVS partition. Because it can be recomputed, erasing or relocating `u2fkeys` or
reflashing the device does not invalidate U2F data created by this firmware.
Older installations that stored a random key keep using that key while it exists
in `u2fkeys` (or in default NVS, which is migrated at boot). The boot log prints
`U2F storage key source=... id=XXXXXXXX`; the id must stay the same across
reboots.

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
  `cnt.u2f`, and `cnt.u2f.b`, and also remove `cert.der` + `cert_key.u2f` so a
  fresh self-signed pair is generated on the next start (or restore your own
  plaintext `cert_key.u2f` + matching `cert.der`), then provision and enroll
  again. Removing only the data files is insufficient because the certificate
  private key was encrypted with the lost key. After this one-time
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
