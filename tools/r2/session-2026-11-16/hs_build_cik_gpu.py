import hashlib, shutil, struct, subprocess
from pathlib import Path
CI = Path('/tmp/cipayload/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/payload/payload.img')
PATCHED = Path('/tmp/cik/kernel.amfi.macho')
ADDON = Path('/home/dev/unixnu/_ref/artifacts/14012210733/gpuaddon.bin')
SKEL = Path('/mnt/d/新建文件夹/instantnoodlep-artifacts/bootEU.img')
OUT = Path('/mnt/d/新建文件夹/instantnoodlep-artifacts')

def build(name, nopersist=False):
    tmp = Path('/tmp/' + name + '-payload.img')
    shutil.copyfile(CI, tmp)
    r = subprocess.run(['mcopy','-o','-i',str(tmp),str(PATCHED),'::/EFI/UniXNU/Payloads/kernel.macho'], capture_output=True)
    assert r.returncode == 0, r.stderr
    r = subprocess.run(['mcopy','-o','-i',str(tmp),str(ADDON),'::/EFI/UniXNU/Payloads/kernel.macho.gpuaddon'], capture_output=True)
    assert r.returncode == 0, r.stderr
    if nopersist:
        subprocess.run(['mcopy','-o','-i',str(tmp),'::/EFI/UniXNU/unixnu.toml','/tmp/'+name+'.toml'], check=True)
        t = Path('/tmp/'+name+'.toml').read_text().replace('data-persist=2','data-persist=0',1)
        Path('/tmp/'+name+'-new.toml').write_text(t)
        subprocess.run(['mcopy','-o','-i',str(tmp),'/tmp/'+name+'-new.toml','::/EFI/UniXNU/unixnu.toml'], check=True)
    payload = tmp.read_bytes()
    skel = bytearray(SKEL.read_bytes())
    (ks,ka,rs,ra,ss,sa,ta,ps,hv,osv) = struct.unpack_from('<10I', skel, 8)
    assert len(payload) == rs
    off = ps + ((ks+ps-1)//ps)*ps
    skel[off:off+rs] = payload
    out = OUT/(name+'.img')
    out.write_bytes(bytes(skel))
    print(name, 'payload_sha', hashlib.sha256(payload).hexdigest()[:16], 'image_sha', hashlib.sha256(bytes(skel)).hexdigest()[:16])

build('boot-CIK-GPU-nopersist', nopersist=True)
build('boot-CIK-GPU')
