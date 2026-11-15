#!/usr/bin/env python3
"""Board live TSENS backend plus temperature service on the original HID ABI.

Recovers the private IOHIDEventService layout from the pinned 21H461 kernelcache,
re-qualifies it against the current build plan, compiles the three board sources
with the target's real compile record, binds every original method to its target
address and adds the objects to the board platform archive.
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(os.environ.get('SMUFS_BUILD_TREE', str(Path(__file__).resolve().parents[1])))
sys.path.insert(0, str(ROOT / 'kits/XnuPrelink'))
from native_prelink import macho, backed_offset, exports, unserialize_xml  # noqa: E402

ORIGINAL_SHA = 'f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7'
HID_EXEC_SHA = '7e4c5104814f2e773d5be13d092dec53ee44ba5a61a81264b229780406e86b72'
HID_BUNDLE = 'com.apple.iokit.IOHIDFamily'
METACLASS = 0xfffffff007ae7090
VPTR = 0xfffffff006d05e10
SLOT_COUNT = 219
CLASS_SIZE = 200
WITH_BYTES = 0xfffffff005b01c90
WITH_BYTES_END = 0xfffffff005b01e7c
META_CTOR = 0xfffffff005b0f5ec
WITH_BYTES_WORDS = {
    0xfffffff005b01c90: 0xd10243ff, 0xfffffff005b01cb8: 0xf100703f,
    0xfffffff005b01cc8: 0x2942d00b, 0xfffffff005b01cec: 0x91007108,
    0xfffffff005b01d18: 0x7100437f, 0xfffffff005b01d28: 0x29406b17,
    0xfffffff005b01d2c: 0x7100a35f, 0xfffffff005b01d54: 0x913e4108,
    0xfffffff005b01d58: 0xb87ad915, 0xfffffff005b01dac: 0xf9400b20,
    0xfffffff005b01dd0: 0xb9401308, 0xfffffff005b01dd4: 0xb9004b28,
    0xfffffff005b01dd8: 0x3dc00300, 0xfffffff005b01ddc: 0x3d800f20,
    0xfffffff005b01e58: 0xaa1503e0,
}
# Type15 minimum event size lives in the original type-size table.
TYPE_SIZE_TABLE = 0xfffffff0052e9f90
TYPE15_MINIMUM = 20

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--kernelcache', type=Path,
               default=ROOT / '.cache/firmware/kernelcache.macho/kernelcache.release.ipad7.decompressed')
p.add_argument('--plan', type=Path, default=ROOT.parent / 'kernel/kernel.json')
p.add_argument('--reference', type=Path, default=ROOT.parent / 'kernel/kernel.macho')
p.add_argument('--output', type=Path, default=ROOT / 'build/sm8250-thermal-hid')
p.add_argument('--platform-archive', type=Path,
               default=ROOT / 'build/sheng-platform-power/libShengPlatformPower.a')
p.add_argument('--standalone', action='store_true')
a = p.parse_args()
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
sha = lambda b: hashlib.sha256(b).hexdigest()

original = a.kernelcache.read_bytes()
assert sha(original) == ORIGINAL_SHA, 'pinned 21H461 kernelcache required'
image = macho(original)


def read(data, img, va, n):
    at = backed_offset(img['segments'], va, n, len(data))
    return data[at:at + n]


def word(va):
    return struct.unpack('<I', read(original, image, va, 4))[0]


def sx(x, bits):
    return x - (1 << bits) if x & (1 << (bits - 1)) else x


# 1. Locate the prelinked IOHIDFamily module in the original kernelcache.
info = next(s for s in image['segments'] if s['name'] == '__PRELINK_INFO')
entries = unserialize_xml(read(original, image, info['va'], info['filesize']))['_PrelinkInfoDictionary']
entry = next(e for e in entries if e['CFBundleIdentifier'] == HID_BUNDLE)
module = macho(original, backed_offset(image['segments'], entry['_PrelinkExecutableLoadAddr'], 32, len(original)))
exec_segment = next(s for s in module['segments'] if s['name'] == '__TEXT_EXEC')
assert sha(read(original, image, exec_segment['va'], exec_segment['filesize'])) == HID_EXEC_SHA

# 2. Recover IOHIDEventService from its own constructor: metaclass, base
#    metaclass, class size and the instance vtable it installs.
init_section = next(s for seg in module['segments'] for s in seg['sections'] if s['name'] == '__mod_init_func')
classes = []
for index in range(init_section['size'] // 8):
    va = struct.unpack('<Q', read(original, image, init_section['va'] + index * 8, 8))[0]
    regs = {}
    for pc in range(va, va + 128, 4):
        w = struct.unpack('<I', read(original, image, pc, 4))[0]
        rd = w & 31
        if w & 0x9f000000 == 0x90000000:
            regs[rd] = (pc & ~4095) + (sx(((w >> 5) & 0x7ffff) * 4 + ((w >> 29) & 3), 21) << 12)
        elif w & 0x9f000000 == 0x10000000:
            regs[rd] = pc + sx(((w >> 5) & 0x7ffff) * 4 + ((w >> 29) & 3), 21)
        elif w & 0x7f000000 == 0x11000000 and (w >> 5) & 31 in regs:
            regs[rd] = regs[(w >> 5) & 31] + ((w >> 10) & 4095) * (4096 if w & (1 << 22) else 1)
        elif w & 0x7f800000 == 0x52800000:
            regs[rd] = ((w >> 5) & 65535) << (((w >> 21) & 3) * 16)
        elif w & 0x7f800000 == 0x72800000 and rd in regs:
            shift = ((w >> 21) & 3) * 16
            regs[rd] = (regs[rd] & ~(65535 << shift)) | (((w >> 5) & 65535) << shift)
        elif w & 0xffe0ffe0 == 0xaa0003e0 and (w >> 16) & 31 in regs:
            regs[rd] = regs[(w >> 16) & 31]
        elif w & 0xffc00000 == 0xf9400000 and (w >> 5) & 31 in regs:
            addr = regs[(w >> 5) & 31] + ((w >> 10) & 4095) * 8
            try:
                regs[rd] = struct.unpack('<Q', read(original, image, addr, 8))[0]
            except ValueError:
                regs.pop(rd, None)
        elif w & 0xfc000000 == 0x94000000:
            try:
                name = read(original, image, regs.get(1, 0), 128).split(b'\0')[0].decode()
            except (ValueError, UnicodeError):
                name = ''
            if name and name.replace('_', '').isalnum() and 0 in regs and \
                    pc + (sx(w & 0x3ffffff, 26) << 2) == META_CTOR:
                classes.append({'name': name, 'metaclass': regs[0],
                                'base_metaclass': regs.get(2, 0), 'size': regs.get(3)})
                break
            for r in range(19):
                regs.pop(r, None)
service = next(c for c in classes if c['name'] == 'IOHIDEventService')
assert service['metaclass'] == METACLASS and service['size'] == CLASS_SIZE, service

# 3. The instance vtable is the __const table whose runtime getMetaClass stub
#    returns that metaclass; it must be the 219 entries the adapter declares.
const = [s for seg in module['segments'] for s in seg['sections'] if s['name'] == '__const']
assert const, 'IOHIDFamily has no __const section'
getters = []
for pc in range(exec_segment['va'], exec_segment['va'] + exec_segment['filesize'] - 12, 4):
    w0, w1, w2 = struct.unpack('<III', read(original, image, pc, 12))
    if w0 & 0x9f00001f == 0x90000000 and w1 & 0xffc003ff == 0x91000000 and w2 == 0xd65f03c0:
        target = (pc & ~4095) + (sx(((w0 >> 5) & 0x7ffff) * 4 + ((w0 >> 29) & 3), 21) << 12) + ((w1 >> 10) & 4095)
        if target == METACLASS:
            getters.append(pc)
assert getters, 'no getMetaClass stub returns the IOHIDEventService metaclass'
tables = []
for section in const:
    for at in range(section['va'], section['va'] + section['size'], 8):
        if struct.unpack('<Q', read(original, image, at, 8))[0] in getters:
            vptr = at - 7 * 8
            slots = []
            for i in range(300):
                value = struct.unpack('<Q', read(original, image, vptr + i * 8, 8))[0]
                if not value:
                    break
                slots.append(value)
            tables.append({'vptr': vptr, 'slots': slots})
assert len(tables) == 1 and tables[0]['vptr'] == VPTR and len(tables[0]['slots']) == SLOT_COUNT, tables
slots = tables[0]['slots']
original_names = exports(original, image)
slot_names = {v: ns for v, ns in original_names.items()}

# 4. Re-qualify the withBytes reader and the type15 minimum size it depends on.
assert all(word(va) == expected for va, expected in WITH_BYTES_WORDS.items())
factory = read(original, image, WITH_BYTES, WITH_BYTES_END - WITH_BYTES)
assert struct.unpack('<I', read(original, image, TYPE_SIZE_TABLE + 15 * 4, 4))[0] == TYPE15_MINIMUM
# The dedicated ringer uses the same original withBytes reader. Re-qualify
# type3 and its actual field readers instead of assuming a public SDK layout.
assert struct.unpack('<I', read(original, image, TYPE_SIZE_TABLE + 3 * 4, 4))[0] == 32
for va, expected in {0xfffffff005afd6b0: 0x79402100,
                     0xfffffff005afd6b8: 0x79402500,
                     0xfffffff005afd7d8: 0xb9401500}.items():
    assert word(va) == expected
for field, destination in [(0,0xfffffff005afd6b0),(1,0xfffffff005afd6b8),(2,0xfffffff005afd7d8)]:
    delta = struct.unpack('<H', read(original, image, 0xfffffff0052e95dc+field*2,2))[0]
    assert 0xfffffff005afd0b8+delta*4 == destination


# Original keyboardEvent and setSenderID code, not an SDK-derived packet.
# The base factory sets options|1, stores that in object and event data,
# stores down32 at20, and pressCount8 at28. Offset24 holds longPress/clickSpeed.
NATIVE_KEYBOARD_FACTORY=0xfffffff005afb5b0
NATIVE_KEYBOARD_WORDS={0xfffffff005afb624:0x32000308,
    0xfffffff005afb63c:0xb9004a88, 0xfffffff005afb640:0x2900a12a,
    0xfffffff005afb64c:0xb9001533, 0xfffffff005afb654:0x39007128}
for va,expected in NATIVE_KEYBOARD_WORDS.items():
    assert word(va)==expected,(hex(va),hex(word(va)),hex(expected))
# The original IOHIDEventService dispatch calls event slot31 to set senderID.
assert word(0xfffffff005aee894)==0xf9407d08

# 5. Current build plan: one slide moves every prelinked segment of this module.
plan = json.loads(a.plan.read_text())
slide = plan['slide']
assert isinstance(slide, int) and slide < 0
assert plan['original_executable_sha256'][HID_BUNDLE] == HID_EXEC_SHA
moved = []
for segment in module['segments']:
    extent = next((e for e in plan['extents'] if e['module'] == HID_BUNDLE and e['segment'] == segment['name']), None)
    if extent is None:
        continue
    assert extent['size'] == segment['filesize'], (segment['name'], extent['size'], segment['filesize'])
    assert extent['va'] - segment['va'] == slide, (segment['name'], hex(extent['va']), hex(segment['va']))
    moved.append(segment['name'])
assert '__TEXT_EXEC' in moved and {'__TEXT', '__DATA_CONST', '__DATA'} <= set(moved), moved

# 6. The previous kernel must already carry the same prelinked code; a slide
#    change is reported before anything is archived. Only the executable
#    segment is compared: the prelink may relocate data and vtable pointers.
if a.reference.is_file():
    reference = a.reference.read_bytes()
    ref_image = macho(reference)
    extent = next(e for e in plan['extents'] if e['module'] == HID_BUNDLE and e['segment'] == '__TEXT_EXEC')
    assert read(reference, ref_image, extent['va'], extent['size']) == \
        read(original, image, exec_segment['va'], exec_segment['filesize'])
    assert read(reference, ref_image, WITH_BYTES + slide, len(factory)) == factory

# 7. Emit the target-qualified opaque ABI declaration.
known = {170: 'virtual OSString *getTransport();', 171: 'virtual UInt32 getLocationID();',
         177: 'virtual OSString *getManufacturer();', 178: 'virtual OSString *getProduct();',
         180: 'virtual OSArray *getReportElements();', 191: 'virtual UInt32 getReportInterval();',
         199: 'virtual UInt32 getPrimaryUsagePage();', 200: 'virtual UInt32 getPrimaryUsage();',
         201: 'virtual OSObject *copyEvent(UInt32,OSObject *,IOOptionBits);'}
header = '''// SPDX-License-Identifier: MIT
// Target-qualified opaque declaration, not a copy of Apple header implementation.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
class OSString;class OSArray;
class IOHIDEventService : public IOService {
 OSDeclareAbstractStructors(IOHIDEventService)
 unsigned char originalPrivate[%d-sizeof(IOService)];
protected:
 const void *originalPointer(size_t offset) const {
  return *reinterpret_cast<const void *const *>(reinterpret_cast<const unsigned char *>(this)+offset);
 }
 bool originalExpansionPresent() const {return originalPointer(0xc0)!=nullptr;}
 bool originalInitResourcesPresent() const {
  const auto *exp=static_cast<const unsigned char *>(originalPointer(0xc0));
  return exp && originalPointer(0xa0) && originalPointer(0xa8) &&
   *reinterpret_cast<const void *const *>(exp+0x20) && *reinterpret_cast<const void *const *>(exp+0x30);
 }
public:
 kern_return_t Dispatch(IORPC) override;
 void free() override;
 bool init(OSDictionary * =nullptr) override;
 IOReturn setProperties(OSObject *) override;
 bool start(IOService *) override;
 void stop(IOService *) override;
 void close(IOService *,IOOptionBits) override;
 bool handleOpen(IOService *,IOOptionBits,void *) override;
 void handleClose(IOService *,IOOptionBits) override;
 bool handleIsOpen(const IOService *) const override;
 bool matchPropertyTable(OSDictionary *,SInt32 *) override;
 IOReturn message(UInt32,IOService *,void *) override;
 IOReturn newUserClient(task_t,void *,UInt32,OSDictionary *,IOUserClient **) override;
''' % CLASS_SIZE
# Uncalled slots are opaque declarations bound to each exact original pointer;
# only called/overridden signatures above are part of this adapter's C++ API.
for i in range(168, SLOT_COUNT):
    header += ' ' + known.get(i, f'virtual void originalSlot{i}();') + '\n'
header += '};\nstatic_assert(sizeof(IOHIDEventService)==%d,"original HID service storage");\n' % CLASS_SIZE
(out / 'OriginalTsensHidABI.h').write_text(header)

# 8. Compile against the target's real compile record.
recipes = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
recipe = json.loads(recipes.read_text().strip().rstrip(','))
sources = {'SmTsensContinuous.cpp': 'SmTsensContinuous.o',
           'SmTsensTemperatureEvent.cpp': 'SmTsensTemperatureEvent.o',
           'SmTsensHid.cpp': 'SmTsensHid.o'}
kit = ROOT / 'kits/instantnoodlep'
objects = {}
compiled_logs = {}
for name, member in sources.items():
    source = kit / name
    assert source.is_file(), source
    obj = out / member
    command = []
    i = 0
    arguments = recipe['arguments']
    while i < len(arguments):
        v = arguments[i]
        if v == recipe['file']:
            command.append(str(source))
        elif v in ('-o', '-MF', '-MT', '-MJ'):
            if v == '-o':
                command += ['-o', str(obj)]
            i += 1
        elif v == '-Xclang' and arguments[i + 1].startswith('-W'):
            i += 1
        elif not v.startswith('-W'):
            command.append(v)
        i += 1
    command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-Wno-missing-designated-field-initializers', '-Wno-c99-designator',
                '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier',
                '-I', str(out), '-I', str(kit), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                '-mgeneral-regs-only']
    (out / (member + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
    result = subprocess.run(command, cwd=recipe['directory'], capture_output=True, text=True)
    compiled_logs[member] = result.stdout + result.stderr
    (out / (member + '.log')).write_text(compiled_logs[member])
    if result.returncode:
        raise SystemExit(compiled_logs[member][-6000:])
    objects[name] = obj


def symbol_table(blob):
    image_ = macho(blob)
    at = next(cmd_at for cmd, cmd_at, _ in image_['commands'] if cmd == 2)
    start, count, strings, _ = struct.unpack_from('<4I', blob, at + 8)
    names = []
    for i in range(count):
        ix, kind, _, _, va = struct.unpack_from('<IBBHQ', blob, start + i * 16)
        names.append((blob[strings + ix:blob.index(b'\0', strings + ix)].decode(), va, kind))
    return image_, names


# 9. Every vtable entry of the board service must be either our override, an
#    original IOHIDEventService method bound below, or the identical base method.
compiled = objects['SmTsensHid.cpp'].read_bytes()
obj_image, symbols = symbol_table(compiled)
vtable = next(va for name, va, _ in symbols if name == '__ZTV10SmTsensHid')
relocations = {}
for segment in obj_image['segments']:
    for section in segment['sections']:
        if section['va'] <= vtable < section['va'] + section['size']:
            at, count = struct.unpack_from('<2I', compiled, section['command_offset'] + 56)
            for j in range(count):
                offset, bits = struct.unpack_from('<2I', compiled, at + j * 8)
                if bits >> 27 & 1 and bits >> 28 == 0 and bits >> 25 & 3 == 3:
                    relocations[section['va'] + offset] = symbols[bits & 0xffffff][0]
entries_ = [relocations.get(vtable + 16 + i * 8) for i in range(SLOT_COUNT)]
assert all(entries_) and vtable + 16 + SLOT_COUNT * 8 not in relocations
kernel_core = {n: va for va, ns in original_names.items() for n in ns}
bindings = {'__ZN17IOHIDEventServiceC2EPK11OSMetaClass': 0xfffffff005aeb014,
            '__ZN17IOHIDEventServiceD2Ev': 0xfffffff005aeb034,
            '__ZN17IOHIDEventService10gMetaClassE': METACLASS,
            '__ZTV17IOHIDEventService': VPTR - 16}
overrides = []
for i, name in enumerate(entries_):
    if '10SmTsensHid' in name:
        overrides.append(i)
    elif '17IOHIDEventService' in name:
        if name in bindings:
            assert bindings[name] == slots[i], (i, name)
        bindings[name] = slots[i]
    else:
        assert kernel_core.get(name) == slots[i], (i, name, hex(slots[i]))
assert set(overrides) == {0, 1, 7, 14, 21, 84, 86, 87, 111, 170, 171, 177, 178, 180,
                          191, 199, 200, 201}, overrides
# The same translation unit registers this second board class with the same
# one-shot constructor. Every one of its219 slots must resolve independently.
ringer_vtable = next(va for name,va,_ in symbols if name == '__ZTV19In2020RingerService')
ringer_entries = [relocations.get(ringer_vtable+16+i*8) for i in range(SLOT_COUNT)]
assert all(ringer_entries) and ringer_vtable+16+SLOT_COUNT*8 not in relocations
ringer_overrides = []
for i,name in enumerate(ringer_entries):
    if '19In2020RingerService' in name:
        ringer_overrides.append(i)
    elif '17IOHIDEventService' in name:
        assert bindings.get(name) == slots[i], (i,name)
    else:
        assert kernel_core.get(name) == slots[i], (i,name)
assert set(ringer_overrides) == set(overrides), ringer_overrides
assert ringer_entries[198] == entries_[198]  # unchanged original dispatchEvent

undefined = {name for name, _, kind in symbols if kind == 1 and '17IOHIDEventService' in name}
assert undefined <= set(bindings), undefined - set(bindings)
# Never bind outside the qualified module: every original method address must
# fall inside one of the IOHIDFamily extents that moved by this plan's slide.
extents = [(e['va'], e['size']) for e in plan['extents'] if e['module'] == HID_BUNDLE]
for name, va in bindings.items():
    if name.startswith('__ZTV') or 'gMetaClass' in name:
        continue
    assert any(lo <= va + slide < lo + size for lo, size in extents), (name, hex(va))
    assert any(lo <= va < lo + size for lo, size in
               [(s['va'], s['filesize']) for s in module['segments']]), (name, hex(va))

# 10. Bindings: original slot address moved by the plan's single slide.
assembly = ''.join(f'.globl {name}\n.set {name}, {va + slide:#x}\n' for name, va in bindings.items())
assembly += f'.globl _sm_tsens_original_event_with_bytes\n.set _sm_tsens_original_event_with_bytes, {WITH_BYTES + slide:#x}\n'
assembly += f'.globl _in2020_original_keyboard_event\n.set _in2020_original_keyboard_event, {NATIVE_KEYBOARD_FACTORY + slide:#x}\n'
(out / 'bindings.S').write_text(assembly)
subprocess.run(['clang', '-target', 'arm64-apple-ios17.0', '-Werror', '-c',
                str(out / 'bindings.S'), '-o', str(out / 'bindings.o')], check=True)

# 11. Remove automatic static initialization but keep the one-shot class
#     registration entry point the boot caller drives explicitly.
late = out / 'SmTsensHid.late.o'
subprocess.run(['llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
                '--globalize-symbol=__GLOBAL__sub_I_SmTsensHid.cpp',
                str(objects['SmTsensHid.cpp']), str(late)], check=True)


def section_of(blob, name):
    return next(s for g in macho(blob)['segments'] for s in g['sections'] if s['name'] == name)


before, after = compiled, late.read_bytes()
a_text, b_text = section_of(before, '__text'), section_of(after, '__text')
assert before[a_text['fileoff']:a_text['fileoff'] + a_text['size']] == \
    after[b_text['fileoff']:b_text['fileoff'] + b_text['size']]
assert not any(s['name'] == '__mod_init_func' for g in macho(after)['segments'] for s in g['sections'])

# BSD and Mach private headers are separate domains in this XNU. Compile the
# exact-name selector and the thread/map observer with their respective native
# records; never reconstruct private structs or disable conflicting type checks.
wait_recipes = {}
for c_name, c_record in [('SmThermalWait.c', 'osfmk/DEVELOPMENT/bsd_kern.o.json'),
                         ('SmGateTrace.c', 'osfmk/DEVELOPMENT/bsd_kern.o.json'),
                         ('SmThermalWaitProc.c', 'bsd/DEVELOPMENT/bsd_init.o.json')]:
    c_recipe_path = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837' / c_record
    c_recipe = json.loads(c_recipe_path.read_text().strip().rstrip(','))
    c_member = c_name.replace('.c', '.o')
    c_command = []
    i = 0
    while i < len(c_recipe['arguments']):
        value = c_recipe['arguments'][i]
        if value == c_recipe['file']:
            c_command.append(str(kit / c_name))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            if value == '-o':
                c_command += ['-o', str(out / c_member)]
            i += 1
        elif value == '-Xclang' and c_recipe['arguments'][i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W'):
            c_command.append(value)
        i += 1
    c_command += ['-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                  '-Wno-missing-field-initializers', '-I', str(kit), '-mgeneral-regs-only']
    (out / (c_member + '.command.json')).write_text(json.dumps(c_command, indent=2) + '\n')
    c_result = subprocess.run(c_command, cwd=c_recipe['directory'], capture_output=True, text=True)
    (out / (c_member + '.log')).write_text(c_result.stdout + c_result.stderr)
    if c_result.returncode:
        raise SystemExit((c_result.stdout + c_result.stderr)[-8000:])
    wait_recipes[c_name] = str(c_recipe_path)
members = ['SmTsensContinuous.o', 'SmTsensTemperatureEvent.o', 'SmTsensHid.late.o', 'bindings.o', 'SmThermalWait.o', 'SmThermalWaitProc.o', 'SmGateTrace.o']
archive = out / 'libSmTsensThermal.a'
archive.unlink(missing_ok=True)
subprocess.run(['llvm-ar', 'rcs', str(archive), *[str(out / m) for m in members]], check=True)
assert subprocess.check_output(['llvm-ar', 't', str(archive)], text=True).splitlines() == members

report = dict(
    target_compile_passed=True, source_sha256={n: sha((kit / n).read_bytes()) for n in sources},
    object_sha256={m: sha((out / m).read_bytes()) for m in members},
    original_kernelcache_sha256=sha(original), reference_kernel_sha256=(
        sha(a.reference.read_bytes()) if a.reference.is_file() else None),
    hid_bundle=HID_BUNDLE, hid_executable_sha256=HID_EXEC_SHA,
    metaclass=hex(METACLASS), vptr=hex(VPTR), virtual_slots_checked=SLOT_COUNT,
    original_base_size=CLASS_SIZE, overrides=overrides, binding_count=len(bindings),
    with_bytes=hex(WITH_BYTES), with_bytes_bytes=len(factory), with_bytes_sha256=sha(factory),
    temperature_type=15, temperature_minimum_event_bytes=TYPE15_MINIMUM,
    encoded_bytes=48, plan_slide=slide, plan_extents=moved,
    archive_members=members, archive_sha256=sha(archive.read_bytes()),
    ringer_virtual_slots_checked=SLOT_COUNT, ringer_overrides=ringer_overrides,
    keyboard_minimum_event_bytes=32, ringer_uses_original_keyboard_factory=True,
    original_keyboard_factory=hex(NATIVE_KEYBOARD_FACTORY),
    automatic_init_removed_without_code_change=True,
    production_enabled=not a.standalone, hardware_tested=False, native_client_tested=False)
(out / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
report['wait_observer'] = {'recipes': wait_recipes, 'priority': 'LOW', 'samples': 2,
                         'initial_seconds': 30, 'after_capture_seconds': 60,
                         'missing_target_retry_seconds': 15, 'lookup_cap': 24,
                         'source_sha256': {n: sha((kit / n).read_bytes()) for n in
                                           ['SmThermalWait.c', 'SmThermalWaitProc.c', 'SmThermalWaitBridge.h', 'SmThermalWaitCore.h', 'SmThermalWaitExpected.h', 'SmGateTrace.h', 'SmGateTrace.c']},
                         'hardware_tested': False}
(out / 'build.json').write_text(json.dumps(report, indent=2) + '\n')

if not a.standalone:
    container = a.platform_archive
    assert container.is_file(), 'board platform archive must be built first'
    existing = subprocess.check_output(['llvm-ar', 't', str(container)], text=True).splitlines()
    assert not any(m in existing for m in members), [m for m in members if m in existing]
    report['platform_archive_members_before'] = existing
    report['platform_archive_sha256_before'] = sha(container.read_bytes())
    subprocess.run(['llvm-ar', 'r', str(container), *[str(out / m) for m in members]], check=True)
    final = subprocess.check_output(['llvm-ar', 't', str(container)], text=True).splitlines()
    assert final == existing + members, final[-8:]
    report['platform_archive_members_after'] = final
    report['platform_archive_sha256'] = sha(container.read_bytes())
    (out / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({k: report[k] for k in ['virtual_slots_checked', 'binding_count', 'overrides',
                                          'plan_slide', 'archive_sha256']}, indent=2))
print('PASS original IOHIDEventService ABI, live backend, temperature event and late service archived')
