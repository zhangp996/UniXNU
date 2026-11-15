#!/usr/bin/env python3
"""Compile the board RAM boot-status interface with actual target headers."""
import hashlib,json,os,subprocess,sys
from pathlib import Path
ROOT=Path(os.environ.get('SMUFS_BUILD_TREE',str(Path(__file__).resolve().parents[1])))
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho
out=ROOT/'build/smufs-boot-status';out.mkdir(exist_ok=True,parents=True)
source=ROOT/'kits/instantnoodlep/SmUfsBootStatus.cpp';obj=out/'SmUfsBootStatus.o'
recipe=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
command=[];i=0;arguments=recipe['arguments']
while i<len(arguments):
 v=arguments[i]
 if v==recipe['file']:command.append(str(source))
 elif v in ('-o','-MF','-MT','-MJ'):
  if v=='-o':command+=['-o',str(obj)]
  i+=1
 elif v=='-Xclang' and arguments[i+1].startswith('-W'):i+=1
 elif not v.startswith('-W'):command.append(v)
 i+=1
command+=['-Wall','-Wextra','-Werror','-Wno-missing-designated-field-initializers','-Wno-unused-parameter','-Wno-c99-designator','-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion','-Wno-unnecessary-virtual-specifier','-I',str(ROOT/'.cache/xnu-work/osfmk'),'-mgeneral-regs-only']
(out/'compile-command.json').write_text(json.dumps(command,indent=2)+'\n')
p=subprocess.run(command,cwd=recipe['directory'],capture_output=True,text=True)
(out/'compile.log').write_text(p.stdout+p.stderr)
if p.returncode:raise SystemExit(p.stderr[-6000:])
late=out/'SmUfsBootStatus.late.o'
subprocess.run(['llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_SmUfsBootStatus.cpp',str(obj),str(late)],check=True)
raw=obj.read_bytes();derived=late.read_bytes()
def text(b):
 s=next(s for g in macho(b)['segments'] for s in g['sections'] if s['name']=='__text');return b[s['fileoff']:s['fileoff']+s['size']]
assert text(raw)==text(derived)
assert not any(s['name']=='__mod_init_func' for g in macho(derived)['segments'] for s in g['sections'])
archive=ROOT/'build/sheng-platform-power/libShengPlatformPower.a'
before=subprocess.check_output(['llvm-ar','t',str(archive)],text=True).splitlines()
assert 'SmUfsBootStatus.late.o' not in before
subprocess.run(['llvm-ar','r',str(archive),str(late)],check=True)
after=subprocess.check_output(['llvm-ar','t',str(archive)],text=True).splitlines()
assert after==before+['SmUfsBootStatus.late.o']
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
(out/'build.json').write_text(json.dumps({'source_sha256':sha(source),'object_sha256':sha(late),'archive_sha256':sha(archive),'members':after,'late_init_only':True,'hardware_pmu':False},indent=2)+'\n')
print('PASS target boot-status compile/late-init/archive; native result pending')
