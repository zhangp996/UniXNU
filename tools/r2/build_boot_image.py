#!/usr/bin/env python3
"""Build a boot image for this board from the R2 baseline skeleton, a kernel and a payload FAT image.

The Android boot v2 skeleton carries the kernel (BootShim + SILICIUM_UEFI.fd) and the payload is the
ramdisk: this tool replaces the payload's kernel, optionally adds a GPU addon, edits the boot
arguments in EFI/UniXNU/unixnu.toml and splices the rebuilt payload into the skeleton at the
page-aligned offset after the kernel.

Example (WSL, repository checkout):
  python3 tools/r2/build_boot_image.py \
      --payload /home/dev/unixnu/inputs/original-booteu/payload.img \
      --kernel  /home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/kernel/kernel.macho \
      --skeleton /mnt/d/新建文件夹/instantnoodlep-artifacts/bootEU.img \
      --gpu-addon /home/dev/unixnu/_ref/artifacts/14012210733/gpuaddon.bin \
      --args-add "sheng-smp-max=7 dpu-scanout=1 sheng-dpu-scanout=2" \
      --args-remove "offline-legacy-console=1 sheng-vsync-fixed=1" \
      --out /tmp/boot-test.img

AMFI: the rebuilt kernels are patched with the device's two post-build scripts unless --no-amfi is
given (patch-amfi-nopmgr.py, patch-amfi-trustcache-gate.py). The payload's unixnu.toml is edited in
place in a temporary copy; the merged command line must stay at most 1000 bytes (DarwinBoot rule).
"""
import argparse
import hashlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

def run(cmd, **kw):
    result = subprocess.run([str(part) for part in cmd], capture_output=True, text=True, **kw)
    if result.returncode:
        sys.exit("%s -> %d: %s" % (cmd[0], result.returncode, (result.stderr or "").strip()[:400]))
    return result

def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--payload", type=Path, required=True, help="payload FAT image (bootstrap ramdisk)")
    parser.add_argument("--kernel", type=Path, required=True, help="raw kernel mach-o")
    parser.add_argument("--skeleton", type=Path, required=True, help="Android boot v2 image to splice into")
    parser.add_argument("--out", type=Path, required=True, help="output boot image")
    parser.add_argument("--device", type=Path, default=Path(__file__).resolve().parents[2] / "r2/device",
                        help="device repository (for the AMFI scripts)")
    parser.add_argument("--gpu-addon", type=Path, help="optional NGPUADD2 addon written next to the kernel")
    parser.add_argument("--args-add", default="", help="space separated boot arguments to add")
    parser.add_argument("--args-remove", default="", help="space separated boot arguments to remove")
    parser.add_argument("--no-amfi", action="store_true", help="do not run the two AMFI patches on the kernel")
    parser.add_argument("--keep-tmp", action="store_true", help="keep the intermediate payload")
    args = parser.parse_args()

    tmpdir = Path(tempfile.mkdtemp(prefix="bootimage-"))
    payload = tmpdir / "payload.img"
    shutil.copyfile(args.payload, payload)
    kernel = tmpdir / "kernel.macho"
    shutil.copyfile(args.kernel, kernel)

    if not args.no_amfi:
        for script in ("patch-amfi-nopmgr.py", "patch-amfi-trustcache-gate.py"):
            path = args.device / "scripts" / script
            if not path.is_file():
                sys.exit("missing AMFI script: %s" % path)
            run(["python3", path, kernel])

    run(["mcopy", "-o", "-i", payload, kernel, "::/EFI/UniXNU/Payloads/kernel.macho"])
    if args.gpu_addon:
        run(["mcopy", "-o", "-i", payload, args.gpu_addon, "::/EFI/UniXNU/Payloads/kernel.macho.gpuaddon"])

    toml = tmpdir / "unixnu.toml"
    run(["mcopy", "-o", "-i", payload, "::/EFI/UniXNU/unixnu.toml", toml])
    text = toml.read_text()
    match = re.search(r'args = "([^"]*)"', text)
    if not match:
        sys.exit("unixnu.toml has no args line")
    merged = match.group(1)
    for token in args.args_remove.split():
        merged = re.sub(r"(?<!\S)" + re.escape(token) + r"(?!\S)", "", merged)
    for token in args.args_add.split():
        if re.search(r"(?<!\S)" + re.escape(token) + r"(?!\S)", merged):
            continue
        merged = (merged + " " + token).strip()
    merged = re.sub(r"\s+", " ", merged).strip()
    if len(merged) > 1000:
        sys.exit("merged command line is %d bytes, DarwinBoot allows 1000" % len(merged))
    text = text[:match.start(1)] + merged + text[match.end(1):]
    toml.write_text(text)
    run(["mcopy", "-o", "-i", payload, toml, "::/EFI/UniXNU/unixnu.toml"])

    payload_bytes = payload.read_bytes()
    skeleton = bytearray(args.skeleton.read_bytes())
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size, second_addr,
     tags_addr, page_size, header_version, os_version) = struct.unpack_from("<10I", skeleton, 8)
    if header_version < 2:
        sys.exit("the skeleton is not an Android boot v2 image")
    if len(payload_bytes) != ramdisk_size:
        sys.exit("payload size %d differs from the skeleton ramdisk size %d" % (len(payload_bytes), ramdisk_size))
    offset = page_size + ((kernel_size + page_size - 1) // page_size) * page_size
    skeleton[offset:offset + ramdisk_size] = payload_bytes
    args.out.write_bytes(bytes(skeleton))
    print("kernel      %s" % hashlib.sha256(kernel.read_bytes()).hexdigest())
    print("payload     %s" % hashlib.sha256(payload_bytes).hexdigest())
    print("args %4d   %s" % (len(merged), merged))
    print("wrote       %s (%d bytes, sha256 %s)" % (args.out, len(skeleton), hashlib.sha256(bytes(skeleton)).hexdigest()))
    if not args.keep_tmp:
        shutil.rmtree(tmpdir, ignore_errors=True)

if __name__ == "__main__":
    main()
