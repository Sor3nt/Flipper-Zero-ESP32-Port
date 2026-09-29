"""Test the production RF sampler and color compositor with mocked hardware."""
from pathlib import Path
from host_build import run
import sys
import struct
import zlib

root = Path(__file__).resolve().parents[2]
tests = Path(__file__).parent
out = root / "build_t_embed/host_tests"
out.mkdir(parents=True, exist_ok=True)
helpers = root / "applications/main/subghz/helpers"
run([sys.executable, str(tests / "check_protected_baseline.py")], check=True)

def compile_test(name, sources):
    binary = out / (name + "_test")
    run(["gcc", "-std=c17", "-Wall", "-Wextra", "-Werror", "-O1",
         "-fsanitize=address,undefined", "-I" + str(tests), "-I" + str(helpers),
         *map(str, sources), "-o", str(binary)], check=True)
    return binary

run([str(compile_test("spectrum_math", [tests / "spectrum_math_test.c"]))], check=True)
worker = (helpers / "subghz_spectrum_worker.c").read_text(encoding="utf-8")
worker = "\n".join(line for line in worker.splitlines() if not line.startswith("#include"))
worker_generated = out / "spectrum_worker_host.c"
worker_generated.write_text('#include "spectrum_worker_backend.h"\n' + worker + "\n" +
    (tests / "spectrum_worker_test.c").read_text(), encoding="utf-8")
run([str(compile_test("spectrum_worker", [worker_generated]))], check=True)

source = (root / "applications/main/subghz/views/subghz_spectrum.c").read_text(encoding="utf-8")
source = source.split("static int32_t subghz_spectrum_render_thread", 1)[0]
source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
generated = out / "spectrum_render_host.c"
generated.write_text('#include "spectrum_render_backend.h"\n' + source + "\n" +
    (tests / "spectrum_render_test.c").read_text(), encoding="utf-8")
preview = out / "spectrogram_synthetic.rgb"
run([str(compile_test("spectrum_render", [generated])), str(preview)], check=True)
rgb = preview.read_bytes()
assert len(rgb) == 320 * 170 * 3
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
scanlines = b"".join(b"\x00" + rgb[row * 960:(row + 1) * 960] for row in range(170))
(out / "spectrogram_synthetic.png").write_bytes(b"\x89PNG\r\n\x1a\n" +
    chunk(b"IHDR", struct.pack(">IIBBBBB", 320, 170, 8, 2, 0, 0, 0)) +
    chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))
