#!/usr/bin/env python3
"""Skip AMFI's "AMFI: No PMGR?" panic in a prelinked kernel.

This board has no PMGR provider yet, so AMFI panics during startup and the
panel only shows the watchdog's 900E. The call site is the same one the
reference port records for this OS build; run --dry-run first, it refuses a
kernel whose instruction differs.

The kext panics because this board has no PMGR provider yet. The panicking call is a
noreturn tail call; the same function's register epilogue sits just above it, so the
call is replaced with a branch to that epilogue: AMFI carries on as if the check had
passed.

Usage: scripts/patch-amfi-nopmgr.py <kernel.macho> [--dry-run]
"""
import struct
import sys

# __PLK_TEXT_EXEC: NOP the call at 0xfffffff004aaed04 (bl 0xfffffff004ac93e4) and
# branch to the epilogue at 0xfffffff004aaec4c instead.
PATCH_VA = 0xFFFFFFF004AAED04
EPILOGUE_VA = 0xFFFFFFF004AAEC4C
EXPECTED = 0x940069B8  # bl 0xfffffff004ac93e4
BRANCH = 0x14000000 | (((EPILOGUE_VA - PATCH_VA) >> 2) & 0x03FFFFFF)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    dry = "--dry-run" in sys.argv[2:]
    data = bytearray(open(path, "rb").read())

    magic, _cpu, _sub, _ftype, ncmds, _sizeofcmds, _flags, _res = struct.unpack_from("<IIIIIIII", data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit("not a 64-bit Mach-O")
    off = 32
    target = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == 0x19:
            name = data[off + 8:off + 24].split(b"\0")[0].decode()
            vmaddr, vmsize, fileoff, filesize = struct.unpack_from("<QQQQ", data, off + 24)
            if vmaddr <= PATCH_VA < vmaddr + vmsize and (PATCH_VA - vmaddr) < filesize:
                target = fileoff + (PATCH_VA - vmaddr)
                break
        off += size
    if target is None:
        raise SystemExit("patch address is outside the kernel segments")

    current = struct.unpack_from("<I", data, target)[0]
    if current == BRANCH:
        print(f"already patched at 0x{target:x}")
        return
    if current != EXPECTED:
        raise SystemExit(f"unexpected instruction 0x{current:08x} at 0x{target:x}; kernel differs")

    print(f"0x{target:x}: 0x{current:08x} -> 0x{BRANCH:08x} (b 0x{EPILOGUE_VA:x})")
    if dry:
        return
    struct.pack_into("<I", data, target, BRANCH)
    open(path, "wb").write(data)
    print("patched", path)


if __name__ == "__main__":
    main()