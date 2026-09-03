#!/usr/bin/env bash
# Build the MyBar firmware with arduino-cli and collect everything a release needs in dist/.
#
# Used by .github/workflows/firmware.yml and works locally too:
#   scripts/build.sh                 # dist/ next to this repo
#   OUT=/tmp/out scripts/build.sh    # custom output directory
#
# Requirements: arduino-cli with the esp32 core installed (see README.md).
#
# Output layout (mirrors the GitHub Pages site so manifest.json paths resolve as-is):
#   dist/manifest.json                    ESP Web Tools manifest (web flasher)
#   dist/version.json                     version, OTA image size and MD5 for the mobile app
#   dist/firmware/mybar-<v>.bin           OTA image (send this through the app)
#   dist/firmware/mybar-<v>.merged.bin    full 4 MB flash image, flash at 0x0 over USB
#   dist/firmware/mybar-<v>.bootloader.bin, .partitions.bin, boot_app0.bin
#   dist/firmware/SHA256SUMS, MD5SUMS
#   dist/debug/mybar-<v>.elf, .map        symbols, not shipped to devices
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${OUT:-$ROOT/dist}"
FQBN="${FQBN:-esp32:esp32:esp32:PartitionScheme=default}"
LIBRARIES="$ROOT/Libraries/library"

# --- locate the sketch and read its version -------------------------------------------------
shopt -s nullglob
inos=("$ROOT"/src/*.ino)
shopt -u nullglob
if [ "${#inos[@]}" -ne 1 ]; then
  echo "error: expected exactly one .ino in src/, found ${#inos[@]}" >&2
  exit 1
fi
INO="${inos[0]}"
SKETCH_NAME="$(basename "$INO" .ino)"
VERSION="$(sed -nE 's/.*firmwareVersion[[:space:]]*=[[:space:]]*"([^"]+)".*/\1/p' "$INO" | head -n 1)"
if [ -z "$VERSION" ]; then
  echo "error: could not read firmwareVersion from $INO" >&2
  exit 1
fi
echo "Sketch:  $SKETCH_NAME"
echo "Version: $VERSION"
echo "FQBN:    $FQBN"

# --- stage the sketch in a folder that carries its own name (Arduino requirement) -------------
STAGE_ROOT="$(mktemp -d)"
trap 'rm -rf "$STAGE_ROOT"' EXIT
STAGE="$STAGE_ROOT/$SKETCH_NAME"
mkdir -p "$STAGE"
cp "$ROOT"/src/* "$STAGE"/
# The esp32 core's post-build hook copies partitions.csv into <sketch>/build/<vendor.arch.board>/
# and fails on Windows when that folder is missing, even with --output-dir set. Pre-create it.
IFS=: read -r fqbn_vendor fqbn_arch fqbn_board _ <<< "$FQBN"
mkdir -p "$STAGE/build/$fqbn_vendor.$fqbn_arch.$fqbn_board"

# --- compile ----------------------------------------------------------------------------------
BUILD="$STAGE_ROOT/build"
arduino-cli compile \
  --fqbn "$FQBN" \
  --libraries "$LIBRARIES" \
  --output-dir "$BUILD" \
  --warnings default \
  "$STAGE"

# boot_app0.bin (OTA data initialiser) ships with the core, not with the sketch.
PLATFORM_PATH="$(arduino-cli compile --fqbn "$FQBN" --show-properties "$STAGE" | sed -n 's/^runtime\.platform\.path=//p' | head -n 1)"
BOOT_APP0="$PLATFORM_PATH/tools/partitions/boot_app0.bin"
if [ ! -f "$BOOT_APP0" ]; then
  echo "error: boot_app0.bin not found at $BOOT_APP0" >&2
  exit 1
fi

# --- collect ----------------------------------------------------------------------------------
rm -rf "$OUT"
mkdir -p "$OUT/firmware" "$OUT/debug"
P="$BUILD/$SKETCH_NAME.ino"
N="mybar-$VERSION"
cp "$P.bin"            "$OUT/firmware/$N.bin"
cp "$P.merged.bin"     "$OUT/firmware/$N.merged.bin"
cp "$P.bootloader.bin" "$OUT/firmware/$N.bootloader.bin"
cp "$P.partitions.bin" "$OUT/firmware/$N.partitions.bin"
cp "$BOOT_APP0"        "$OUT/firmware/boot_app0.bin"
cp "$P.elf"            "$OUT/debug/$N.elf"
[ -f "$P.map" ] && cp "$P.map" "$OUT/debug/$N.map"

(cd "$OUT/firmware" && sha256sum -- *.bin > SHA256SUMS && md5sum -- *.bin > MD5SUMS)

SIZE="$(wc -c < "$OUT/firmware/$N.bin" | tr -d ' ')"
MD5="$(md5sum "$OUT/firmware/$N.bin" | cut -d' ' -f1)"
BUILT_AT="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
COMMIT="${GITHUB_SHA:-$(git -C "$ROOT" rev-parse HEAD 2>/dev/null || echo unknown)}"

# Consumed by the mobile app to find the current OTA image (otaStart:<size>:<md5>).
cat > "$OUT/version.json" <<JSON
{
  "version": "$VERSION",
  "bin": "firmware/$N.bin",
  "size": $SIZE,
  "md5": "$MD5",
  "mergedBin": "firmware/$N.merged.bin",
  "commit": "$COMMIT",
  "builtAt": "$BUILT_AT"
}
JSON

# ESP Web Tools manifest. Offsets are the standard ESP32 (non S/C series) layout used by the
# Arduino core: bootloader 0x1000, partition table 0x8000, otadata 0xE000, app0 0x10000.
cat > "$OUT/manifest.json" <<JSON
{
  "name": "MyBar",
  "version": "$VERSION",
  "new_install_prompt_erase": true,
  "new_install_improv_wait_time": 0,
  "builds": [
    {
      "chipFamily": "ESP32",
      "parts": [
        { "path": "firmware/$N.bootloader.bin", "offset": 4096 },
        { "path": "firmware/$N.partitions.bin", "offset": 32768 },
        { "path": "firmware/boot_app0.bin",     "offset": 57344 },
        { "path": "firmware/$N.bin",            "offset": 65536 }
      ]
    }
  ]
}
JSON

echo
echo "Build complete -> $OUT"
echo "  OTA image: firmware/$N.bin  ($SIZE bytes, md5 $MD5)"
ls -la "$OUT/firmware"
