import hashlib, shutil, struct, subprocess
from pathlib import Path
OUT = Path('/mnt/d/新建文件夹/instantnoodlep-artifacts')
SKEL = OUT/'bootEU.img'
BASE = Path('/home/dev/unixnu/inputs/original-booteu/payload.img')
ADDON = Path('/home/dev/unixnu/_ref/artifacts/14012210733/gpuaddon.bin')
KERNEL = Path('/home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/kernel/kernel.macho')
AMFI_NP = Path('/home/dev/unixnu/full-project/r2/device/scripts/patch-amfi-nopmgr.py')
AMFI_TC = Path('/home/dev/unixnu/full-project/r2/device/scripts/patch-amfi-trustcache-gate.py')

def sh(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    print(' '.join(str(x) for x in cmd[:3]), '->', r.returncode, (r.stderr or '')[:120])
    return r

# 1) patched kernel copy
kp = Path('/tmp/kernel-8c-amfi.macho')
shutil.copyfile(KERNEL, kp)
sh(['python3', str(AMFI_NP), str(kp)])
sh(['python3', str(AMFI_TC), str(kp)])
ksha = hashlib.sha256(kp.read_bytes()).hexdigest()
print('patched kernel sha', ksha[:16])

def build(name, persist=2, addon=True):
    tmp = Path('/tmp/'+name+'-payload.img')
    shutil.copyfile(BASE, tmp)
    r = sh(['mcopy','-o','-i',str(tmp),str(kp),'::/EFI/UniXNU/Payloads/kernel.macho'])
    assert r.returncode == 0
    if addon:
        r = sh(['mcopy','-o','-i',str(tmp),str(ADDON),'::/EFI/UniXNU/Payloads/kernel.macho.gpuaddon'])
        assert r.returncode == 0
    sh(['mcopy','-o','-i',str(tmp),'::/EFI/UniXNU/unixnu.toml','/tmp/'+name+'.toml'])
    t = Path('/tmp/'+name+'.toml').read_text()
    assert 'sheng-smp=1' in t and 'sheng-smp-max' not in t
    t = t.replace('sheng-smp=1', 'sheng-smp=1 sheng-smp-max=7', 1)
    import re
    m = re.search(r'args = "([^"]*)"', t)
    print(name, 'args bytes', len(m.group(1)))
    Path('/tmp/'+name+'-new.toml').write_text(t)
    sh(['mcopy','-o','-i',str(tmp),'/tmp/'+name+'-new.toml','::/EFI/UniXNU/unixnu.toml'])
    payload = tmp.read_bytes()
    skel = bytearray(SKEL.read_bytes())
    (ks,ka,rs,ra,ss,sa,ta,ps,hv,osv) = struct.unpack_from('<10I', skel, 8)
    assert len(payload) == rs, (len(payload), rs)
    off = ps + ((ks+ps-1)//ps)*ps
    skel[off:off+rs] = payload
    out = OUT/(name+'.img')
    out.write_bytes(bytes(skel))
    print('wrote', out.name, 'image_sha', hashlib.sha256(bytes(skel)).hexdigest()[:16])

build('boot-8C-GPU', persist=2)
