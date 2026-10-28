"""Verify the unchanged GPS/config/NMEA sources against this review snapshot."""
from pathlib import Path
import hashlib, json
root = Path(__file__).resolve().parents[2]
for name, digest in json.loads(Path(__file__).with_name('gps_baseline.json').read_text()).items():
    assert hashlib.sha256((root/name).read_bytes().replace(b'\r\n',b'\n')).hexdigest() == digest, name
print('PASS: known-good GPS/config/NMEA source snapshot preserved')
