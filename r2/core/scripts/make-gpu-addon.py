#!/usr/bin/env python3
"""Pack the owner-built Adreno Metal plugin into a GPU addon file for the boot payload.

  make-gpu-addon.py [--publish] [--no-daemon] [--wallpaper DIR] [--wallpaper-floating DIR] [--prime-list FILE] [--pipeline-pack FILE] [--cache-export]
                    [--no-plugin] [--extra SRC:DEST[:MODE]]... [--addon EXISTING.bin] OUTPUT

The files are the ones add-gpu-plugin-data.py stages into a Data seed, with the same paths below /private/var/ngpu and the same modes:
addons/NativeAdrenoMetal.bundle/*, addons/NativeAdrenoMetal.publish (--publish),
bin/adrenoprobed and daemons/*.plist. --wallpaper adds the files of DIR (scripts/make-wallpaper-overlay.py) as wallpaper/*, which the
kernel mounts over the default wallpaper's layered animation; --wallpaper-floating DIR as wallpaper-floating/*, mounted over the same
wallpaper's floating layer (iPadOS 27). --prime-list adds FILE (the absolute paths of the OS image's .metallib
files, one per line) as addons/NativeAdrenoMetal.primelist, the libraries the render server compiles ahead (PrimeShaders.h). The payload carries the file next to the kernel (unixnu build payload --gpu-addon), the loader
hands it to the kernel as /chosen/memory-map/GpuAddon, and the kernel writes every file whose content or mode differs into the mounted
Data volume before its addon mounts (sheng-gpu-addon=1). The Data seed then no longer changes with the plugin, so its persistent store
(whose identity is the seed's content) survives a plugin rebuild. The kernel creates missing directories.

--extra SRC:DEST[:MODE] adds any other file under /private/var/ngpu/DEST (MODE in octal, default 0644) and --no-plugin packs only the
--extra files: the addon is also how a board hands its own data (a launchd cache, a job directory) to the kernel, which must work on a
host with no plugin build.

The data of every file is zlib-compressed: the payload FAT (the boot image's ramdisk) has to stay at 192 MiB, which the tablet's boot
loader accepts (a 256 MiB ramdisk does not start the firmware), and the kernel and the Data seed already take most of it.

Layout, little endian: "NGPUADD2", u32 count, u32 0, u64 total bytes; per file u32 path bytes (with its NUL, at most 160), u32 mode,
u64 size, u64 compressed size, the path padded to 8 bytes, the zlib stream padded to 8 bytes."""
import os
import re
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# ADRENO_PLUGIN_DIR: another plugin build (build/adreno-metal-plugin27 for iPadOS 27).
PLUGIN = Path(os.environ.get('ADRENO_PLUGIN_DIR') or ROOT / 'build/adreno-metal-plugin')
BUNDLE = PLUGIN / 'NativeAdrenoMetal.bundle'


def pad(data):
    return data + bytes(-len(data) % 8)


def main():
    no_daemon = '--no-daemon' in sys.argv[1:]
    no_plugin = '--no-plugin' in sys.argv[1:]
    args = [a for a in sys.argv[1:] if a not in ('--publish', '--no-daemon', '--no-plugin')]
    imported = []
    while '--addon' in args:
        at = args.index('--addon')
        if at + 1 >= len(args):
            sys.exit('--addon takes an existing NGPUADD2 file')
        imported.append(Path(args[at + 1]))
        del args[at:at + 2]
    publish = '--publish' in sys.argv[1:]
    extras = []
    while '--extra' in args:
        at = args.index('--extra')
        if at + 1 >= len(args):
            sys.exit('--extra takes SRC:DEST[:MODE]')
        source, _, rest = args[at + 1].partition(':')
        destination, _, mode = rest.partition(':')
        del args[at:at + 2]
        if not source or not destination or destination.startswith('/') or '..' in destination.split('/'):
            sys.exit('--extra takes SRC:DEST[:MODE] with a DEST relative to /private/var/ngpu')
        if mode and not re.fullmatch('[0-7]{3,4}', mode):
            sys.exit('--extra MODE is octal, e.g. 0644')
        extras.append((Path(source), destination, int(mode, 8) if mode else 0o644))
    wallpaper = None
    floating = None
    if '--wallpaper-floating' in args:
        at = args.index('--wallpaper-floating')
        floating = Path(args[at + 1])
        del args[at:at + 2]
        if not floating.is_dir():
            sys.exit('--wallpaper-floating takes a directory')
    if '--wallpaper' in args:
        at = args.index('--wallpaper')
        wallpaper = Path(args[at + 1])
        del args[at:at + 2]
        if not wallpaper.is_dir():
            sys.exit('--wallpaper takes a directory')
    prime_list = None
    if '--prime-list' in args:
        at = args.index('--prime-list')
        prime_list = Path(args[at + 1])
        del args[at:at + 2]
        lines = prime_list.read_text().split()
        if not lines or any(not line.startswith('/') or not line.endswith('.metallib') for line in lines):
            sys.exit('--prime-list takes a file of absolute .metallib paths')
    cache_export = '--cache-export' in args
    if cache_export:
        args.remove('--cache-export')
    pipeline_pack = None
    if '--pipeline-pack' in args:
        at = args.index('--pipeline-pack')
        pipeline_pack = Path(args[at + 1])
        del args[at:at + 2]
        if pipeline_pack.read_bytes()[:8] != b'SHEXP001':
            sys.exit('--pipeline-pack takes a SHEXP001 pack (scripts/make-m2vpack.py)')
    if len(args) != 1:
        sys.exit(__doc__)
    output = Path(args[0]).resolve()
    if output.exists():
        sys.exit('Refuse to overwrite ' + str(output))
    files = []
    # --addon: merge the entries of an existing NGPUADD2 addon (the reference port's GPU userspace)
    # into this one, so the loader still hands over a single addon.
    for existing in imported:
        blob = existing.read_bytes()
        if blob[:8] != b'NGPUADD2' or len(blob) < 24:
            sys.exit('not an NGPUADD2 addon: ' + str(existing))
        at, count = 24, struct.unpack_from('<I', blob, 8)[0]
        for _ in range(count):
            length, mode, size, packed = struct.unpack_from('<IIQQ', blob, at)
            at += 24
            name = blob[at:at + length - 1].decode()
            at += (length + 7) & ~7
            body = zlib.decompress(blob[at:at + packed])
            at += (packed + 7) & ~7
            if len(body) != size:
                sys.exit('bad entry size in ' + str(existing) + ': ' + name)
            files.append((name, mode, body))
    if not no_plugin:
        if not (BUNDLE / 'NativeAdrenoMetal').is_file():
            sys.exit('Build the plugin first: python3 scripts/build-adreno-metal-plugin.py')
        for path in sorted(p for p in BUNDLE.rglob('*') if p.is_file()):
            mode = 0o755 if path.name in ('NativeAdrenoMetal', 'libvulkan_freedreno.dylib') else 0o644
            files.append(('addons/NativeAdrenoMetal.bundle/' + path.relative_to(BUNDLE).as_posix(), mode, path.read_bytes()))
        if publish:
            files.append(('addons/NativeAdrenoMetal.publish', 0o644, b'1\n'))
        if prime_list:
            files.append(('addons/NativeAdrenoMetal.primelist', 0o644, prime_list.read_bytes()))
        if cache_export:   # the plugin sends its cache directory to the UFS log (CacheExport.h)
            files.append(('addons/NativeAdrenoMetal.cacheexport', 0o644, b'1\n'))
        if pipeline_pack:
            files.append(('addons/NativeAdrenoMetal.m2vpack', 0o644, pipeline_pack.read_bytes()))
        # --no-daemon: only the plugin (iPadOS 27 runs no probe daemon and keeps its launchd jobs).
        if not no_daemon:
            files.append(('bin/adrenoprobed', 0o755, (PLUGIN / 'adrenoprobed').read_bytes()))
        for plist in sorted(PLUGIN.glob('*.plist')) if not no_daemon else []:
            if not plist.name.endswith('-entitlements.plist'):   # build input, not a launchd job
                files.append(('daemons/' + plist.name, 0o644, plist.read_bytes()))
        if wallpaper:
            for path in sorted(p for p in wallpaper.rglob('*') if p.is_file()):
                files.append(('wallpaper/' + path.relative_to(wallpaper).as_posix(), 0o644, path.read_bytes()))
        if floating:
            for path in sorted(p for p in floating.rglob('*') if p.is_file()):
                files.append(('wallpaper-floating/' + path.relative_to(floating).as_posix(), 0o644, path.read_bytes()))
    for source, destination, mode in extras:
        if not source.is_file():
            sys.exit('--extra source not found: ' + str(source))
        files.append((destination, mode, source.read_bytes()))
    if not files:
        sys.exit('nothing to add: build the plugin (drop --no-plugin) or pass --extra SRC:DEST')
    body = b''
    for name, mode, data in files:
        encoded = name.encode() + b'\0'
        if len(encoded) > 160 or '..' in name.split('/') or name.startswith('/'):
            sys.exit('Bad addon path ' + name)
        packed = zlib.compress(data, 9)
        body += struct.pack('<IIQQ', len(encoded), mode, len(data), len(packed)) + pad(encoded) + pad(packed)
    blob = b'NGPUADD2' + struct.pack('<IIQ', len(files), 0, 24 + len(body)) + body
    output.write_bytes(blob)
    for name, mode, data in files:
        print(f'{mode:o} {len(data):>10} {name}')
    print(f'{output}: {len(files)} files, {len(blob)} bytes')


if __name__ == '__main__':
    main()
