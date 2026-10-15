# T-Embed Native Tools — local video playback

The video player was originally developed from the October 4 source snapshot
`135627e8a0747620cca9262d66d81d27156a0d1f`. It adds local video to the existing
Streaming app. It retains the active pi3 SDK settings, the installed NRF/BW16 I2C
profile, Flip Pi, Wardriver, WDGWars and ProtoPirate.

## Why MP4 only had Stream

In pi3, `scene_action_menu.c` adds Play only for `MediaKindAudio` (MP3).
The local backend decodes MP3 to the speaker. MP4 is served to Google Cast or
DLNA receivers; it is not decoded on the T-Embed display. There is no hidden
local MP4 setting to enable.

## Use the new player

1. Build and flash the matching T-Embed Native Tools firmware.
2. Copy the contents of `SD/` to your SD card's root. Do not delete existing files.
3. Open **Streaming**, select `video-demo.tvid`, then **Play on screen**.
4. Press the encoder once to pause/resume. Rotate to change volume in 5% steps.
   Back, or a long encoder press, stops and returns to the file's menu.

The paused symbol appears at the upper left. The temporary white bar near the
bottom shows volume. The thin green line at the bottom shows progress.
Playback ends automatically. Select Play on screen again to replay.

TVID files also open from the ordinary SD file browser. Keep them in
`apps_data/medien/` so the Streaming app finds them immediately.

## Convert your own MP4, MKV or other local video

Use a computer with Python 3.9 or newer and FFmpeg/FFprobe installed on PATH.
No additional Python packages are needed for conversion.
Inside the release ZIP's `converter/` directory, on Windows:

```powershell
py .\convert_video.py "C:\Videos\movie.mp4" "C:\Videos\movie.tvid"
```

On Linux, macOS, or Termux with Python and FFmpeg installed:

```bash
python3 convert_video.py movie.mp4 movie.tvid
```

Copy `movie.tvid` to `apps_data/medien/` on the SD card, then choose
**Streaming → movie → Play on screen**. Conversion does not change the input.
Existing output files are refused unless `--force` is given.

Options: `--mute` removes audio; `--fps 5` reduces decoding demand;
`--fps 20` requests a higher frame rate (device performance unverified).
Default: 320 × 170, 10 fps, JPEG quality 5, 16 kHz mono PCM audio. The converter
preserves the original aspect ratio with black bars. Files without an audio
stream work as silent videos. Use `--quality 8` for smaller frames; lower numbers
give sharper JPEGs. Only unencrypted local files FFmpeg can read are supported.

## Scope and limitations

- This is a local full-colour MJPEG/PCM player using `.tvid` files, not a native
  H.264/H.265/MP4 decoder. MP4 still offers Stream plus Local video help.
- Conversion runs on the computer, not on the T-Embed or Pi. Wi-Fi and a Pi are
  not required to play a converted file from SD.
- Pause/resume, volume, stop and a progress indicator are implemented.
  Seeking, subtitles and playlists are not implemented in this version.
- Playback is designed for 10 fps. Actual frame rate, audio/video alignment,
  button mapping and long playback need testing on the physical device.
- Pause/stop response can include a short audio tail from the 64 ms I2S DMA
  buffer. Slow SD reads can cause stutter or audio gaps.
- JPEG and colour frame buffers use PSRAM. The LCD DMA stripe is 5,120 bytes;
  JPEG workspace is 4,096 bytes. Memory allocation failures and invalid files
  return an error message and release resources.

## Validation performed

- ESP-IDF 5.4.1 / ESP32-S3 build passed. App: 3,555,952 bytes in a 6,225,920-byte
  partition (43% free). Image checksum and SHA-256 trailer verified by esptool.
- Every active sdkconfig key matches pi3. No partition-layout change.
- All five original FAPs resolve their imports against the firmware API table.
  Original SD apps, plugins and Pi companion files are retained byte-for-byte.
- Real FFmpeg conversion tested at 5, 10 and 20 fps and without audio. Every
  output JPEG decoded on the host at 320 × 170; PCM length and 440 Hz tone checked.
- Production C container parser checked with valid media, truncation, header
  bit mutations and 100,000 random malformed inputs per test file under ASAN/UBSAN.
- Production player tested with mocked hardware for repeated playback, memory
  failures, display/audio/decoder failures, missing files, colour byte order,
  stereo expansion, pause/resume, volume and stop. Resource ownership returns
  to its initial state after every test.
- Physical hardware, SD throughput, actual ROM JPEG decoding and speaker output
  have **not** been tested. Host mocks do not establish hardware compatibility.

## Rebuild and rerun tests

Activate ESP-IDF 5.4.1. The build used GCC 14.2.0_20241119,
esptool 4.12.0 and idf-component-manager 2.2.2, with the existing dependency lock.
From the source directory in an activated bash shell:

```bash
idf.py -B build_video -D SDKCONFIG="$PWD/sdkconfig.video" -D FLIPPER_BOARD=lilygo_t_embed_cc1101 build
```

Host tests require GCC, FFmpeg and Pillow (Pillow is only needed by tests):

```bash
ASAN_OPTIONS=detect_leaks=0 python3 tests/video/test_converter.py
ffmpeg -f lavfi -i testsrc2=size=640x360:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 3 -c:v mpeg4 -c:a aac test.mp4
python3 tools/convert_video.py test.mp4 test.tvid
ASAN_OPTIONS=detect_leaks=0 python3 tests/video/test_player.py test.tvid
```

LeakSanitizer needs process introspection unavailable in this build environment;
address and undefined-behaviour sanitizers remain enabled.

## TVID v1 format

All multibyte fields are little-endian. The 32-byte header is
`<8sHHHHIIII`: magic `TVID0001`, width 320, height 170, fps (5, 10 or 20),
audio flag (0 or 1), sample rate 16000, frame count, largest JPEG size, reserved 0.
Each frame is `<II` (JPEG byte count, PCM byte count), JPEG bytes, then PCM bytes.
PCM is signed 16-bit little-endian mono; audio frames have exactly
`16000 / fps` samples. Silent frames have no PCM bytes. Maximum JPEG: 65,536 bytes;
maximum frames: 432,000. The ROM decoder consumes baseline JPEG with standard
Huffman tables. The converter explicitly requests those tables from FFmpeg.

Implementation references: [ESP32-S3 ROM JPEG API](https://github.com/espressif/esp-idf/blob/v5.4.1/components/esp_rom/esp32s3/include/esp32s3/rom/tjpgd.h),
[FFmpeg documentation](https://ffmpeg.org/ffmpeg.html).

New player, container and converter code is MIT licensed; existing firmware
and third-party components retain their original licences.
