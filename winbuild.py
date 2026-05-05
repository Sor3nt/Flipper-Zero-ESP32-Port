#!/usr/bin/env python3
"""Windows build/flash/monitor helper for Flipper-Zero-ESP32-Port.

Replaces the legacy _winbuild_*.bat and _winbuild_monitor*.py scripts with a
single CLI. Designed for Windows + ESP-IDF v5.4.x. The bash-side build.sh is
unaffected.

Usage examples:
    python winbuild.py setup                 # install ESP-IDF python env (rare)
    python winbuild.py check                 # verify env activates
    python winbuild.py build                 # build T-Embed CC1101 firmware
    python winbuild.py build --board waveshare_c6
    python winbuild.py flash --port COM14
    python winbuild.py monitor --duration 60          # stream live output
    python winbuild.py monitor --duration 15 --reset  # pulse DTR/RTS reset (UART bridges only)
    python winbuild.py all --port COM14      # build + flash + monitor (boot log captured)

Env vars (overridable):
    ESP_IDF_DIR  - path to ESP-IDF (default: C:\\Espressif\\frameworks\\esp-idf-v5.4.1)
    ESPPORT      - serial port (default: COM14)
"""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

DEFAULT_ESP_IDF_DIR = r"C:\Espressif\frameworks\esp-idf-v5.4.1"
DEFAULT_PORT = "COM14"
DEFAULT_DURATION = 8.0

# Mirrors build.sh board mapping.
BOARDS = {
    "t_embed":      ("lilygo_t_embed_cc1101", "esp32s3", "build_t_embed"),
    "esp32s3":      ("esp32s3_generic",       "esp32s3", "build_s3"),
    "waveshare_c6": ("waveshare_c6_1.9",      "esp32c6", "build_waveshare_c6"),
}

REPO_ROOT = Path(__file__).resolve().parent


def get_esp_idf_dir() -> Path:
    p = Path(os.environ.get("ESP_IDF_DIR", DEFAULT_ESP_IDF_DIR))
    if not p.exists():
        sys.exit(
            f"ESP_IDF_DIR '{p}' does not exist. "
            "Set ESP_IDF_DIR or install ESP-IDF v5.4.x there.")
    return p


def get_port(arg_port):
    return arg_port or os.environ.get("ESPPORT", DEFAULT_PORT)


def env_for_idf(extra=None):
    env = os.environ.copy()
    env.pop("MSYSTEM", None)  # Git Bash detection breaks export.bat
    if extra:
        env.update(extra)
    return env


def run_with_idf_env(esp_idf_dir: Path, args: str, extra_env=None) -> int:
    # shell=True is required on Windows so cmd.exe parses the && and the quoted path
    # in `call "..."`; passing as a list mangles the quoting.
    cmd = f'call "{esp_idf_dir}\\export.bat" && idf.py {args}'
    return subprocess.run(cmd, shell=True, env=env_for_idf(extra_env), cwd=str(REPO_ROOT)).returncode


def cmd_setup(args):
    esp_idf_dir = get_esp_idf_dir()
    idf_tools = esp_idf_dir / "tools" / "idf_tools.py"
    if not idf_tools.exists():
        sys.exit(f"{idf_tools} not found.")
    # Match _winbuild_step1.bat: unset MSYSTEM, set IDF_PATH so idf_tools resolves
    # python_requirements.txt and friends from the right ESP-IDF tree.
    env = env_for_idf({"IDF_PATH": str(esp_idf_dir)})
    return subprocess.run(
        [sys.executable, str(idf_tools), "install-python-env"], env=env).returncode


def cmd_check(args):
    return run_with_idf_env(get_esp_idf_dir(), "--version")


def cmd_build(args):
    flipper_board, target, build_dir = BOARDS[args.board]
    common = f"-B {build_dir} -DFLIPPER_BOARD={flipper_board}"
    esp_idf_dir = get_esp_idf_dir()
    # FLIPPER_BOARD must also be in the env: fam_config.py reads it via
    # os.environ to filter board-incompatible apps (e.g. NFC/IR on Waveshare C6).
    extra = {"FLIPPER_BOARD": flipper_board}
    rc = run_with_idf_env(esp_idf_dir, f"{common} set-target {target}", extra)
    if rc != 0:
        return rc
    return run_with_idf_env(esp_idf_dir, f"{common} reconfigure build", extra)


def cmd_flash(args):
    flipper_board, _, build_dir = BOARDS[args.board]
    port = get_port(args.port)
    return run_with_idf_env(
        get_esp_idf_dir(),
        f"-B {build_dir} -DFLIPPER_BOARD={flipper_board} -p {port} flash",
        {"FLIPPER_BOARD": flipper_board})


def _open_serial_clean(port, timeout_s=5.0):
    # Pre-pin DTR/RTS LOW before open so Windows' default assert-on-open
    # doesn't accidentally drive the chip into download mode. Retry through
    # USB re-enumeration windows.
    try:
        import serial
    except ImportError:
        sys.exit("pyserial not installed. Run: pip install pyserial")
    deadline = time.time() + timeout_s
    last_err = None
    while time.time() < deadline:
        try:
            s = serial.Serial()
            s.port = port
            s.baudrate = 115200
            s.timeout = 1
            s.dtr = False
            s.rts = False
            s.open()
            return s
        except serial.SerialException as e:
            last_err = e
            time.sleep(0.3)
    sys.exit(f"Could not open {port}: {last_err}")


def cmd_monitor(args):
    port = get_port(args.port)
    ser = _open_serial_clean(port)
    if args.reset:
        # DTR=False keeps IO0 high (normal-boot strap); RTS pulse asserts/releases EN.
        # Only works on USB-UART bridges, not on native USB-Serial/JTAG.
        ser.dtr = False
        ser.rts = True
        time.sleep(0.1)
        ser.rts = False
    start = time.time()
    try:
        while time.time() - start < args.duration:
            data = ser.read(4096)
            if data:
                sys.stdout.buffer.write(data)
                sys.stdout.buffer.flush()
    except KeyboardInterrupt:
        pass
    finally:
        ser.close()
    return 0


def cmd_all(args):
    rc = cmd_build(args)
    if rc != 0:
        return rc
    rc = cmd_flash(args)
    if rc != 0:
        return rc
    return cmd_monitor(args)


def build_parser():
    p = argparse.ArgumentParser(
        prog="winbuild.py",
        description="Windows build/flash/monitor helper for Flipper-Zero-ESP32-Port.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "Boards: " + ", ".join(BOARDS.keys()) + "\n"
            "Defaults: board=t_embed, port=$ESPPORT or COM14, duration=8s.\n"
            "Override ESP-IDF location via ESP_IDF_DIR env var.\n"
            "monitor does not reset by default; pass --reset on USB-UART bridges, or use\n"
            "`flash` (or `all`) to capture boot logs (flash performs a real hard reset)."))
    sub = p.add_subparsers(dest="command", required=True)

    sub.add_parser("setup", help="Install ESP-IDF python env (one-time)."
                   ).set_defaults(func=cmd_setup)
    sub.add_parser("check", help="Verify env activation + idf.py --version."
                   ).set_defaults(func=cmd_check)

    pb = sub.add_parser("build", help="Build firmware.")
    pb.add_argument("--board", choices=BOARDS, default="t_embed")
    pb.set_defaults(func=cmd_build)

    pf = sub.add_parser("flash", help="Flash firmware over USB.")
    pf.add_argument("--board", choices=BOARDS, default="t_embed")
    pf.add_argument("--port", default=None, help="Serial port (default: $ESPPORT or COM14)")
    pf.set_defaults(func=cmd_flash)

    pm = sub.add_parser("monitor", help="Stream serial output for N seconds.")
    pm.add_argument("--port", default=None, help="Serial port (default: $ESPPORT or COM14)")
    pm.add_argument("--duration", type=float, default=DEFAULT_DURATION,
                    help=f"Capture seconds (default: {DEFAULT_DURATION})")
    pm.add_argument("--reset", action="store_true",
                    help="Pulse DTR/RTS for a hard reset on attach "
                         "(USB-UART bridges only -- not supported on "
                         "ESP32-S3 native USB-Serial/JTAG)")
    pm.set_defaults(func=cmd_monitor)

    pa = sub.add_parser("all", help="build + flash + monitor.")
    pa.add_argument("--board", choices=BOARDS, default="t_embed")
    pa.add_argument("--port", default=None)
    pa.add_argument("--duration", type=float, default=DEFAULT_DURATION)
    # `flash` already performs a real hard reset via the ROM bootloader,
    # so the follow-up monitor never needs (and on USB-Serial/JTAG must not
    # attempt) its own reset pulse.
    pa.set_defaults(func=cmd_all, reset=False)

    return p


if __name__ == "__main__":
    args = build_parser().parse_args()
    sys.exit(args.func(args))
