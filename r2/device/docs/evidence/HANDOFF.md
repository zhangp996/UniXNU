# Handoff: OnePlus 8 Pro (in2020) iPadOS 17.7.11 bring-up

Written at the end of the session that fixed the keybag chain, brought up real touch and the IN2020
DSI backlight, and built the offline APFS/launchd analysis tooling.  Everything below is committed.

## 1. What works on the phone (verified on hardware)

| Subsystem | Evidence in the serial log |
| --- | --- |
| Display | 3 userland frames copied to the framebuffer (`present copied` x3), vsync armed (`DISPLAY: vsync 5 120`) |
| Backlight | `BACKLIGHT published class=AppleARMBacklight level=399 max=2047`, `request=1 level=1024`, `applied target=1024 observed=1024 verified=1`; the owner confirmed the panel brightness changed |
| Touch | S6SY791: `device id 0x3791`, `panel 1440 x 3168`, 5525 reports / 6249 finger positions; HID digitizer reports accepted (`SYNTOUCH report event=6 status=0`) |
| Keybag | AppleKeyStore service published (`AKSSTAGE keyevents_stage=3 keyevents_status=0`), liveactivitiesd stops crashing (33 -> 0), chronod starts |

The screen still shows the Apple logo: the Apple render server (backboardd) has no GPU accelerator to
post work to, and SpringBoard never reaches the Setup Assistant path.

## 2. The four defects fixed this session

1. **Keybag service never created** (`package/KeyStoreProbe.cpp` in the framework repo):
   - the `absent` gate could never pass because the component's own `OSDefineMetaClassAndStructors(AppleKeyStore, ...)` registers the class when the object is linked,
   - the creation path ran `OSMetaClass::preModLoad`/`postModLoad`, which deadlocks the boot.
   Fix: keep the gate only for builds without the component and allocate the service directly.
2. **`native_legacy_gpu_offer` returned true**, so every userland frame took the GPU hand-off branch and was retired without a copy. Now returns false (CPU copy path, as `docs/porting/display` requires).
3. **`native_adreno_fences`/_detail returned false** ("fence unknown", retried every 200 us forever). Now they report a fence that retires immediately.
4. **skip-setup wrote `SetupDone=true` unconditionally** in `zzz-skip-setup-prefs.patch`. Now gated on the `setup-skip` boot argument.

## 3. Repo state

- Device repo `/home/dev/unixnu/UniXNU-instantnoodlep`, branch `bringup-2026-11-12`, HEAD `65b66c1`, working tree clean.
- Kernel builds green: `bash /mnt/d/新建文件夹/_agent/build_kernel.sh` (prints the kernel sha256).
- Payload + boot image: `wsl -d Ubuntu-22.04 -u root -- bash /mnt/d/新建文件夹/_agent/pack_nolc.sh` -> `D:\新建文件夹\instantnoodlep-artifacts\bootEU.img`.
- Boot: `fastboot boot bootEU.img` with a capture running:
  `capture_eu.ps1 -Out <log> -StartDelaySeconds 6 -Seconds 300` (start it first; COM14 is exclusive).

## 4. Boot arguments that matter (board.toml `[payload] kernel_args`)

```
sheng-aks=1 offline-key-events=1 sheng-aks-sls=3      keybag (offline-keystore=1 must NOT be set)
touchscreen=1 sheng-touch-hid=1                        S6SY791 + HID digitizer descriptor and sink
backlight=1                                            IN2020 DSI backlight service
setup-skip=0                                           leave the Setup state alone (Hello path)
accel_stub=0                                           do not publish the empty IOAcceleratorES
offline-legacy-* sheng-vsync=120                        display backend
```

## 5. Local tooling built this session (important, reuse it)

The IPSW for this exact build is on disk: `/mnt/d/迅雷下载/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw`, and the
framework already extracted the DMGs to `UniXNU/out/ipsw/21H461/`.  Those DMGs are **LZFSE** compressed: `7z`
lists the partition table but extracts a corrupt APFS image.

```
# apfs-fuse built in WSL, bundles lzfse, mounts the DMG directly:
cd /tmp/apfs-fuse/build
./apfs-fuse -o allow_other /home/dev/unixnu/UniXNU/out/ipsw/21H461/044-76040-038.dmg /mnt/apfs
# SystemOS root:      /mnt/apfs/root
# launchd job cache:  /mnt/apfs/root/System/Library/xpc/launchd.plist  (1,640,626 bytes)
```

**The launchd cache is a plain `bplist00`**: Python `plistlib.loads()` parses it, `plistlib.dumps(...,
fmt=FMT_BINARY)` writes it back.  Top-level keys: `LaunchDaemons` (561 jobs, 49 with `RunAtLoad`),
`AppExtensions`, `AppRemovalServices`, `SystemLibraryTreeState`, `Symlinks`, `VersionNumber`.

## 6. Why the UI never appears - the two blockers, both identified

1. **Render server without a GPU**: backboardd waits in `semaphore_wait_continue`, SpringBoard in
   `ipc_mqueue_receive_continue`, and no `IOAccelerator` is ever opened.  The Apple stack needs the GPU stage
   (`docs/porting/gpu`).
2. **PreBoard is an on-demand launchd job**, so the CPU-rendered Setup Assistant never starts:
   ```
   /System/Library/LaunchDaemons/com.apple.preboardservice_v2.plist
       MachServices com.apple.preboardservice_v2, /usr/libexec/PreboardService -v 2, user mobile
       -> NO RunAtLoad
   ```
   A rewritten cache with `RunAtLoad = true` on both PreBoard jobs is committed at
   `docs/evidence/launchd/launchd-modified.plist` (887,340 bytes, round-trips through plistlib).

## 7. State and next step

1. **Done (2026-11-14): the rewritten cache is delivered and launchd runs it.**  The cache rides in the GPU
   addon (`scripts/make-launchd-cache-addon.py`), `patches/xnu/zzz-launchd-cache.patch` (last entry of
   `patches/xnu/series`) unions `/private/var/ngpu/xpc` over `/System/Library/xpc`, and the boot argument
   `launchd_unsecure_cache=1` makes the OS boot task `launchd_cache_loader` skip the detached-signature
   check.  That check is real: without the argument the kernel validates the mapped pages against
   `launchd.plist.sig`, kills the loader and the boot panics (boot lc1, 2026-11-14; the disassembly and the
   panic are in `docs/evidence/2026-11-14-launchd-cache-overlay.md`).
2. **Done: PreBoard starts.**  Both jobs spawn and stay running -
   `Successfully spawned PreboardService[50]` and `[68]` - together with `SpringBoard[27]`,
   `backboardd[58]` and `chronod[97]`.  `launchd_log_serial=1` is the boot argument that puts these lines
   on the console.
3. **Open: the screen still shows the boot logo.**  Every UI process is parked in a wait: SpringBoard in
   `_semaphore_wait_continue` (13 of 13 threads waiting), backboardd in `_ipc_mqueue_receive_continue`
   (20 of 20), launchd asleep.  The render server has no GPU to composite with (blocker 1 of section 6).
   `boot-ui-trace=1` prints a `PROC[i]` row per slot every two seconds; the slots follow
   `boot-ui-focus`/2/3/4 and must use the process *name* (`PreboardService`, not `PreBoard`).
   The Data seed is not the blocker: it carries no `com.apple.purplebuddy` preferences and
   `setup-skip=0` keeps the kernel from writing them, so the Setup state is untouched.
4. **Next: the GPU stage** (`docs/porting/gpu`): Adreno650 GMU plus the AdrenoMetal plugin in the addon
   (`scripts/make-gpu-addon.py --publish`, framework `build-adreno-metal-plugin.py` /
   `build-turnip-darwin.py`), which is what lets backboardd composite instead of parking.

## 7b. GPU stage, archived 2026-11-14 evening

The launchd-cache work is done (section 7).  The GPU stage is under way: the A650 driver archive is
linked into the kernel (sha 3a450574...), the reference port's iOS 17 GPU userspace ships inside the
one addon together with the launchd cache, and a boot proves the whole delivery chain:

    GPU: addon files=10 written=0 failed=0; extensions stage=5 status=0; accelerator=0 open=0 service=0

So /System/Library/Extensions shows NativeAdrenoMetal.bundle and the owner trust module is loaded
(3 external modules).  What is missing is the board glue that powers the GPU, starts GMU/ZAP/CP and
calls native_adreno_publish() + sm8250_metal_register(); the six reference files and their sizes are
in docs/evidence/2026-11-14-gpu-port-plan.md.  The hard prerequisite there is the two firmware
images a650_gmu.bin and a650_sqe.fw: proprietary-files.txt records their SHA256, no copy is on disk
and no build step embeds them yet.

## 8. Gotchas learned the hard way

- The XNU patch series is idempotent: a patch that is already applied is skipped, and a **modified** patch then
  fails to apply.  When a patch changes, reverse the old one first (the work tree is
  `out/<device>/<os>/<config>/tree/.cache/xnu-work`) or reset that tree.
- `git apply --stat` on these patches reports "corrupt patch" without `--recount`; the build tolerates it, so
  judge by the build log, not by `--stat`.
- COM14 is exclusive: kill a stale `capture_eu.ps1` job before starting another.
- `package/` exists in both the framework repo and the device repo, and the build compiles the **tree** copy under
  `out/.../tree/`; a change that only lands in one of them silently does nothing.
- The verifier `check_var_expected()` must run after the Data volume is mounted (tick 20), not earlier.
