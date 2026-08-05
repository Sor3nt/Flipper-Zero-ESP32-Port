#!/bin/bash
# Rebuild optimized SD card ZIP

cd "github_sdcard_extracted"

echo "[*] Rebuilding optimized sdcard.zip..."

# Create archive without .DS_Store and unnecessary files
zip -r ../github_sdcard_optimized.zip . \
  -x "*.DS_Store" \
  -x "__MACOSX/*" \
  -x "Manifest"

cd ..

echo "[OK] Optimized SD card created: github_sdcard_optimized.zip"

# Show size comparison
ORIG_SIZE=$(du -h github_sdcard.zip | cut -f1)
NEW_SIZE=$(du -h github_sdcard_optimized.zip | cut -f1)

echo ""
echo "Size comparison:"
echo "  Original: $ORIG_SIZE"
echo "  Optimized: $NEW_SIZE"
echo ""
echo "Use github_sdcard_optimized.zip for firmware flashing."
