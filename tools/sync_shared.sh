#!/bin/sh
# Copies shared/ into both sketch folders and usbmic/ into the remote (Arduino can only
# include files that are inside the sketch folder). Run after editing shared/.
# With --check, only reports whether the copies match (used by tests).
set -e
cd "$(dirname "$0")/.."
status=0
for f in shared/*.h; do
  for d in AudioExtender_Remote AudioExtender_Local; do
    if [ "$1" = "--check" ]; then
      cmp -s "$f" "$d/$(basename "$f")" || { echo "out of sync: $d/$(basename "$f")"; status=1; }
    else
      cp "$f" "$d/"
    fi
  done
done
# The USB audio host driver (usbmic/) is used by the remote only.
for f in usbmic/*; do
  d=AudioExtender_Remote
  if [ "$1" = "--check" ]; then
    cmp -s "$f" "$d/$(basename "$f")" || { echo "out of sync: $d/$(basename "$f")"; status=1; }
  else
    cp "$f" "$d/"
  fi
done
[ "$1" = "--check" ] && [ $status -eq 0 ] && echo "shared files in sync"
exit $status
