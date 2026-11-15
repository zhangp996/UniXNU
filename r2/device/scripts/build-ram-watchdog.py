#!/usr/bin/env python3
"""Archive the community RamWatchdog backend for this board's kernel link.

The backend is mouse233/unixnu-fuxi's implementation, vendored verbatim under
kits/vendor-ram-watchdog: a real IOWatchdog subclass with a software deadline observer. Its builder
pins the 21H461 kernelcache, which is byte-identical to the one this board installs, so the object
and its original-symbol bindings are produced here and archived for the kernel link.
"""
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
vendor = ROOT / 'kits/instantnoodlep/vendor-ram-watchdog'
if not vendor.is_dir():
    vendor = ROOT / 'package/vendor-ram-watchdog'
if not vendor.is_dir():
    raise SystemExit('vendor-ram-watchdog is missing from the tree')

kernelcache = None
for base in (ROOT, *ROOT.parents):
    candidate = base / 'ipsw/21H461/kernelcache.decompressed'
    if candidate.is_file():
        kernelcache = candidate
        break
if kernelcache is None:
    raise SystemExit('ipsw/21H461/kernelcache.decompressed is missing')

out = ROOT / 'build/ram-watchdog'
out.mkdir(parents=True, exist_ok=True)
# A prelinked kernel of this tree is the reference. A fresh tree has none yet: the archives run
# before the final link, so fall back to the prelink candidate the CI restores under
# build/xnu-prelink (same original kernelcache plan, slide and module extents).
reference = ROOT.parent / 'kernel/kernel.macho'
if not reference.is_file() or not reference.with_suffix('.json').is_file():
    candidates = sorted((ROOT / 'build/xnu-prelink').glob('native-owner-*-candidate.macho'))
    reference = next((c for c in candidates if c.with_suffix('.json').is_file()), None)
    if reference is None:
        raise SystemExit('no prelinked kernel or prelink candidate to use as the reference')
    print(f'reference: {reference}')
command = [sys.executable, str(vendor / 'build-ram-watchdog.py'),
           '--reference', str(reference),
           '--kernelcache', str(kernelcache), '--output', str(out)]
import os
os.environ['RAM_WATCHDOG_ROOT'] = str(ROOT)
os.environ['RAM_WATCHDOG_SOURCE'] = str(vendor / 'RamWatchdog.cpp')
subprocess.run(command, check=True, cwd=ROOT)

archive = out / 'libRamWatchdog.a'
archive.unlink(missing_ok=True)
import platform, shutil
if platform.system() == 'Darwin':
    ar = ['xcrun', 'ar']
elif shutil.which('llvm-ar'):
    ar = ['llvm-ar']
else:
    ar = ['ar']
subprocess.run(ar + ['rcs', str(archive), str(out / 'RamWatchdog.late.o'),
                     str(out / 'original-bindings.o')], check=True)
print(archive)

