#!/usr/bin/env python3
"""Run production application code: GCC+sanitizers on Linux, native Zig on Windows."""
from pathlib import Path
from host_build import run as run_native
import os
import subprocess
import csv
import tempfile
import re
import sys
ROOT=Path(__file__).resolve().parents[2]
APP=ROOT/'applications_user/wardriver'
OUTPUT=ROOT/'build_t_embed/host_tests'
OUTPUT.mkdir(parents=True,exist_ok=True)
run_native([sys.executable,str(Path(__file__).with_name('run_radio_lifecycle_tests.py'))],check=True)
run_native([sys.executable,str(Path(__file__).with_name('run_wigle_tests.py'))],check=True)
run_native([sys.executable,str(Path(__file__).with_name('test_gps_pins.py'))],check=True)
run_native([sys.executable,str(Path(__file__).with_name('test_startup.py'))],check=True)
sources=[APP/name for name in ('wardriver_nmea.c','wardriver_detect.c','wardriver_table.c','wardriver_csv.c')]
binary=OUTPUT/'wardriver_test'
run_native(['gcc','-std=c17','-Wall','-Wextra','-Werror','-g','-O1',
    '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(APP),
    str(Path(__file__).with_name('wardriver_test.c')),*map(str,sources),'-lm','-o',str(binary)],check=True)
run_native([str(binary)],check=True,env={**os.environ,'ASAN_OPTIONS':'detect_leaks=1:halt_on_error=1'})

HOST=Path(__file__).with_name('wardriver_host')
config_binary=OUTPUT/'wardriver_config_test'
run_native(['gcc','-std=c17','-Wall','-Wextra','-Werror','-g','-O1',
    '-fsanitize=address,undefined','-ffunction-sections','-fdata-sections',
    '-I'+str(HOST),'-I'+str(APP),
    str(Path(__file__).with_name('wardriver_config_test.c')),str(APP/'wardriver_config.c'),
    '-Wl,--gc-sections','-o',str(config_binary)],check=True)
run_native([str(config_binary)],check=True)
oui_binary=OUTPUT/'wardriver_oui_test'
run_native(['gcc','-std=c17','-Wall','-Wextra','-Werror','-g','-O1',
    '-fsanitize=address,undefined','-I'+str(HOST),'-I'+str(APP),
    str(Path(__file__).with_name('wardriver_oui_test.c')),str(APP/'wardriver_oui.c'),
    '-o',str(oui_binary)],check=True)
run_native([str(oui_binary)],check=True)
database=ROOT/'build_t_embed/sdcard/apps_data/wifi/mac-vendor.txt'
if database.exists(): run_native([str(oui_binary),str(database)],check=True)
storage_binary=OUTPUT/'wardriver_storage_test'
run_native(['gcc','-std=c17','-Wall','-Wextra','-Werror','-g','-O1',
    '-fsanitize=address,undefined','-I'+str(HOST),'-I'+str(APP),
    str(Path(__file__).with_name('wardriver_storage_test.c')),str(APP/'wardriver_storage.c'),
    str(APP/'wardriver_csv.c'),str(APP/'wardriver_nmea.c'),str(APP/'wardriver_table.c'),
    '-lm','-o',str(storage_binary)],check=True)
with tempfile.TemporaryDirectory() as temp:
    run_native([str(storage_binary),temp],check=True)
    logs=Path(temp)
    with (logs/'wardrive_1790118000_00000001_local.csv').open(newline='') as f:
        rows=list(csv.DictReader(f))
    assert len(rows)==4 and all(len(row)==21 and None not in row for row in rows)
    assert rows[0]['SSID']=='Cafe, "hello"\nWiFi' and rows[0]['Latitude']==''
    assert rows[1]['GPSFix']=='1' and rows[1]['HDOP']=='0.90' and rows[2]['Latitude']==''
    with (logs/'wardrive_1790118000_00000001_wigle.csv').open(newline='') as f:
        assert f.readline().startswith('WigleWifi-1.6,')
        rows=list(csv.DictReader(f))
    assert len(rows)==1 and len(rows[0])==14 and None not in rows[0]
    assert rows[0]['Type']=='WIFI' and rows[0]['Frequency']=='2437'
    assert rows[0]['CurrentLatitude']=='48.1173000' and rows[0]['AltitudeMeters']=='545'
    assert rows[0]['AccuracyMeters']=='0'
    print('PASS: parsed actual local/WiGLE files, 21/14 columns, escaped SSID, valid-fix-only WiGLE')

u8g2=ROOT/'components/u8g2'
renderer_sources=[u8g2/name for name in re.findall(r'"([^"]+\.c)"',(u8g2/'CMakeLists.txt').read_text())]
preview=OUTPUT/'wardriver_ui_preview'
run_native(['gcc','-std=gnu17','-O1','-ffunction-sections','-fdata-sections',
    '-I'+str(HOST),'-I'+str(APP),'-I'+str(u8g2),
    str(Path(__file__).with_name('wardriver_ui_preview.c')),str(APP/'wardriver_ui.c'),
    str(APP/'wardriver_csv.c'),str(APP/'wardriver_nmea.c'),str(APP/'wardriver_table.c'),
    *map(str,renderer_sources),'-Wl,--gc-sections','-lm','-o',str(preview)],check=True)
run_native([str(preview),str(OUTPUT/'wardriver-ui.pgm')],check=True)
