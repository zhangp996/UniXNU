#!/bin/bash
set -x
cd /home/dev/unixnu/UniXNU || exit 1
TREE=$PWD/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/tree
# The A650 GPU driver archive has no reference from the kernel proper yet (the board publish hook
# is still to come), so it has to be force-loaded or the linker drops every symbol.  The display
# archive must not carry MiniAcceleratorES.o at the same time (duplicate IOAcceleratorES bindings);
# build-gop-legacy-display.py drops it when it sees this archive.
export SM8250_A650_REFERENCE=/home/dev/unixnu/_ref/a650-ref-combined
export XNU_EXTRA_LD_ARCHIVES="-Wl,-force_load,$TREE/build/ram-watchdog/libRamWatchdog.a -Wl,-force_load,$TREE/build/adreno650/libAdreno650.a -Wl,-force_load,$TREE/build/io-storage-bindings/libIOStorageBindings.a -Wl,-force_load,$TREE/build/board-compat/libBoardCompat.a"
bash /mnt/d/新建文件夹/_agent/ensure_touchhid.sh
bash /mnt/d/新建文件夹/_agent/ensure_bindings.sh
bash /mnt/d/新建文件夹/_agent/ensure_compat.sh
python3 ./unixnu build kernel -D ../UniXNU-instantnoodlep 2>&1 | tee /tmp/kbuildEU.log | tail -40
echo "KERNEL BUILD EXIT: ${PIPESTATUS[0]}"
sha256sum out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/kernel/kernel.macho
