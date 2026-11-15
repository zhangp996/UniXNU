#!/usr/bin/env python3
"""Neutralise the __mod_init_func section and globalize a symbol, in place on the object file.

Apple ld rejects a section whose size is zeroed while its symbols and relocations still point into
it ("address=... points to section(N) with no content"). Instead of removing the content, rename the
section and change its type back to S_REGULAR: linkers and the kernel no longer treat it as a
module-initialiser array, the section keeps its content, its symbols and relocations stay valid, and
the Mach-O stays self-consistent for ld64.
"""
import struct
import sys

src, dst, symbol = sys.argv[1], sys.argv[2], sys.argv[3]
raw = bytearray(open(src, 'rb').read())
magic, cpu, sub, filetype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', raw, 0)
assert magic == 0xfeedfacf, hex(magic)
assert filetype == 1, 'expected MH_OBJECT'

LC_SEGMENT_64, LC_SYMTAB = 0x19, 0x2
N_EXT = 0x01
SECTION_SIZE = 80
SECT_TYPE_MASK = 0x000000ff

# section_64: sectname[16], segname[16], addr(+32 u64), size(+40 u64), offset(+48 u32),
#             align(+52 u32), reloff(+56 u32), nreloc(+60 u32), flags(+64 u32), ...
# segment_command_64: cmd, cmdsize, segname[16], vmaddr, vmsize, fileoff, filesize,
#                     maxprot, initprot, nsects(+64 u32), flags(+68 u32); sections at +72.
neutralised = 0
symtab = None
off = 32
for _ in range(ncmds):
    cmd, cmdsize = struct.unpack_from('<II', raw, off)
    if cmd == LC_SEGMENT_64:
        nsects = struct.unpack_from('<I', raw, off + 64)[0]
        sect = off + 72
        for _ in range(nsects):
            name = raw[sect:sect + 16].rstrip(b'\0')
            if name == b'__mod_init_func':
                raw[sect:sect + 16] = b'__mod_init_off\0\0'  # exactly 16 bytes
                flags_now = struct.unpack_from('<I', raw, sect + 64)[0]
                struct.pack_into('<I', raw, sect + 64, flags_now & ~SECT_TYPE_MASK)
                size = struct.unpack_from('<Q', raw, sect + 40)[0]
                fileoff = struct.unpack_from('<I', raw, sect + 48)[0]
                assert fileoff + size <= len(raw), (fileoff, size, len(raw))
                raw[fileoff:fileoff + size] = bytes(size)
                neutralised += 1
            sect += SECTION_SIZE
    elif cmd == LC_SYMTAB:
        symtab = struct.unpack_from('<IIII', raw, off + 8)
    off += cmdsize

assert neutralised, 'no __mod_init_func section'
assert symtab, 'no symtab'
symoff, nsyms, stroff, strsize = symtab
assert symoff + nsyms * 16 <= len(raw) and stroff + strsize <= len(raw), 'symtab outside file'
renamed = 0
for i in range(nsyms):
    at = symoff + i * 16
    strx, ntype, nsect, desc, value = struct.unpack_from('<IBBHQ', raw, at)
    end = raw.index(b'\0', stroff + strx)
    name = raw[stroff + strx:end].decode('utf-8', 'replace')
    if name == symbol and not (ntype & N_EXT):
        struct.pack_into('<B', raw, at + 4, ntype | N_EXT)
        renamed += 1

open(dst, 'wb').write(raw)
print('mod_init sections neutralised:', neutralised, 'symbols globalized:', renamed)
