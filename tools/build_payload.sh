#!/bin/bash
set -eu
cd /home/dev/unixnu/UniXNU
OUT=out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep
DR=/home/dev/unixnu/UniXNU-instantnoodlep
GPU_ADDON="/mnt/d/新建文件夹/instantnoodlep-artifacts/gpu-r3/gpuaddon-mini.bin"
mkdir -p /tmp/amfiEU
cp "$OUT/kernel/kernel.macho" /tmp/amfiEU/kernel.macho
echo "--- dry run ---"
python3 $DR/scripts/patch-amfi-nopmgr.py /tmp/amfiEU/kernel.macho --dry-run
python3 $DR/scripts/patch-amfi-trustcache-gate.py /tmp/amfiEU/kernel.macho --dry-run
echo "--- apply ---"
python3 $DR/scripts/patch-amfi-nopmgr.py /tmp/amfiEU/kernel.macho
python3 $DR/scripts/patch-amfi-trustcache-gate.py /tmp/amfiEU/kernel.macho
sha256sum "$OUT/kernel/kernel.macho" /tmp/amfiEU/kernel.macho
echo "--- launchd cache files ---"
rm -f "$OUT/payload/xpc-addon.bin"
python3 "$DR/scripts/make-launchd-cache-addon.py" "$OUT/payload/xpc-addon.bin" --unixnu "$PWD" \
  --report "$OUT/payload/xpc-addon.json" >/dev/null
echo "--- merged addon: launchd cache + the reference GPU userspace ---"
rm -f "$OUT/payload/gpu-addon-merged.bin"
python3 scripts/make-gpu-addon.py --no-plugin --addon "$OUT/payload/xpc-addon.bin" --addon "$GPU_ADDON" \
  "$OUT/payload/gpu-addon-merged.bin"
sha256sum "$OUT/payload/gpu-addon-merged.bin"
echo "--- owner trust: original bundle + plugin/turnip CDHashes ---"
rm -f "$OUT/payload/owner-trust.bundle"
python3 scripts/trust-append-module.py out/ipsw/21H461/trust.bundle "$OUT/payload/owner-trust.bundle" \
  0804e32473d2722dbe869f41040b8fea7cc0ceaa 234f3a73c72033479ef977c806b8f55c67071998
sha256sum "$OUT/payload/owner-trust.bundle"
echo "--- payload ---"
python3 ./unixnu build payload -D ../UniXNU-instantnoodlep \
  --kernel /tmp/amfiEU/kernel.macho --unique-chip-id 0123456789abcdef \
  --owner-trust-cache "$OUT/payload/owner-trust.bundle" \
  --data-ramdisk "$OUT/data/owner-data-3g.dbr" \
  --gpu-addon "$OUT/payload/gpu-addon-merged.bin" 2>&1 | tail -12
sha256sum "$OUT/payload/payload.img"
