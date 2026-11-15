
#!/usr/bin/env python3
"""Replace llvm-objcopy's Mach-O surgery: zero the __mod_init_func section and globalize a symbol.

LLVM objcopy has never supported --remove-section for Mach-O; the upstream builder assumes an
Apple toolchain. This does exactly the two edits it performs, in place on the object file.
"""
import struct, sys

src, dst, symbol = sys.argv[1], sys.argv[2], sys.argv[3]
raw = bytearray(open(src, 'rb').read())
magic, cpu, sub, filetype, ncmds, sizeofcmds, flags, _ = struct.unpack_from('<IiiIIIII', raw, 0)
assert magic == 0xfeedfacf, hex(magic)

MH_OBJECT = 1
LC_SEGMENT_64, LC_SYMTAB = 0x19, 0x2
N_EXT = 0x01
removed = 0
symtab = None

off = 32
for _ in range(ncmds):
    cmd, cmdsize = struct.unpack_from('<II', raw, off)
    if cmd == LC_SEGMENT_64:
        segname = raw[off+8:off+24].rstrip(b'\0')
        nsects = struct.unpack_from('<I', raw, off+64)[0]
        sect = off + 72
        for _ in range(nsects):
            sectname = raw[sect:sect+16].rstrip(b'\0')
            if sectname == b'__mod_init_func':
                # section_64.resize point: offset 40 within the section structure
                struct.pack_into('<Q', raw, sect + 40, 0)
                removed += 1
            sect += 80
    elif cmd == LC_SYMTAB:
        symtab = struct.unpack_from('<IIII', raw, off+8)
    off += cmdsize

assert removed, 'no __mod_init_func section'
assert symtab, 'no symtab'
symoff, nsyms, stroff, strsize = symtab
renamed = 0
for i in range(nsyms):
    at = symoff + i * 16
    strx, ntype, nsect, desc, value = struct.unpack_from('<IBBHQ', raw, at)
    name = raw[stroff+strx:raw.index(b'\0', stroff+strx)].decode('utf-8', 'replace')
    if name == symbol and not (ntype & N_EXT):
        struct.pack_into('<B', raw, at + 4, ntype | N_EXT)
        renamed += 1

open(dst, 'wb').write(raw)
print('mod_init sections zeroed:', removed, 'symbols globalized:', renamed)

