#!/bin/sh
# Compiles both sketches with arduino-cli.
# Usage: tools/build.sh [output-dir]      (default: build/)
# Needs arduino-cli with the esp32 core 3.3.x installed.
set -e
cd "$(dirname "$0")/.."
OUT=${1:-build}
mkdir -p "$OUT"
tools/sync_shared.sh --check
[ -f AudioExtender_Local/voice_clips.h ] || python3 tools/make_voice.py
BOARD=esp32:esp32:XIAO_ESP32S3
build() { # name sketch menu-options extra-flags
  echo "== $1"
  arduino-cli compile --warnings default --fqbn "$BOARD:$3" \
    ${4:+--build-property "compiler.cpp.extra_flags=$4"} \
    ${CTAGS_PATH:+--build-property "runtime.tools.ctags.path=$CTAGS_PATH"} \
    --build-path "$OUT/tmp-$1" --output-dir "$OUT/$1" "$2" | grep -E "Sketch uses|Global variables"
}
build local      AudioExtender_Local USBMode=default,CDCOnBoot=cdc
build remote      AudioExtender_Remote USBMode=hwcdc,CDCOnBoot=cdc
rm -rf "$OUT"/tmp-*
# Named, merged images (flash at 0x0) for firmware/
VERSION=$(sed -n 's/.*kFirmware = "\(.*\)".*/\1/p' AudioExtender_Local/AudioExtender_Local.ino)
mkdir -p firmware
rm -f firmware/*.bin
for v in local remote; do
  # full: whole flash image for a first install (resets settings and voice files), at 0x0
  cp "$OUT/$v"/*.merged.bin "firmware/wireless-usb-audio-extender-$v-$VERSION-full.bin"
  # update: application only, keeps settings and voice files, at 0x10000
  cp "$OUT/$v"/*.ino.bin "firmware/wireless-usb-audio-extender-$v-$VERSION-update.bin"
done
(cd firmware && sha256sum *.bin > SHA256SUMS)
ls -1 firmware
