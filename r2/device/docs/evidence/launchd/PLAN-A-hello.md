# Getting the Hello screen: launchd cache overwrite (plan A)

State: the launchd cache was analysed offline.  PreBoard is an on-demand launchd job
(`com.apple.preboardservice_v2` and `com.apple.preboardservice`), so it never starts while nothing looks the
Mach services up, and SpringBoard never gets to the Setup Assistant.  A rewritten cache with `RunAtLoad = true`
on both jobs is in `docs/evidence/launchd/launchd-modified.plist` (887,340 bytes, round-trips through plistlib).

Everything below is local work; the phone only runs the final image.

## 1. Put the file where the kernel can copy it

The payload ramdisk is assembled from `out/<device>/<os>/<config>/payload/`.  Drop the modified cache in a
directory the boot patch can read, e.g.

    mkdir -p out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/payload/ngpu/xpc
    cp docs/evidence/launchd/launchd-modified.plist \
       out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/payload/ngpu/xpc/launchd.plist

then rebuild the payload and the boot image (`./unixnu bootimg`).

## 2. Copy it onto the Data volume and mount it over /System/Library/xpc

`zzz-launchd-cache.patch` already carries the pieces: it creates `/private/var/ngpu/xpc`, writes a patched copy
there and calls `native_gpu_addon_mount` with source `/private/var/ngpu/xpc` and point `/System/Library/xpc`.
Two ways to reuse it:

- **A1 (small)**: change the byte-slot rewrite for a whole-file copy from the payload path, then keep the existing
  mount call.  The patch must be rebuilt as a before/after diff of `bsd/kern/kern_exec.c` (back up the file first;
  `git diff` against the clone's HEAD pulls in every series hunk and is useless), and its header paths fixed to
  `a/bsd/kern/kern_exec.c`, `b/bsd/kern/kern_exec.c`.
- **A2 (smaller)**: write the copy from `package/LegacyDisplayProbe.cpp` (it already reads and writes files through
  `vnode_open`/`VNOP_WRITE` in the skip-setup path) and only keep the patch's mount call.

## 3. Signatures

The cache ships with `launchd.plist.sig`.  The kernel runs `amfi_get_out_of_my_way=1`; whether launchd accepts a
cache whose signature no longer matches is untested.  If it refuses, the mount must also shadow the `.sig` with a
copy that launchd does not read (or the framework's launchd patch must skip the check).

## 4. What success looks like

- serial: a PreboardService process appears (boot-ui-focus can sample it),
- the panel leaves the Apple logo and draws the Hello / Setup Assistant, which the iPadOS 17 records reach
  CPU-rendered without a GPU (docs/devices/umi.md, qemu.md).

## 5. Tooling built for this (keep it)

    apfs-fuse (sgan81, with the lzfse submodule) in /tmp/apfs-fuse/build
    mount: apfs-fuse -o allow_other out/ipsw/21H461/044-76040-038.dmg /mnt/apfs
    cache:  /mnt/apfs/root/System/Library/xpc/launchd.plist  (bplist00; plistlib reads and writes it)

The same mount answers any future userland question about this OS build without flashing the phone.
