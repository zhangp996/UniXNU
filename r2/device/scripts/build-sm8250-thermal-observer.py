#!/usr/bin/env python3
"""Board two-snapshot thermal acquisition using actual target compile record."""
import hashlib,json,os,subprocess
from pathlib import Path
ROOT=Path(os.environ.get('SMUFS_BUILD_TREE',str(Path(__file__).resolve().parents[1])))
out=ROOT/'build/sm8250-thermal-observer';out.mkdir(exist_ok=True,parents=True)
source=ROOT/'kits/instantnoodlep/SmTsensReadOnly.cpp';obj=out/'SmTsensReadOnly.o'
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
command+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-missing-designated-field-initializers','-Wno-c99-designator','-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion','-I',str(ROOT/'.cache/xnu-work/osfmk'),'-mgeneral-regs-only']
(out/'compile-command.json').write_text(json.dumps(command,indent=2)+'\n')
p=subprocess.run(command,cwd=recipe['directory'],capture_output=True,text=True);(out/'compile.log').write_text(p.stdout+p.stderr)
if p.returncode:raise SystemExit(p.stderr[-6000:])
archive=ROOT/'build/sheng-platform-power/libShengPlatformPower.a'
before=subprocess.check_output(['llvm-ar','t',str(archive)],text=True).splitlines();assert obj.name not in before
subprocess.run(['llvm-ar','r',str(archive),str(obj)],check=True)
after=subprocess.check_output(['llvm-ar','t',str(archive)],text=True).splitlines();assert after==before+[obj.name]
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
(out/'build.json').write_text(json.dumps({'source_sha256':sha(source),'header_sha256':sha(source.with_suffix('.h')),'object_sha256':sha(obj),'members':after,'register_writes':False,'readonly':True,'hardware_qualified':False},indent=2)+'\n')
print('PASS target readonly TSENS acquisition compile/archive; native measurement pending')
