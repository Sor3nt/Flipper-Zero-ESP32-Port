"""Check retained power implementation against the imported source snapshot."""
from pathlib import Path
import hashlib, json
root=Path(__file__).resolve().parents[2]
for path, expected in json.loads(Path(__file__).with_name('power_baseline.json').read_text()).items():
    assert hashlib.sha256((root/path).read_bytes().replace(b'\r\n',b'\n')).hexdigest()==expected, path
print('PASS: retained shutdown source snapshot; physical power-off acceptance is separate')
