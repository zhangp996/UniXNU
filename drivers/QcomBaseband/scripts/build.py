#!/usr/bin/env python3
"""Build QcomBaseband with a prepared XNU tree's C++ and C compile records."""
import argparse, hashlib, json, subprocess
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--tree', type=Path, required=True)
parser.add_argument('--coachz', action='store_true', help='include the SC7180 IPA v4.2 board adapter')
args = parser.parse_args()
tree = args.tree.resolve()
out = tree / 'build/qcom-baseband'
out.mkdir(parents=True, exist_ok=True)

def record_for(name):
    matches = sorted((tree / 'build/xnu/ios-obj').glob('**/' + name))
    if len(matches) != 1:
        raise SystemExit(f'Expected one {name} compile record, found {len(matches)}')
    return json.loads(matches[0].read_text().strip().rstrip(','))

cpp = record_for('IOPlatformExpert.cpo.json')
c = record_for('vfs_conf.o.json') if args.coachz else None
sources = [(SOURCE / 'baseband/coachz_baseband.cpp', cpp)]
if args.coachz:
    sources += [(SOURCE / name, c) for name in ('ipa/coachz_ipa_config.c', 'ipa/coachz_gsi.c', 'platform/coachz/coachz_modem.c')]
objects, report = [], []
for source, record in sources:
    obj = out / (source.stem + '.o')
    argv, i = [], 0
    raw = record['arguments']
    while i < len(raw):
        value = raw[i]
        if value in ('-o', '-MF', '-MT', '-MJ'):
            i += 2
            continue
        if value == '-Xclang' and i + 1 < len(raw) and raw[i + 1].startswith('-W'):
            i += 2
            continue
        if value == record['file'] or (value.endswith(('.cpp', '.c')) and not value.startswith('-')):
            value = str(source)
        if value not in ('-MD', '-MP') and not value.startswith('-W'):
            argv.append(value)
        i += 1
    argv += ['-o', str(obj), '-I', str(SOURCE / 'baseband'), '-I', str(SOURCE / 'ipa'), '-mgeneral-regs-only', '-Wall', '-Wextra']
    if str(source) not in argv:
        raise SystemExit('Compile record did not name its source: ' + record['file'])
    result = subprocess.run(argv, cwd=record['directory'], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    (out / (source.stem + '.log')).write_text(result.stdout)
    (out / (source.stem + '.command.json')).write_text(json.dumps(argv, indent=2) + '\n')
    if result.returncode:
        raise SystemExit(result.stdout[-6000:])
    objects.append(obj)
    report.append(dict(source=str(source.relative_to(SOURCE)), sha256=hashlib.sha256(source.read_bytes()).hexdigest(), object_sha256=hashlib.sha256(obj.read_bytes()).hexdigest()))
archive = out / 'libQcomBaseband.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), *map(str, objects)], check=True)
(out / 'build-report.json').write_text(json.dumps(dict(coachz=args.coachz, sources=report, archive_sha256=hashlib.sha256(archive.read_bytes()).hexdigest()), indent=2) + '\n')
print(f'QcomBaseband: {len(objects)} objects -> {archive}')
