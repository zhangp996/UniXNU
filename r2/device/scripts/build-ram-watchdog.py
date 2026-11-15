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
command = [sys.executable, str(vendor / 'build-ram-watchdog.py'),
           '--reference', str(ROOT.parent / 'kernel/kernel.macho'),
           '--kernelcache', str(kernelcache), '--output', str(out)]
import os
os.environ['RAM_WATCHDOG_ROOT'] = str(ROOT)
os.environ['RAM_WATCHDOG_SOURCE'] = str(vendor / 'RamWatchdog.cpp')
subprocess.run(command, check=True, cwd=ROOT)

archive = out / 'libRamWatchdog.a'
archive.unlink(missing_ok=True)
subprocess.run(['llvm-ar', 'rcs', str(archive), str(out / 'RamWatchdog.late.o'),
                str(out / 'original-bindings.o')], check=True)
print(archive)

