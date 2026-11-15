#!/usr/bin/env python3
"""Add the verified block bridge to the storage archive.

The kernel's owner-root code asks the block driver whether a device backs a signed volume, and on
arm64 that bridge lives with the driver (VirtioMmioBlock and RamBlock carry their own copy). This
board links ShengUfs alone, so the board package supplies the driver-agnostic form and this step
puts it in the same archive, next to the transport it answers for.
"""
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# The device package is linked as kits/<kit> in the assembled tree and sits beside the tree in a
# direct device checkout, so look in both places.
source = next((path for path in (ROOT / 'kits/instantnoodlep/VerifiedBlockBridge.cpp',
                               ROOT.parent / 'UniXNU-instantnoodlep/package/VerifiedBlockBridge.cpp',
                               ROOT / 'package/VerifiedBlockBridge.cpp') if path.is_file()), None)
# Any C++ record of this kernel carries the same language, include and define set.
record_path = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOStringFuncs.o.json'
output = ROOT / 'build/verified-block-bridge'
archive = ROOT / 'build/sheng-ufs/libShengUfs.a'

if source is None:
    raise SystemExit('package/VerifiedBlockBridge.cpp is missing from the device repository')
for path in (source, record_path, archive):
    if not path.is_file():
        raise SystemExit(str(path) + ' is missing; build the kernel once and select the storage driver')
record = json.loads(record_path.read_text().strip().rstrip(','))
output.mkdir(parents=True, exist_ok=True)
object_file = output / 'VerifiedBlockBridge.o'

arguments, command, index = record['arguments'], [], 0
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        # The record may carry a C driver mode; this source is C++.
        command += ['-x', 'c++', str(source)]
    elif value == '-o':
        command += ['-o', str(object_file)]
        index += 1
    elif value in ('-MF', '-MJ'):
        index += 1
    elif value not in ('-MD', '-MP'):
        command.append(value)
    index += 1
# This clang reports the record own include layout as duplicate headers; the bridge keeps -Werror
# for its own diagnostics and silences the two checks that only restate that layout.
command += ['-Wno-shadow-header', '-Wno-missing-include-dirs', '-Wno-unsafe-buffer-usage']
result = subprocess.run(command, cwd=record['directory'], capture_output=True, text=True)
(output / 'compile.log').write_text(result.stdout + result.stderr)
if result.returncode:
    print(result.stdout[-4000:], file=sys.stderr)
    raise SystemExit('the bridge must compile with the kernel own flags')
subprocess.run(['llvm-ar', 'r', str(archive), str(object_file)], check=True)
print(archive)
