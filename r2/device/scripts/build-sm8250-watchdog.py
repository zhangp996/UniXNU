#!/usr/bin/env python3
"""Qualify the original 21H461 watchdog ABI and build its software backend."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys

ROOT = Path(__import__('os').environ.get('SMUFS_BUILD_TREE',str(Path(__file__).resolve().parents[1])))
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho, backed_offset, exports, unserialize_xml

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--reference', type=Path, default=ROOT.parent/'kernel/kernel.macho')
p.add_argument('--kernelcache', type=Path, default=ROOT/'.cache/firmware/kernelcache.macho/kernelcache.release.ipad7.decompressed')
p.add_argument('--output', type=Path, default=ROOT/'build/smufs-watchdog')
p.add_argument('--source',type=Path,default=ROOT/'kits/instantnoodlep/SmUfsWatchdog.cpp')
p.add_argument('--standalone',action='store_true')
a = p.parse_args()
out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
sha = lambda b: hashlib.sha256(b).hexdigest()
raw = a.kernelcache.read_bytes()
assert sha(raw) == 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
original = macho(raw)
read = lambda va, n: raw[backed_offset(original['segments'], va, n, len(raw)):][:n]
ptr = lambda va: struct.unpack('<Q', read(va, 8))[0]
assert read(0xfffffff0055adce4, 11) == b'IOWatchdog\0'
assert struct.unpack('<I', read(0xfffffff00663e450, 4))[0] == 0x52801f03  # class size 248
assert struct.unpack('<I', read(0xfffffff00663e3a0, 4))[0] == 0x52801d03  # client size 232
vptr = 0xfffffff006f247e8
native = {n: va for va, names in exports(raw, original).items() for n in names}
assert [ptr(vptr+i*8) for i in (168,169,170)] == [native['___cxa_pure_virtual']]*3
assert ptr(vptr+171*8) == 0
assert ptr(vptr+87*8) == 0xfffffff00663eb6c and ptr(vptr+140*8) == 0xfffffff00663ee14
assert ptr(vptr+84*8) == native['__ZN9IOService15registerServiceEj']
# Exact original heartbeat fields/updates, before accepting the private offsets.
for va, word in [(0xfffffff00663f134,0x52800028), (0xfffffff00663f138,0x3902ae68),
                 (0xfffffff00663f144,0x91032262), (0xfffffff00663f160,0xf9005e68),
                 (0xfffffff00663f20c,0x3902a27f), (0xfffffff00663f298,0x3902a288),
                 (0xfffffff00663f29c,0x91032282)]:
    assert struct.unpack('<I', read(va,4))[0] == word, hex(va)
# Original start dispatches registerService(options=0) through virtual slot 84.
for va, word in [(0xfffffff00663e7f0,0xf9415108), (0xfffffff00663e7f8,0x52800001),
                 (0xfffffff00663e7fc,0xd63f0100)]:
    assert struct.unpack('<I', read(va,4))[0] == word, hex(va)
reference = a.reference.read_bytes(); ref = macho(reference)
plan = json.loads(a.reference.with_suffix('.json').read_text())
slide = plan['slide']; assert slide == -0x1000000
info = next(s for s in original['segments'] if s['name']=='__PRELINK_INFO')
entries = unserialize_xml(read(info['va'],info['filesize']))['_PrelinkInfoDictionary']
entry = next(e for e in entries if e['CFBundleIdentifier']=='com.apple.driver.AppleARMWatchdogTimer')
module = macho(raw, backed_offset(original['segments'],entry['_PrelinkExecutableLoadAddr'],32,len(raw)))
for s in module['segments']:
    if not s['filesize']: continue
    bound = next(e for e in plan['extents'] if e['module']==entry['CFBundleIdentifier'] and e['segment']==s['name'])
    assert bound['va']==s['va']+slide
text = next(s for s in module['segments'] if s['name']=='__TEXT_EXEC')
assert sha(read(text['va'],text['filesize'])) == plan['original_executable_sha256'][entry['CFBundleIdentifier']]
header = '''#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
class IOWatchdog : public IOService {
 OSDeclareAbstractStructors(IOWatchdog)
 unsigned char originalPrivate[0xf8-sizeof(IOService)];
public:
 void stop(IOService *) override;
 IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) override;
 virtual void enableBackend() = 0;
 virtual void disableBackend() = 0;
 virtual void restartBackend() = 0;
};
static_assert(sizeof(IOWatchdog)==0xf8,"Original watchdog storage");
'''
(out/'OriginalWatchdogABI.h').write_text(header)
source = a.source.resolve(); obj = out/'SmUfsWatchdog.o'
recipe = json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
command=[]; i=0; arguments=recipe['arguments']
while i<len(arguments):
    v=arguments[i]
    if v==recipe['file']: command.append(str(source))
    elif v in ('-o','-MF','-MT','-MJ'):
        if v=='-o': command+=['-o',str(obj)]
        i+=1
    elif v=='-Xclang' and arguments[i+1].startswith('-W'): i+=1
    elif not v.startswith('-W'): command.append(v)
    i+=1
command+=['-Wall','-Wextra','-Werror','-Wno-missing-designated-field-initializers','-Wno-unused-parameter','-Wno-c99-designator',
          '-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion','-Wno-unnecessary-virtual-specifier',
          '-I',str(out),'-I',str(ROOT/'.cache/xnu-work/osfmk'),'-mgeneral-regs-only']
(out/'compile-command.json').write_text(json.dumps(command,indent=2)+'\n')
compiled_result=subprocess.run(command,cwd=recipe['directory'],capture_output=True,text=True)
(out/'compile.log').write_text(compiled_result.stdout+compiled_result.stderr)
if compiled_result.returncode:
    raise SystemExit(compiled_result.stderr[-5000:])
compiled=obj.read_bytes(); image=macho(compiled)
sym=next(at for cmd,at,_ in image['commands'] if cmd==2)
at,n,strings,_=struct.unpack_from('<4I',compiled,sym+8);symbols=[]
for i in range(n):
    ix,kind,_,_,va=struct.unpack_from('<IBBHQ',compiled,at+i*16)
    symbols.append((compiled[strings+ix:compiled.index(b'\0',strings+ix)].decode(),va,kind))
table=next(va for name,va,_ in symbols if name=='__ZTV21SmUfsPlatformWatchdog')
relocations={}
for segment in image['segments']:
    for section in segment['sections']:
        if section['va']<=table<section['va']+section['size']:
            at,n=struct.unpack_from('<2I',compiled,section['command_offset']+56)
            for i in range(n):
                offset,bits=struct.unpack_from('<2I',compiled,at+i*8)
                if bits>>27&1 and bits>>28==0 and bits>>25&3==3:
                    relocations[section['va']+offset]=symbols[bits&0xffffff][0]
slots=[relocations.get(table+16+i*8) for i in range(171)]
assert all(slots) and table+16+171*8 not in relocations
for i,name in enumerate(slots):
    if i in (0,1,7,14,84,86,87,140,168,169,170): assert '21SmUfsPlatformWatchdog' in name,(i,name)
    else: assert native.get(name)==ptr(vptr+i*8),(i,name,hex(ptr(vptr+i*8)))
bindings={
 '__ZN10IOWatchdogC2EPK11OSMetaClass':0xfffffff00663e400,
 '__ZN10IOWatchdogD2Ev':0xfffffff00663e420,
 '__ZN10IOWatchdog10gMetaClassE':0xfffffff007b2d268,
 '__ZTV10IOWatchdog':vptr-16,
 '_smufs_original_watchdog_start':0xfffffff00663e498}
undefined={name for name,_,kind in symbols if kind==1 and ('IOWatchdog' in name or name=='_smufs_original_watchdog_start')}
assert undefined==set(bindings),undefined
asm=out/'original-bindings.S'
asm.write_text(''.join(f'.globl {name}\n.set {name}, {va+slide:#x}\n' for name,va in bindings.items()))
subprocess.run(['xcrun','clang','-target','arm64-apple-ios17.0','-Werror','-c',str(asm),'-o',str(out/'SmUfsWatchdog-bindings.o')],check=True)
late=out/'SmUfsWatchdog.late.o'
subprocess.run(['llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_SmUfsWatchdog.cpp',str(obj),str(late)],check=True)
for b in [compiled,late.read_bytes()]:
    section=next(s for g in macho(b)['segments'] for s in g['sections'] if s['name']=='__text')
    if b is compiled: original_text=b[section['fileoff']:section['fileoff']+section['size']]
    else: assert b[section['fileoff']:section['fileoff']+section['size']]==original_text
report=dict(source_sha256=sha(source.read_bytes()),object_sha256=sha(late.read_bytes()),bindings_sha256=sha((out/'SmUfsWatchdog-bindings.o').read_bytes()),
            original_kernelcache_sha256=sha(raw),original_executable_sha256=plan['original_executable_sha256'][entry['CFBundleIdentifier']],
            reference_kernel_sha256=sha(reference),virtual_slots=171,original_base_size=248,original_client_size=232,
            bindings={name:hex(va+slide) for name,va in bindings.items()},native_slot_names=slots,
            hardware_watchdog_implemented=False,original_executable_modified=False,static_checks_passed=True,standalone=a.standalone)
(out/'watchdog-build.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS original watchdog ABI, 171 slots, heartbeat offsets, original client and late registration')

archive=out/'libSmUfsWatchdog.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun','ar','rcs',str(archive),str(late),str(out/'SmUfsWatchdog-bindings.o')],check=True)
report['archive_sha256']=sha(archive.read_bytes())
if not a.standalone:
    # This is a board service, collected with the existing board PMGR bridge.
    # The normal platform-power recipe regenerates that archive before us.
    container=ROOT/'build/sheng-platform-power/libShengPlatformPower.a'
    assert container.is_file(), 'Board platform archive must be built first'
    before=subprocess.check_output(['xcrun','ar','t',str(container)],text=True).splitlines()
    assert before and not any(n in before for n in ['SmUfsWatchdog.late.o','SmUfsWatchdog-bindings.o'])
    report['platform_archive_before_sha256']=sha(container.read_bytes())
    subprocess.run(['xcrun','ar','r',str(container),str(late),str(out/'SmUfsWatchdog-bindings.o')],check=True)
    after=subprocess.check_output(['xcrun','ar','t',str(container)],text=True).splitlines()
    assert after==before+['SmUfsWatchdog.late.o','SmUfsWatchdog-bindings.o']
    report['platform_archive_members_before']=before
    report['platform_archive_members_after']=after
    report['platform_archive_sha256']=sha(container.read_bytes())
(out/'watchdog-build.json').write_text(json.dumps(report,indent=2)+'\n')
print('PASS board watchdog service and original bindings archived; native startup remains pending')
