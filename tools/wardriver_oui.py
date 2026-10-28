#!/usr/bin/env python3
"""Convert IEEE's public MA-L registry to Sor3nt's offline vendor text format."""
import argparse
import csv
from datetime import datetime, timezone
import io
from pathlib import Path
import re
from urllib.request import Request, urlopen

URL='https://standards-oui.ieee.org/oui/oui.csv'
ROOT=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path,help='Use an already downloaded IEEE MA-L CSV')
    parser.add_argument('--output',type=Path,default=ROOT/'build_t_embed/sdcard/apps_data/wifi/mac-vendor.txt')
    args=parser.parse_args()
    if args.input:
        data=args.input.read_bytes()
    else:
        with urlopen(Request(URL,headers={'User-Agent':'Wardriver/1.0'}),timeout=45) as response:
            data=response.read(8*1024*1024+1)
    if len(data)>8*1024*1024: raise ValueError('Registry exceeds 8 MiB limit')
    vendors={}
    for row in csv.DictReader(io.StringIO(data.decode('utf-8-sig'))):
        prefix=row.get('Assignment','').upper()
        name=' '.join(row.get('Organization Name','').replace(',',' ').split())
        if re.fullmatch('[0-9A-F]{6}',prefix) and name:
            vendors[prefix]=name.encode('utf-8')[:110].decode('utf-8',errors='ignore')
    if not vendors or len(vendors)>65536: raise ValueError('Invalid or unsupported MA-L registry')
    text=f'# Source: {URL}\n# Converted: {datetime.now(timezone.utc).isoformat()}\n'
    text+=''.join(f'{prefix}\t{name}\n' for prefix,name in sorted(vendors.items()))
    args.output.parent.mkdir(parents=True,exist_ok=True)
    pending=args.output.with_suffix('.tmp')
    pending.write_text(text,encoding='utf-8'); pending.replace(args.output)
    print(f'{len(vendors)} vendor prefixes -> {args.output}')

if __name__=='__main__': main()
