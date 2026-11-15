#!/bin/sh
# Restore the qualified prelink candidate into a laid-out tree.
#
# The device display builders audit tree/build/xnu-prelink/native-owner-mobile-graphics-candidate.macho
# against the original kernelcache. The candidate is Apple-derived, so it is stored split into
# parts under artifacts/r2-prelink and joined here. Both names are the same image: the older
# builder looks for native-owner-mobile-graphics-candidate, the newer one for the
# native-owner-ufs-cpu-desktop-mobile-graphics-* glob.
set -eu
tree=${1:?usage: join-r2-prelink.sh <tree>}
here=$(cd "$(dirname "$0")/.." && pwd)
src="$here/artifacts/r2-prelink"
out="$tree/build/xnu-prelink"
mkdir -p "$out"
cat "$src"/native-owner-mobile-graphics-candidate.macho.part-* > "$out/native-owner-mobile-graphics-candidate.macho"
cp "$src/native-owner-mobile-graphics-candidate.json" "$out/native-owner-mobile-graphics-candidate.json"
( cd "$out" && sha256sum -c "$src/SHA256SUMS" )
cp "$out/native-owner-mobile-graphics-candidate.macho" "$out/native-owner-ufs-cpu-desktop-mobile-graphics-candidate.macho"
cp "$out/native-owner-mobile-graphics-candidate.json" "$out/native-owner-ufs-cpu-desktop-mobile-graphics-candidate.json"
echo "restored the prelink candidates in $out"
