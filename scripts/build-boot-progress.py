#!/usr/bin/env python3
"""Build the opt-in on-screen diagnostic page against this source kernel's ABI.

The renderer is UniXNU-sheng's package/BootProgress.c, vendored byte-identical
under kits/boot-progress (see its PROVENANCE.md). Only that object is built:
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
source = ROOT / 'kits/boot-progress/BootProgress.c'
obj = OUT / 'BootProgress.o'
# The renderer draws with Mu-Silicium's GLYPH_FONT. Take the firmware checkout's copy
# when the tree has one, so the page and the panic screen share glyphs; the kernel step
# runs on a runner without that checkout, so the vendored copy is the normal case.
font_dir = (Path(os.environ.get('UNIXNU_UEFI') or ROOT / 'uefi-sheng')
            / 'Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib')
if not (font_dir / 'Font.h').is_file():
    font_dir = ROOT / 'kits/boot-progress'

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

archive = OUT / 'libBootProgress.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj)], check=True, timeout=30)
print('PASS: boot progress renderer built against the source kernel ABI')
