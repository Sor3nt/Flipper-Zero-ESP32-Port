# cSploit Mini 1.0 for T-Embed T-Embed Native Tools

An independent native C application inspired by selected cSploit/dSploit functions. It runs on the T-Embed ESP32-S3, with the existing firmware Wi-Fi connection. No Java/Android runtime, Raspberry Pi, Linux host or Linux security binaries are involved.

It is not a complete port of cSploit or dSploit. No Metasploit integration, exploit modules, password cracking, credential/hash harvesting, HTTPS decryption or IPv6 interception is included.

## Functions

| Module | Implemented behavior | Practical limit |
| --- | --- | --- |
| LAN discovery | TCP 80/443/445 probes, records successful connections or explicit TCP rejections | Local /24 constrained by the actual subnet; silence means unknown |
| TCP scan | Configurable inclusive port range, bounded nonblocking connections, basic SSH/FTP/SMTP banners | Service names inferred from port numbers are labelled guesses |
| DNS lookup | IPv4 A-record query to a configured resolver | UDP, 3-second bound, no recursive resolver implementation |
| HTTP inspection | HTTP HEAD with configured Host header | Plain HTTP, headers only, 4 KiB bound |
| TCP/UDP terminal | One text request with LF, displays replies | 5 seconds, 4 KiB; no persistent interactive shell |
| ARP relay test | Resolves one target and gateway, sends paired unicast ARP replies, relays selected IPv4 Ethernet frames | Same subnet, one target, max 5 minutes; stops on backpressure |
| DNS redirect inside relay | Exact configured name gets a forged IPv4 DNS answer; other traffic follows the relay | UDP/53 only; fragmented DNS is forwarded unchanged |
| Standalone DNS redirect | Exact configured-name A answer, 30-second TTL | Client must use this device as DNS; unmatched names are ignored |
| LLMNR responses | Exact-name UDP/5355 response after multicast-group registration | IPv4; UDP subset only, no TCP fallback or conflict-resolution engine |
| NBT-NS responses | Exact-name UDP/137 positive NB response | Unscoped 15-character names, workstation/file-server suffixes only |
| Exposure indicator | Plain HTTP connection and Authorization-header presence counts | Header values are discarded; no authentication challenge, hash or secret collection |

All protocol services require an explicit start action. ARP/name-response/exposure modules display a warning and require holding OK. The indicator is a limited exposure check, not a credential-capture feature.

## Installation and use

Copy `csploit_mini.fap` to `apps/WiFi/` on the SD card. Connect to the intended network using the firmware's Wi-Fi application first. Configure the target IP, exact name, reply IP and shared service port before starting a test. Turn selects items; OK opens/edits; hold OK commits an edited value; BACK stops an active worker and waits for cleanup.

The ARP relay and TCP connection-based tools require the included Native Tools firmware extension. On an older firmware, the FAP still loads and reports the missing extension. UDP name services and the exposure indicator use already-exported sockets. Configuration is session-only. Hold OK on the terminal screen to save the last 256 display lines under `apps_data/csploit_mini/`.

Only run on networks and devices you are authorized to test. ARP/DNS changes can interrupt traffic. BACK triggers three ARP-restoration reply pairs, then restores the normal Wi-Fi receive path. Transmission success does not prove the peers accepted the restoration. AP isolation, anti-spoofing, cached DNS, encrypted DNS and TLS certificate checks can prevent the behavior you expect.

DNS redirection changes a matching hostname's answer; it is not a blanket redirect of all traffic. HTTPS remains protected by certificate validation. The relay's `Forwarded` counter and `DNS-replies` count indicate frames accepted by the local transmit API, not delivery or browser success at the client.

## Validation

The ESP32-S3 FAP and firmware compile. Every FAP import resolves. Host AddressSanitizer/UBSan tests cover DNS/LLMNR/NBT-NS wire formats, exact-name filtering, truncation, IPv4/UDP checksum construction, ARP parsing and relay selection, and 50,000 malformed/random packets.

The actual worker code passed local loopback UDP response tests, repeated start/stop, a TCP scan and an HTTP exposure-indicator test that verified a synthetic secret did not appear in logs. ARP resolution, relay and restoration were tested against a simulated packet backend. These tests did not send ARP poisoning frames onto a real network.

**Not hardware verified:** T-Embed Wi-Fi receive-hook behavior, client ARP acceptance, multicast/broadcast reception, forwarding throughput, DNS cache effects, and restoration under radio disconnect. This release is ready for controlled device testing, not a claim of production reliability.

Run: `bash applications_user/csploit_mini/tests/run.sh`.

## References and provenance

The application was rewritten for ESP-IDF/Furi; it does not incorporate the Android applications' Java code or pretend to run their Linux executables.

- cSploit: https://github.com/cSploit/android
- dSploit: https://github.com/Androguide/dsploit
- DNS wire format: https://www.rfc-editor.org/rfc/rfc1035
- LLMNR: https://www.rfc-editor.org/rfc/rfc4795
- NetBIOS name service: https://www.rfc-editor.org/rfc/rfc1002
- ESP-IDF pinned version: v5.4.1

New application and packet-adapter source: MIT. Included font attribution is in `LICENSE.font`. Existing firmware retains its original licenses.
