# r2-verified-root

The qualified VerifiedRoot probe archive the kernel link force-loads:
`libVerifiedRootProbe.a` (73345880 bytes, sha256 92089b8ad1f1223483eb2663ef6556e57ad1fba9d5b633554b42ed87f901071a),
split into parts. `tools/join-r2-verified-root.sh <tree>` joins it into
`<tree>/build/verified-root-os-xnu-bundle/` and verifies SHA256SUMS.

It is built by scripts/build-verified-root-probe.py from the original OS image and the
owner-signed policy in the device repository, so a fresh CI tree cannot produce it before the
kernel compile records exist; the workflow restores it before the kernel build.
