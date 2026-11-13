#!/bin/sh
# Copies the shared USB audio host driver (usbmic/) into another project that
# uses it, e.g. a checkout of a firmware whose sketch folder holds UsbMic.cpp.
#   tools/export_usbmic.sh <sketch folder>          copy the three files
#   tools/export_usbmic.sh <sketch folder> --check  only report differences
# The copies may diverge; --check just shows which files differ.
set -e
[ -n "$1" ] && [ -d "$1" ] || { echo "usage: $0 <sketch folder> [--check]" >&2; exit 2; }
src="$(cd "$(dirname "$0")/../usbmic" && pwd)"
status=0
for f in "$src"/*; do
  name=$(basename "$f")
  if [ "$2" = "--check" ]; then
    cmp -s "$f" "$1/$name" || { echo "differs: $1/$name"; status=1; }
  else
    cp "$f" "$1/$name"
  fi
done
[ "$2" = "--check" ] && [ $status -eq 0 ] && echo "usbmic driver identical in $1"
exit $status
