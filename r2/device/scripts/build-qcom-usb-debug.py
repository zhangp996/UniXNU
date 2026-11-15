#!/usr/bin/env python3
"""Build the QcomXnuUsbDebug CDC-ACM console archive for this tree.

The module compiles itself from the selected XNU's own compile record, so the archive matches the
kernel it is force-loaded into; this wrapper supplies the record, the kona profile and the output
path, then adds the board hooks the console calls (this repository has no boot-progress store yet).
"""
import importlib.util
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
module = ROOT / 'drivers/QcomXnuUsbDebug'
record = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_kprintf.o.json'
out = ROOT / '.cache/qcom-usb-debug'
archive = out / 'libQcomXnuUsbDebug.a'
destination = ROOT / 'build/qcom-usb-debug/libQcomXnuUsbDebug.a'
# The device package is linked as kits/<kit> in the assembled tree and sits beside the tree in a
# direct device checkout, so look in both places.
board = next((path for path in (ROOT / 'kits' / 'instantnoodlep' / 'QcomUsbBoard.c',
                               ROOT.parent / 'UniXNU-instantnoodlep/package/QcomUsbBoard.c',
                               ROOT / 'package/QcomUsbBoard.c') if path.is_file()), None)

if board is None:
    raise SystemExit('package/QcomUsbBoard.c is missing from the device repository')
for path in (module / 'scripts/build.py', record):
    if not path.is_file():
        raise SystemExit(str(path) + ' is missing; build the kernel once and select QcomXnuUsbDebug')
subprocess.run([sys.executable, str(module / 'scripts/build.py'),
                '--compile-record', str(record), '--soc', 'soc/Sm8250.h',
                '--output', str(out), '--replace'], check=True)

# The console imports two board hooks; compile them with the same flags the module uses.
spec = importlib.util.spec_from_file_location('qcom_usb_build', module / 'scripts/build.py')
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
import json
reference = json.loads(record.read_text().rstrip().rstrip(','))
flags, cwd = builder.compile_flags(reference)
object_file = out / 'QcomUsbBoard.o'
subprocess.run(flags + ['-I' + str(module.parent), '-I' + str(module / 'include'),
                '-c', str(board), '-o', str(object_file)],
               cwd=cwd, check=True)
ar = shutil.which('llvm-ar') or 'llvm-ar'
subprocess.run([ar, 'rcs', str(archive), str(object_file)], check=True)

destination.parent.mkdir(parents=True, exist_ok=True)
shutil.copyfile(archive, destination)
print(destination)

