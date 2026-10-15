import csv
import json
from pathlib import Path
import sys

p = Path(sys.argv[1])
doc = json.loads((p / "fields.json").read_text())
assert json.loads((p / "fields-pretty.json").read_text()) == doc
with (p / "fields.csv").open(newline="") as f:
    rows = list(csv.reader(f))
assert len(rows) == 1 and len(rows[0]) == 7
assert rows[0][0] == 'quoted"model\nsecond line'
assert json.loads(rows[0][6]) == doc
d = doc["data"]
assert len(d) == 16 and "absent" not in d
assert len(d["id"]) == 299 and len(d["long_text"]) == 6999
assert d["ints"] == [-123, 0, 2147483647]
assert d["doubles"] == [23.4, 1.2345678901234567, -0.00000012345678901234567]
assert d["precise"] == 1.2345678901234567 and d["not_finite"] is None
assert d["strings"] == ['quote" slash\\', 'degree ° C', 'line\nnext', 'emoji 🌡']
assert d["objects"] == [{"last_nested": 42}] and d["arrays"] == [[-123, 0, 2147483647], []]
assert d["nested"] == {"battery_ok": 1} and d["conditional"] == "present"
tpms = json.loads((p / "toyota.json").read_text())
assert tpms["data"] == dict(model="Toyota", type="TPMS", id="12345678", status=0,
                           pressure_PSI=35.5, temperature_C=24, mic="CRC")
assert json.loads((p / "toyota-pretty.json").read_text()) == tpms
print("PASS: independent JSON/CSV parse confirms all fields/values and all 7 Toyota TPMS fields")
