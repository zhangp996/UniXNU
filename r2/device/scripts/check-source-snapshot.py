#!/usr/bin/env python3
"""Validate the published source inventory; this does not build or boot an image."""
from pathlib import Path
import hashlib, json

root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / 'source-manifest.json').read_text())
for name, digest in manifest.items():
    path = root / name
    assert path.is_file(), 'Missing source: ' + name
    assert hashlib.sha256(path.read_bytes()).hexdigest() == digest, 'Changed source: ' + name
assert not any(p.name == 'OwnerKey.bin' for p in root.rglob('*'))
assert not (root / 'patches/xnu/sm8250-metal-upstream-r2.patch').exists()
assert hashlib.sha256((root / 'package/GopFramebufferR20/NativeLegacyDisplay.cpp').read_bytes()).hexdigest() == \
    '09d7811db8fc6b659e12a05690867326e851f0d11bec7ffbb36b72c33ea035ca'
assert (root / 'patches/xnu/series').read_text().endswith('sm8250-metal-ownerdata.patch\n')
for name in (root / 'patches/xnu/series').read_text().splitlines():
    name = name.split('#', 1)[0].strip()
    if not name: continue
    if name.startswith('component:'):
        component, relative = name.removeprefix('component:').split('/', 1)
        candidates = [root / 'support/UniXNU' / kind / component / relative for kind in ['drivers', 'kits']]
        assert sum(path.is_file() for path in candidates) == 1, name
    else:
        assert (root / 'patches/xnu' / name).is_file(), name
print('PASS:', len(manifest), 'source hashes; R1 display and patch order; no owner key or R2 patch')
