# Source provenance

This source-built controller uses GPL-3.0-or-later licensing. It implements the
R4TKN v4 UART protocol independently and is not an upstream R4TKN release.

`radio_amebad_319.cpp` adapts the raw management-frame adapter and deauth frame
layout from tesa-klebeband's GPL-3.0 project:
https://github.com/tesa-klebeband/RTL8720dn-WiFi-Packet-Injection/tree/8e722c79cbe08f2ff23eb4e428290de9ecb171a6

The original files are `packet-injection.cpp` and `packet-injection.h` at the
pinned commit above. Their license is included as `LICENSE`.

Only the raw adapter and existing deauth frame layout are used. The unused
beacon generator is omitted. Driver offsets require **Realtek AmebaD 3.1.9**;
the build script rejects a different package version. The SDK's prebuilt
libraries and Arduino core retain their own respective licenses.

The native scheduler runs only an explicitly validated cached selection, for
at most 30 seconds, with one existing three-frame group per 100 ms visit.
It uses no packet-format, transmit-power, or radio-driver bypass patches.
Both bands still share one radio; effectiveness requires hardware validation.
