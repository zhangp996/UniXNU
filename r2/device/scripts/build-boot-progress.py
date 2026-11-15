#!/usr/bin/env python3
"""Build the opt-in on-screen diagnostic page against this source kernel's ABI.

The renderer is UniXNU-sheng's package/BootProgress.c, vendored byte-identical
into this repository's package directory (see package/PROVENANCE.md), which the
tree assembles as kits/<kit> (scripts/unixnu/tree.py). Only that object is built:
sheng's four companion probes cover its watchdog policy, file export and ADSP
bring-up and are not part of this port.

The page is enabled with CONFIG_XNU_BOOT_PROGRESS=y and boot-progress=1 (freeze)
or boot-progress=2 (continuous); docs/guide/debugging.md describes the rows.
"""
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'build/boot-progress'
OUT.mkdir(parents=True, exist_ok=True)
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json')
                    .read_text().strip().rstrip(','))
source = next((p for p in (ROOT / 'kits' / 'instantnoodlep' / 'BootProgress.c',
                          ROOT / 'package' / 'BootProgress.c') if p.is_file()),
             ROOT / 'kits' / 'instantnoodlep' / 'BootProgress.c')
obj = OUT / 'BootProgress.o'
# The renderer draws with Mu-Silicium's GLYPH_FONT. Take the firmware checkout's copy
# when the tree has one, so the page and the panic screen share glyphs; the kernel step
# runs on a runner without that checkout, so the vendored copy is the normal case.
font_dir = (Path(os.environ.get('UNIXNU_UEFI') or ROOT / 'uefi-sheng')
            / 'Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib')
if not (font_dir / 'Font.h').is_file():
    font_dir = ROOT / 'kits' / 'instantnoodlep'

command = []
args = record['arguments']
index = 0
while index < len(args):
    arg = args[index]
    if arg == record['file']:
        command.append(str(source))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o':
            command += ['-o', str(obj)]
        index += 1
    elif arg == '-Xclang' and args[index + 1].startswith('-W'):
        index += 1
    elif arg == '-D' and args[index + 1].startswith('XNU_BOOT_PROGRESS='):
        index += 1
    elif not arg.startswith(('-W', '-DXNU_BOOT_PROGRESS=')):
        command.append(arg)
    index += 1
command += ['-DXNU_BOOT_PROGRESS=1', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
            '-I', str(ROOT / '.cache/xnu-work/osfmk'),
            '-I', str(font_dir)]
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
subprocess.run(command, cwd=record['directory'], check=True, timeout=300)

# The renderer names the counters of sheng's own board (Virtio, verified blocks, touch,
# legacy display, Adreno, PMGR). The kernel link does not honour weak_import, so those
# names have to exist: BootProgressStubs.c defines them weakly, and a strong definition
# from a component still wins. The file is generated from the extern declarations of
# BootProgress.c; see its header.
stubs = ROOT / 'kits' / 'instantnoodlep' / 'BootProgressStubs.c'
stub_obj = OUT / 'BootProgressStubs.o'
stub_command = [str(stubs) if arg == str(source) else str(stub_obj) if arg == str(obj) else arg
                for arg in command]
subprocess.run(stub_command, cwd=record['directory'], check=True, timeout=300)


# Uke's crash-report probe (package/CrashLogProbe.c). It is a BSD source: compile it with the
# kern_exec record, and always put it in the archive because this kernel link does not honour
# weak imports.
bsd = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json')
                  .read_text().strip().rstrip(','))
probe_source = next((p for p in (ROOT / 'kits' / 'instantnoodlep' / 'CrashLogProbe.c',
                                 ROOT / 'package' / 'CrashLogProbe.c') if p.is_file()), None)
if probe_source is None:
    raise SystemExit('package/CrashLogProbe.c is missing from the device repository')
probe_obj = OUT / 'CrashLogProbe.o'
probe_command, idx = [], 0
while idx < len(bsd['arguments']):
    arg = bsd['arguments'][idx]
    if arg == bsd['file']:
        probe_command += ['-x', 'c', str(probe_source)]
    elif arg == '-o':
        probe_command += ['-o', str(probe_obj)]
        idx += 1
    elif arg in ('-MF', '-MT', '-MJ'):
        idx += 1
    else:
        probe_command.append(arg)
    idx += 1
probe_command += ['-Wno-error', '-I', str(ROOT / '.cache/xnu-work/osfmk')]
subprocess.run(probe_command, cwd=bsd['directory'], check=True, timeout=300)



# sheng's userland stack capture: Mach-side sources, so they use the same osfmk record as BootProgress.
# The include of the source directory is required: the kit copy sits beside its own headers.
trace_sources = ['BootProcessProbe.c', 'BootProcessWorker.c', 'BootUiTraceProbe.c', 'BootLatencyTrace.c',
                 'BootTraceStubs.c']
trace_objects = []
for trace_name in trace_sources:
    trace_src = next((p for p in (ROOT / 'kits' / 'instantnoodlep' / trace_name,
                                  ROOT / 'package' / trace_name) if p.is_file()), None)
    if trace_src is None:
        raise SystemExit('package/' + trace_name + ' is missing from the device repository')
    trace_obj = OUT / (trace_src.stem + '.o')
    trace_command = [str(trace_src) if arg == str(source) else str(trace_obj) if arg == str(obj) else arg
                     for arg in command]
    trace_command = [arg for arg in trace_command if not arg.startswith('-Werror')]
    trace_command += ['-I', str(trace_src.parent), '-I', str(ROOT / 'package')]
    subprocess.run(trace_command, cwd=record['directory'], check=True, timeout=300)
    trace_objects.append(str(trace_obj))

archive = OUT / 'libBootProgress.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj), str(stub_obj), str(probe_obj)] + trace_objects, check=True, timeout=30)
print('PASS: boot progress renderer built against the source kernel ABI')
