#!/usr/bin/env python3
"""
Capture SubGHz RAW data streamed from Flipper Zero ESP32 port over serial.

Usage:
    python subghz_usb_capture.py /dev/ttyUSB0
    python subghz_usb_capture.py COM3 -b 115200 -o output.sub

Requires: pip install pyserial
"""
import serial
import sys
import argparse
from datetime import datetime


def main():
    parser = argparse.ArgumentParser(description="SubGHz RAW USB Capture")
    parser.add_argument("port", help="Serial port (e.g. /dev/ttyUSB0, COM3)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate")
    parser.add_argument("-o", "--output", default=None, help="Output .sub file (auto-generated if not set)")
    args = parser.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
    except serial.SerialException as e:
        print(f"Error opening {args.port}: {e}", file=sys.stderr)
        sys.exit(1)

    recording = False
    frequency = 0
    preset = ""
    raw_data_lines = []
    capture_count = 0

    print(f"Listening on {args.port} at {args.baud} baud...")
    print("Waiting for #SUBGHZ_RAW_START... (Press Ctrl+C to quit)")

    try:
        while True:
            raw_line = ser.readline()
            if not raw_line:
                continue

            try:
                line = raw_line.decode("utf-8", errors="ignore").strip()
            except Exception:
                continue

            if not line.startswith("#SUBGHZ_RAW_"):
                continue

            if line.startswith("#SUBGHZ_RAW_START:"):
                parts = line.split(":")
                if len(parts) >= 3:
                    frequency = int(parts[1])
                    preset = parts[2]
                    raw_data_lines = []
                    recording = True
                    print(f"\nRecording started: {frequency} Hz, preset: {preset}")

            elif line.startswith("#SUBGHZ_RAW_DATA:") and recording:
                data = line[len("#SUBGHZ_RAW_DATA:"):].strip()
                if data:
                    raw_data_lines.append(data)
                    sample_count = sum(len(l.split()) for l in raw_data_lines)
                    print(f"\r  Samples: {sample_count}", end="", flush=True)

            elif line.startswith("#SUBGHZ_RAW_STOP") and recording:
                recording = False
                capture_count += 1

                if args.output:
                    output = args.output if capture_count == 1 else f"{args.output.rsplit('.', 1)[0]}_{capture_count}.sub"
                else:
                    output = f"capture_{datetime.now():%Y%m%d_%H%M%S}.sub"

                total_samples = sum(len(l.split()) for l in raw_data_lines)

                with open(output, "w") as f:
                    f.write("Filetype: Flipper SubGhz RAW File\n")
                    f.write("Version: 1\n")
                    f.write(f"Frequency: {frequency}\n")
                    f.write(f"Preset: {preset}\n")
                    f.write("Protocol: RAW\n")
                    for data_line in raw_data_lines:
                        f.write(f"RAW_Data: {data_line}\n")

                print(f"\n  Saved: {output} ({total_samples} samples, {len(raw_data_lines)} data lines)")
                print("Waiting for next recording...")

    except KeyboardInterrupt:
        print("\nDone.")
    finally:
        ser.close()


if __name__ == "__main__":
    main()
