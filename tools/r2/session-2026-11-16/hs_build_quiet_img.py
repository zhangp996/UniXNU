import hashlib, shutil, struct, subprocess, re
from pathlib import Path
OUT = Path("/mnt/d/新建文件夹/instantnoodlep-artifacts")
SKEL = OUT/"bootEU.img"
BASE = Path("/home/dev/unixnu/inputs/original-booteu/payload.img")
ADDON = Path("/home/dev/unixnu/_ref/artifacts/14012210733/gpuaddon.bin")
KERNEL = Path("/home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/kernel/kernel.macho")
NP = "/home/dev/unixnu/full-project/r2/device/scripts/patch-amfi-nopmgr.py"
TC = "/home/dev/unixnu/full-project/r2/device/scripts/patch-amfi-trustcache-gate.py"

def sh(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(cmd[0].split("/")[-1], "->", r.returncode, (r.stderr or "")[:100])
    return r

kp = Path("/tmp/kernel-8c-consoleoff.macho")
shutil.copyfile(KERNEL, kp)
sh(["python3", NP, str(kp)])
sh(["python3", TC, str(kp)])
print("patched kernel sha", hashlib.sha256(kp.read_bytes()).hexdigest()[:16])
tmp = Path("/tmp/boot-8C-GPU-quiet-payload.img")
shutil.copyfile(BASE, tmp)
sh(["mcopy","-o","-i",str(tmp),str(kp),"::/EFI/UniXNU/Payloads/kernel.macho"])
sh(["mcopy","-o","-i",str(tmp),str(ADDON),"::/EFI/UniXNU/Payloads/kernel.macho.gpuaddon"])
sh(["mcopy","-o","-i",str(tmp),"::/EFI/UniXNU/unixnu.toml","/tmp/boot-8C-GPU-quiet.toml"])
t = Path("/tmp/boot-8C-GPU-quiet.toml").read_text()
assert " offline-legacy-console=1" in t
t = t.replace(" offline-legacy-console=1", "", 1)
t = t.replace("sheng-smp=1", "sheng-smp=1 sheng-smp-max=7", 1)
m = re.search(r'args = "([^"]*)"', t)
args = m.group(1)
print("args bytes", len(args))
assert "offline-legacy-console" not in args
Path("/tmp/boot-8C-GPU-quiet-new.toml").write_text(t)
sh(["mcopy","-o","-i",str(tmp),"/tmp/boot-8C-GPU-quiet-new.toml","::/EFI/UniXNU/unixnu.toml"])
payload = tmp.read_bytes()
skel = bytearray(SKEL.read_bytes())
(ks,ka,rs,ra,ss,sa,ta,ps,hv,osv) = struct.unpack_from("<10I", skel, 8)
assert len(payload) == rs
off = ps + ((ks+ps-1)//ps)*ps
skel[off:off+rs] = payload
out = OUT/"boot-8C-GPU-quiet.img"
out.write_bytes(bytes(skel))
print("wrote", out.name, "image_sha", hashlib.sha256(bytes(skel)).hexdigest()[:16])
