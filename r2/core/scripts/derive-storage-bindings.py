#!/usr/bin/env python3
"""Derive the absolute IOBlockStorageDevice bindings a storage driver object needs.

A kernel object cannot link against the prelinked storage family: IOBlockStorageDevice lives in
com.apple.iokit.IOStorageFamily, and XNU's kernel link runs before the prelink. A subclass's vtable
repeats every entry it does not override, so each of those entries names the base method it needs;
the original vtable, at a fixed address in the pinned prelink, holds those implementations at the
same slot. The derived addresses go into one assembly file that the driver's build archives next to
the object (scripts/data/native-storage-<build>-original-bindings.S).

The four entries the layout does not describe (vtable, metaclass, constructor, base D2) are the
recorded anchors; everything else is read out of the original vtable.
"""
import argparse
import struct
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
import native_prelink as np

# Anchors for each pinned OS build, recovered once from the prelink (marble-milacy recorded them for
# 21H461: vtable address point +16, 190 slots, 14 pure). The values below were re-verified against
# this port's own image; see the header of the emitted assembly file.
ANCHORS = {
    '21H461': {'vtable': 0xfffffff005cb44c0, 'metaclass': 0xfffffff006add1d0,
               'constructor': 0xfffffff00496ed98, 'destructor': 0xfffffff00496edb8},
}
SLOTS = 190
BASE = 'IOBlockStorageDevice'


def object_symbols(obj):
    image = np.macho(obj)
    table = [(offset, length) for command, offset, length in image['commands'] if command == 2]
    if len(table) != 1:
        raise SystemExit(f'{obj.name}: expected one symbol table')
    symoff, count, stroff, strsize = struct.unpack_from('<4I', obj, table[0][0] + 8)
    strings = obj[stroff:stroff + strsize]
    symbols = []
    for index in range(count):
        strx, kind, _, _, value = struct.unpack_from('<IBBHQ', obj, symoff + index * 16)
        end = strings.index(b'\0', strx)
        symbols.append({'name': strings[strx:end].decode(), 'kind': kind, 'value': value})
    return image, symbols


def section_of(image, address):
    for segment in image['segments']:
        for section in segment['sections']:
            if section['va'] <= address < section['va'] + section['size']:
                return section
    return None


def vtable_entries(obj, image, symbols, vtable_va):
    section = section_of(image, vtable_va)
    if section is None:
        raise SystemExit('derived class vtable is not in a section')
    records, count = struct.unpack_from('<II', obj, section['command_offset'] + 56)
    names = {}
    for index in range(count):
        address, word = struct.unpack_from('<II', obj, records + index * 8)
        if word >> 27 & 1 and word >> 28 == 0 and word >> 25 & 3 == 3:
            names[section['va'] + address] = symbols[word & 0xffffff]['name']
    return {index: names[vtable_va + 16 + index * 8]
            for index in range(SLOTS) if vtable_va + 16 + index * 8 in names}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--object', type=Path, required=True, help='compiled driver object')
    parser.add_argument('--kernel', type=Path, required=True, help='prelinked kernel Mach-O of this build')
    parser.add_argument('--build-id', required=True, help='OS build id, e.g. 21H461')
    parser.add_argument('--class', dest='klass', default='NativeUfsBlock', help='derived block class')
    parser.add_argument('--output', type=Path, help='assembly file to write (default: stdout)')
    args = parser.parse_args()
    anchors = ANCHORS.get(args.build_id)
    if anchors is None:
        raise SystemExit(f'no recorded IOStorageFamily anchors for build {args.build_id}')
    obj = args.object.read_bytes()
    image, symbols = object_symbols(obj)
    vtable_symbol = next((s for s in symbols if s['name'] == '__ZTV' + str(len(args.klass)) + args.klass), None)
    if vtable_symbol is None:
        raise SystemExit(f'{args.object}: no vtable for {args.klass}')
    entries = vtable_entries(obj, image, symbols, vtable_symbol['value'])
    if len(entries) != SLOTS:
        raise SystemExit(f'{args.object}: expected {SLOTS} vtable entries, found {len(entries)}')
    kernel = args.kernel.read_bytes()
    kernel_image = np.macho(kernel)
    code = [(s['va'], s['va'] + s['size']) for s in kernel_image['segments'] if s['initprot'] & 4]
    offset = np.backed_offset(kernel_image['segments'], anchors['vtable'] + 16, SLOTS * 8, len(kernel))
    original = struct.unpack_from(f'<{SLOTS}Q', kernel, offset)
    bindings = {'__ZTV20' + BASE: anchors['vtable'],
                '__ZN20' + BASE + '10gMetaClassE': anchors['metaclass'],
                '__ZN20' + BASE + 'C2EPK11OSMetaClass': anchors['constructor'],
                '__ZN20' + BASE + 'D2Ev': anchors['destructor']}
    # The abstract base repeats one stub in every slot a subclass must override; a binding that
    # pointed at it would fail at call time instead of at build time.
    stub = Counter(original).most_common(1)[0][0]
    derived = []
    for index, name in sorted(entries.items()):
        if BASE not in name or name in bindings:
            continue
        target = original[index]
        if not any(low <= target < high for low, high in code):
            raise SystemExit(f'slot {index} {name}: original entry {target:#x} is not in mapped code')
        if target == stub:
            raise SystemExit(f'slot {index} {name}: original entry is the shared virtual stub')
        bindings[name] = target
        derived.append((index, name, target))
    lines = ['// SPDX-License-Identifier: BSD-2-Clause-Patent',
             f'// IOStorageFamily ABI addresses for the iPadOS {args.build_id} prelink.',
             '//',
             '// A kernel object cannot link against IOBlockStorageDevice: the class lives in the prelinked',
             '// com.apple.iokit.IOStorageFamily kext and the kernel link runs before the prelink. The',
             '// addresses below are where that class sits in the pinned prelink, so a driver that',
             '// subclasses it links with absolute symbols, as scripts/data/native-aes-original-bindings.S',
             '// does for IOAESAccelerator.',
             '//',
             f'// Generated by scripts/derive-storage-bindings.py from {args.object.name} and the prelinked',
             f'// {args.kernel.name}. The vtable address point +16 holds {SLOTS} entries; the ones below list',
             '// the base methods this driver repeats and takes the original implementation at the same',
             '// slot. D1/D0 are not bound: they are brk traps in the abstract base, and a reference to them',
             '// means abstract complete/deleting destruction, which must fail the link instead.',
             '']
    for name, value in bindings.items():
        lines += [f'.globl {name}', f'.set {name}, {value:#x}']
    text = '\n'.join(lines) + '\n'
    if args.output:
        args.output.write_text(text)
        print(f'{args.output}: {len(bindings)} bindings ({len(derived)} derived from the vtable)')
    else:
        print(text, end='')


if __name__ == '__main__':
    main()

