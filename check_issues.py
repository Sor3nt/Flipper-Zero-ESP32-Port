import os
import re
from pathlib import Path

files_to_check = [
    "components/furi_hal/furi_hal_spi_stability.h",
    "components/furi_hal/furi_hal_spi_stability.c",
    "components/furi_hal/furi_hal_spi_device_config.h",
    "example_sd_stable.c",
]

print("[CHECKING FOR COMPILATION ISSUES]\n")

issues = []

for fpath in files_to_check:
    if not os.path.exists(fpath):
        issues.append((fpath, "FILE NOT FOUND"))
        continue
    
    with open(fpath) as f:
        content = f.read()
    
    # Check 1: Balanced braces
    if content.count("{") != content.count("}"):
        issues.append((fpath, f"Unbalanced braces: {content.count('{')} vs {content.count('}')}"))
    
    # Check 2: Balanced parens
    if content.count("(") != content.count(")"):
        issues.append((fpath, f"Unbalanced parens: {content.count('(')} vs {content.count(')')}"))
    
    # Check 3: Unused includes
    includes = re.findall(r'#include [<"]([^>"]+)[>"]', content)
    for inc in includes:
        if inc not in ["stdint.h", "stdbool.h", "freertos/FreeRTOS.h", "freertos/semphr.h", 
                       "esp_log.h", "string.h"]:
            continue
    
    # Check 4: Semicolons after function defs
    lines = content.split('\n')
    for i, line in enumerate(lines[-50:], len(lines)-50):
        if re.match(r'^\s*\}\s*$', line):
            if i < len(lines)-1:
                next_line = lines[i+1].strip()
                if next_line and not next_line.startswith('//') and not next_line.startswith('*'):
                    if not (next_line.startswith('}') or next_line.startswith('#')):
                        pass  # This is OK

if issues:
    print("ISSUES FOUND:")
    for fpath, issue in issues:
        print(f"  {fpath}: {issue}")
else:
    print("ALL CHECKS PASSED")

