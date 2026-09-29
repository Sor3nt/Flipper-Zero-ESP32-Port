"""Verify the Plus profile retains every built-in app from the working UART build."""
from pathlib import Path
import re
R=Path(__file__).resolve().parents[2]
def config(path):
 return {line.split('=',1)[0]:line.split('=',1)[1] for line in path.read_text().splitlines() if line.startswith('CONFIG_') and '=' in line}
a=config(R/'sdkconfig');b=config(R/'sdkconfig.plus_i2c')
changed={name for name in a.keys()|b.keys() if a.get(name)!=b.get(name)}
assert changed=={'CONFIG_T_EMBED_EXTERNAL_UART','CONFIG_T_EMBED_BW16_I2C'},changed
assert b['CONFIG_T_EMBED_BW16_I2C']=='y' and 'CONFIG_T_EMBED_EXTERNAL_UART' not in b
registry=Path('esp-idf/main/generated/applications.c')
def apps(build):return set(re.findall(r'\.appid = "([^"]+)"',(R/build/registry).read_text()))
old=apps('build_t_embed');new=apps('build_t_embed_plus_i2c')
assert old and old==new,(old-new,new-old)
assert 'rf_waterfall' in new and 'subghz' in new
print('PASS: only two transport flags changed; all',len(new),'built-in app identifiers retained')
