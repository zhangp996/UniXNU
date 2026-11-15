#!/usr/bin/env python3
"""Let any process query the runtime trust cache (bring-up only).

AMFI's _check_cdhash_in_trustcache rejects callers whose policy predicate fails, which on this board
is every service that verifies code: keybagd, amfid, securityd, rapportd and friends all exit with
status 9 right after the kernel prints "Only allowed processes can check the trustcache".  The gate
is a single tbz wN, #0, <reject> right after the predicate call at 0xfffffff004ab73b4; replacing it
with a nop lets the normal trust-cache lookup run for any caller.

Usage: scripts/patch-amfi-trustcache-gate.py <kernel.macho> [--dry-run]
"""
import struct
import sys

GATE_VA = 0xFFFFFFF004AB73B8
EXPECTED = 0x360010A0      # tbz w0, #0, 0xfffffff004ab75cc
NOP = 0xD503201F


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    path = sys.argv[1]
    dry = '--dry-run' in sys.argv[2:]
    data = bytearray(open(path, 'rb').read())
    magic, _c, _s, _f, ncmds, _sc, _fl, _r = struct.unpack_from('<IIIIIIII', data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit('not a 64-bit Mach-O')
    off = 32
    target = None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<II', data, off)
        if cmd == 0x19:
            sva, vs, fo, fs = struct.unpack_from('<QQQQ', data, off + 24)
            if sva <= GATE_VA < sva + fs:
                target = fo + (GATE_VA - sva)
                break
        off += size
    if target is None:
        raise SystemExit('the gate address is outside the kernel segments')
    word = struct.unpack_from('<I', data, target)[0]
    if word != EXPECTED:
        raise SystemExit('unexpected instruction at the gate: 0x%08x' % word)
    if dry:
        print('dry run: would replace 0x%08x with a nop at 0x%x' % (EXPECTED, GATE_VA))
        return
    struct.pack_into('<I', data, target, NOP)
    open(path, 'wb').write(data)
    print('trust-cache caller gate nop at 0x%x' % GATE_VA)


if __name__ == '__main__':
    main()

