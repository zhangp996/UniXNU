# launchd job cache overlay: PreBoard gets RunAtLoad (HANDOFF §7 step 1)

Date: 2026-11-14.  Device repo `bringup-2026-11-12`, framework checkout `UniXNU`.

Step 1 of the handoff: *deliver the rewritten cache to the phone and mount it over
`/System/Library/xpc`*.  Everything below is local; the phone only runs the final image.

## What was delivered

```
docs/evidence/launchd/launchd-modified.plist   887,340 B   OS job cache, RunAtLoad = true on both PreBoard jobs
docs/evidence/launchd/launchd-original.plist 1,640,626 B   the cache as shipped (unchanged)
```

The rewrite was re-checked with plistlib: all six top-level keys are present, `LaunchDaemons`
has the same 561 jobs, and the **only** differences are `RunAtLoad` on

```
/System/Library/LaunchDaemons/com.apple.preboardservice.plist       RunAtLoad absent -> true
/System/Library/LaunchDaemons/com.apple.preboardservice_v2.plist    RunAtLoad absent -> true
```

(49 -> 51 jobs with RunAtLoad; no other job changes, nothing lost.)

## How the file reaches the Data volume

The payload builder has no extra-files field, and the Data seed cannot be edited on Linux
(`seed-data-skip-setup.py` needs macOS `hdiutil`), so the file travels in the **GPU addon**:
the loader hands it to the kernel as `/chosen/memory-map/GpuAddon` and
`native_gpu_addon_install()` writes every entry below `/private/var/ngpu` before the overlays
mount.  That path already existed for the Adreno plugin; it now carries the cache as well.

```
scripts/make-launchd-cache-addon.py      (device repo)  stages and packs the four files
scripts/make-gpu-addon.py                (framework)    new --no-plugin and --extra SRC:DEST[:MODE]
```

The addon (`out/.../payload/xpc-addon.bin`, 773,152 B, sha256 `00af5218…`) carries

| entry | bytes | why |
| --- | --- | --- |
| `xpc/launchd.plist` | 887,340 | the rewritten cache, mounted over `/System/Library/xpc` |
| `xpc-raw/launchd.plist` | 1,640,626 | the unchanged cache, for the `launchd-cache=2` control boot |
| `daemons/com.apple.preboardservice.plist` | 621 | same job with `RunAtLoad`, visible through the daemons union |
| `daemons/com.apple.preboardservice_v2.plist` | 1,209 | same job with `RunAtLoad`, visible through the daemons union |

The two `daemons/` files are the belt-and-braces path: the union over
`/System/Library/LaunchDaemons` is already mounted by `zzz-launchd-overlay.patch` (a boot reads
the shadowed `com.apple.chronod.plist` back through it), so PreBoard also starts if launchd
falls back to the directory scan instead of the cache.

## The kernel side

`patches/xnu/zzz-launchd-cache.patch` (new, 2,620 B, one hunk `@@ -7902,6 +7902,40 @@` in
`bsd/kern/kern_exec.c`), added at the end of `patches/xnu/series`.  Inside
`native_gpu_addon_prepare()`, after the wallpaper mount:

* `launchd-cache` boot argument: `1` (default) mounts `/private/var/ngpu/xpc`, `2` mounts
  `/private/var/ngpu/xpc-raw`, `0` skips the mount;
* `native_gpu_addon_mount(…, 0xa00, source, …, "/System/Library/xpc", …)` — a read-only
  `bindfs` union with the sealed lower directory, exactly like the extensions/daemons/wallpaper
  overlays; the mount is in place before launchd's `main()` (the whole prepare runs inside the
  exec of `/sbin/launchd`);
* the boot log line

```
launchd cache: mode=1 mount stage=5 status=0 visible=887340 error=0
```

`stage` 5 is "union shows the sealed entry and the owner entry", `visible` is the size of
`/System/Library/xpc/launchd.plist` read through the mount: **887340** means launchd will read the
rewritten cache, **1640626** the original, `-1` means the lookup failed.

The patch was produced the way the handoff prescribes: back up `bsd/kern/kern_exec.c` from a
freshly prepared work tree, edit, `git diff --no-index -U3 before.c after.c`, rewrite the header
paths to `a/bsd/kern/kern_exec.c` / `b/bsd/kern/kern_exec.c`.  `git apply --check` on the
pristine file passes and reproduces the edited file byte for byte.

## The signature question (handoff §7 step 3)

`/System/Library/xpc/launchd.plist.sig` (21,346 B) is a detached code signature:

```
CSMAGIC_EMBEDDED_SIGNATURE, 5 blobs
  CodeDirectory  sha1    flags 0x10002 (adhoc)  401 pages  cdhash 34ad6026…
  Requirements   (empty) 12 B
  CodeDirectory  sha256  flags 0x10002 (adhoc)  401 pages  cdhash 1dceadc2…
  CMS wrapper    (empty) 8 B        identification slot (empty) 28 B
identifier launchd-b870d2bfc5324009c3e904a4506f78e102d89b2b
```

None of those cdhashes is in `out/ipsw/21H461/trust.bundle` or in
`044-76040-038.dmg.trustcache`, and `/sbin/launchd` has no `SecStaticCode`/`cs_` imports and no
`.sig` string — so whatever checks the file is a self-consistency check inside launchd/libxpc,
not a trust-anchor check.  `launchd-cache=2` settles it empirically: the unmodified cache carries
a signature that matches it, so if mode 2 boots while mode 1 stops at "Unable to load cache" the
overlay is fine and the edited *bytes* are refused; if both boot and only mode 1 starts
PreBoardService, the cache path is accepted.

## Built artifacts

```
kernel (tree build)     8a4843dd5980a90f…  125,054,976 B
kernel in payload       7ef94860edff9d1f…  (AMFI patches from build_payload.sh)
payload.img             53645f0c0f970af0…  184,549,376 B, 56,700,928 B free, gpuaddon inside verified
bootEU.img              7a29372118c7f919…  199,782,400 B  D:\新建文件夹\instantnoodlep-artifacts\bootEU.img
```

`payload.json` carries `"gpu_addon_sha256": "00af5218…"` (was `null` in every earlier payload).

## Boot

```
capture_eu.ps1 -Out <log> -StartDelaySeconds 6 -Seconds 300      # start first, the COM port is exclusive
fastboot boot bootEU.img
```

Read in the log, in this order:

1. `launchd cache: mode=1 mount stage=5 status=0 visible=887340 error=0` — the overlay is live.
2. launchd's own log: a rejected cache logs `Unable to load cache` (and possibly
   "disk will be searched for uncached services"); a load says nothing.
3. a `PreboardService` process (`boot-ui-focus` samples it), then the Hello screen instead of the
   Apple logo.

Control boots: `--kernel-arg launchd-cache=2` (unchanged cache over the same union) and
`--kernel-arg launchd-cache=0` (no union at all), then rebuild payload + boot image as usual.

## Reproduce

```
wsl -d Ubuntu-22.04 -u root -- bash /mnt/d/新建文件夹/_agent/build_kernel.sh      # after a series change
wsl -d Ubuntu-22.04 -u root -- bash /mnt/d/新建文件夹/_agent/pack_nolc.sh         # payload + bootEU.img
```

`build_payload.sh` now builds the addon with `scripts/make-launchd-cache-addon.py` and passes
`--gpu-addon`, so a rebuilt payload always carries the cache.

## Boot results (2026-11-14 evening, four boots on the phone)

### 1. The cache overlay is live, but the OS checks the signature (boot lc1)

```
launchd cache: mode=1 mount stage=5 status=0 visible=887340 error=0
Using default cache paths
Code: /System/Library/xpc/launchd.plist Sig: /System/Library/xpc/launchd.plist.sig
cdhash: {length = 20, bytes = 0x1dceadc254af12d78ea5ac8e8c6458dfdae5470e} is trusted
Attached signature to file, checking ...
CODE SIGNING: process 22[launchd_cache_lo]: rejecting invalid page at address 0x100e7a000
             from offset 0x0 in file "/System/Library/xpc/launchd.plist"
userspace panic: boot task failure: launchd_cache_loader - exited due to SIGKILL
```

So the loader does attach the detached `launchd.plist.sig` to the vnode and lets the **kernel
validate the mapped pages**; the rewritten bytes fail page 0 and the loader is SIGKILLed, which is a
boot-task panic.  (AMFI reported the shipped cdhash as trusted, so it is the detached signature, not
the trust cache, that decides.)

### 2. `launchd_unsecure_cache` skips the whole block (boot lc2/lc3)

`/usr/libexec/launchd_cache_loader` parses `launchd_unsecure_cache=` from `kern.bootargs` (a number:
`strtoul`), prints `Using unsecure cache: %s` and branches from `0x1000024b0` straight to
`0x100002828` - past `SecStaticCodeCreateWithPath`, `SecCodeSetDetachedSignature`,
`SecStaticCodeCheckValidity` and the AMFI trust check.  With `launchd_unsecure_cache=1` in
`[payload] kernel_args`:

```
Using unsecure cache: /System/Library/xpc/launchd.plist
Trying to send bytes to launchd: 2819 4096
Sending validated cache to launchd
Cache sent to launchd successfully
```

No panic, and launchd runs the rewritten cache.

### 3. PreBoard now starts (boot lc3, `launchd_log_serial=1`)

```
(user/501/com.apple.preboardservice)    : Successfully spawned PreboardService[50] because speculative
(user/501/com.apple.preboardservice_v2) : Successfully spawned PreboardService[68] because speculative
(user/501/com.apple.SpringBoard [27])   : Successfully spawned SpringBoard[27] because system support
(user/501/com.apple.backboardd [58])    : Successfully spawned backboardd[58] because speculative
(user/501/com.apple.chronod [97])       : Successfully spawned chronod[97] because ipc (mach)
DISPLAY: present copied 2376 rows x 4320 bytes (frame 1/2/3)
```

Both PreBoard jobs are on-demand in the shipped cache and never start; with `RunAtLoad` added they
both spawn and stay running.  This is the step-1 goal and it works.

### 4. What still blocks the screen (boot lc4, `boot-ui-trace=1`)

The kernel's process probe prints one row per traced slot when it changes:

```
PROC[0] launchd     p1  n7  r0 w7  s1/31 Wfffffff282135aa0 Cfffffff00bd412a0 U1dd88d128
PROC[2] SpringBoard p27 n13 r0 w13 s1/47 Wfffffff00700a83c Cfffffff00b8ce3d0 U1dd888ad4
PROC[4] backboardd  p58 n20 r0 w20 s1/31 W0                Cfffffff00b860aa4 U1dd888b58
```

Symbolized against the built kernel (`llvm-nm -n`):

| slot | continuation | meaning |
| --- | --- | --- |
| SpringBoard | `_semaphore_wait_continue+0` | 13 threads, all waiting on a semaphore |
| backboardd | `_ipc_mqueue_receive_continue+0` | 20 threads, waiting for a Mach message |
| launchd | `__sleep_continue+0` | normal idle |

So every UI process is up and parked in IPC/semaphore waits: the screen still shows the boot logo
because the render server cannot composite without a GPU, which is HANDOFF §6 blocker 1 and §7 item 4,
not the launchd cache.

The probe's PreBoard slot printed "absent from this process pass" because it matches the **process
name** and the binary runs as `PreboardService`, not `PreBoard`; `board.toml` now passes
`boot-ui-focus3=PreboardService` so the next boot samples it too.

### 5. The Data seed is not the blocker either

`owner-data-3g.dbr` unpacked with `packed_ramdisk.regions` and mounted read-only with apfs-fuse has
**no** `com.apple.purplebuddy` preferences at all, and `setup-skip=0` keeps
`native_skip_setup_write()` from writing them (`kern_exec.c` returns before the first write), so the
Setup state is untouched and PreBoard has a Hello screen to show.
