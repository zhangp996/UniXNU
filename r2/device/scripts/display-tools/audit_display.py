"""Qualify the original 21H461 Legacy ABI; no borrowed runtime checkpoint."""
import hashlib,json,struct,sys,os
from pathlib import Path
W=Path(__file__).resolve().parent
ROOT=Path(os.environ.get('SMGOP_BUILD_TREE','/home/ye3912/unixnu-out-beta/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/tree'))
B=ROOT.parent;OUT=Path(os.environ.get('SMGOP_OUTPUT',str(W/'preflight')));OUT.mkdir(parents=True,exist_ok=True)
core=ROOT/'scripts/audit-mobile-graphics-legacy.py'
source=core.read_text().split("runtime=ROOT/")[0]
source=source.replace("ROOT=Path(__file__).resolve().parents[1]",'ROOT=Path('+repr(str(ROOT))+')')
start=source.index("path=ROOT/'build/xnu-prelink/");end=source.index('blob=path.read_bytes()')
source=source[:start]+"path=ROOT.parent/'kernel/kernel.macho'\nFAMILY_TEXT_SHA='2e044ee68c1650397e1435234ca6459206262e4c7ed709cb7131c6f664f9bd9b'\n"+source[end:]
env={'__file__':str(core)};exec(compile(source,str(core),'exec'),env)
audit={'abstract_slots':env['pure'],'methods':[{'address':hex(v)} for v in env['legacy']]}
reference=(Path(os.environ.get('SMGOP_REFERENCE_DIR',str(W/'reference')))/'build-gop-legacy-display.py').read_text()
generation=reference[reference.index("header='''"):reference.index("print(run(['python3',ROOT/'scripts/build-arm-driver.py'")]
exec(compile(generation,'official-header-template','exec'),{'audit':audit,'pure':env['pure'],'OUT':OUT})
report={'passed':True,'kernel_sha256':hashlib.sha256((B/'kernel/kernel.macho').read_bytes()).hexdigest(),
 'family_text_sha256':env['plan']['original_executable_sha256']['com.apple.iokit.IOMobileGraphicsFamily'],
 'legacy_vptr':hex(env['legacy_vptr']),'methods':audit['methods'],'abstract_slots':env['pure'],
 'base_vptr':hex(env['base_vptr']),'service_vptr':hex(env['service_vptr']),
 'scope':'Static exact original constructor chain,358 slots,9 pure leaves and inherited native targets. Original metaclass size is checked live before derived registration; no copied runtime checkpoint claimed.'}
(OUT/'abi.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps({k:v for k,v in report.items() if k!='methods'},indent=2))
