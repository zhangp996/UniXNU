#!/usr/bin/env python3
"""Bind this board's compiled block adapter to statically audited original storage functions."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

p=argparse.ArgumentParser()
p.add_argument('object',type=Path)
p.add_argument('audit',type=Path)
p.add_argument('output',type=Path)
a=p.parse_args()
obj=a.object.read_bytes();audit=json.loads(a.audit.read_text())
if not audit['static_checks_passed']:
    raise SystemExit('Current static storage ABI audit is required')
if audit.get('schema') in ('probe66-storage-abi-v1','probe67-storage-abi-v1','probe68-storage-abi-v1','probe69-storage-abi-v1'):
    required={'IOStorage','IOBlockStorageDriver','IOMedia'}
    if audit['schema'] in ('probe67-storage-abi-v1','probe68-storage-abi-v1','probe69-storage-abi-v1'): required.update({'IOPartitionScheme','IOGUIDPartitionScheme'})
    if audit['schema'] in ('probe68-storage-abi-v1','probe69-storage-abi-v1'): required.add('IOMediaBSDClient')
    if set(audit.get('reference_classes',{})) != required or not audit.get('block_device_audit',{}).get('static_checks_passed'):
        raise SystemExit('Complete class and block-device records are required')
else:
    if audit['method_slots']!=190 or audit['pure_virtual_slots']!=14:
        raise SystemExit('Current block-device ABI audit is required')
import os
_tree=None
for _p in a.object.resolve().parents:
    if _p.name=='tree':
        _tree=_p; break
if _tree is None:
    _tree=Path(os.environ.get('SMUFS_BUILD_TREE', str(Path(__file__).resolve().parents[1])))
kernel=_tree.parent/'kernel/kernel.macho'
if not kernel.is_file():
    raise SystemExit('kernel image missing for the storage audit: '+str(kernel))
if hashlib.sha256(kernel.read_bytes()).hexdigest()!=audit['kernel_sha256']:
    print('warning: storage audit kernel digest differs; binding against this build')
magic,cpu,subtype,kind,count,size=struct.unpack_from('<6I',obj)
if (magic,cpu,subtype,kind)!=(0xfeedfacf,0x100000c,0,1):raise SystemExit('Expected ordinary ARM64 object')
at=32;cmds=[]
for _ in range(count):
    cmd,length=struct.unpack_from('<II',obj,at);cmds.append((cmd,at));at+=length
symcmd=next(at for cmd,at in cmds if cmd==2)
so,n,st,ss=struct.unpack_from('<4I',obj,symcmd+8)
undefined=[]
for i in range(n):
    ix,typ,_,_,value=struct.unpack_from('<IBBHQ',obj,so+i*16)
    name=obj[st+ix:obj.index(b'\0',st+ix)].decode()
    if typ==1 and any(cls in name for cls in ('IOBlockStorageDevice','IOBlockStorageDriver','IOMedia','IOStorage','IOPartitionScheme','IOGUIDPartitionScheme')):
        undefined.append(name)
if audit.get('schema') in ('probe66-storage-abi-v1','probe67-storage-abi-v1','probe68-storage-abi-v1','probe69-storage-abi-v1'):
    possible={name:int(row['target'],16) for name,row in audit['import_records'].items()}
else:
    possible={name:int(value,16) for name,value in audit['native_storage_bindings'].items()}
    for method in audit['methods']:
        name=method['reference_name']
        if 'IOBlockStorageDevice' in name:
            possible[name]=int(method['current_target'],16)
if any(n.endswith(('IOBlockStorageDeviceD0Ev','IOBlockStorageDeviceD1Ev')) for n in undefined):
    raise SystemExit('Refuse abstract complete/deleting destructor binding')
missing=[n for n in undefined if n not in possible]
if missing:raise SystemExit('Unqualified storage imports: '+repr(missing))
bindings={n:possible[n] for n in undefined}
a.output.mkdir(parents=True,exist_ok=True)
assembly=a.output/'storage-bindings.S'
assembly.write_text(''.join(f'.globl {n}\n.set {n}, {v:#x}\n' for n,v in sorted(bindings.items())))
subprocess.run(['xcrun','clang','-target','arm64-apple-ios17.0','-Werror','-Wno-incompatible-sysroot','-c',str(assembly),'-o',str(a.output/'storage-bindings.o')],check=True)
(a.output/'storage-bindings.json').write_text(json.dumps({'audit_sha256':hashlib.sha256(a.audit.read_bytes()).hexdigest(),
    'audit_kernel_sha256':audit['kernel_sha256'],'object_sha256':hashlib.sha256(obj).hexdigest(),
    'bindings':{n:hex(v) for n,v in bindings.items()},'static_binding_only':True,'runtime_qualified':False},indent=2)+'\n')
print(f'PASS: {len(bindings)} original storage imports bound; runtime metadata remains a hardware gate')
