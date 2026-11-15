
import hashlib, shutil, struct, subprocess, re
from pathlib import Path
BASE = Path("/home/dev/unixnu/inputs/original-booteu/payload.img")
ADDON = Path("/home/dev/unixnu/_ref/artifacts/14012210733/gpuaddon.bin")
SKEL = Path("/tmp/bootEU.img")
if not SKEL.is_file():
    import shutil as _sh
    _sh.copyfile("/mnt/d/新建文件夹/instantnoodlep-artifacts/bootEU.img", SKEL)
NP = "/home/dev/unixnu/full-project/r2/device/scripts/patch-amfi-nopmgr.py"
TC = "/home/dev/unixnu/full-project/r2/device/scripts/patch-amfi-trustcache-gate.py"
KERNEL = Path("/home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/kernel/kernel.macho")
OUT = Path("/mnt/d/新建文件夹/instantnoodlep-artifacts")

def sh(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True, stdin=subprocess.DEVNULL)
    print(cmd[0].split("/")[-1], "->", r.returncode, (r.stderr or "")[:100])
    return r

kp = Path("/tmp/kernel-dpu.macho")
shutil.copyfile(KERNEL, kp)
sh(["python3", NP, str(kp)]); sh(["python3", TC, str(kp)])
print("patched kernel sha", hashlib.sha256(kp.read_bytes()).hexdigest()[:16])
tmp = Path("/tmp/boot-dpu-payload.img")
shutil.copyfile(BASE, tmp)
sh(["mcopy","-o","-i",str(tmp),str(kp),"::/EFI/UniXNU/Payloads/kernel.macho"])
sh(["mcopy","-o","-i",str(tmp),str(ADDON),"::/EFI/UniXNU/Payloads/kernel.macho.gpuaddon"])
base_toml = Path("/tmp/base-toml.toml")
assert base_toml.is_file(), "need /tmp/base-toml.toml"
t = base_toml.read_text()
assert " offline-legacy-console=1" in t and " sheng-vsync-fixed=1" in t
t = t.replace(" offline-legacy-console=1", "", 1)
t = t.replace(" sheng-vsync-fixed=1", "", 1)
t = t.replace("sheng-smp=1", "sheng-smp=1 sheng-smp-max=7", 1)
t = t.replace("sheng-vsync=60", "sheng-vsync=120", 1)
t = re.sub(r'(args = "[^"]*)"', r'\1 dpu-scanout=1 sheng-dpu-scanout=2"', t, count=1)
m = re.search(r'args = "([^"]*)"', t)
print("args bytes", len(m.group(1)), "dpu:", "dpu-scanout=1" in m.group(1))
assert len(m.group(1)) <= 1000, "args too long: %d" % len(m.group(1))
Path("/tmp/boot-dpu-new.toml").write_text(t)
sh(["mcopy","-o","-i",str(tmp),"/tmp/boot-dpu-new.toml","::/EFI/UniXNU/unixnu.toml"])
payload = tmp.read_bytes()
skel = bytearray(SKEL.read_bytes())
(ks,ka,rs,ra,ss,sa,ta,ps,hv,osv) = struct.unpack_from("<10I", skel, 8)
assert len(payload) == rs
off = ps + ((ks+ps-1)//ps)*ps
skel[off:off+rs] = payload
out = Path("/tmp/boot-dpu.img")
out.write_bytes(bytes(skel))
print("image sha", hashlib.sha256(bytes(skel)).hexdigest()[:16], "bytes", len(skel))
print("built", out)
