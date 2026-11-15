#!/usr/bin/env python3
"""Link the alioth A650 bring-up subset into the display archive."""
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import sys

tree = Path(os.environ.get('SMUFS_BUILD_TREE', Path(__file__).resolve().parents[1]))
reference = Path(os.environ.get('SM8250_A650_REFERENCE', '/home/ye3912/unixnu-work/a650-r54-reference'))
driver = reference / 'Adreno650'
board = tree / 'kits/instantnoodlep'
kits = reference / 'kits'
obj_dir = tree / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837'
out = tree / 'build/native-legacy-display'


def recipe(name):
    return json.loads((obj_dir / name).read_text().strip().rstrip(','))


def compile_source(source, obj, compiler):
    args = iter(compiler['arguments'])
    command = []
    for value in args:
        if value == compiler['file']:
            command.append(str(source))
        elif value == '-o':
            next(args)
            command += ['-o', str(obj)]
        elif value in ('-MF', '-MJ', '-MT'):
            next(args)
        elif value == '-Xclang':
            arg = next(args)
            if not arg.startswith('-W'):
                command += [value, arg]
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
    for directory in (board, board / 'a650', driver, kits / 'ArmSmmu4K', kits / 'AdrenoFirmware', kits / 'AdrenoPM4',
                      tree / '.cache/xnu-work/osfmk'):
        command += ['-I', str(directory)]
    subprocess.run(command, cwd=compiler['directory'], check=True)


c = recipe('bsd/DEVELOPMENT/bsd_init.o.json')
cxx = recipe('iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
firmware_inputs = {
    'gmu': ('a650_gmu.bin', 'df79fa8482fcc7587bb239b9c9e2b24bce4729893b990fe3f82b30da8175b01c'),
    'sqe': ('a650_sqe.fw', '2a6a0a8390434de2f27df2ec9d6fa7a30a31b32aa4e799aa75c809167c228a3f'),
    'zap': ('a650_zap.elf', 'b43bd7a7a93f2c488dad27ef550ffe8d1b5c5d098d1637d73e8c7b4c04cad058'),
}
assembly = out / 'Sm8250A650Firmware.S'
firmware = []
sections = ['.section __TEXT,__const\n']
for name, (filename, digest) in firmware_inputs.items():
    path = reference / filename
    data = path.read_bytes()
    assert hashlib.sha256(data).hexdigest() == digest, filename
    if name == 'zap':
        assert data[:6] == b'\x7fELF\x01\x01' and struct.unpack_from('<I', data, 28)[0] == 52
        assert struct.unpack_from('<HH', data, 42) == (32, 3)
        assert struct.unpack_from('<8I', data, 52) == (0, 0, 0, 0, 0x94, 0, 0x7000000, 0)
        assert struct.unpack_from('<8I', data, 84) == (0, 0x1000, 0x6000, 0x6000, 0x1a38, 0x2000, 0x2200000, 0x1000)
        assert struct.unpack_from('<8I', data, 116) == (1, 0x3000, 0x5000, 0x5000, 0x68c, 0x68c, 0x8000007, 0x100000)
    firmware.append(path)
    symbol = f'_native_gpu_{name}_firmware'
    sections.append(f'''
.p2align 2
.globl {symbol}
{symbol}:
.incbin "{path}"
.globl {symbol}_size
{symbol}_size:
.long {symbol}_size - {symbol}
''')
assembly.write_text(''.join(sections))
firmware_obj = assembly.with_suffix('.o')
subprocess.run([c['arguments'][0], '--target=arm64-apple-ios17.0', '-c', assembly, '-o', firmware_obj], check=True)

sources = sorted((kits / 'ArmSmmu4K').glob('ArmSmmu4K*.c'))
sources += [kits / 'AdrenoFirmware' / (name + '.c') for name in (
    'AdrenoFirmware', 'AdrenoHfi', 'AdrenoHfiStartup', 'AdrenoHfiVotes',
)]
sources.append(kits / 'AdrenoPM4/AdrenoPM4.c')
sources += [driver / (name + '.cpp') for name in (
    'NativeAdrenoGmuContext', 'NativeAdrenoGmuMemory', 'NativeAdrenoMemory',
    'NativeAdrenoSmmuContext', 'NativeAdrenoSmmuMemory', 'NativeAdrenoCpu',
    'NativeAdrenoGpuContext', 'NativeAdrenoRing', 'NativeAdrenoCpInit', 'NativeAdrenoIrq',
    'NativeAdrenoSubmit', 'NativeAdrenoTimer',
)]
for filename, patch_name in (
    ('NativeAdrenoVm.cpp', 'adreno650-upstream-import-alias.patch'),
    ('NativeAdrenoUserClient.cpp', 'adreno650-explicit-contiguous-bo.patch'),
    ('NativeAdrenoPublish.cpp', 'adreno650-release-cpu-mapping.patch'),
):
    patched = out / filename
    subprocess.run(['patch', '--batch', '--forward', '--fuzz=0', '-o', patched,
                    driver / filename, board / patch_name], check=True)
    # The kernel-side IOMemoryDescriptor marker (release-cpu-mapping) is not in the
    # official core patch series yet; keep the driver patch but drop the single call.
    _text = patched.read_text()
    if 'NativeAdrenoCpuView' in _text:
        _out = []
        for _line in _text.splitlines():
            if 'NativeAdrenoCpuView' not in _line:
                _out.append(_line)
            elif 'IOReturn' in _line and '=' in _line:
                _out.append('        const IOReturn status = kIOReturnSuccess;  /* native cpu view marker: kernel patch pending */')
            else:
                _out.append('    /* native cpu view marker: kernel patch pending */')
        patched.write_text('\n'.join(_out) + '\n')
        print('replaced NativeAdrenoCpuView calls with safe defaults (kernel marker pending)')
    sources.append(patched)
sources += [board / 'a650/A650Gmu.c', board / 'Sm8250AliothCold.cpp', board / 'Sm8250AliothCp.cpp',
            board / 'Sm8250AliothZap.cpp', board / 'Sm8250AliothPublish.cpp', board / 'Sm8250AliothMetal.cpp',
            board / 'SmGateTrace.c', board / 'BoardGlue.cpp']
objects = [firmware_obj]
for source in sources:
    obj = out / ('a650-' + source.stem + '.o')
    compile_source(source, obj, c if source.suffix == '.c' else cxx)
    objects.append(obj)

# The original 21H461 parent is retained in the prelink. Qualify the absolute imports before linking.
sys.path.insert(0, str(tree / 'kits/XnuPrelink'))
from native_prelink import backed_offset, macho

record = json.loads((board / 'ioaccelerator-base-abi.json').read_text())
parent = (tree.parent / 'kernel/kernel.macho').read_bytes()
image = macho(parent)
for address, expected in record['original_entry_sha256'].items():
    offset = backed_offset(image['segments'], int(address, 16), 32, len(parent))
    assert hashlib.sha256(parent[offset:offset + 32]).hexdigest() == expected, address
bindings = {
    '__ZN15IOAcceleratorESC2EPK11OSMetaClass': record['protected_compatible_constructor_aliases'][0],
    '__ZN15IOAcceleratorESD2Ev': record['nondeleting_destructor'],
    '__ZN15IOAcceleratorES10gMetaClassE': record['metaclass'],
}
accelerator = driver / 'NativeAdrenoAccelerator.cpp'
accelerator_obj = out / 'a650-NativeAdrenoAccelerator.o'
compile_source(accelerator, accelerator_obj, cxx)
undefined = subprocess.check_output(['llvm-nm', '-u', accelerator_obj], text=True)
assert {line.split()[-1] for line in undefined.splitlines() if 'IOAcceleratorES' in line} == set(bindings)
late = out / 'a650-NativeAdrenoAccelerator.late.o'
subprocess.run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
                '--globalize-symbol=__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp', accelerator_obj, late], check=True)
binding_asm = out / 'a650-accelerator-bindings.S'
binding_asm.write_text(''.join(f'.globl {name}\n.set {name}, {address}\n' for name, address in bindings.items()))
binding_obj = binding_asm.with_suffix('.o')
subprocess.run([c['arguments'][0], '--target=arm64-apple-ios17.0', '-c', binding_asm, '-o', binding_obj], check=True)
objects.extend((late, binding_obj))
archive = out / 'libNativeLegacyDisplay.late.a'
subprocess.run(['llvm-ar', 'rcs', archive, *objects], check=True)
report_path = out / 'build-report.json'
report = json.loads(report_path.read_text())
report['archive_sha256'] = hashlib.sha256(archive.read_bytes()).hexdigest()
report['source_sha256'].update({str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                                for path in (*sources, *firmware, accelerator)})
report_path.write_text(json.dumps(report, indent=2) + '\n')
