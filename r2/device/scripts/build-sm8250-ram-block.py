#!/usr/bin/env python3
"""Audit RamBlock's real storage ABI and share the board's existing aliases.

Runs after component:RamBlock and build-sm8250-ufs.py. It retains the actual
transport implementation, not a second IOBlockStorageDevice implementation.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import difflib

ROOT = Path(__file__).resolve().parents[1]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--tree', type=Path, default=ROOT)
p.add_argument('--device', type=Path)
p.add_argument('--object', type=Path)
p.add_argument('--audit', type=Path)
p.add_argument('--output', type=Path)
p.add_argument('--inspect-only', action='store_true')
a = p.parse_args()
ROOT = a.tree.resolve()
device = a.device
if device is None:
    for kit in sorted((ROOT / 'kits').iterdir()):
        if kit.is_symlink() and (kit.resolve().parent / 'board.toml').is_file():
            device = kit.resolve().parent
            break
if device is None:
    sys.exit('Board repository was not resolved')
audit_path = a.audit or device / 'drivers/Sm8250Ufs/storage-binding-audit.json'
audit = json.loads(audit_path.read_text())
b = audit['block_device_audit']
if not audit['static_checks_passed'] or not b['static_checks_passed'] or b['method_slots'] != 190 or not b['abstract_complete_destructors_trap']:
    sys.exit('Require qualified original static storage ABI')
out = a.output or ROOT / 'build/ram-block'
out.mkdir(parents=True, exist_ok=True)
obj = a.object or out / 'NativeRamBlock.o'
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
base = ROOT / 'drivers/RamBlock/NativeRamBlock.cpp'
variant_path = device / 'patches/ram-block/sm8250-ownerdata.json'
variant = json.loads(variant_path.read_text())
if sha(base) != variant['base_sha256'] or variant['base_revision'] != '25c57e0e9b68bf452dd1986914525edc9d2320bc':
    sys.exit('Pinned upstream RamBlock source changed')
text = base.read_text()
for operation in variant['operations']:
    if text.count(operation['old']) != 1:
        sys.exit('OwnerData board delta no longer applies uniquely')
    text = text.replace(operation['old'],operation['new'],1)
if hashlib.sha256(text.encode()).hexdigest() != variant['variant_sha256']:
    sys.exit('Board RAM variant identity/backend check failed')
diff = ''.join(difflib.unified_diff(base.read_text().splitlines(True),text.splitlines(True),'a/NativeRamBlock.cpp','b/NativeRamBlock.cpp'))
if diff != variant_path.with_suffix('.patch').read_text():
    sys.exit('Reviewable board patch differs from applied source operations')
if not a.inspect_only:
    command = json.loads((out / 'compile-command.json').read_text())
    if command.count(str(base)) != 1:
        sys.exit('Expected exact component compile source')
    derived = out / 'NativeRamBlock-ownerdata.cpp'
    derived.write_text(text)
    command = [str(derived) if value == str(base) else value for value in command]
    command += ['-I',str(base.parent)]
    # Kernel driver callbacks cannot use the user floating/SIMD register state.
    command += ['-mgeneral-regs-only']
    record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
    (out / 'board-compile-command.json').write_text(json.dumps(command, indent=2)+'\n')
    with (out / 'board-compile.log').open('w') as stream:
        result = subprocess.run(command, cwd=record['directory'], stdout=stream, stderr=subprocess.STDOUT)
    if result.returncode:
        sys.exit((out / 'board-compile.log').read_text()[-5000:])
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
import native_prelink as np
data = obj.read_bytes()
image = np.macho(data)
symcmd = next(at for cmd, at, _ in image['commands'] if cmd == 2)
so, n, st, _ = struct.unpack_from('<4I', data, symcmd+8)
symbols = []
descriptions = {}
for i in range(n):
    ix, typ, section, desc, value = struct.unpack_from('<IBBHQ', data, so+16*i)
    name = data[st+ix:data.index(b'\0', st+ix)].decode()
    symbols.append((name, value, typ, section))
    descriptions[name] = desc
vtable = next(value for name,value,typ,section in symbols if name == '__ZTV14NativeRamBlock' and section)
relocs = {}
for seg in image['segments']:
    for sec in seg['sections']:
        if sec['va'] <= vtable < sec['va']+sec['size']:
            ro, rn = struct.unpack_from('<II', data, sec['command_offset']+56)
            for i in range(rn):
                address, word = struct.unpack_from('<II', data, ro+8*i)
                if word>>27&1 and word>>28 == 0 and word>>25&3 == 3:
                    relocs[sec['va']+address] = symbols[word&0xffffff][0]
methods = [relocs.get(vtable+16+8*i) for i in range(190)]
if not all(methods) or vtable+16+190*8 in relocs or '___cxa_pure_virtual' in methods:
    sys.exit('Incomplete/extra/abstract RAM transport vtable')
expected = [row['reference_name'] for row in b['methods']]
names = list(dict.fromkeys(methods+expected))
plain = subprocess.check_output(['xcrun','c++filt'], input='\n'.join(name[1:] for name in names)+'\n', text=True).splitlines()
readable = dict(zip(names,plain))
overrides = 0
for i,(actual,reference) in enumerate(zip(methods,expected)):
    own = actual.startswith(('__ZN14NativeRamBlock','__ZNK14NativeRamBlock'))
    if reference == '___cxa_pure_virtual' and not own:
        sys.exit(f'Missing required concrete operation at {i}')
    if reference != '___cxa_pure_virtual' and actual != reference and (not own or (i not in (0,1) and readable[actual].split('::',1)[1] != readable[reference].split('::',1)[1])):
        sys.exit(f'RAM transport dispatch mismatch at {i}')
    overrides += own
ufs = json.loads((ROOT / 'build/sheng-ufs/storage-bindings.json').read_text())
possible = dict(b['native_storage_bindings'])
possible.update({name: row['target'] for name,row in audit['import_records'].items()})
undefined = [name for name,value,typ,section in symbols if typ == 1 and 'IOBlockStorageDevice' in name]
if any(name.endswith(('IOBlockStorageDeviceD0Ev','IOBlockStorageDeviceD1Ev')) for name in undefined):
    sys.exit('Refuse abstract complete/deleting destructor calls')
if any(name not in possible or (name in ufs['bindings'] and ufs['bindings'][name] != possible[name]) for name in undefined):
    sys.exit('RAM imports do not exactly match the board storage aliases')
additional = {name:possible[name] for name in undefined if name not in ufs['bindings']}
actual_optional = {name for name,value,typ,section in symbols if typ == 1 and name.startswith('_native_ufs_store_')}
expected_optional = {'_native_ufs_store_' + operation for operation in ('mode', 'open', 'read', 'write', 'sync', 'gpt_check')}
if actual_optional != expected_optional:
    sys.exit('RamBlock persistent store imports do not match the board backend')
record = dict(object_sha256=sha(obj), static_checks_passed=True, virtual_methods=190,
              concrete_overrides=overrides, source_revision='25c57e0e9b68bf452dd1986914525edc9d2320bc',
              storage_audit_sha256=sha(audit_path), storage_code_sha256=audit['current_storage_code_sha256'],
              bindings={name:possible[name] for name in undefined}, duplicate_alias_objects_linked=False,
              additional_nonoverlapping_bindings=additional,
              ownerdata_variant_sha256=variant['variant_sha256'],ownerdata_patch_sha256=sha(variant_path.with_suffix('.patch')),
              persistent_store_imports=sorted(actual_optional),
              actual_io_qualified=False, inspect_only=a.inspect_only)
if not a.inspect_only:
    archive = out / 'libRamBlock.a'
    assembly = out / 'board-extra-bindings.S'
    assembly.write_text(''.join(f'.globl {name}\n.set {name}, {value}\n' for name,value in sorted(additional.items())))
    extra = out / 'board-extra-bindings.o'
    subprocess.run(['xcrun','clang','-target','arm64-apple-ios17.0','-c',assembly,'-o',extra],check=True)
    archive.unlink(missing_ok=True)
    subprocess.run(['xcrun','ar','rcs',archive,obj,extra], check=True)
    record['archive_sha256'] = sha(archive)
(out / 'board-bindings.json').write_text(json.dumps(record,indent=2)+'\n')
print(json.dumps(record,indent=2))
