#!/bin/sh
# Builds the release firmware and the installer page into site/: the page, its images, and
# firmware/ with one merged image (bootloader, partition table, boot selection, app) plus the
# ESP Web Tools manifest. CI publishes site/ to GitHub Pages; locally:
#
#   docs/build_site.sh && python3 -m http.server -d site 8000
#
# Needs PlatformIO. The merged image starts at 0x0 and resets the boot selection to app0, so a
# board that took over-the-air updates boots what was just flashed.
set -eu
cd "$(dirname "$0")/.."
ROOT=$(pwd)
FW="$ROOT/firmware"
OUT="$ROOT/site"

(cd "$FW" && pio run -e amoled175)
VERSION=$(sed -n 's/.*kFirmware = "\([^"]*\)".*/\1/p' "$FW/src/core/app.h")
BUILD="$FW/.pio/build/amoled175"
BOOT_APP0="$HOME/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"

rm -rf "$OUT"
mkdir -p "$OUT/firmware"
cp -R "$ROOT/docs/index.html" "$ROOT/docs/images" "$OUT/"

# Offsets: the ESP32-S3 bootloader at 0x0, the table at 0x8000, otadata and app0 from
# partitions/monav_16mb.csv.
(cd "$FW" && pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32s3 merge_bin \
  -o "$OUT/firmware/monav-display-$VERSION.bin" \
  --flash_mode keep --flash_freq keep --flash_size 16MB \
  0x0 "$BUILD/bootloader.bin" 0x8000 "$BUILD/partitions.bin" 0xe000 "$BOOT_APP0" 0x10000 "$BUILD/firmware.bin")
# The app alone, as Monav sends it over the air.
cp "$BUILD/firmware.bin" "$OUT/firmware/monav-display-$VERSION-ota.bin"

cat > "$OUT/firmware/manifest.json" <<JSON
{
  "name": "Monav Display",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "builds": [
    {"chipFamily": "ESP32-S3", "parts": [{"path": "monav-display-$VERSION.bin", "offset": 0}]}
  ]
}
JSON
echo "site/ ready: firmware $VERSION"
