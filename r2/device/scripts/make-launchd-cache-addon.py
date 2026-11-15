#!/usr/bin/env python3
"""Pack the rewritten launchd cache into a GPU addon for the boot payload.

  make-launchd-cache-addon.py [--unixnu DIR] [--report FILE] OUTPUT

docs/evidence/launchd/launchd-modified.plist is the OS job cache of this build with RunAtLoad = true on the two
PreBoard jobs (com.apple.preboardservice and com.apple.preboardservice_v2). They are on-demand in the original
cache, so nothing ever starts PreBoard and SpringBoard never gets to the Setup Assistant.

The addon carries, below /private/var/ngpu (scripts/make-gpu-addon.py in the framework checkout):

  xpc/launchd.plist                     the rewritten cache; the kernel's boot overlay
                                        (patches/xnu/zzz-launchd-cache.patch) mounts this directory over
                                        /System/Library/xpc before launchd starts
  xpc-raw/launchd.plist                 the unchanged cache, for launchd-cache=2: its detached signature
                                        still matches, so a boot with it tells an overlay launchd refuses
                                        from an edited cache it refuses
  daemons/com.apple.preboardservice.plist
  daemons/com.apple.preboardservice_v2.plist
                                        the same two jobs as job files with RunAtLoad; the daemons overlay
                                        that is already mounted over /System/Library/LaunchDaemons shows them,
                                        so PreBoard also starts if launchd falls back to the directory scan

The script refuses a modified cache whose only difference from the original is not exactly those two RunAtLoad
values, and verifies the packed addon before it reports success."""
import argparse
import json
import plistlib
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / 'docs/evidence/launchd'
ORIGINAL = CACHE / 'launchd-original.plist'
MODIFIED = CACHE / 'launchd-modified.plist'
JOBS = ('com.apple.preboardservice', 'com.apple.preboardservice_v2')
CACHE_KEY = '/System/Library/LaunchDaemons/%s.plist'


def check(cache, original):
    """The rewritten cache may differ from the original only in RunAtLoad of the PreBoard jobs."""
    for key in sorted(set(cache) | set(original)):
        if key != 'LaunchDaemons':
            if cache.get(key) != original.get(key):
                sys.exit('cache differs from the original outside LaunchDaemons: ' + key)
    jobs, jobs_original = cache['LaunchDaemons'], original['LaunchDaemons']
    if len(jobs) != len(jobs_original):
        sys.exit('cache has %d jobs, the original %d' % (len(jobs), len(jobs_original)))
    changed = []
    for key in sorted(jobs):
        if key not in jobs_original:
            sys.exit('cache has a job the original does not: ' + key)
        if jobs[key] == jobs_original[key]:
            continue
        fields = sorted(name for name in set(jobs[key]) | set(jobs_original[key])
                        if jobs[key].get(name) != jobs_original[key].get(name))
        if fields != ['RunAtLoad'] or jobs[key].get('RunAtLoad') is not True:
            sys.exit('job %s differs from the original in %s, not only RunAtLoad' % (key, fields))
        changed.append(key)
    expected = [CACHE_KEY % name for name in JOBS]
    if sorted(changed) != sorted(expected):
        sys.exit('changed jobs are %s, expected the two PreBoard jobs' % sorted(changed))
    for key in expected:
        if JOBS and jobs[key].get('Label') not in JOBS:
            sys.exit('%s does not carry a PreBoard label: %s' % (key, jobs[key].get('Label')))
    return expected


def verify(path, expected):
    data = path.read_bytes()
    if data[:8] != b'NGPUADD2' or struct.unpack_from('<I', data, 8)[0] != len(expected):
        sys.exit('addon header mismatch: %s' % path)
    at, entries = 24, {}
    for _ in range(len(expected)):
        length, mode, size, packed = struct.unpack_from('<IIQQ', data, at)
        at += 24
        name = data[at:at + length - 1].decode()
        at += (length + 7) & ~7
        body = zlib.decompress(data[at:at + packed])
        at += (packed + 7) & ~7
        if len(body) != size:
            sys.exit('addon entry size mismatch: ' + name)
        entries[name] = size
    if at != len(data):
        sys.exit('addon length mismatch')
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', type=Path)
    parser.add_argument('--unixnu', type=Path, default=ROOT.parent / 'UniXNU')
    parser.add_argument('--report', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        sys.exit('Refuse to overwrite ' + str(args.output))
    original_bytes, modified_bytes = ORIGINAL.read_bytes(), MODIFIED.read_bytes()
    original, modified = plistlib.loads(original_bytes), plistlib.loads(modified_bytes)
    changed = check(modified, original)
    jobs = original['LaunchDaemons']
    names = ['xpc/launchd.plist', 'xpc-raw/launchd.plist']
    with tempfile.TemporaryDirectory(prefix='launchd-cache-addon-') as work:
        work = Path(work)
        for name in names[2:]:
            (work / name).parent.mkdir(parents=True, exist_ok=True)
        for key in changed:
            label = jobs[key]['Label']
            job = dict(jobs[key])
            job['RunAtLoad'] = True
            path = work / 'daemons' / (label + '.plist')
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(plistlib.dumps(job, fmt=plistlib.FMT_XML, sort_keys=True))
            names.append('daemons/' + path.name)
        sources = {'xpc/launchd.plist': MODIFIED, 'xpc-raw/launchd.plist': ORIGINAL}
        for key in changed:
            label = jobs[key]['Label']
            sources['daemons/' + label + '.plist'] = work / 'daemons' / (label + '.plist')
        command = [sys.executable, str(args.unixnu / 'scripts/make-gpu-addon.py'), '--no-plugin']
        for name in sorted(sources):
            command += ['--extra', '%s:%s' % (sources[name], name)]
        command.append(str(args.output))
        subprocess.run(command, check=True)
        entries = verify(args.output, sorted(sources))
    report = dict(output=str(args.output), sha256=__import__('hashlib').sha256(args.output.read_bytes()).hexdigest(),
                  entries=entries, changed_jobs=changed,
                  modified=dict(path=str(MODIFIED), bytes=len(modified_bytes)),
                  original=dict(path=str(ORIGINAL), bytes=len(original_bytes)))
    if args.report:
        args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
