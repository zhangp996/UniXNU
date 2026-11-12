#!/usr/bin/env python3
"""Compile the generic platform expert against the actual source-kernel ABI."""
import argparse
import json
import os
import shutil
import tomllib
from pathlib import Path
import subprocess
import sys

from kernel_target import ARCH, TRIPLE, check_object

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
catalog = {entry['id']: entry for entry in tomllib.loads((ROOT / 'components.toml').read_text())['component']
           if entry['kind'] == 'driver' and any(name.endswith('.cpp') for name in entry.get('source_files', []))}
aliases = {entry.get('legacy_name', identifier): identifier for identifier, entry in catalog.items()}
auxiliary = ['NativeLegacyDisplay', 'NativeGOPPresenter', 'NativeOfflineGPU', 'NativeOfflineAccelerator', 'NativeIOGPUProviderProbe']
parser.add_argument('component', choices=sorted(set(catalog) | set(aliases) | set(auxiliary)))
parser.add_argument('--negative-aes-kat', action='store_true', help='Isolated own-source wrong-answer refusal control; never changes original inputs')
parser.add_argument('--verified-root', action='store_true', help='Authenticate all actual disk reads against an independently signed immutable policy')
parser.add_argument('--verified-root-bundle', action='store_true', help='Select exactly one capacity-matching policy per device, then independently authenticate it')
parser.add_argument('--negative-verified-block', action='store_true', help='Negative own-buffer control: corrupt only the completed DMA snapshot')
args = parser.parse_args()
identifier = aliases.get(args.component, args.component)
if identifier == 'ShengUfs' and ARCH == 'arm64e':
    if any((args.negative_aes_kat, args.verified_root, args.verified_root_bundle, args.negative_verified_block)):
        parser.error('The source UFS runtime always uses the authenticated policy bundle of the OS profile')
    raise SystemExit(subprocess.call([sys.executable, ROOT / 'scripts/build-sheng-source-storage.py', '--tree', ROOT]))
metadata = catalog.get(identifier, {})
runtime = metadata.get('legacy_name', identifier)
args.component = runtime
if args.verified_root_bundle and not args.verified_root:
    parser.error('A policy bundle requires verified block mode')
if args.verified_root_bundle and args.negative_verified_block:
    parser.error('Select either bundle policy qualification or the isolated DMA snapshot fault')
if (args.verified_root or args.negative_verified_block) and args.component != 'NativeVirtioBlock':
    parser.error('Verified block mode requires the concrete read-only block transport')
if args.negative_verified_block and not args.verified_root:
    parser.error('The isolated input fault requires verified block mode')
component = args.component
recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
record = json.loads(recipe.read_text().strip().rstrip(','))
directory = 'GopFramebuffer' if runtime in ('NativeLegacyDisplay', 'NativeGOPPresenter') else 'AdrenoMetal' if runtime in ('NativeOfflineGPU', 'NativeOfflineAccelerator', 'NativeIOGPUProviderProbe') else identifier
filename = next((name for name in metadata.get('source_files', []) if name.endswith('.cpp')), runtime + '.cpp')
source = ROOT / 'drivers' / directory / filename
outputs = metadata.get('outputs', [])
archive_output = next((path for path in outputs if path.endswith('.a')), None)
aux_dirs = {'NativeLegacyDisplay':'native-legacy-display', 'NativeGOPPresenter':'native-gop-presenter',
            'NativeOfflineGPU':'native-offline-gpu', 'NativeOfflineAccelerator':'native-offline-accelerator',
            'NativeIOGPUProviderProbe':'native-original-provider'}
out = ROOT / Path(archive_output or outputs[0]).parent if outputs else ROOT / 'build' / aux_dirs[runtime]
if args.verified_root:
    out = ROOT / ('build/virtio-mmio-block-verified-fault' if args.negative_verified_block else 'build/virtio-mmio-block-verified')
    if args.verified_root_bundle:
        out = ROOT / 'build/virtio-mmio-block-verified-bundle'
out.mkdir(parents=True, exist_ok=True)
if args.negative_aes_kat:
    if component != 'NativeAES':
        parser.error('The own-source KAT control requires NativeAES')
    out = ROOT / 'build/software-aes-kat-negative'
    out.mkdir(parents=True, exist_ok=True)
    text = source.read_text()
    marker = '0x76,0x49,0xab,0xac'
    if text.count(marker) != 1:
        raise SystemExit('The exact own-source NIST answer was not uniquely identified')
    (out / 'OriginalAESABI.h').write_bytes(source.with_name('OriginalAESABI.h').read_bytes())
    source = out / 'NativeAES.cpp'
    source.write_text(text.replace(marker, '0x77,0x49,0xab,0xac'))
destination = out / (component + '.o')
arguments = record['arguments']
command = []
index = 0
while index < len(arguments):
    value = arguments[index]
    if value == record['file']:
        command.append(str(source))
    elif value == '-o':
        command.extend(['-o', str(destination)])
        index += 1
    elif value == '-Xclang' and arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        command.append(value)
    index += 1
command.extend(['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-Wno-c99-designator', '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier', '-Wno-missing-designated-field-initializers',
                '-I', str(ROOT / '.cache/xnu-work/osfmk')])
if metadata.get('soc_headers'):
    # The component's sources include the selected SoC repository's headers; scripts/unixnu/tree.py
    # links that repository into the assembled tree as soc/.
    command += ['-I', str(ROOT / 'soc/include')]
if component in ('NativeVirtioBlock', 'NativeRamBlock', 'NativeUfs'):
    command += ['-I', str(ROOT / 'build/storage-abi/include')]
    if args.verified_root:
        command += ['-I', str(ROOT / 'kits/VerifiedRoot/include'), '-DNATIVE_VERIFIED_BLOCK=1']
    if args.negative_verified_block:
        command += ['-DNATIVE_VERIFIED_BLOCK_INJECT_FAULT=1']
    if args.verified_root_bundle:
        command += ['-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1']
if component == 'NativeUfs':
    command += ['-I', str(ROOT / 'kits/VerifiedRoot/include'), '-I', str(ROOT / 'drivers/VirtioMmioBlock'),
                '-mgeneral-regs-only']
    # The owner-signed policy chain needs the VerifiedRoot archive and the board's root images, and
    # the kernel build force-loads that archive only for XNU_VERIFIED_ROOT_PROBE=1. A board without
    # the inputs compiles the same device class without verification: the windows are still published
    # as an IOMedia, which is what bring-up needs (the reference port records the same split).
    if os.environ.get('XNU_VERIFIED_ROOT_PROBE', '0') == '1':
        command += ['-DNATIVE_VERIFIED_BLOCK=1', '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1']
if component=='NativeOfflineGPU':
    command += ['-I',str(ROOT/'drivers/VirtioMmioConsole')]
if component in ('NativeLegacyDisplay','NativeHidInput'):
    command += ['-I', str(out)]
if component == 'NativeAdreno':
    command += ['-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / 'kits/AdrenoPM4'),
                '-I', str(ROOT / 'kits/sheng'), '-I', str(ROOT / 'kits/Sm8550Platform'), '-I', str(ROOT / 'kits/AdrenoFirmware'),
                '-I', str(ROOT / 'build/sheng-gmu'), '-I', str(ROOT / 'build/adreno740/include')]
(out / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
result = subprocess.run(command, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(out / 'compile.log').write_text(result.stdout)
if result.returncode:
    raise SystemExit(result.stdout[-3500:])
check_object(destination.read_bytes(), component)
archive = out / ('lib' + identifier + '.a')
members = [destination]
if component == 'NativeUfs':
    # The prelinked IOStorageFamily supplies IOBlockStorageDevice, and a kernel link runs before the
    # prelink: the base vtable resolves through the addresses recorded for this OS build.
    build_id = os.environ.get('UNIXNU_IPSW_BUILD', '')
    bindings = ROOT / 'scripts/data' / f'native-storage-{build_id}-original-bindings.S'
    if not build_id or not bindings.is_file():
        raise SystemExit(f'no recorded IOStorageFamily bindings for build {build_id!r}: {bindings}')
    bound = out / 'original-bindings.o'
    # The kernel step exports CCC_OVERRIDE_OPTIONS to the archive builders; its extra warnings are
    # for the replay of xnu's compile records, and -Werror would reject them on this assembly.
    clean = {key: value for key, value in os.environ.items() if key != 'CCC_OVERRIDE_OPTIONS'}
    subprocess.run(['xcrun', 'clang', '-target', TRIPLE, '-Werror', '-Wno-incompatible-sysroot',
                    '-c', str(bindings), '-o', str(bound)], check=True, env=clean)
    # Every storage-family symbol the object imports has to be bound: a new base call in the driver
    # must fail here rather than at the kernel link or on the device.
    declared = {line.split()[1].rstrip(',') for line in bindings.read_text().splitlines()
                if line.startswith('.set ')}
    listing = subprocess.run([shutil.which('llvm-nm') or 'llvm-nm', '-u', str(destination)],
                             capture_output=True, text=True, check=True).stdout
    imported = set()
    for line in listing.splitlines():
        name = line.strip()
        if 'IOBlockStorageDevice' not in name:
            continue
        imported.add(name[1:] if name.startswith('_') and not name.startswith('__') else name)
    missing = sorted(imported - declared)
    if missing:
        raise SystemExit('storage ABI bindings miss: ' + ', '.join(missing)
                         + '; rerun scripts/derive-storage-bindings.py')
    members.append(bound)
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *members], check=True)
print(f'{identifier}: -Werror build and {TRIPLE} object audit passed')
