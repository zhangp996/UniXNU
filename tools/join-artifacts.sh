#!/bin/sh
# Reassemble the split baseline artifacts and verify them.
set -eu
here=$(dirname "$0")/../artifacts/original
cd "$here"
for stem in bootEU.img kernel.macho payload.img; do
  if [ -f "$stem" ]; then continue; fi
  parts=$(ls "$stem".part-* 2>/dev/null || true)
  [ -n "$parts" ] || continue
  cat $parts > "$stem"
done
sha256sum -c SHA256SUMS
