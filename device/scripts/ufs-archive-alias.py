#!/usr/bin/env python3
"""Publish this board's UFS archive under the name the kernel build script links.

scripts/build-xnu-4k-baseline.sh links build/sheng-ufs/libShengUfs.a whenever XNU_NATIVE_UFS=1; the
component this board selects builds build/kona-ufs/libKonaUfs.a.  Rather than duplicate the driver
under a second component id, this step copies the archive to the linked name, so the kernel runs the
source this repository maintains.
"""
import shutil
import sys
from pathlib import Path

root = Path('.').resolve()
built = root / 'build/kona-ufs/libKonaUfs.a'
linked = root / 'build/sheng-ufs/libShengUfs.a'
if not built.is_file():
    sys.exit('ufs-archive-alias: ' + str(built) + ' was not built')
linked.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(built, linked)
print('PASS: published ' + str(built) + ' as ' + str(linked))
