"""Build the Legacy display and upstream GPU GOP presenter."""
import hashlib,json,os,struct,subprocess,sys
from pathlib import Path
W=Path(__file__).resolve().parent
ROOT=Path(os.environ.get('SMGOP_BUILD_TREE','/home/ye3912/unixnu-out-beta/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/tree'))
OUT=Path(os.environ.get('SMGOP_OUTPUT',str(W/'preflight')));REF=Path(os.environ.get('SMGOP_REFERENCE_DIR',str(W/'reference')));GOP=Path(os.environ.get('SMGOP_DRIVER_DIR',str(REF/'gop')))
BOARD=Path(os.environ.get('SMGOP_BOARD_SOURCE',str(W/'SmGopDisplay.cpp')))
sys.path.insert(0,str(ROOT/'kits/XnuPrelink'))
from native_prelink import macho,backed_offset,exports
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
audit=json.loads((OUT/'abi.json').read_text());assert audit['passed']
assert sha(GOP/'NativeLegacyDisplay.cpp')=='09d7811db8fc6b659e12a05690867326e851f0d11bec7ffbb36b72c33ea035ca'
assert sha(GOP/'NativeLegacyRequest.h')=='81e89e1f40f2f1d0919567f53bc0564c650488843082693d00b9ea256cafae21'
old=(ROOT.parent/'kernel/kernel.macho').read_bytes();old_image=macho(old)
assert sha(ROOT.parent/'kernel/kernel.macho')==audit['kernel_sha256']
names={n:v for v,group in exports(old,old_image).items() for n in group}
def read(va,n):
 at=backed_offset(old_image['segments'],va,n,len(old));return old[at:at+n]
def require(v,why):
 if not v:raise SystemExit(why)
def run(cmd,label,cwd=None):
 r=subprocess.run(list(map(str,cmd)),cwd=cwd,capture_output=True,text=True)
 (OUT/(label+'.log')).write_text(r.stdout+r.stderr)
 (OUT/(label+'.command.json')).write_text(json.dumps(list(map(str,cmd)),indent=2)+'\n')
 require(r.returncode==0,(r.stdout+r.stderr)[-6000:]);return r.stdout
recipe=json.loads((ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
presenter_upstream=ROOT/'drivers/GopFramebuffer/NativeGOPPresenter.cpp'
presenter=OUT/presenter_upstream.name
presenter_patch=BOARD.parent/'sm8250-gop-presenter.patch'
run(['patch','--batch','--forward','--fuzz=0','-o',presenter,presenter_upstream,presenter_patch],'presenter-patch')
for src,obj in [(GOP/'NativeLegacyDisplay.cpp',OUT/'NativeLegacyDisplay.o'),(BOARD,OUT/'SmGopDisplay.o'),
                (presenter,OUT/'NativeGOPPresenter.o')]:
 args=recipe['arguments'];cmd=[];i=0
 while i<len(args):
  a=args[i]
  if a==recipe['file']:cmd.append(str(src))
  elif a in ('-o','-MF','-MT','-MJ'):
   if a=='-o':cmd+=['-o',str(obj)]
   i+=1
  elif a=='-Xclang' and args[i+1].startswith('-W'):i+=1
  elif not a.startswith('-W'):cmd.append(a)
  i+=1
 cmd+=['-Wall','-Wextra','-Werror','-Wno-unused-parameter','-Wno-missing-designated-field-initializers','-Wno-c99-designator','-Wno-reorder-init-list','-Wno-deprecated-enum-enum-conversion','-Wno-unnecessary-virtual-specifier','-I',str(OUT),'-I',str(presenter_upstream.parent),'-I',str(ROOT/'.cache/xnu-work/osfmk'),'-mgeneral-regs-only']
 run(cmd,src.stem,recipe['directory'])
raw=(OUT/'NativeLegacyDisplay.o').read_bytes();image=macho(raw)
st=next(p for c,p,_ in image['commands'] if c==2);sy,count,strings,_=struct.unpack_from('<4I',raw,st+8);symbols=[]
for i in range(count):
 ix,kind,section,_,va=struct.unpack_from('<IBBHQ',raw,sy+i*16);symbols.append((raw[strings+ix:raw.index(b'\0',strings+ix)].decode(),va,kind))
vtable=next(va for name,va,_ in symbols if name=='__ZTV19NativeLegacyDisplay');relocations={}
for segment in image['segments']:
 for sec in segment['sections']:
  if sec['va']<=vtable<sec['va']+sec['size']:
   ro,rc=struct.unpack_from('<2I',raw,sec['command_offset']+56)
   for i in range(rc):
    offset,bits=struct.unpack_from('<2I',raw,ro+i*8)
    if bits>>27&1 and bits>>28==0 and bits>>25&3==3:relocations[sec['va']+offset]=symbols[bits&0xffffff][0]
compiled=[relocations.get(vtable+16+i*8) for i in range(358)]
require(all(compiled) and vtable+16+358*8 not in relocations,'Exact358-slot derived table')
bindings={'__ZN25IOMobileFramebufferLegacyC2EPK11OSMetaClass':0xfffffff0057865cc,'__ZN25IOMobileFramebufferLegacyD2Ev':0xfffffff005786618,'__ZN25IOMobileFramebufferLegacy10gMetaClassE':0xfffffff006b35940,'__ZTV25IOMobileFramebufferLegacy':int(audit['legacy_vptr'],16)-16}
pure=audit['abstract_slots'];methods=audit['methods'];owned=[]
for i,name in enumerate(compiled):
 expected=int(methods[i]['address'],16)
 if name.startswith(('__ZN19NativeLegacyDisplay','__ZNK19NativeLegacyDisplay')):owned.append(i)
 elif '25IOMobileFramebufferLegacy' in name:
  require(i not in pure,'No pure base alias');require(name not in bindings or bindings[name]==expected,'Consistent base alias');bindings[name]=expected
 else:require(i<168 and names.get(name)==expected,'Native inherited slot'+str(i))
require(owned==sorted([0,1,7,14,84,86,87,141,155,184,188,288,290,296,301]+pure),'Exact owned method set')
undefined={n for n,_,k in symbols if k==1 and 'IOMobileFramebufferLegacy' in n};require(undefined==set(bindings),'All original-class imports covered')
# Reuse the official exact-byte assertions and aliases for IOSurface getters,
# vsync, refresh, constructor ancestry, dimensions and completion conventions.
ref=(REF/'build-gop-legacy-display.py').read_text();block=ref[ref.index('for name,address,field,wide in ['):ref.index("asm=OUT/")]
exec(compile(block,'official-original-byte-checks','exec'))
asm=OUT/'bindings.S';asm.write_text(''.join(f'.globl {n}\n.set {n}, {v:#x}\n' for n,v in sorted(bindings.items())))
run(['clang','-target','arm64-apple-ios17.0','-c',asm,'-o',OUT/'bindings.o'],'bindings')
run(['llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp',OUT/'NativeLegacyDisplay.o',OUT/'NativeLegacyDisplay.late.o'],'late')
run(['llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_NativeGOPPresenter.cpp',OUT/'NativeGOPPresenter.o',OUT/'NativeGOPPresenter.late.o'],'presenter-late')
run(['llvm-objcopy','--remove-section=__DATA,__mod_init_func','--globalize-symbol=__GLOBAL__sub_I_SmGopDisplay.cpp',OUT/'SmGopDisplay.o',OUT/'SmGopDisplay.late.o'],'board-late')
archive=OUT/'libNativeLegacyDisplay.late.a'
import tempfile
with tempfile.TemporaryDirectory(prefix='display-archive-',dir=OUT) as tmp:
 candidate=Path(tmp)/archive.name
 run(['llvm-ar','rcs',candidate,OUT/'NativeLegacyDisplay.late.o',OUT/'NativeGOPPresenter.late.o',OUT/'SmGopDisplay.late.o',OUT/'bindings.o'],'archive')
 candidate.replace(archive)
report={'passed':True,'archive_sha256':sha(archive),'source_sha256':{str(p):sha(p) for p in [GOP/'NativeLegacyDisplay.cpp',GOP/'NativeLegacyRequest.h',BOARD,presenter_upstream,presenter_upstream.with_name('NativeGOPWire.h'),presenter_patch]},
 'bindings':{n:hex(v) for n,v in bindings.items()},'owned_entries':owned,'original_kernel_sha256':audit['kernel_sha256'],'abi_sha256':sha(OUT/'abi.json'),
 'gop_revision':'cf3b42979a9c44219e7bb149543e6fc3388b58ed','hardware_tested':False}
(OUT/'build-report.json').write_text(json.dumps(report,indent=2)+'\n');print('Display and upstream GPU GOP presenter compiled')
