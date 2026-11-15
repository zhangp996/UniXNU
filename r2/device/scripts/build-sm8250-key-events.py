#!/usr/bin/env python3
"""Build the board-scoped AppleKeyStore state service with target headers."""
import hashlib
import json
import os
import subprocess
from pathlib import Path

ROOT = Path(os.environ.get('SMUFS_BUILD_TREE', str(Path(__file__).resolve().parents[1])))
OUT = ROOT / 'build/native-key-events'
OUT.mkdir(exist_ok=True, parents=True)
COMPONENT = ROOT / 'drivers/AppleKeyStoreState/NativeKeyStoreEvents.cpp'
SERVICE = OUT / 'state-src/NativeKeyStoreEvents.cpp'
BOARD = ROOT / 'kits/instantnoodlep/SmKeyStoreEvents.cpp'
original = COMPONENT.read_bytes()
assert hashlib.sha256(original).hexdigest() == '273b030afee897e48c30bc917c4df8a9c4d94a189d073ef80c0e0a15caef82ee'
SERVICE.parent.mkdir(exist_ok=True)
SERVICE.write_bytes(original)
subprocess.run(['patch', '--batch', '--fuzz=0', SERVICE, BOARD.with_name('NativeKeyStoreEvents36.patch')], check=True)
RECORD = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'

recipe = json.loads(RECORD.read_text().strip().rstrip(','))

def compile_source(source: Path, output: Path) -> None:
    command = []
    arguments = recipe['arguments']
    i = 0
    while i < len(arguments):
        value = arguments[i]
        if value == recipe['file']:
            command.append(str(source))
        elif value in ('-o', '-MF', '-MT', '-MJ'):
            if value == '-o':
                command += ['-o', str(output)]
            i += 1
        elif value == '-Xclang' and arguments[i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W'):
            command.append(value)
        i += 1
    command += [
        '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
        '-Wno-missing-designated-field-initializers', '-Wno-c99-designator',
        '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
        '-Wno-unnecessary-virtual-specifier', '-I', str(ROOT / '.cache/xnu-work/osfmk'),
        '-mgeneral-regs-only',
    ]
    result = subprocess.run(command, cwd=recipe['directory'], capture_output=True, text=True)
    (OUT / (source.stem + '.command.json')).write_text(json.dumps(command, indent=2) + '\n')
    (OUT / (source.stem + '.log')).write_text(result.stdout + result.stderr)
    if result.returncode:
        raise SystemExit((result.stdout + result.stderr)[-6000:])

service_object = OUT / 'NativeKeyStoreEvents.o'
late_object = OUT / 'NativeKeyStoreEvents.late.o'
board_object = OUT / 'SmKeyStoreEvents.o'
compile_source(SERVICE, service_object)
compile_source(BOARD, board_object)
subprocess.run([
    'llvm-objcopy', '--remove-section=__DATA,__mod_init_func',
    '--globalize-symbol=__GLOBAL__sub_I_NativeKeyStoreEvents.cpp',
    service_object, late_object,
], check=True)

# The component marker is required for source selection and coverage. The kernel
# linker orders component archives before auxiliary archives, so put the late
# service beside its only caller in the already-qualified display archive. This
# also leaves the canonical component output untouched for provenance.
component_archive = ROOT / 'build/apple-key-store-state/libAppleKeyStoreState.a'
archive = ROOT / 'build/native-legacy-display/libNativeLegacyDisplay.late.a'
display_report_path = archive.with_name('build-report.json')
assert component_archive.exists(), 'component:AppleKeyStoreState must precede this builder'
assert archive.exists(), 'build-sm8250-display.py must precede this builder'
display_report = json.loads(display_report_path.read_text())
assert hashlib.sha256(archive.read_bytes()).hexdigest() == display_report['archive_sha256']
before = subprocess.check_output(['llvm-ar', 't', archive], text=True).splitlines()
assert 'NativeKeyStoreEvents.late.o' not in before and 'SmKeyStoreEvents.o' not in before
subprocess.run(['llvm-ar', 'r', archive, late_object, board_object], check=True)
members = subprocess.check_output(['llvm-ar', 't', archive], text=True).splitlines()
assert members == before + ['NativeKeyStoreEvents.late.o', 'SmKeyStoreEvents.o']

sha = lambda path: hashlib.sha256(Path(path).read_bytes()).hexdigest()
report = {
    'passed': True,
    'mode': 'read-only state queries for verified factory RAM Data',
    'source_sha256': {str(SERVICE): sha(SERVICE), str(BOARD): sha(BOARD)},
    'object_sha256': {str(late_object): sha(late_object), str(board_object): sha(board_object)},
    'archive': str(archive),
    'archive_sha256': sha(archive),
    'component_archive_sha256': sha(component_archive),
    'members': members,
    'hardware_tested': False,
}
(OUT / 'build.json').write_text(json.dumps(report, indent=2) + '\n')
display_report['archive_sha256'] = sha(archive)
display_report['source_sha256'][str(SERVICE)] = sha(SERVICE)
display_report['source_sha256'][str(BOARD)] = sha(BOARD)
display_report['key_events'] = {
    'mode': report['mode'],
    'archive_members': ['NativeKeyStoreEvents.late.o', 'SmKeyStoreEvents.o'],
    'build_report': str(OUT / 'build.json'),
}
display_report_path.write_text(json.dumps(display_report, indent=2) + '\n')
print('PASS target key-events compile/archive; RAM Data state runtime pending')
