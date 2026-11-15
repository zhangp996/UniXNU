#!/usr/bin/env python3
"""Globalise a Mach-O symbol in place (the Linux replacement for llvm-objcopy --globalize-symbol)."""
import struct
import sys
from pathlib import Path

path = Path(sys.argv[1]); wanted = sys.argv[2].encode()
data = bytearray(path.read_bytes())
ncmds = struct.unpack_from('<I', data, 16)[0]
off = 32; sym = None
for _ in range(min(ncmds, 64)):
    if off + 8 > len(data):
        break
    cmd, cmdsize = struct.unpack_from('<II', data, off)
    if cmdsize < 8 or off + cmdsize > len(data):
        break
    if cmd == 2:
        sym = struct.unpack_from('<IIII', data, off + 8)
    off += cmdsize
patched = 0
if sym:
    symoff, nsyms, stroff, strsize = sym
    for i in range(nsyms):
        at = symoff + i * 16
        if at + 16 > len(data):
            break
        strx, ntype, nsect, ndesc, nval = struct.unpack_from('<IBBHQ', data, at)
        end = data.find(b'\0', stroff + strx)
        if end < 0:
            continue
        name = data[stroff + strx:end]
        if name == wanted and not (ntype & 0x01):
            data[at + 4] = ntype | 0x01
            patched += 1

# __DATA,__mod_init_func handling is the caller's job (the board recipes drop it explicitly).
path.write_bytes(bytes(data))
print(f'globalised {sys.argv[2]}: {patched} site(s)')

