#!/usr/bin/env python3
"""Build the pinned USB logger with real target flags; no online hardware work."""
import hashlib,importlib.util,json,os,shutil,subprocess,tempfile
from pathlib import Path
ROOT=Path(os.environ.get('SMUFS_BUILD_TREE',str(Path(__file__).resolve().parents[1])))
KIT=ROOT/'kits/instantnoodlep';M=KIT/'QcomXnuUsbDebug';OUT=ROOT/'build/qcom-usb-debug';OUT.mkdir(parents=True,exist_ok=True)
record=ROOT/'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_kprintf.o.json'
with tempfile.TemporaryDirectory(prefix='build-',dir=OUT) as scratch:
 out=Path(scratch)/'.cache/module'
 subprocess.run(['python3',str(M/'scripts/build.py'),'--compile-record',str(record),'--soc','soc/Sm8250.h','--output',str(out)],check=True)
 spec=importlib.util.spec_from_file_location('usb_builder',M/'scripts/build.py');b=importlib.util.module_from_spec(spec);spec.loader.exec_module(b)
 recipe=json.loads(record.read_text().strip().rstrip(','));flags,cwd=b.compile_flags(recipe)
 obj=out/'SmUsbPanicNote.o';source=KIT/'SmUsbPanicNote.c'
 cmd=flags+['-c',str(source),'-o',str(obj)]
 p=subprocess.run(cmd,cwd=cwd,capture_output=True,text=True);(out/'panic-note.log').write_text(p.stdout+p.stderr)
 assert p.returncode==0,p.stderr
 subprocess.run(['llvm-ar','r',str(out/'libQcomXnuUsbDebug.a'),str(obj)],check=True)
 report=json.loads((out/'build-report.json').read_text());report['panic_note_sha256']=hashlib.sha256(source.read_bytes()).hexdigest()
 report['archive_sha256']=hashlib.sha256((out/'libQcomXnuUsbDebug.a').read_bytes()).hexdigest()
 (out/'build-report.json').write_text(json.dumps(report,indent=2)+'\n')
 for f in out.iterdir():shutil.copy2(f,OUT/f.name)
print('Built SM8250 USB CDC logger and bounded panic-note bridge; native result pending')
