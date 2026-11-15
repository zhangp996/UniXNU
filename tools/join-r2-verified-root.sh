#!/bin/sh
# Restore the qualified VerifiedRoot probe archive the kernel link force-loads.
#
# CONFIG_XNU_VERIFIED_ROOT_ARCHIVE pins tree/build/verified-root-os-xnu-bundle/libVerifiedRootProbe.a.
# The archive is built from the original OS image and the owner-signed policy (build-verified-root-probe.py),
# so a fresh tree has none and the final link refuses to run. It is stored split under
# artifacts/r2-verified-root and joined here.
set -eu
tree=${1:?usage: join-r2-verified-root.sh <tree>}
here=$(cd "$(dirname "$0")/.." && pwd)
src="$here/artifacts/r2-verified-root"
out="$tree/build/verified-root-os-xnu-bundle"
mkdir -p "$out"
cat "$src"/libVerifiedRootProbe.a.part-* > "$out/libVerifiedRootProbe.a"
( cd "$out" && sha256sum -c "$src/SHA256SUMS" )
echo "restored $out/libVerifiedRootProbe.a"
