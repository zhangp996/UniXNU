#!/bin/bash
# Official-core build wrapper for the OnePlus 8 Pro (SM8250 / Adreno 650).
set -u
CORE=${CORE:-/home/dev/unixnu/upstream-core}
DEVICE=${DEVICE:-/home/dev/unixnu/device-a650}
OUT=${OUT:-/home/dev/unixnu/device-a650-out}
REF=${SM8250_A650_REFERENCE:-/home/dev/unixnu/_ref/a650-ref-combined}
LOCAL_FEEDS=${LOCAL_FEEDS:-/home/dev/unixnu/local-feeds}
KERNEL_REF=${SMUFS_KERNEL_REFERENCE:-/home/dev/unixnu/inputs/21H461/kernelcache.decompressed}
export SM8250_A650_REFERENCE=$REF
export UNIXNU_DEVICE=$DEVICE
export SMUFS_KERNEL_REFERENCE=$KERNEL_REF
export UNIXNU_IOSTORAGEFAMILY_COMMIT=$(git -C "$LOCAL_FEEDS/IOStorageFamily" rev-parse HEAD)
export UNIXNU_LIBPTHREAD_COMMIT=$(git -C "$LOCAL_FEEDS/libpthread" rev-parse HEAD)
cd "$CORE"
python3 ./unixnu defconfig instantnoodlep_defconfig -D "$DEVICE" --out "$OUT" 2>&1 | tail -2
python3 ./unixnu build kernel -D "$DEVICE" --out "$OUT" > "$OUT-build.log" 2>&1
rc=$?
echo "BUILD_RC=$rc"
tail -30 "$OUT-build.log" | cut -c1-220
exit 0
