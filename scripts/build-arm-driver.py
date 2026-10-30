#!/usr/bin/env python3
"""Compile the generic platform expert against the actual source-kernel ABI."""
import argparse
import json
import tomllib
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def storage_imports(data):
    """Undefined IOStorageFamily symbols of a compiled object, read from its Mach-O table."""
    count = struct.unpack_from('<6I', data)[4]
    offset = 32
    for _ in range(count):
        command, length = struct.unpack_from('<II', data, offset)
        if command == 2:
            symoff, symbols, stroff, strsize = struct.unpack_from('<4I', data, offset + 8)
            names = []
            for index in range(symbols):
                strx, kind, _, _, _ = struct.unpack_from('<IBBHQ', data, symoff + index * 16)
                if kind & 0x0e or strx >= strsize:
                    continue
                name = data[stroff + strx:data.find(b'\0', stroff + strx)].decode()
                if 'IOBlockStorageDevice' in name:
                    names.append(name)
            return sorted(set(names))
        offset += length
    return []


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
                '-Wno-unnecessary-virtual-specifier', '-I', str(ROOT / '.cache/xnu-work/osfmk')])
if component in ('NativeVirtioBlock', 'NativeRamBlock', 'NativeUfs', 'KonaUfs'):
    command += ['-I', str(ROOT / 'build/storage-abi/include')]
    if args.verified_root:
        command += ['-I', str(ROOT / 'kits/VerifiedRoot/include'), '-DNATIVE_VERIFIED_BLOCK=1']
    if args.negative_verified_block:
        command += ['-DNATIVE_VERIFIED_BLOCK_INJECT_FAULT=1']
    if args.verified_root_bundle:
        command += ['-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1']
if component == 'NativeUfs':
    command += ['-I', str(ROOT / 'kits/VerifiedRoot/include'), '-I', str(ROOT / 'drivers/VirtioMmioBlock'),
                '-DNATIVE_VERIFIED_BLOCK=1', '-DNATIVE_VERIFIED_BLOCK_MULTI_POLICY=1', '-mgeneral-regs-only']
if component == 'KonaUfs':
    # Bring-up build: the owner-root verified-block chain needs a signed policy bundle
    # this board does not have yet, and the root auth is bypassed for now. Compile the
    # driver without NATIVE_VERIFIED_BLOCK so it does not import the policy symbols.
    # The ShengUfs fork predates this clang, so keep its warnings as warnings.
    command += ['-mgeneral-regs-only', '-Wno-error']
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
data = destination.read_bytes()
magic, cpu, subtype, kind, count, size = struct.unpack_from('<6I', data)
if (magic, cpu, subtype, kind) != (0xfeedfacf, 0x100000c, 0, 1):
    raise SystemExit('Unexpected platform object architecture')
offset = 32
deployment = None
for _ in range(count):
    cmd, length = struct.unpack_from('<II', data, offset)
    if cmd == 0x32:
        deployment = struct.unpack_from('<II', data, offset + 8)
    offset += length
if deployment != (2, 0x110000):
    raise SystemExit(f'Unexpected platform deployment {deployment}')
archive = out / ('lib' + identifier + '.a')
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, destination], check=True)
if component == 'KonaUfs':
    # IOStorageFamily is a kext of the pinned kernelcache, so the kernel link has no definition
    # for the class this driver subclasses. scripts/data/storage-bindings.json holds the
    # original addresses derive-storage-bindings.py proved against that kernelcache; at the
    # prelink slide they are the addresses the driver calls, and original-bindings.o defines
    # them as absolute symbols. The import list comes from the compiled object, never from a
    # hand-written table, so a shape change in the driver fails here instead of at the link.
    record = json.loads((ROOT / 'scripts/data/storage-bindings.json').read_text())
    addresses = {name: int(value, 16) for name, value in record['addresses'].items()}
    imports = storage_imports(data)
    if not imports:
        raise SystemExit(f'{identifier}: the object imports no IOStorageFamily symbol; the driver shape changed')
    missing = [name for name in imports if name not in addresses]
    if missing:
        raise SystemExit('the driver imports storage symbols the record does not cover: ' + ', '.join(missing))
    if any(name.endswith(('D0Ev', 'D1Ev')) for name in imports):
        raise SystemExit('the driver must not use the abstract complete or deleting destruction')
    assembly = out / 'original-bindings.S'
    assembly.write_text(''.join(f'.globl {name}\n.set {name}, {addresses[name] + record["slide"]:#x}\n'
                                for name in imports))
    bindings_object = out / 'original-bindings.o'
    compiled = subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror',
                               '-Wno-incompatible-sysroot', '-c', str(assembly), '-o', str(bindings_object)],
                              capture_output=True, text=True)
    if compiled.returncode:
        raise SystemExit((compiled.stdout + compiled.stderr)[-4000:])
    # A warm tree still has the kernelcache, so the record is proved against it here; a cold
    # tree (CI) reaches the prelink afterwards.
    if sorted(ROOT.parents[3].glob('ipsw/*/kernelcache.decompressed')):
        check = subprocess.run(['python3', str(ROOT / 'scripts/derive-storage-bindings.py'), '--check'],
                               cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        (out / 'derive-check.log').write_text(check.stdout)
        if check.returncode:
            raise SystemExit(check.stdout[-4000:])
    subprocess.run(['xcrun', 'ar', 'rcs', archive, bindings_object], check=True)
    print(f'{identifier}: {len(imports)} storage imports bound from the recorded kernelcache '
          f'({record["kernelcache_sha256"][:16]}, slide {record["slide"]:#x})')
print(f'{identifier}: -Werror build and actual ARM64/iOS 17 object audit passed')
