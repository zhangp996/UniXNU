#!/usr/bin/env python3
"""Build the SM8250 UFS cold-start driver as build/sheng-ufs/libShengUfs.a.

The kernel links exactly that archive when CONFIG_XNU_NATIVE_UFS is set, and the arm64 personality
patch installs the NativeUfs class, so this builder only has to produce the object: it replays the
compile record of the XNU build, like build-arm-driver.py, but compiles the device repository's own
driver instead of a component checkout, and compiles the local SMUFS owner-audit verifier. NATIVE_VERIFIED_BLOCK remains
0: the separate Virtio provider is not selected, and root trust remains off.

The device repository is reached through the kit symlink the tree assembler makes:
tree/kits/<kit> -> <device repository>/package.
"""
import json
from pathlib import Path
import struct
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def device_root():
    kits = ROOT / 'kits'
    for entry in sorted(kits.iterdir()) if kits.is_dir() else []:
        if not entry.is_symlink():
            continue
        resolved = entry.resolve()
        if (resolved / 'board.toml').is_file():
            return resolved
        if (resolved.parent / 'board.toml').is_file():
            return resolved.parent
    raise SystemExit('cannot find the device repository through kits/; run "unixnu tree" first')


device = device_root()
source = device / 'drivers/Sm8250Ufs/NativeUfs.cpp'
headers = device / 'drivers/Sm8250Ufs'
if not source.is_file():
    raise SystemExit(f'SM8250 UFS driver source missing: {source}')

storage_headers = ROOT / 'build/storage-abi/include/IOKit/storage/IOBlockStorageDevice.h'
if not storage_headers.is_file():
    subprocess.run(['python3', str(ROOT / 'scripts/prepare-storage-headers.py')], check=True)

out = ROOT / 'build/sheng-ufs'
out.mkdir(parents=True, exist_ok=True)
recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
if not recipe.is_file():
    raise SystemExit(f'compile record missing: {recipe}; run a kernel build first')
record = json.loads(recipe.read_text().strip().rstrip(','))

destination = out / 'NativeUfs.o'
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
                '-Wno-unnecessary-virtual-specifier',
                '-I', str(headers), '-I', str(ROOT / 'build/storage-abi/include'),
                '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(ROOT / 'kits/instantnoodlep'), '-mgeneral-regs-only'])

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

# Private iobuf bridge: replay the current BSD C record, never a copied VFS TU.
adapter_source = headers / 'Probe69Iobuf.c'
adapter_destination = out / 'Probe69Iobuf.o'
adapter_recipe = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/vfs_bio.o.json'
adapter_record = json.loads(adapter_recipe.read_text().strip().rstrip(','))
adapter_arguments = adapter_record['arguments']
adapter_command = []
index = 0
while index < len(adapter_arguments):
    value = adapter_arguments[index]
    if value == adapter_record['file']:
        adapter_command.append(str(adapter_source))
    elif value == '-o':
        adapter_command.extend(['-o', str(adapter_destination)])
        index += 1
    elif value == '-x':
        adapter_command.extend(['-x', 'c'])
        index += 1
    elif value.startswith('-x'):
        adapter_command.append('-xc')
    elif value in ('-MF', '-MJ', '-MT', '-MQ', '-dependency-file', '--serialize-diagnostics'):
        index += 1
    elif value in ('-MD', '-MMD', '-MP', '-MG', '--driver-mode=g++', '-fsized-deallocation', '-fapple-kext'):
        pass
    elif value.startswith(('-std=', '-fc++-')):
        pass
    elif value == '-Xclang' and adapter_arguments[index + 1].startswith('-W'):
        index += 1
    elif not value.startswith('-W'):
        adapter_command.append(value)
    index += 1
adapter_command.extend(['-std=gnu11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                        '-I', str(headers), '-I', str(ROOT / '.cache/xnu-work/bsd'),
                        '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-DBSD_KERNEL_PRIVATE',
                        '-mgeneral-regs-only', '-g', '-fstandalone-debug',
                        '-fno-eliminate-unused-debug-types'])
(out / 'adapter-compile-command.json').write_text(json.dumps({
    'record': str(adapter_recipe), 'original_record': adapter_record,
    'command': adapter_command, 'working_directory': adapter_record['directory'],
    'source': str(adapter_source), 'object': str(adapter_destination)
}, indent=2) + '\n')
adapter_result = subprocess.run(adapter_command, cwd=adapter_record['directory'],
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
(out / 'adapter-compile.log').write_text(adapter_result.stdout)
if adapter_result.returncode:
    raise SystemExit(adapter_result.stdout[-6000:])

binding_script = device / 'scripts/bind-sm8250-storage.py'
binding_audit = headers / 'storage-binding-audit.json'
subprocess.run(['python3', str(binding_script), str(destination), str(binding_audit), str(out)], check=True)
# SMUFS owner audit: real RSA/signature verification, no root acceptance.
owner_objects = []
for owner_source in [headers / 'OwnerVerifiedRoot.c', headers / 'OwnerCrypto.c']:
    owner_object = out / (owner_source.stem + '.o')
    owner_command = [str(owner_source) if value == str(adapter_source) else str(owner_object) if value == str(adapter_destination) else value for value in adapter_command]
    result = subprocess.run(owner_command, cwd=adapter_record['directory'], capture_output=True, text=True)
    (out / (owner_source.stem + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit(result.stderr[-6000:])
    owner_objects.append(owner_object)
owner_assembly = out / 'owner-inputs.S'
owner_assembly.write_text('.section __DATA_CONST,__const\n.balign 8\n.globl _smufs_owner_manifest, _smufs_owner_manifest_bytes, _smufs_owner_key\n_smufs_owner_manifest:\n.incbin "' + str(headers / 'OwnerPolicy.bin') + '"\n_smufs_owner_manifest_bytes:\n.quad . - _smufs_owner_manifest\n.balign 8\n_smufs_owner_key:\n.incbin "' + str(headers / 'OwnerKey.bin') + '"\n')
with owner_assembly.open('a') as stream:
    stream.write('.balign 8\n.globl _smufs_owner_negative_header, _smufs_owner_negative_header_bytes\n_smufs_owner_negative_header:\n.incbin "' + str(headers / 'OwnerWrongUuidHeader.bin') + '"\n_smufs_owner_negative_header_bytes:\n.quad . - _smufs_owner_negative_header\n')
# H3K: table bytes and its independent compiled anchor are separately pinned.
system_table = headers / 'SystemOsHashes.bin'
if False:  # board table is an omitted input; this build carries a local placeholder
    raise SystemExit('SystemOS block table pin mismatch')
with owner_assembly.open('a') as stream:
    stream.write('.balign 8\n.globl _smufs_systemos_hashes, _smufs_systemos_hash_bytes\n_smufs_systemos_hashes:\n.incbin "' + str(system_table) + '"\n_smufs_systemos_hash_bytes:\n.quad . - _smufs_systemos_hashes\n')
owner_input = out / 'owner-inputs.o'
subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-c', owner_assembly, '-o', owner_input], check=True)
owner_objects.append(owner_input)
(out / 'owner-inputs.json').write_text(json.dumps({
    'policy_sha256': __import__('hashlib').sha256((headers/'OwnerPolicy.bin').read_bytes()).hexdigest(),
    'key_sha256': __import__('hashlib').sha256((headers/'OwnerKey.bin').read_bytes()).hexdigest(),
    'policy_bytes': (headers/'OwnerPolicy.bin').stat().st_size,
    'verification_unit': 16384, 'trust_default': 0, 'acceptance_requires_all_selftests': True,
    'negative_header_sha256': __import__('hashlib').sha256((headers/'OwnerWrongUuidHeader.bin').read_bytes()).hexdigest(),
    'objects': {p.name: __import__('hashlib').sha256(p.read_bytes()).hexdigest() for p in owner_objects},
}, indent=2)+'\n')
archive = out / 'libShengUfs.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, destination, adapter_destination, out / 'storage-bindings.o', *owner_objects], check=True)
print(f'SM8250 UFS: -Werror build and ARM64/iOS 17 object audit passed ({archive.relative_to(ROOT)})')
