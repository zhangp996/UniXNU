#!/usr/bin/env python3
"""Restore omitted DriverKit headers from Apple's earlier Darwin 23 source."""
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import urllib.parse
import urllib.request
import time

# Tags of the Apple sources that still carry the headers newer xnu omits; feeds.toml sets them for the unixnu driver.
TAG = os.environ.get('UNIXNU_DRIVERKIT_TAG', 'xnu-10063.121.3')
TRUST_TAG = os.environ.get('UNIXNU_TRUSTCACHE_TAG', 'xnu-8796.101.5')
DESTINATION = Path('.cache/xnu-work/iokit/DriverKit')
CACHE = Path(f'.cache/sources/xnu-driverkit-{TAG.removeprefix("xnu-")}.json')
URL = f'https://api.github.com/repos/apple-oss-distributions/xnu/contents/iokit/DriverKit?ref={TAG}'
# Anonymous GitHub API calls share a small per-address quota that hosted CI runners exhaust (HTTP 403);
# a token in GITHUB_TOKEN or GH_TOKEN is sent to GitHub hosts only.
TOKEN = os.environ.get('GITHUB_TOKEN') or os.environ.get('GH_TOKEN')


def fetch(url):
    # curl retries the proxy's connection limit where urllib gives up immediately
    import subprocess
    for attempt in range(6):
        command = ["curl", "-sSL", "--max-time", "60", "--retry", "3", "--retry-delay", "2"]
        if TOKEN:
            command += ["-H", "Authorization: Bearer " + TOKEN]
        r = subprocess.run(command + [url], capture_output=True)
        if r.returncode == 0 and r.stdout:
            return r.stdout
        time.sleep(3 * (attempt + 1))
    raise OSError("curl failed for " + url)

def entries_of(path, url):
    """GitHub API file lists only; refetch a cache that holds an error object or a rate-limit message."""
    def valid(data):
        return isinstance(data, list) and all(isinstance(entry, dict) and 'name' in entry for entry in data)
    try:
        data = json.loads(path.read_text())
    except Exception:
        data = None
    if not valid(data):
        path.unlink(missing_ok=True)
        path.write_bytes(fetch(url))
        data = json.loads(path.read_text())
    if not valid(data):
        raise SystemExit('GitHub API did not return a file list for ' + url)
    return data


def restore(entry, destination=DESTINATION, tag=TAG):
    if entry['type'] != 'file' or not entry['name'].endswith('.h'):
        return None
    name = entry.get('relative', entry['name'])
    target = destination / name
    if target.exists():
        return None
    data = b''
    digest = ''
    for attempt in range(8):
        data = fetch(entry['download_url'])
        digest = hashlib.sha1(b'blob ' + str(len(data)).encode() + b'\0' + data).hexdigest()
        if digest == entry['sha']:
            break
        time.sleep(2 * (attempt + 1))
    if digest != entry['sha']:
        raise ValueError(f'Git blob hash mismatch: {entry["name"]}')
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_bytes(data)
    return {'name': name, 'source_tag': tag, 'git_blob': digest,
            'sha256': hashlib.sha256(data).hexdigest()}


if __name__ == '__main__':
    if not DESTINATION.is_dir():
        raise SystemExit('Create the local XNU work source before preparing headers')
    if not CACHE.exists():
        CACHE.parent.mkdir(parents=True, exist_ok=True)
        CACHE.write_bytes(fetch(URL))
    entries = entries_of(CACHE, URL)
    crypto_cache = CACHE.with_name(f'xnu-driverkit-crypto-{TAG.removeprefix("xnu-")}.json')
    crypto_url = URL.replace('/DriverKit?', '/DriverKit/crypto?')
    if not crypto_cache.exists():
        crypto_cache.write_bytes(fetch(crypto_url))
    for entry in entries_of(crypto_cache, crypto_url):
        entry['relative'] = 'crypto/' + entry['name']
        entries.append(entry)
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        restored = [entry for entry in executor.map(restore, entries) if entry]
        trust_tag = TRUST_TAG
        trust_cache = CACHE.with_name(f'xnu-trustcache-{TRUST_TAG.removeprefix("xnu-")}.json')
        trust_url = ('https://api.github.com/repos/apple-oss-distributions/xnu/'
                     f'contents/EXTERNAL_HEADERS/TrustCache?ref={trust_tag}')
        if not trust_cache.exists():
            trust_cache.write_bytes(fetch(trust_url))
        trust_destination = Path('.cache/xnu-work/EXTERNAL_HEADERS/TrustCache')
        def restore_trust(entry):
            return restore(entry, trust_destination, trust_tag)
        restored.extend(entry for entry in executor.map(
            restore_trust, entries_of(trust_cache, trust_url)) if entry)
    previous = Path('.cache/sources/xnu-restored-headers.json')
    existing = json.loads(previous.read_text()) if previous.exists() else []
    by_name = {entry['name']: entry for entry in existing + restored}
    previous.write_text(json.dumps(list(by_name.values()), indent=2) + '\n')
    print(f'Restored {len(restored)} omitted public headers from Apple; blob hashes verified')
