#!/usr/bin/env python3
"""Sign and fully verify an original OS or SystemOS disk image on the build host.

scripts/build-owner-os-policy.py does the same on macOS, where the image is attached by hdiutil and
read through /dev/rdiskN. This portable form reads the identical raw disk out of the UDIF container
(the reader the userdata step already uses) and makes the same two claims: the header was signed
with the owner key, and every block re-verifies through the kit's own C.
"""
import argparse
import ctypes
import hashlib
import importlib.util
import json
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))

from unixnu import udif

BLOCK = 4096


def block_stream(image):
    """The raw disk, one 4096-byte block at a time, whatever chunk sizes the container yields."""
    pending = bytearray()
    for chunk in image.blocks():
        pending += chunk
        start = 0
        while len(pending) - start >= BLOCK:
            yield bytes(pending[start:start + BLOCK])
            start += BLOCK
        if start:
            del pending[:start]
    if pending:
        yield bytes(pending)


class Blocks:
    """The raw disk as a file-like reader, so the manifest signs exactly the bytes the disk holds.

    The manifest asks for one block at a time, so hand it whole blocks: a general-purpose buffer here
    would copy the pending chunk on every read and turn a 6 GiB image into a quadratic walk.
    """

    def __init__(self, blocks):
        self.blocks = blocks
        self.current = b''

    def read(self, count):
        if not self.current:
            try:
                self.current = next(self.blocks)
            except StopIteration:
                return b''
        data, self.current = self.current[:count], self.current[count:]
        return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--component', choices=('os', 'systemos'), required=True)
    parser.add_argument('--dmg', type=Path, required=True, help='the original IPSW member')
    parser.add_argument('--volume-uuid', required=True, help='the APFS volume UUID the policy signs')
    parser.add_argument('--generation', type=int, default=1)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()

    out = args.output or (ROOT / ('build/verified-root-' + args.component))
    keys = ROOT / 'build/verified-root-host/keys'
    library = ROOT / 'build/verified-root-host/verified-root.dylib'
    for path in (args.dmg, keys / 'owner.pem', keys / 'owner-public.pem', library):
        if not path.is_file():
            raise SystemExit(str(path) + ' is missing')
    expected_uuid = uuid.UUID(args.volume_uuid)
    image = udif.Image(args.dmg)
    size = image.size

    digest = hashlib.sha256()
    with args.dmg.open('rb') as stream:
        for chunk in iter(lambda: stream.read(8 << 20), b''):
            digest.update(chunk)

    spec = importlib.util.spec_from_file_location('verified_manifest', ROOT / 'kits/VerifiedRoot/tools/manifest.py')
    manifest = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(manifest)
    record = manifest.make(Blocks(block_stream(image)), size, keys / 'owner.pem', keys / 'owner-public.pem',
                           expected_uuid, args.generation, out)

    class View(ctypes.Structure):
        _fields_ = [('hashes', ctypes.c_void_p), ('image_bytes', ctypes.c_uint64), ('block_count', ctypes.c_uint64),
                    ('generation', ctypes.c_uint64), ('volume_uuid', ctypes.c_uint8 * 16),
                    ('image_sha256', ctypes.c_uint8 * 32)]

    native = ctypes.CDLL(str(library))
    native.vr_host_open.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_uint64,
                                    ctypes.POINTER(View)]
    native.vr_host_block.argtypes = [ctypes.POINTER(View), ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t]
    encoded = ctypes.create_string_buffer((out / 'manifest.bin').read_bytes())
    anchor = ctypes.create_string_buffer((out / 'trusted-key.bin').read_bytes())
    view = View()
    if native.vr_host_open(encoded, len(encoded) - 1, anchor, 1, ctypes.byref(view)) or \
            bytes(view.volume_uuid) != expected_uuid.bytes:
        raise SystemExit('Real C signature/volume authentication failed')

    complete = hashlib.sha256()
    indices = [0, 1, view.block_count // 2, view.block_count - 1]
    samples, lengths = [], []
    written = 0
    for index, block in enumerate(block_stream(image)):
        if index >= view.block_count:
            raise SystemExit('The image exceeds the signed block count')
        if native.vr_host_block(ctypes.byref(view), index, block, len(block)) != 0:
            raise SystemExit('Actual C block verification failed at ' + str(index))
        complete.update(block)
        written += len(block)
        if index in indices:
            samples.append(block.ljust(BLOCK, b'\0'))
            lengths.append(len(block))
    if written != view.image_bytes or len(samples) != len(indices):
        raise SystemExit('The image does not match the signed bounds')
    if complete.digest() != bytes(view.image_sha256) or complete.hexdigest() != record['image_sha256']:
        raise SystemExit('Actual complete original digest differs')
    (out / 'original-samples.bin').write_bytes(b''.join(samples))
    report = dict(image_bytes=view.image_bytes, blocks_verified=view.block_count, image_sha256=complete.hexdigest(),
                  volume_uuid=str(expected_uuid).upper(), generation=view.generation,
                  source_compressed_sha256=digest.hexdigest(), source_format='UDIF',
                  trusted_key_sha256=record['trusted_key_sha256'], actual_sample_indices=indices,
                  actual_sample_lengths=lengths, real_c_signature_and_block_verification=True,
                  original_image_modified=False, host_only=True, boot_root_authenticated=False)
    (out / 'verification.json').write_text(json.dumps(report, indent=2) + '\n')
    print('PASS: signed and verified', view.block_count, 'blocks of', args.dmg.name, 'as', expected_uuid)


if __name__ == '__main__':
    main()

