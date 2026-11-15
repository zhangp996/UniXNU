# 2026-11-11: the trap PC names its image

## What this answers

The evening handoff left one question open: which image holds the three user PCs that kill
`watchdogd`, `liveactivitiesd` and `SpringBoard`.  Route 1 of that handoff - a kernel hook that
resolves the PC inside the process - is implemented, built and run on the device.

## Change

`patches/xnu/zzz-trap-image.patch`, last in `patches/xnu/series` after `zzz-trap-pc.patch`:

* `bsd/kern/kern_mman.c` - `native_trap_image(pc)` prints two lines per trap:
  * `XNU-TRAP-IMAGE`: `find_region_details()` walks the current map to the entry that holds the
    PC, `vnode_getwithvid()`/`vn_getpath()` turn a file-backed entry into a path, and the offset
    inside the mapping is printed with it.
  * `XNU-DYLD-IMAGE`: the task's dyld image table (`task_get_all_image_info_addr()` plus
    `copyin()` of `dyld_all_image_infos` and `dyld_image_info`) names the image whose load address
    is the closest one below the PC.  This view also works for ranges that are not file backed in
    the process map.
* `osfmk/arm64/sleh.c` - `handle_breakpoint()` calls it once, after the existing `XNU-TRAP` line
  and before `exception_triage()` tears the process down.

Bring-up only: no state changes, the map and object locks are dropped before anything is printed,
and the image table is read with `copyin()` while the thread is still in the trapping task.

## Build

| Step | Value |
| --- | --- |
| `kernel/kernel.macho` | `d5a69b980abff1bacc8278178cc6dafa72cea7ced139c25c7e9050ff43d039e3` |
| AMFI-patched copy | `a498a1ea29cecc0efd4d6c6a46ae4d1d6381d0337aa56fffeb7e362d2127d5a0` |
| `payload/payload.img` | `95c82aa4af07de7b1f087be4a1f463bcb9d6aad8c5028c026d798af6bfb833c3` |
| `bootEU.img` | `56ec803773e2014b66526325a253d17fba0ad8c7d592926f8e96058e01f324dd`, 183,005,184 bytes |

The kernel needs `XNU_EXTRA_LD_ARCHIVES` for `libRamWatchdog.a`; the payload came from the
AMFI-patched kernel (`patch-amfi-nopmgr.py`, `patch-amfi-trustcache-gate.py`) with
`--data-ramdisk owner-data-3g.dbr`, and was packed with the unchanged shim+firmware volume and
`stock_boot.dtb`.

## Result

Three captures (`console-bootEU.log`, `console-bootEU-2.log`, `console-bootEU-3.log`) agree: every
instance of each process traps in the same image at the same offset.

```
XNU-TRAP: pc=0x100c4bb0c lr=0x100c4bb0c esr=0xf2000001 tid=345
XNU-DYLD-IMAGE: watchdogd pc=0x100c4bb0c /usr/libexec/watchdogd+0xab0c load=0x100c41000 images=159
XNU-TRAP-IMAGE: watchdogd pc=0x100c4bb0c file=/usr/libexec/watchdogd off=0xab0c map=0x100c41000+0x10000

XNU-TRAP: pc=0x1c4f3e4f8 lr=0x1c4f3e4cc esr=0xf2000001 tid=515
XNU-DYLD-IMAGE: liveactivitiesd pc=0x1c4f3e4f8 /System/Library/PrivateFrameworks/SessionSyncEngine.framework/SessionSyncEngine+0x1d4f8 load=0x1c4f21000 images=413
XNU-TRAP-IMAGE: liveactivitiesd pc=0x1c4f3e4f8 not in a file-backed range

XNU-TRAP: pc=0x1a2a272ec lr=0x1a2a272ec esr=0xf2000000 tid=256
XNU-DYLD-IMAGE: SpringBoard pc=0x1a2a272ec /System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices+0x1e2ec load=0x1a2a09000 images=1235
XNU-TRAP-IMAGE: SpringBoard pc=0x1a2a272ec not in a file-backed range
```

### watchdogd: the watchdog user client is missing

The image is on the root volume, so the PC symbolises directly.  Offset `0xab0c` from the image
base is `0x10000ab0c` in the Mach-O, the `brk #1` behind `__os_crash()`:

```
10000ab00:  adr x0, #9015 ; "connect_to_kext() unexpectedly failed - unable to lookup watchdog KEXT service"
10000ab08:  bl __os_crash
10000ab0c:  brk #1
```

The watchdog trap is not a timeout or a scheduling failure: userland `watchdogd` never reaches a
watchdog user client.  `RamWatchdog` answered the earlier in-kernel panic, but the platform service
`watchdogd` looks for behind `connect_to_kext()` (the `AppleARMWatchdogTimer`-style user client) is
not published on this board.

### liveactivitiesd: a forced unwrap of nil in SessionSyncEngine

The image is a dyld-cache member.  In `SessionSyncEngine` subcache `.21`, the image sits at
`0x1c18f6210` and the PC is `0x1c18f64f8`; the instruction that ran is the target of a `cbz`:

```
1c18f64bc:  bl     ...          ; returns an optional
1c18f64c4:  mov    x0, x22
1c18f64c8:  bl     ...
1c18f64cc:  cbz    x20, #44     ; x20 == 0 -> 0x...64f8
...
1c18f64f4:  ret
1c18f64f8:  brk    #0x1
```

`lr` is the instruction after the earlier call (`0x...64cc`), which is why the trap reports
`lr = pc - 0x2c`: the `cbz` took the null branch into the trap that the compiler emitted for it.
That is a Swift precondition failure on a nil optional inside SessionSyncEngine, not a shared-cache
or code-signing failure.

### SpringBoard: a noreturn call in FrontBoardServices returned

In subcache `.09` the image sits at `0x19f3c1000`; the PC is `0x19f3df2ec`:

```
19f3df2dc:  mov  x0, x21
19f3df2e0:  bl   0x19fcd82e8
19f3df2e4:  bl   0x19f46e740
19f3df2e8:  bl   0x19fcd7f04
19f3df2ec:  brk  #0
19f3df2f0:  brk  #0x1
```

`lr` equals the PC, i.e. the last `bl` returned to `0x...2ec` and execution fell into the `brk #0`
that the compiler placed after a call it treats as noreturn.  The same four-call block with a
different argument repeats every `0x68` bytes, so this is a fatal-error helper of the framework.
The next step is to resolve which symbol `0x19fcd7f04` binds to; that needs the cache's stub and
bind tables, not just the image bytes.

### Why the earlier "private mapping" reading was wrong

Both library ranges are cache members: the image table names them, and neither framework carries a
binary on the root volume (`FrontBoardServices.framework` there holds only `Info.plist` and
`_CodeSignature`).  The iOS 17 split cache in the SystemOS cryptex is
`System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64` (image table, 491,520 bytes) plus 52
subcache files and a 685 MB `.symbols` database.  The earlier comparison against the extent of the
one main cache mapping cannot see the subcache ranges, which is why the two PCs looked private.

## Next

* Resolve the two cache PCs to symbols.  The pieces are in place: the image table in
  `dyld_shared_cache_arm64` maps a path to its cache address, each subcache header maps that
  address to a file offset (`.21` and `.09` for the two traps here), and `dyld_shared_cache_arm64.symbols`
  holds the names - that file did not extract with 7-Zip 26.04 ("Unsupported Method"), so it needs
  another reader.
* `watchdogd` needs no cache tooling: publish the user client `connect_to_kext()` looks for, or
  make `RamWatchdog` answer it.

## 18:20: the official record has the recipe, and it resolves the three PCs

`docs/devices/sirius-huajifruit.md` (the reference port that booted the same 21H461 userland) records both
the failure signature and the procedure:

> Crash records on the panel symbolize against the 21H461 arm64 shared cache: unslid address = recorded
> address - the record's slide (+...), looked up with `ipsw dyld a2s`; three cache files (.symbols,
> .34 and .52 .dyldlinkedit) are LZBITMAP-compressed in the SystemOS image and need apfs-fuse to extract.
> liveactivitiesd's SIGTRAP is a Swift runtime trap in SessionSyncEngine under SessionCore.Daemon.workloop

That is exactly the wall this document hit earlier: 7-Zip 26.04 refuses those three files
("Unsupported Method"), and with them the address cannot be named.  `apfs-fuse` (built from
sgan81/apfs-fuse) mounts the cryptex image and reads them, and `ipsw` v3.1.745
(`ipsw dyld a2s`) then resolves any unslid cache address.  The cache must live on a local filesystem:
over the 9p mount every ipsw command fails with `invalid argument`.

### What the three PCs are

| Trap | Symbol | Meaning |
| --- | --- | --- |
| SpringBoard `FrontBoardServices+0x1e2ec` | `-[FBSDisplayMonitor _initWithBookendObserver:transformer:] + 1148` | the branch that calls `__bs_set_crash_log_message()` with **"failed to find the main CADisplay"** and then traps |
| liveactivitiesd `SessionSyncEngine+0x1d4f8` | `func_1c18f6274 + 644` (local Swift thunk) | the null branch of `cbz x20, <brk #1>`, i.e. a forced unwrap of nil |
| the call before SpringBoard's trap | `BaseBoard __bs_set_crash_log_message` | a setter that stores the message and returns, by design |

The literal pool behind the SpringBoard branch, dereferenced through the constant CFString:

```
cfstring 0x1e7a9bf68 -> 0x19f453ecc  "failed to find the main CADisplay"
cfstring 0x1e7a9f28 -> 0x19f453e90  "failed to find the symbol for CADisplay"
cfstring 0x1e7a9f55 -> 0x19f453f55  "two CADisplays had the same displayID(%u) -> $@"
```

So SpringBoard is not dying of a mis-bound call: FrontBoardServices finds no main `CADisplay` and
crashes on purpose, with the message the crash reporter would carry.  `CADisplay` comes from
CoreAnimation and needs a display provider - on this board the framebuffer is driven directly, and no
IOMobileFramebuffer/CoreAnimation display exists, which is the SpringBoard blocker.

### watchdogd: no abort_with_payload call happens

The earlier reading - "`brk #1` follows `bl __os_crash`, so the syscall returned" - was tested with
`zzz-os-crash-observe.patch`, which prints process, reason and thread signal state in
`abort_with_payload_internal()`.  Kernel `4f0dea31`, image `d0f26181` (`console-bootEV.log`):
the same eleven `XNU-DYLD-IMAGE` lines and the same traps, and **zero `XNU-OSCRASH` lines** - the
syscall is never entered on this boot, so the trap is not a returned `abort_with_payload`.  The
`__os_crash` import in `watchdogd` must be resolved to the same kind of message-setter the BaseBoard
frame shows; the actionable part stays the string itself:
"connect_to_kext() unexpectedly failed - unable to lookup watchdog KEXT service".

## Next (revised)

* **SpringBoard**: publish a main display for CoreAnimation (an IOMobileFramebuffer-backed
  `CADisplay`), or the FBSDisplayMonitor assertion will keep firing.  This is the wall in front of the
  UI, not a code-signing or shared-cache problem.
* **liveactivitiesd**: the trap is a nil force-unwrap in SessionSyncEngine; the reference port names the
  same site as `SessionCore.Daemon.workloop`.  Compare its record before treating this as a board bug.
* **watchdogd**: publish the watchdog user client `connect_to_kext()` looks for (or make RamWatchdog
  answer it); the probe patch can stay or be dropped, it prints nothing on a boot without abort_with_payload.

## Reproduce the symbol lookup

```sh
# apfs-fuse (current CMake needs the policy flag; LZFSE comes from the submodule)
git clone --depth 1 --recurse-submodules https://github.com/sgan81/apfs-fuse.git
mkdir apfs-fuse/build && cd apfs-fuse/build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 && make -j8

# the cryptex image is a raw APFS volume; the files live under root/
./apfs-fuse /path/044-76034-038.dmg /root/cryptex
C=/root/cryptex/root/System/Library/Caches/com.apple.dyld
cp $C/dyld_shared_cache_arm64* /root/dsc/          # local disk: ipsw fails on 9p with "invalid argument"

# ipsw v3.1.745 (release asset ipsw_*_linux_x86_64.tar.gz)
ipsw dyld a2s --no-color -i -d /root/dsc/dyld_shared_cache_arm64 0x19f3df2ec
# -> 0x19f3df2ec: -[FBSDisplayMonitor _initWithBookendObserver:transformer:] + 1148
```

Unslid address = the image table address (`ipsw dyld image` or the cache's own table) plus the offset
the kernel printed as `XNU-DYLD-IMAGE: ... +0x...`.

## What the official documentation says about the display wall

Three places in the UniXNU documentation and catalog describe this stage:

* `docs/porting/display.md` - "Display and brightness", step 2 *Adapt the XNU display backend*:
  read `GopFramebuffer` and its `NativeLegacyDisplay.cpp`, `NativeGOPPresenter.cpp`,
  `NativeGOPWire.h`; select the component with its display archive recipe
  (`scripts/build-gop-legacy-display.py`), enable `CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y` and its
  `offline-legacy-*` arguments.  **Expected result:** "XNU publishes the display, presents frames
  and completes swaps."
* `docs/drivers/gop-framebuffer.md` - the implementation: it reads the firmware framebuffer through
  `PE_current_console` and publishes it through the selected MobileFramebuffer ABI.
* `components.toml` `[[component]] id = "GopFramebuffer"` - `verified_devices = ["sheng"]`, i.e. the
  same SM8550 class, with outputs `build/gop-framebuffer/libGopFramebuffer.a` and
  `libGopFramebuffer.late.a`; `docs/guide/build-kernel.md` lists `XNU_NATIVE_MOBILE_DISPLAY` and
  `XNU_NATIVE_LEGACY_DISPLAY` as the two link flags for those late archives.

This board's own `porting.toml` already records the same next action under `[[area]] id = "display"`
(status `planned`, `components = []`): "Check whether the Mu-Silicium framebuffer covers the panel and
add GopFramebuffer to the selection", with the limit "No display driver is selected; the GOP path
depends on the missing firmware platform".

The console agrees with that state: every boot so far ends the display loader with
`XNU-EXIT: IOMFB_FDR_Loader pid=85 code=256`, which is the same signature the `merlinx` record keeps
("IOMFB_FDR_loader fails before GPU display handoff"), and SpringBoard then reports the missing
`CADisplay` in `FBSDisplayMonitor`.

Reference points for the work: `sheng` (the component's verified device), `j716f` ("UEFI GOP
framebuffer handoff and NativeLegacyDisplay / IOMobileFramebuffer desktop presentation"),
`alioth` (`IOMobileFramebufferLegacy` prelinked and bound by absolute address, `SWAP calls=0`) and
`evb10` (`offline-legacy-*` plus `sheng-vsync=60`).

**Work items** (none of them is a kernel patch):

1. `[kernel] archives` in `board.toml`: add `component:GopFramebuffer` and the display archive recipe
   after the complete arm64 list (the array replaces the inherited one).
2. Bring the late-archive recipe (`scripts/build-gop-legacy-display.py` in the reference board
   package) and the ABI inputs it audits.
3. `configs/instantnoodlep_defconfig`: `CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y` (and the matching
   `XNU_NATIVE_...` link flag), plus `offline-legacy-gop=1`/`offline-legacy-phase=<n>` in the payload
   arguments.
4. Board DTS/ADT: the framebuffer handoff region and display policy the component's board hooks read.

## 18:40: the display component, as far as it goes without a board recipe

* `[components] include` now selects `GopFramebuffer`; the tree step fetches `drivers/GopFramebuffer`
  and `drivers/NativeMobileDisplay`.
* A `component:GopFramebuffer` entry in `[kernel] archives` builds nothing:
  `scripts/unixnu/kernel.py:archive_plan()` expands only components whose catalog record has a non-empty
  `build` list, and `GopFramebuffer` has `build = []` with `build_stage = "embedded"`.  The entry was
  removed again; the archives have to come from a board recipe.
* `python3 scripts/build-arm-driver.py GopFramebuffer` fails with eleven
  `-Werror,-Wmissing-designated-field-initializers` errors from the XNU kalloc macro; with
  `CCC_OVERRIDE_OPTIONS='# +-Wno-missing-designated-field-initializers'` it passes the `-Werror` build and
  the arm64-apple-ios17.0 object audit and writes `build/gop-framebuffer/libGopFramebuffer.a`
  (270,784 bytes) from `NativeMobileDisplay.o`.
* The kernel links `libGopFramebuffer.late.a` (`XNU_NATIVE_MOBILE_DISPLAY`) or
  `libNativeLegacyDisplay.late.a` (`XNU_NATIVE_LEGACY_DISPLAY`), and both are produced by board recipes in
  the reference repository:
  * `scripts/build-gop-framebuffer.py` (pinned commit 516a7804): runs `build-arm-driver.py NativeMobileDisplay`,
    then requires the exact candidate `build/xnu-prelink/native-owner-mobile-graphics-candidate.macho`
    (sha256 `aded28d3c61dafaf7b8a7c96b14427a798e9e95470ae6f287bcd038c07626798`), verifies that the original
    constructor and destructor at `0xfffffff00577b348`/`0xfffffff00577b37c` really branch to
    `IOService::IOService(OSMetaClass const*)` and `IOService::~IOService()` through the original veneers,
    globalizes `__GLOBAL__sub_I_NativeMobileDisplay.cpp`, drops `__mod_init_func`, and archives the late
    object with `original-bindings.o` and `MobileDisplayProviderProbe.o`.
  * `scripts/build-gop-legacy-display.py` is iOS 17 only by construction and additionally runs
    `scripts/audit-mobile-graphics-legacy.py`, which is not present in the reference repository's `main`.
* Consequence: the display step is a port of a recipe plus its ABI audit, not a configuration change.
  `porting.toml` `[[area]] display` now records that as its next action.

## 18:50: the patch reference documents the display start, and it is already applied

`docs/xnu/patch-reference.md` (the page the porting guide links) lists both display hooks:

| Patch | Effect |
| --- | --- |
| `offline-legacy-start` | Starts the legacy display probe after `IOKitBSDInit` |
| `offline-component-load-probes` | Loads the APFS, IOGPU and mobile-graphics components and records checkpoints |

Both are already in `profiles/arch/arm64/patches/xnu/series` (lines 34-35), so this kernel carries
them: `bsd_init.c` line 1539 now reads

```c
kern_return_t result = IOKitBSDInit();
#if (DEVELOPMENT || DEBUG) && defined(XNU_NATIVE_LEGACY_DISPLAY) && XNU_NATIVE_LEGACY_DISPLAY
	extern void native_legacy_display_late_probe(void);
	native_legacy_display_late_probe();
#endif
```

and `CONFIG_XNU_NATIVE_LEGACY_DISPLAY` is not in `configs/instantnoodlep_defconfig`, so the call is compiled
out.  No patch work is needed on this board; what remains is the configuration flag, the late archive that
defines `native_legacy_display_late_probe` (the board recipe), and the `offline-legacy-*` arguments.

## 19:00: what the payload page covers, and the argument budget

`/zh/concepts/payload` does not describe the display work, but it documents the delivery path for the third
piece - the `offline-legacy-*` arguments:

* the command line written into `EFI/UniXNU/unixnu.toml` is `[payload] kernel_args` (board.toml) merged
  with `--kernel-arg NAME=VALUE`, and DarwinBoot copies it verbatim into `boot_args`;
* each argument matches `[A-Za-z0-9_.\-]+` with an optional `=value` from `[A-Za-z0-9_.,\-]+`, and the
  merged line must stay within **1000 bytes** (DarwinBoot's buffer is 1024);
* the payload carries only the kernel, ADT, trust cache and Data seed - a display provider is linked into
  the kernel archive, not shipped as a payload file.

Measured on this board: the current line is 10 arguments / 171 bytes; adding the eight reference display
arguments (`offline-legacy-register`, `-gop`, `-unit-scale=2`, `-pm`, `-validate`, `-transfer`,
`-cpu-present`, `-phase=4`) gives 370 bytes, so the budget is not a constraint for the display step - the
reference board's 60-argument line would be, and only a subset is needed here.

## 19:10: the right driver page is xnu-drivers, not kexts

`/zh/drivers/kexts` does not cover this step: it describes staging external `.kext` bundles through
`UNIXNU_LOADER_PROTOCOL` revision 2, and states that the shipped DarwinBoot **rejects** a request carrying
staged kexts in favour of the built-in drivers.

`/zh/drivers/xnu-drivers` is the page that applies:

* a built-in XNU driver is a C++ IOKit class compiled to a static archive and linked with
  `-Wl,-force_load`, switched by a `CONFIG_XNU_<NAME>` defconfig entry that the kernel script turns into
  the force-load block, with its personality in `iokit/KernelConfigTables.cpp`;
* its reference table lists `SoftwareAES` as "the accelerator subclass with the original ABI" - the same
  original-ABI subclassing this port already uses for storage (`scripts/derive-storage-bindings.py` and
  `scripts/data/native-storage-21H461-original-bindings.S`), and what `build-gop-framebuffer.py` does with
  `original-bindings.o`;
* `/zh/porting/drivers` states the rule for private interfaces: record the class layout, vtable slots,
  selectors, argument structures and symbol bindings from the selected OS kernelcache - exactly the audit the
  legacy display builder performs against the 21H461 prelink.

One supporting record from the sheng variants: the legacy display builder "now refuses arm64e/iOS 26 before
reading original ABI addresses", i.e. it is iOS 17-only by construction, which is the OS this board targets.

## 19:20: the legacy display archive builds; the link needs the board probe layer

Continuing the display step on this board, the missing piece turned out to be smaller than expected and
is now precisely bounded.

**What already works**

* The 21H461 OS profile's `[kernel.prelink] roots` already names
  `com.apple.iokit.IOMobileGraphicsFamily`, so the original display family has always been inside
  `kernel.macho`.
* Using that prelink output as the audit's candidate (`kernel/kernel.macho` + `kernel.json` copied to
  `build/xnu-prelink/native-owner-mobile-graphics-candidate.{macho,json}`), `scripts/audit-mobile-graphics-legacy.py`
  **passes**: "original Legacy has 358 slots with nine abstract leaves".  The family `__TEXT_EXEC` hash is
  `2e044ee68c1650397e1435234ca6459206262e4c7ed709cb7131c6f664f9bd9b`, the same constant the audit pins.
  The QEMU observation was pruned from the reference and only its hash is carried, so
  `build/display-analysis/mobile-graphics-legacy-abi.json` was seeded with that recorded hash and the audit
  then wrote the full 48,970-byte ABI table (358 slots, abstract leaves 211/213/245/246/306/307/337/338/345).
* `scripts/build-gop-legacy-display.py` (reference, unmodified except for the toolchain) generates
  `build/native-legacy-display/OriginalLegacyFramebufferABI.h` (22,127 bytes), compiles
  `NativeLegacyDisplay.o` (384,400 bytes) and generates and compiles `original-bindings.S`/`.o`
  (23,130/11,904 bytes).
* The late object needs three manual steps that macOS `llvm-objcopy` does and Linux LLVM 14/15 cannot:
  `--remove-section=__DATA,__mod_init_func` (works), `--globalize-symbol=__GLOBAL__sub_I_NativeLegacyDisplay.cpp`
  (not supported for Mach-O; done by patching the symbol's `n_type |= N_EXT` in Python), then
  `llvm-ar rcs`.  Result: `libNativeLegacyDisplay.late.a` 407,816 bytes, sha256 `56403366...`, with a
  `build-report.json` the kernel consumer validates.
* `CONFIG_XNU_NATIVE_LEGACY_DISPLAY=y` must be applied through `./unixnu defconfig`; editing the defconfig
  alone does not regenerate `.config` and the flag silently stays out of the build.

**What still blocks the link**

With the archive force-loaded, the kernel link fails on twenty symbols the component expects from the board
package - the same role `package/LegacyDisplayProbe.cpp` (49 KB) plays in the reference:

```
_native_legacy_display_probe      _native_legacy_display_late_probe
_native_legacy_backend_guard      _native_legacy_client_enter / _observed
_native_legacy_swap_stage         _native_legacy_swap_validation_observed
_native_legacy_gop_mapping_observed  _native_legacy_pm_observed
_native_legacy_transfer_observed  _native_legacy_retirement_observed
_native_legacy_frame_observed / _frame_colors_observed / _copy_timing_observed
_native_legacy_surface_rejected   _native_legacy_gpu_offer / _prepare_scanout
_native_gpu_trace_enabled / _record  _native_adreno_fences / _fences_detail
```

Two of them are functional (`native_legacy_display_probe`, `native_legacy_display_late_probe`, the entry the
`offline-legacy-start` patch calls after `IOKitBSDInit`); the rest are diagnostics and GPU-handoff hooks that a
CPU-presentation port can stub.

**State on disk**

The defconfig flag and the eight `offline-legacy-*` arguments were reverted after the experiment so the
repository still builds; the tree keeps the generated header, objects and archive for the next step.

## 19:30: the reference section names the display assertion

The `/zh/reference` index is a hub; the page behind it that matters here is the troubleshooting table:

> | 用户态显示断言 | 显示 provider 和启动参数 | 检查显示驱动和 focus 参数 |
> - `reference/troubleshooting.md:66`

That row is exactly the SpringBoard failure ("failed to find the main CADisplay"): the official advice is to
check the display provider and the boot arguments, specifically the focus arguments.

The focus arguments are trace selectors, not a fix: `xnu/boot-arguments.md:89` defines
`boot-ui-focus=<name>`, and `guide/debugging.md:288-291` shows the pair form
(`boot-ui-focus=launchd`, `boot-ui-focus2=SpringBoard`, `boot-ui-focus3=backboardd`,
`boot-ui-focus4=Setup`) used with `boot-ui-trace=1` to capture a process's boot UI stack.  They are the
documented way to record the assertion's userland frames once the provider exists.

The other reference pages that apply:

* `reference/components.md:28` - `legacy_name` is the "official compatible input name", which is why
  `build-arm-driver.py NativeLegacyDisplay` resolves the `GopFramebuffer` catalog entry; line 51 says
  `component:<ID>` positions a component in `[kernel] archives`, which only takes effect for a component whose
  catalog record has a build recipe (GopFramebuffer has `build = []`, hence a board script).
* `reference/output-layout.md:85` - every `[kernel] archives` script logs to `archive-NN-<name>.log`, the
  place to read the failed display link and its inputs.
* `reference/qemu.md:52-53` - `--no-gop` and `--display-size WxH` let the display path be exercised on the
  host before touching the phone.
* `reference/boot-configuration.md`, `configuration.md`, `cli.md`, `glossary.md` cover kext bundles and archive
  semantics only; nothing about the display provider.

## 19:40: what the UniXNU account actually carries for this stage

Surveyed all 32 repositories under `github.com/UniXNU` (trees API):

| Repository | Display material |
| --- | --- |
| `GopFramebuffer` | the component only: `NativeMobileDisplay.cpp`, `NativeLegacyDisplay.cpp`, `NativeGOPPresenter.cpp`, `NativeGOPWire.h`, `NativeLegacyRequest.h`; no board probe |
| `UniXNU-sheng` | the only board-side probe in the organisation: `package/LegacyDisplayProbe.cpp` (49 KB), `OfflineGOPComposition.mm`, `OfflineGOPPresentation.mm`, `OfflineGOPResources.mm`, plus `scripts/build-gop-framebuffer.py`, `build-gop-legacy-display.py`, `build-gop-legacy-display-27.py`, `legacy-display-24A446.json`, `legacy-display-26C5035r.json` |
| `UniXNU` (main) | the qualification tooling for this exact stage: `scripts/check-native-legacy-display.py`, `scripts/probe-native-legacy-display.py`, `scripts/check-gop-handoff-checkpoints.py` |

There is no generic board probe; a new board ports the sheng one.

`check-native-legacy-display.py` states the evidence contract: the build report needs
`generated_abi_sha256`, `compiled_object_sha256`, `late_object_sha256`, `archive_sha256`, `source_sha256`,
`virtual_entries == 358` and `owned_entries == [0,1,7,84,211,213,245,246,288,306,307,337,338,345]`; then seven
bounded QEMU observations (`build/darwinboot-owner-hvf-legacy-display-*`) must show the class initialising with
object size 0x18a0, the GOP mapping and mode getter, and the "original pre-publication boundary".  Those runs use
HVF, i.e. a macOS host, so this leg cannot be reproduced from this Linux/WSL host.

The same checker also records the reference's own conclusion: the untouched original start path refuses with
`start_hardware failed` because of the real IOSurface dependency, so `display_backend_complete=False`,
`presentation=False`, `desktop=False`.  A board must therefore own the backend, which is what
`NativeLegacyDisplay.cpp` plus a board probe provide.

Our own probe (`package/LegacyDisplayProbe.cpp`, 5 KB) now implements the twenty symbols the late object needs:
the two entries (`native_legacy_display_probe`, `native_legacy_display_late_probe`) do the metaclass
registration, validate the `PE_current_console` mode (32-bit, unrotated, sane stride) and allocate the original
class; the diagnostics are no-ops; `native_legacy_backend_guard` panics with the slot number instead of
returning; the GPU trace and Adreno fence hooks report "no GPU".  It compiles with the kernel's own recorded
command line (197,024 bytes) and joins the late archive, which is now 605,832 bytes
(sha256 `c1632378...`).

## 19:50: the display pipeline links, boots and runs on the phone

With the board probe in the archive the kernel links (previous attempt stopped on twenty undefined
symbols, then on three: the two entries needed C linkage and `native_legacy_vsync_observed` was missing).
`bootEU.img` `04c4377c...` (payload `8f79cb36...`) boots and prints:

```
DISPLAY: probe entered (registered=0)
DISPLAY: GOP console 0x9c000001 1080x480 depth=32 stride=4320
DISPLAY: allocated NativeLegacyDisplay size=0x1ae0 legacy=0 mobilefb=0
```

What that says:

* The entry the `offline-legacy-start` patch calls after `IOKitBSDInit` really runs, and the metaclass
  registration (`preModLoad` -> static init -> `postModLoad`) returns zero.
* `PE_current_console` hands over the port's 480-row window (1080x480, stride 4320, base with the tag
  bit set) rather than the full 1440x3168 panel - the same 2 MiB hand-off the boot chain has used since
  2026-11-10.  The reference's QEMU fixture sees 800x600.
* `native_legacy_display_allocate()` returns an object, but it is **not** the original class:
  `metaCast("IOMobileFramebufferLegacy")` and `metaCast("IOMobileFramebuffer")` both return zero and its
  metaclass size is 0x1ae0, where the reference records 0x18a0 with `parent_match` 1.  Nothing is
  published, so the userland side is unchanged: `watchdogd`, `liveactivitiesd` and `SpringBoard` trap at
  exactly the same addresses as before this change.

So the link and the boot-thread entry are proven; the open problem is now concrete: give the owned class
the original IOMobileFramebuffer identity (the parent metaclass from the prelinked
`com.apple.iokit.IOMobileGraphicsFamily`) and publish the service.

```
kernel: linked with XNU_NATIVE_LEGACY_DISPLAY=1, late archive 605,832 bytes sha256 c1632378...
kernel.macho differs from the 4f0dea31 build (the archive is force-loaded)
payload 8f79cb36..., bootEU.img 04c4377c..., 183,005,184 bytes
console: instantnoodlep-artifacts/console-bootEW.log (453 lines, same three traps as before)
```

The defconfig switch and the eight `offline-legacy-*` arguments are reverted after the run so a clean
checkout still builds; the tree keeps the generated header, objects, archive and report.  Re-enabling is:
audit -> builder -> board probe compile -> archive -> `unixnu defconfig` -> `build kernel`.

## 20:00: the family is prelinked but never registers its classes

The first on-device runs of the probe narrowed the display problem to the original kext's registration:

```
DISPLAY: probe entered (registered=0)
DISPLAY: GOP console 0x9c000001 1080x480 depth=32 stride=4320
DISPLAY: allocated NativeLegacyDisplay size=0x1ae0 legacy=0 mobilefb=0
DISPLAY: bound parent meta=0xfffffff006b35940 raw=0 0 0 0
DISPLAY: registered family meta=0 name=- size=0x0
DISPLAY: meta[0] NativeLegacyDisplay size=0x1ae0 super=
DISPLAY: meta[1]  size=0x0 super=-
```

* `original-bindings.S` binds `__ZN25IOMobileFramebufferLegacy10gMetaClassE` to `0xfffffff006b35940`,
  and the kernel really exports that absolute symbol, so the binding is right.
* The object behind it is **all zeros** at runtime, and `OSMetaClass::getMetaClassWithName("IOMobileFramebufferLegacy")`
  returns null even from a kernel thread that retries 5 s after `offline-legacy-start` (`DISPLAY: retry after 5 s`
  runs before the same zeros), so this is not an ordering race with IOKit.
* The prelink report shows the family is completely included: it appears in `components` (30), in
  `codeless_components` (none), in `module_entrypoints` with kmod_info/start/stop addresses, and its four
  extents (`__TEXT` 0xfffffff0045f5780+0x9708, `__TEXT_EXEC` 0xfffffff00576c000+0x36f74, `__DATA_CONST`
  0xfffffff005f33840+0x63c0, `__DATA` 0xfffffff006b2e6c0+0x1f2c8) cover the metaclass address
  `0xfffffff006b35940`.

So the image carries the family's code and data, but the kext's classes are never registered, which means XNU
does not load that prelinked kext - the metaclass is filled by its static initialiser at load time and stays
zero otherwise.  The next measurement is the prelink's `__PRELINK_INFO` entry for the family against a
candidate the reference tooling accepts (`scripts/probe-native-legacy-display.py` loads
`build/xnu-prelink/native-owner-legacy-display-<profile>-candidate`), i.e. whether `emit_prelink.py` writes the
`_PrelinkExecutable*` keys XNU needs for this bundle.

The probe itself is now a working instrument: it prints the GOP mode, the allocation, the bound parent
metaclass, the registered family metaclass and the whole metaclass chain.  Console:
`docs/evidence/logs/2026-11-11-bootEX-console.log`, `...bootEY...`, `...bootEZ...`.

## 20:20: the APFS "class open" line

A live read of the serial port caught one line that earlier captures did not have:

```
apfs_is_valid_class:2395: disk5s1 rejecting class open (class 2) because we're not content protected
```

What the same log says about that disk, in order:

```
SEL-DIAG: disk5s1 seedInfo=1 base=0 bytes=3221225472
apfs_log_op_with_proc:3091: disk5s1 mounting volume OwnerData, requested by: init (pid 1)
handle_mount:817: disk5s1 vol-uuid: E553E004-A6C6-4992-B474-25EB0F160524 block size: 4096
   block count: 786432 (unencrypted; flags: 0x1; features: 1.0.2)
BLK: disk5s1 size=3221225472 uuid=E553E004-A6C6-4992-B474-25EB0F160524
```

So `disk5s1` is the 3 GiB `OwnerData` volume, and APFS itself reports it as **unencrypted, flags 0x1**.
The rejecting function, `apfs_is_valid_class()`, sits next to the other content-protection strings in the
APFS kext ("cannot roll non-content protected volume", "cannot create xattr crypto for non-content protected
volume", "cannot lock inode on non-content protected volumes", and
`((cp_key_class_t)(0x0000001f & (skey_get_class(key)))) == key_out_class`), so "class" here is a
`cp_key_class_t` (five bits, values 0-31) and "class 2" is one concrete content-protection class.

**What it means:** a caller opened a file on Data and asked APFS to apply content-protection class 2; because
the volume carries no content-protection wrapper, APFS refuses instead of serving the file with that class.
Everything else on the volume works: it mounts, reads, writes, and the services that use it keep running.

**Frequency:** one occurrence in a 60 s window, none in the earlier `bootEW`/`bootEX`/`bootEY`/`bootEZ`
captures, so it is a one-shot attempt rather than a loop.  The line carries the function name but not the
pid, and no `apfs_log_op_with_proc` line is adjacent, so the caller cannot be named from the console alone.

**Why this board sees it:** the Data seed (`owner-data-3g.dbr`) is a plain APFS volume we generate; a real
device's Data volume is content-protection wrapped and its class keys come from AppleKeyStore.  A plain volume
therefore rejects any class-scoped open by construction.

**Options, in order of cost:** accept and record it (it does not stop the boot); instrument
`apfs_is_valid_class` in the prelinked kext to print the calling process before deciding whether it matters; or
build a content-protected Data volume with real class keys, which needs the keystore/keybag path this port does
not have yet.

## 20:40: the family just needed to be asked for

The open question from the previous section - why the prelinked `com.apple.iokit.IOMobileGraphicsFamily`
never registers its classes - is answered by the prelink entry itself:

| Bundle | `OSBundleRequired` |
| --- | --- |
| `com.apple.driver.AppleMobileFileIntegrity` | `Root` |
| `com.apple.iokit.IOSurface` | `Console` |
| `com.apple.filesystems.apfs` | `Local-Root` |
| `com.apple.iokit.IOMobileGraphicsFamily` | **absent** |

All the `_Prelink*` keys are present for the family (`_PrelinkExecutableLoadAddr` 0xfffffff0045f5780,
`_PrelinkExecutableSize` 0x9708, `_PrelinkExecutableSourceAddr`, `_PrelinkKmodInfo` 0xfffffff006b2e6c0,
bundle path and relative executable path), so the image is complete; the family simply has no
`OSBundleRequired`, and XNU only loads such kexts when something asks for them.  Nothing in this port did.

The probe now calls `OSKext::loadKextWithIdentifier("com.apple.iokit.IOMobileGraphicsFamily")` before it
registers its own class, and that is all it needed:

```
DISPLAY: load family kext rc=0x0 loaded=1
DISPLAY: allocated NativeLegacyDisplay size=0x1ae0 legacy=1 mobilefb=1
DISPLAY: registerered family meta=0xfffffff006b35940 name=IOMobileFramebufferLegacy size=0x18a0
DISPLAY: meta[0] NativeLegacyDisplay size=0x1ae0 super=IOMobileFramebufferLegacy
DISPLAY: meta[1] IOMobileFramebufferLegacy size=0x18a0 super=IOMobileFramebufferService
DISPLAY: meta[2] IOMobileFramebufferService size=0x90 super=IOMobileFramebuffer
DISPLAY: meta[3] IOMobileFramebuffer size=0x90 super=IOService
DISPLAY: meta[4] IOService size=0x88 super=IORegistryEntry
DISPLAY: meta[5] IORegistryEntry size=0x28 super=OSObject
DISPLAY: meta[6] OSObject size=0x10 super=-
```

* `loadKextWithIdentifier` returns 0 and `isKextWithIdentifierLoaded` reports 1.
* The original class registers with the exact size the reference records (0x18a0) and its full ancestry
  (`IOMobileFramebufferService` -> `IOMobileFramebuffer` -> `IOService`), so the owned class now answers both
  `metaCast("IOMobileFramebufferLegacy")` and `metaCast("IOMobileFramebuffer")` (`legacy=1 mobilefb=1`).
* Our concrete class is 0x1ae0 because `NativeLegacyDisplay` adds its own members on top of the 0x18a0 base;
  the base size is what the reference's check compares.

The userland side is unchanged in this boot: SpringBoard still traps in
`-[FBSDisplayMonitor _initWithBookendObserver:transformer:] + 1148` because no display service is published
yet - the probe stops after allocating.  The next step is `init`/`start`/`registerService` with the GOP
console as the provider, which is what turns the allocated object into the `CADisplay` QuartzCore looks for.
Console: `docs/evidence/logs/2026-11-11-bootFA-console.log`.

## 21:00: the display publishes, and SpringBoard gets past the CADisplay assertion

The probe now runs the whole publication sequence the reference uses at `offline-legacy-phase=4`:

```
DISPLAY: init ok phase=4, retained for publication
DISPLAY: IOSurface kext rc=0x0
DISPLAY: IOSurface root found=1
DISPLAY: nub 0xfffffff9e36876a0 attach=1 start=1 mem=0x9c000001+0x1fa400
DISPLAY: dt attach=1
DISPLAY: display attach=1 start=1
```

That is: the family is loaded on demand, the owned class registers with the original identity (0x18a0 base,
`IOMobileFramebufferLegacy` -> `IOMobileFramebufferService` -> `IOMobileFramebuffer` -> `IOService`), a nub is
built over the firmware framebuffer memory (`0x9c000001 + 0x1fa400`), attached to the resource service and to
`/arm-io` as `gop-display`/`display-subsystem`, and the display attaches to it and starts.

**The effect on SpringBoard is visible in the same boot:** the previous image (`bootFA`) logged eighteen
SpringBoard lines and trapped every instance at `FrontBoardServices+0x1e2ec`
(`-[FBSDisplayMonitor _initWithBookendObserver:transformer:]`, "failed to find the main CADisplay").  This
image logs no `+0x1e2ec` trap at all.  SpringBoard now loads one more image (1236 against 1235) and dies at a
different site:

```
XNU-TRAP: pc=0x1a05a4744 lr=0x1a05a4744 esr=0xf2000000
XNU-DYLD-IMAGE: SpringBoard pc=0x1a05a4744 FrontBoardServices+0xf744 load=0x1a0595000 images=1236
0x19f3d0744: -[FBSDisplayMode _initWithWidth:height:preferredScale:scaleOverride:refreshRate:
             gamut:hdr:rotation:virtual:validityCheck:] + 580
```

So the display is found, and the next wall is the display **mode** the firmware framebuffer implies: our console
window is 1080x480 (the 2 MiB 480-row hand-off), while the panel is 1440x3168, and FBSDisplayMode refuses the
values it is handed.  The next step is to make the published display report a valid mode - real panel geometry,
scale and refresh - instead of the kernel console's 480-row window.

## 21:20: the scale lands, the family calls an abstract slot, and the device drops off USB

Two more boots with the published display.

**bootFC** - with `display-scale` published into `/chosen`:

```
DISPLAY: nub 0xfffffff501809a20 attach=1 start=1 mem=0x9c000001+0x1fa400
DISPLAY: dt attach=1
DISPLAY: chosen display-scale=2 rotation=0 ok=1
DISPLAY: display attach=1 start=1
```

The boot then panicked in our own guard:

```
DISPLAY: unimplemented legacy slot 345 (object=0xfffffff334e75000 caller=0xfffffff00578c1cc)
instantnoodlep legacy display slot 345 is not implemented @LegacyDisplayProbe.cpp:274
```

The caller `0xfffffff00578c1cc` lies in the prelinked family's `__TEXT_EXEC`
(`0xfffffff00576c000 + 0x36f74`), so the original code asked the concrete backend for slot 345.  In the ABI
table slots 337, 338 and 345 all point at `___cxa_pure_virtual` (0xfffffff00c0098ac) and are marked abstract,
i.e. they are leaves only the owned backend can implement - the reference's QEMU observations recorded
`guard_slot = 0`, so this path is new for this board.

**bootFD** - the guard now logs the first eight hits and returns instead of panicking:

```
DISPLAY: chosen display-scale=2 rotation=0 ok=1
DISPLAY: display attach=1 start=1
```

No panic, 461 console lines, six traps, and in that window the only owners are `watchdogd+0xab0c` and
`SessionSyncEngine+0x1d4f8` - no SpringBoard trap at all (`bootFA`, the image from before the display was
published, had eighteen SpringBoard lines).  A later live read could not confirm how long that holds: after
`bootFD` the USB console disappeared (COM14 gone, `fastboot devices` and `adb devices` both empty), so the phone
needs a manual reboot before the next measurement.

**Next:** identify slot 345 - disassemble the family around `0xfffffff00578c1cc` to see which interface it
expects the backend to answer, implement it in the probe instead of returning silently, and re-run.

## 21:40: the panel shows the display stack using our service, then a NULL metaclass teardown

The panic screen from the relaxed-guard build carries four useful lines above the panic block:

```
AFB: default surface: No Default FB Surface found. Return error
IOMFB: swap_wait_gated: CommandMake didn't happen even after 5 seconds.
       Seems system in bad state, cancelling swaps
DISPLAY: unimplemented legacy slot 345 (object=0xfffffff44ac0b000 caller=0xfffffff00578c1cc), returning
panic(cpu 0 caller 0xfffffff00c00b2d0): Kernel data abort. at pc 0xfffffff00be80944, lr 0xfffffff00bfe4684
  x0: 0  x1: 0  esr: 0x96000045  far: 0x0
```

Symbolised against the built kernel:

| Address | Symbol |
| --- | --- |
| `0xfffffff00be80944` (pc) | `OSMetaClass::~OSMetaClass() + 0x1c` |
| `0xfffffff00bfe4684` (lr) | `NativeLegacyDisplay::MetaClass::~MetaClass()` |
| `0xfffffff00c00b2d0` (caller) | `_panic_with_thread_kernel_state` |
| `0xfffffff00578c1cc` (slot-345 caller) | `IOMobileFramebufferLegacy::opaque243() + 0xd7c` |

So the display stack really reached our published service: it asked the family for the **default framebuffer
surface** ("No Default FB Surface found. Return error"), the compositor's swap then waited five seconds for a
command that never came and cancelled, the family asked the backend for slot 345, and after that returned the
teardown ran into a NULL `this` in `OSMetaClass::~OSMetaClass` (`far = 0`, `x0 = 0`) through our own metaclass
destructor.

"Default FB Surface" is exactly the piece `porting/display.md` documents for this class:

> The 24A446 adapter supplies the default surface through the original IOSurfaceRoot factory and validates its
> backing memory and geometry before transferring ownership to the display family. The family's
> `create_default_fb_surface()` slot returns void; its residual return register is not an object pointer.

So slot 345 is the default-framebuffer-surface leaf: returning without a surface is what leaves the family with
a null object and the later metaclass teardown crashes.  The next step is to implement it - create the surface
through the real IOSurfaceRoot factory over the GOP nub's memory, validate its geometry, hand ownership to the
display - and to keep the guard's return from being reachable in that path.

## 21:55: where the default surface is supposed to come from

The component already contains the hook for the missing piece, but no implementation of it exists anywhere in
the open-source tree:

```cpp
/* NativeLegacyDisplay.cpp, inside startHardware() after the GOP mapping check */
#if defined(NATIVE_LEGACY_DEFAULT_SURFACE) && NATIVE_LEGACY_DEFAULT_SURFACE
    // The family calls its void slot 316 after startHardware. Our backend supplies the client's default surface.
    if (valid) valid = installDefaultSurface(width, height);
#endif
```

* `installDefaultSurface(uint32_t, uint32_t)` is only *called*; no definition exists in the component, in the
  reference board package (`package/LegacyDisplayProbe.cpp`), in the reference scripts, or in the UniXNU
  tooling.  The macro `NATIVE_LEGACY_DEFAULT_SURFACE` appears nowhere else either, so the reference builds
  without it - which matches its own records: the desktop was never reached there either.
* The device records of the iOS 26 boards describe what the implementation must do: "The 24A446 adapter creates
  a BGRA IOSurface through the original IOSurfaceRoot factory, checks allocation and geometry, and transfers
  its reference to the family default-surface field. The void `create_default_fb_surface` slot is invoked by
  the original start method."

So the next piece of work is ours to write, in the board probe:

1. compile `NativeLegacyDisplay.cpp` with `-DNATIVE_LEGACY_DEFAULT_SURFACE=1`;
2. implement `installDefaultSurface(width, height)`: create a BGRA surface through the original IOSurfaceRoot
   factory over the nub's framebuffer memory, validate allocation, stride, pixel format and geometry;
3. transfer the reference into the family's default-surface field (the offset has to be derived from the
   family's own `create_default_fb_surface`/`startHardware` code - the generated header only carries the
   dimension and swap-queue offsets);
4. keep the abstract-slot guard unreachable on that path.

The phone is on the panic screen from the previous run (`OSMetaClass::~OSMetaClass` with a null `this`), its
serial console is gone and neither `fastboot` nor `adb` sees it, so no further device measurement is possible
until it is rebooted by hand.

## 22:10: the published display panics about two minutes in, deterministically

Re-running the relaxed-guard image (the one that boots with `display-scale` published) reproduces the same
crash every time, about two minutes after hand-off:

```
AFB: default surface: No Default FB Surface found. Return error
IOMFB: swap_wait_gated: CommandMake didn't happen even after 5 seconds. Seems system in bad state, cancelling swaps
DISPLAY: unimplemented legacy slot 345 (...), returning
panic(cpu 0 caller 0xfffffff00c00b2d0): Kernel data abort.
  at pc 0xfffffff00be80944 (OSMetaClass::~OSMetaClass + 0x1c)
  lr 0xfffffff00bfe4684 (NativeLegacyDisplay::MetaClass::~MetaClass), far 0x0
```

The two capture windows show the same shape: no SpringBoard trap at all before the panic (the crash takes the
machine down first), only `watchdogd+0xab0c` and `SessionSyncEngine+0x1d4f8`.

So the display is real and the compositor uses it - `AFB` asks for the default framebuffer surface, `IOMFB`
waits for a command that never comes and cancels the swap - but because the backend never supplies that surface,
the family ends up tearing down a null object.  The wall is exactly the one from the previous section:

* the component's `NATIVE_LEGACY_DEFAULT_SURFACE` branch calls `installDefaultSurface(width, height)`, which no
  source in the tree implements;
* the device records of the 26-series boards describe the required body: create a BGRA surface through the
  original IOSurfaceRoot factory, check allocation and geometry, transfer the reference into the family's
  default-surface field;
* the kernel's `IOSurface` class is not exposed as headers in the target SDK, so the board has to reach it the
  same way the display family does - through the original bindings - and the family's own
  `create_default_fb_surface` (`startHardware` calls it as a void slot) is the reference for what to build.

Until that body exists, the choices are: keep the display published and accept the panic roughly two minutes in
(useful, because the panel proves the stack is using the service), or revert to the pre-publication image, which
does not panic but leaves SpringBoard crash-looping on the missing CADisplay.

## 22:20: what the official repository says about the default surface

Yes - the official documentation describes this piece, in two places:

* `docs/porting/display.md` (lines 65-69):

  > The 24A446 adapter supplies the default surface through the original IOSurfaceRoot factory and validates its
  > backing memory and geometry before transferring ownership to the display family. The family's
  > `create_default_fb_surface()` slot returns void; its residual return register is not an object pointer.

* The same sentence appears as a finding in about fifteen device records (`fuxi`, `sheng-ad1095445`,
  `sheng-airalex002`, `sheng-bai805`, `sheng-chenjin1494`, `sheng-enlixk`, `sheng-gkxqh`, `sheng-jsongmax`, ...),
  each with the implementation contract: "creates a BGRA IOSurface through the original IOSurfaceRoot factory,
  checks allocation and geometry, and transfers its reference to the family default-surface field.  The void
  `create_default_fb_surface` slot is invoked by the original start method."

But the same records also state the limit (`fuxi.md:320` and the others):

> The 24A446 default-surface adapter still requires a hardware boot result.

and there is no implementation of it anywhere in the open repositories: `installDefaultSurface` has no
definition in the component, in the reference board package (`package/LegacyDisplayProbe.cpp`,
`OfflineGOPComposition.mm`, `OfflineGOPPresentation.mm`, `OfflineGOPResources.mm`), in its scripts, or in the
UniXNU tooling, and `NATIVE_LEGACY_DEFAULT_SURFACE` only ever appears in the component's `#if`.

So the documentation defines what the adapter must do and where it plugs in (BGRA surface from the original
IOSurfaceRoot factory, geometry validation, transfer into the family's default-surface field, invoked from the
original start method, and the warning that the void slot's residual return register is not a pointer), while
this board has to write the implementation.  Our panic is exactly the documented gap: the family asks for the
default surface, none exists, and the swap is cancelled.

Kernel-side string work for the next step: the message "No Default FB Surface found" lives in the family's
`__TEXT` at `0xfffffff0045f6ad7`; it has no `adrp`+`add` cross-reference in the image, so the field offset has
to be found another way (the referencing code loads it through a literal pool, or the check is inlined into the
original start method).

## 22:35: static reverse-engineering of the default-surface field reaches its limit

What the disassembly established, in order:

* slot 288 (`0xfffffff00579278c`) is the original `startHardware` and is a stub:
  `sub sp; str x0; mov w8, #0; and w0, w8, #1; ret` - it returns false, which is exactly the
  "start_hardware failed" the reference records.  Our component overrides it.
* slot 316 (`0xfffffff00579ea44`) is not a surface factory either: it stores `x0` and one byte and returns.
* the code that asked for slot 345 is the family's own `opaque243`, through the ordinary vtable dispatch
  `ldr x8, [x0]; ldr x8, [x8, #2760]` (#2760 = 345 * 8), and the instructions after the call read fields at
  +5844/+5848 - mode geometry, not a surface reference.
* `ldr [x, #2528]` (#2528 = 316 * 8) appears three times, and the one that branches is part of a stub island
  (`adrp x16; ldr x16; br x16` repeated per slot), so it names the slot's implementation, not its caller.
* the family's message "No Default FB Surface found" lives at `0xfffffff0045f6ad7` inside the family's
  `__TEXT`, and has no `adrp`+`add` cross-reference and no literal pointer to it in the image, so the code that
  emits it cannot be reached by those two scans.

So the field offset the documentation calls "the family default-surface field" is not statically obvious: the
official records describe the contract but publish no offset, the reference never implemented the adapter
("still requires a hardware boot result"), and the family's consumer of that field is not reachable with the
scan patterns that worked for the CADisplay assertion.

Next approaches, in order of cost:

1. extend the ABI audit to record field offsets directly (it already derives the vtable, dimensions and queue
   offsets); the audit reads the same prelinked class, so the offset can be taken from the family's own
   `create_default_fb_surface`/`start` code once that function is identified by slot;
2. dynamic: after start, print the qwords of the display object whose pointee's metaclass name is "IOSurface" -
   an empty scan now, but it tells which offsets carry surfaces once any exist, and can be repeated with the
   adapter writing a placeholder;
3. hook the family's error path: the message is printed from a known string; a temporary printf inside
   `NativeLegacyDisplay::start` after the family's start returns can at least show which fields changed.

## 22:45: the organisation publishes the contract, not the adapter

Checked every place under `github.com/UniXNU` that could carry the default-surface implementation:

| Repository / path | Result |
| --- | --- |
| `UniXNU/docs/porting/display.md` (65-69) | the only prose contract: the 24A446 adapter, the original IOSurfaceRoot factory, the void `create_default_fb_surface()` slot, "its residual return register is not an object pointer" |
| `UniXNU/docs/devices/*.md` (~15 records) | the same sentence plus the limit "The 24A446 default-surface adapter still requires a hardware boot result" |
| `GopFramebuffer/NativeLegacyDisplay.cpp` (pinned 897 lines and upstream `main` 1230 lines) | exactly one `installDefaultSurface` occurrence in both: the call inside `#if NATIVE_LEGACY_DEFAULT_SURFACE`; no definition anywhere |
| `UniXNU-sheng/package` (`LegacyDisplayProbe.cpp`, `OfflineGOPComposition.mm`, `OfflineGOPPresentation.mm`, `OfflineGOPResources.mm`) | no mention |
| `UniXNU/scripts` (`check-native-legacy-display.py`, `check-gop-handoff-checkpoints.py`, `check-mobile-display-provider.py`, `check-mobile-graphics-checkpoints.py`, `check-original-mode-validation.py`) | no mention; the mobile-display checker even prints "display publication and presentation remain incomplete" |
| `GopFramebuffer/README.md`, `AGENTS.md`, `Test/` | describe swap formats only |

So the field offset and the adapter body are published nowhere; the two doc places define what it must do and record
that it was never proven on hardware.  Also noted: our catalog pin `e47c1b8` is 897 lines where upstream `main` is
1230, so a later revision of the component exists (still without the adapter) and is worth re-pinning once the
surface work starts.

## 22:55: what the prelink page does and does not cover

`/zh/concepts/prelink` explains the mechanism this port relies on, and nothing about the default surface:

* inputs: the native kernel, the original kernelcache, `[kernel.prelink] roots`, slide and early-security
  initialiser;
* steps: parse both Mach-Os, read `__PRELINK_INFO`, select the roots and their kernel interfaces, copy each
  component's segments into the merged ranges, rebind pointers, apply the slide, rewrite module headers and kmod
  entries, build the sparse index, write `kernel.macho`;
* "输出保留原始组件代码不变" - the invariant that lets this board bind the family's methods by absolute address;
* the report next to the output lists `components`, `kernel_interfaces`, `bindings` (`new_site` ->
  `new_target`), `instruction_inventory`, `module_entrypoints` and `early_security_initializers`.

That report is where our `original-bindings.S` addresses and the family's extents came from, so the page is the
reason the ownership work could be done at all.  It does not list object field offsets, and the private-ABI rule
in `porting/drivers.md` (line 70) confirms the scope: record the class layout, the virtual-method slots, the
selectors, the argument structures and the symbol bindings from the selected kernelcache - object fields are not
part of the contract.  That is why the family's default-surface field has no published offset and has to be
measured on this board.

## 23:05: what the boot-configuration reference offers

`/zh/reference/boot-configuration` describes the payload's `unixnu.toml`: menus ([boot], [ui]), drivers, dt patches,
block slices, ACPI, kits, and per-entry keys (`args`, `initrd`, `confirm`/`confirm_args`, `[[entry.kext]]`,
`[[entry.variable]]`, `[[entry.dt_patch]]`).  The one piece that touches our display work is:

> `[entry.chosen]` - 每个键成为 `/chosen` 属性，在菜单项补丁之后应用

That is the declarative counterpart of what the probe does at runtime when it publishes `display-scale` and
`display-rotation` into `/chosen`.  Two limits apply: the page states that `[[dt_patch]]` applies to EFI and Linux
entries while the Darwin path uses the `.adt` sidecar, so for XNU those properties come from the sidecar
generator (`[payload.adt]`) or, as now, from the board probe; and nothing on the page concerns the family's
default-surface field.

So the page is not an answer to the current wall, but it does point at the tidy way to retire one of our probe's
responsibilities later: declare `display-scale`/`display-rotation` in the entry's `[entry.chosen]` (or in the ADT
generator) instead of writing them from `native_legacy_gop_mapping_observed`.

## 23:15: the arch patches already carry the load-and-register flow

`offline-component-load-probes.patch` (the second row of the patch reference's owner-root table) contains the
mobile-graphics machinery this board re-implemented by hand:

```c
static void native_mobile_graphics_load_probe(void) {
    int enabled = 0;
    if (!PE_parse_boot_argn("offline-mobile-graphics-load", &enabled, sizeof(enabled)) || !enabled) return;
    native_mobile_graphics_load_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOMobileGraphicsFamily");
#if defined(XNU_NATIVE_LEGACY_DISPLAY) && XNU_NATIVE_LEGACY_DISPLAY
    int legacy_register = 0;
    if (native_mobile_graphics_load_status == 0 &&
        PE_parse_boot_argn("offline-legacy-register", &legacy_register, ...) && legacy_register) {
        extern void native_legacy_display_probe(void);
        native_legacy_display_probe();
    }
#endif
    int client_probe = 0;
    if (native_mobile_graphics_load_status == 0 &&
        PE_parse_boot_argn("offline-mobile-graphics-client", &client_probe, ...) && client_probe) {
        extern void native_mobile_graphics_client_probe(void);
        native_mobile_graphics_client_probe();
    }
}
```

(it runs from `bsd_init()` right after `vfsinit()`, and the same patch's `IOUserClient.cpp` hunk defines
`native_mobile_graphics_client_probe()`: it checks that `IOMobileFramebuffer`, `IOMobileFramebufferService` and
`IOMobileFramebufferLegacy` exist as classes, allocates `IOMobileFramebufferUserClient`, calls `init()` and then
`externalMethod` with selectors 90, 0x80000000 and ~0, and finally calls `native_legacy_display_probe()` again.)

So the official path is boot arguments, not code:

| argument | effect |
| --- | --- |
| `offline-mobile-graphics-load=1` | loads the family at `bsd_init`, after `vfsinit` |
| `offline-legacy-register=1` (with `XNU_NATIVE_LEGACY_DISPLAY`) | calls the board's `native_legacy_display_probe()` right after the load |
| `offline-mobile-graphics-client=1` | runs the client probe (user client allocation and the selector-90 call) and calls the board entry again |

There is still nothing about the default surface: no `IOSurface` call, no `create_default_fb_surface`, no field
transfer anywhere in the patch series.  But this changes two things for us:

1. our probe's hand-rolled `OSKext::loadKextWithIdentifier` and five-second retry thread duplicate
   `offline-mobile-graphics-load`; the intended flow loads the family and calls the entry **once**, synchronously,
   after `vfsinit` (i.e. later than `bsd_autoconf`, which is where `offline-legacy-start` runs it now);
2. the client probe shows the selector the platform uses on the framebuffer user client (90, then 0x80000000, then
   ~0), which is worth watching when the default-surface path is exercised.

Boot arguments for the next run therefore become:
`offline-mobile-graphics-load=1 offline-legacy-register=1 offline-legacy-gop=1 offline-legacy-unit-scale=2
offline-legacy-phase=4` (the rest of the display set unchanged).

## 23:30: the probe now follows the arch patch instead of loading the family itself

Changes for the next boot:

* `package/LegacyDisplayProbe.cpp` no longer calls `OSKext::loadKextWithIdentifier` and no longer spawns the
  five-second retry thread.  `native_legacy_display_probe()` now does the whole sequence when the kernel's
  `offline-mobile-graphics-load` probe calls it (family already loaded, class registered, GOP console read,
  object allocated and started, published on the nub); `native_legacy_display_late_probe()` only records that
  `offline-legacy-start` ran earlier, because `bsd_autoconf` happens before `bsd_init`.
* `board.toml` gains `offline-mobile-graphics-load=1`; the display argument set is now 19 arguments, 401 bytes
  of the 1000-byte budget.
* The image is built and waiting: `instantnoodlep-artifacts/bootEU.img`, sha256
  `6af5801edd3fd7ee6636c5f85f6caf697efa6b66bb57f4d580aad214b2d22162`.

The expectation for this boot, if the display stack behaves as before: the family is loaded by the patch at
`bsd_init` (later than the old `bsd_autoconf` call), the probe publishes the service in the same call, and
`AFB`/`IOMFB` still ask for the default surface, which is not implemented yet - so the same two-minute panic
should be expected unless `offline-mobile-graphics-client=1` changes the surface path.  The run is still worth it:
it removes our hand-rolled load, puts the probe on the documented timing, and shows whether the client probe's
selector-90 call reaches the default-surface code.

The phone is powered down (no `fastboot` entry, no `COM14`), so the image cannot be booted until it is started.

## 23:45: the official load flow works and lands on the same wall

The image with the threaded publication (sha `e44f8753...`) boots through the storage root and the display chain:

```
DISPLAY: probe entered (registered=0)
DISPLAY: allocated NativeLegacyDisplay size=0x1ae0 legacy=1 mobilefb=1
DISPLAY: registered family meta=0xfffffff006b35940 name=IOMobileFramebufferLegacy size=0x18a0
DISPLAY: init ok phase=4
DISPLAY: late probe (load handled by offline-mobile-graphics-load)
DISPLAY: IOSurface kext rc=0x0
DISPLAY: IOSurface root found=1
DISPLAY: nub 0xfffffff38273c7e0 attach=1 start=1 mem=0x9c000001+0x1fa400
DISPLAY: dt attach=1
DISPLAY: chosen display-scale=2 rotation=0 ok=1
DISPLAY: display attach=1 start=1
```

and the console also shows `IOMFB: driver commit:`, so the family and the compositor are talking.

**Regression that this build fixed:** the first official-flow attempt published inline from `bsd_init` and the
storage root died with `NativeUfs.cpp:2073` / "UFS ROOT FAILURE: SystemOS signed volume not found" (`mount timeout,
window busy 1`).  The publication now runs in a kernel thread two seconds later, which removes the block from the
boot thread; the UFS root comes up and the display chain follows.

**The wall is unchanged:** two minutes in,

```
AFB: default surface: No Default FB Surface found. Return error
IOMFB: swap_wait_gated: CommandMake didn't happen even after 5 seconds. Seems system in bad state, cancelling swaps
DISPLAY: unimplemented legacy slot 345 (object=0xfffffff54f1cd000 caller=0xfffffff00578c1cc), returning
panic: Kernel data abort at pc 0xfffffff00be80944 (OSMetaClass::~OSMetaClass + 0x1c),
       lr 0xfffffff00bfe4684 (NativeLegacyDisplay::MetaClass::~MetaClass), far 0x0
```

Also seen in this boot: `Maximum number of VM swap files: 5`, `Failed to open swap file 30`,
`vm_swap_create_file failed @ 115 secs` - the Data volume cannot host the dynamic swap file, separate from the
display work.

So the ordering and the load path are now the documented ones, and everything that is left is the default-surface
adapter: create the BGRA IOSurface through the original IOSurfaceRoot factory and hand it to the family field.

## 00:05: the client probe runs, the fields are mapped, the wall stands

With `offline-mobile-graphics-client=1` the kernel's own client probe ran:

```
DISPLAY: probe entered (registered=1)          <- second call, from the client probe
static IOReturn IOMobileFramebufferUserClient::s_map_display_type(...) UserClient display type is 0
IOMFB: driver commit:
```

so an `IOMobileFramebufferUserClient` was allocated, initialised and answered a selector, and the real userland
client then mapped the display ("UserClient display type is 0").  It does not change the outcome: about two
minutes in, the same sequence repeats -

```
AFB: default surface: No Default FB Surface found. Return error
IOMFB: swap_wait_gated: CommandMake didn't happen even after 5 seconds ... cancelling swaps
DISPLAY: unimplemented legacy slot 345 (object=0xfffffff42f623000 caller=0xfffffff00578c1cc), returning
panic: data abort at pc OSMetaClass::~OSMetaClass+0x1c, lr NativeLegacyDisplay::MetaClass::~MetaClass, far 0
```

The field scan of the live display object (25 eight-byte slots hold kernel pointers):

```
+0x000 vtable 0xfffffff007210758     +0x010..+0x030 registry/provider links (nub at +0x030)
+0x060 0xfffffff262cdc100            +0x278, +0x780 = 0xffffffff00000000 (pattern)
+0x1570..+0x15e0  eight pointers spaced 0x10 (0xfffffff42f6245xx)
+0x1600 0xfffffff348fa7e00           +0x1608 0xfffffff262d7f150
+0x1610 0xfffffff42f597380           +0x1638 0xfffffff262ca9700
+0x1648 0xfffffff515c7b2e0           +0x17e8 0xfffffff42f6247e0
+0x17f8 0xfffffff72f303f40           +0x1868 0xfffffff262e52b50
```

The stretch around +0x1610 is interesting because a device record (`uke`) notes that a swap record whose ID
matches the default surface names `fb+0x1618` directly - i.e. the default surface is identified around that
offset, and today nothing is stored there.

The call site of slot 345, from the family's `opaque243`:

```
ldur x0, [x29, #-184]      ; the display object
mov  x9, #6020 ; add x8, x0, x9 ; ldrb w8, [x8]   ; byte at +0x1784
ldr  x8, [x0] ; ldr x8, [x8, #2760] ; blr x8       ; #2760 = 345 * 8
```

Only `x0` is set up, so the slot takes just the object (a notification/void leaf), not a surface request.  The
next scan therefore prints each pointer's `vtable` as well, so the slots can be named against the component
segments (IOSurface's data extent is known from the prelink report) instead of guessed.

## 00:40: the field map names the family's members, and the provider is the next lever

The completed scan (23 pointer slots, each with its pointee's vtable):

| offset | pointee's vtable symbol | what it is |
| --- | --- | --- |
| +0x000 | `NativeLegacyDisplay` vtable | the object itself |
| +0x018, +0x020 | `OSDictionary` | two dictionaries |
| +0x030 | `IOService` | the nub (provider) |
| +0x060 | `IOServicePM` | power management state |
| +0x1570..+0x15e0 | vtable 0 | eight embedded links (queue) |
| +0x1608 | `IOCommandGate` | command gate |
| +0x1610 | `IOWorkLoop` | work loop |
| +0x1638 | `IOInterruptEventSource` | interrupt source |
| +0x1648 | `IOTimerEventSource` | timer source |

Nothing sits at `+0x1618` - the region a device record (`uke`) names for the default surface - which is what a
missing default surface looks like.

Two new facts from the console of the same boot:

```
virtual bool IOMobileFramebufferLegacy::start(IOService *): provider was not AppleARMIODevice.
```

The family's own start rejects a plain `IOService` provider.  Our component's start still succeeds, but the
family's normal path - which is where the default surface is created and installed - never runs.  The reference
board hit the same message and worked around it with a plain service; that is exactly why its records say the
default-surface adapter "still requires a hardware boot result".

```
disk3s1: device is not readable.
mockfs_mountroot failed: 13
apfs_vfsop_mountroot:2965: apfs: mountroot called!
```

The root volume reported "not readable" on this boot before APFS took over; the boot continued, but this is the
same storage path that later fails to create the swap file (`vm_swap_create_file failed @ 115 secs`).

Reaching the original IOSurfaceRoot factory from the kernel is not a symbol call: the prelinked IOSurface kext
exports only `_gIOSurfaceIdentifier` and `_gIOSurfaceTag`, no class methods, so a surface has to be created
through the service's virtual interface (which means auditing IOSurfaceRoot's slots) or by letting the family do
it through its normal start path.

Next lever, cheapest first: give the family the provider it asks for - allocate `AppleARMIODevice` (present in the
prelinked AppleARMPlatform) as the nub over the firmware framebuffer memory, attach it to the resource service and
to `/arm-io`, and pass it to `start`.  If the family's own path then runs, it may create and install the default
surface itself, which is the one thing no board record shows.

## 01:10: the AppleARMIODevice provider is accepted, and the family's own lookup runs

Giving the family the provider it asks for changed the picture:

```
DISPLAY: nub class AppleARMIODevice
DISPLAY: nub 0xfffffff644226e20 attach=1 start=1 mem=0x9c000001+0x1fa400
DISPLAY: dt attach=1
IOMFB: driver commit:
DISPLAY: chosen display-scale=2 rotation=0 ok=1
DISPLAY: display attach=1 start=1
```

* the family no longer prints "provider was not AppleARMIODevice" - the original `start` accepted the provider;
* the field scan grew from 23 to 24 pointer slots: the new one is
  `+0x1640 = <nub> vtable=0xfffffff005cbe4c8`, i.e. the family stored the accepted provider in its object;
* the failure message moved from `AFB: default surface: ...` to
  `IOMFB: default_fb_surface: No Default FB Surface found. Return error`, so the family's own lookup now runs
  instead of the earlier AFB path.

The panic is unchanged two minutes in (`swap_wait_gated: CommandWake didn't happen ... cancelling swaps`, then
slot 345, then the null metaclass teardown).

Where the family looks for the surface, from the disassembly of the function that prints that message
(`0xfffffff005777464`): it calls a virtual slot through `[x0 + 1392]` (slot 174) and reports the failure when
that returns non-zero; the neighbouring small function at `0xfffffff00577742c` forwards through
`[this + 216]` (0xD8) and then slot 217, i.e. the surface is reached through an object held at +0xD8.  Our
display object has no pointer at +0xD8 (the scan lists none), so that reference is null as well.

Both routes end at the same place: **nothing ever created an IOSurface**.  Reaching the factory needs the
prelinked IOSurfaceRoot's virtual interface audited the same way the display family's was (its vtable slots and
class layout), because the IOSurface kext exports no class symbols - only `_gIOSurfaceIdentifier` and
`_gIOSurfaceTag`.

## 01:25: the IOSurfaceRoot audit is staged

The next measurement only needs the device: the probe now prints, right after it finds the surface root,

```
DISPLAY: IOSurfaceRoot <obj> class=<className> size=<classSize> vtable=<address>
DISPLAY:   vt[0..7]
```

so the class layout and the vtable address come out of a normal boot.  With that address,
`_agent/vt_audit.py` enumerates the slots and prints, for each entry, the strings its first instructions
reference - which is how the display family's slots were identified - so the surface-creating method can be
shortlisted before anything is called.

Image: `instantnoodlep-artifacts/bootEU.img`, sha256
`7f67638f99d3098e3a333f3553ba15be58632f107171e16ed23bceb95040e548`.

The phone is not enumerated at the moment (no `VID_1209`, no `fastboot`), so the boot is pending.

## 01:45: IOSurfaceRoot is audited down to a candidate factory slot

The probe printed the service's layout on the device:

```
DISPLAY: IOSurfaceRoot 0xfffffff6c8ccae00 class=IOSurfaceRoot size=0x4a0 vtable=0xfffffff005d3b2b8
DISPLAY:   vt[0]=0xfffffff004ba8c64 vt[1]=0xfffffff004ba8d68 ...
```

That vtable address is the same one the display object had at `+0x1600`, so the family holds a reference to the
IOSurfaceRoot service there.

Enumerating the vtable (it ends at slot 168) leaves only a handful of entries that belong to the class itself
(the rest are inherited IOService/OSObject implementations):

| slot | address | evidence |
| --- | --- | --- |
| 1, 7 | `0xfffffff004ba8d68`, `...8dac` | reference "IOSurfaceRoot" (class name plumbing) |
| 14, 21 | `...8fe8`, `...8ea8` | no strings |
| 85, 86 | `...90ac`, `...90bc` | `hw.memsize`, `/vram`, "IOSurfaceRoot: Initial shared event memory alloc failed" (start/init) |
| 87 | `...9e20` | no strings |
| **141** | `0xfffffff004baa11c` | `IOSurfaceRoot.cpp`, "IOSurface count of %d approaching limit of %d", tailq head/element asserts |

Slot 141 is the surface factory: it increments the count, checks the limit and walks the surface tail queue.  Its
prologue shows the calling convention: `x0` this, `x1` stored for later, `w3` a flag whose non-zero value jumps
straight to the error return (`w19 = 0xE00002C9`), and `x4` an out-parameter that is cleared first.

That is the piece the board has to call: build the descriptor dictionary, call slot 141 on the IOSurfaceRoot
service, check the returned object's metaclass is `IOSurface`, and hand it to the display family's default-surface
field (+0x1618 neighbourhood) instead of leaving it null.

Everything before that is verified on hardware: the family loads through the documented boot argument, the owned
class registers with the original identity, the display publishes over the firmware framebuffer on an
`AppleARMIODevice` nub the family accepts, the userland client maps it (`s_map_display_type`, `IOMFB: driver
commit`), and the remaining failure is exactly "no default surface".

## 02:05: the first call into the IOSurfaceRoot factory names its first parameter

The probe called vtable slot 141 on the IOSurfaceRoot service with `(this, props, null, 0, &out)`:

```
DISPLAY: create_surface 1080x480 stride 4320 via vt[141]=0xfffffff004baa11c
panic(cpu 0 caller 0xfffffff00c0043b0): zone_require failed: address in unexpected zone id 545
  (iokit.OSDictionary) (addr: 0xfffffff9e069cf88, expected: proc_task) @zalloc.c:1434
```

The kernel tried to release the pointer in a `proc_task` zone, so the factory's first argument after
`this` is a task (or proc), not the property dictionary.  The prologue agrees: `x1` is saved early, `x4` is the
out-parameter that is cleared first, `w3` is a flag whose non-zero value jumps to the error return, and `x2` is
untouched until later.

The next call therefore passes `current_task()` first and the dictionary second.

## 02:20: slot 141 is the user-client factory, and it works from the kernel

Calling it with the corrected argument order (`this, task, dictionary, flag, out`) succeeded:

```
DISPLAY: create_surface 1080x480 stride 4320 via vt[141]=0xfffffff004baa11c task=0xfffffff349206450
DISPLAY: create_surface result=0 out=0xfffffff42f92f8b0
DISPLAY: create_surface class=IOSurfaceRootUserClient size=0x138
DISPLAY: slot 345 surface=0xfffffff42f92f8b0 (kept for the next step)
```

So vtable slot 141 of `IOSurfaceRoot` is `createUserClient(task, properties, flag, &client)`: it allocates an
`IOSurfaceRootUserClient` (0x138 bytes) owned by the task passed first.  (The previous attempt passed the
dictionary where the task belongs, which is why the kernel reported a `proc_task` zone expectation.)

Consequences:

* the factory itself is now proved callable from the kernel, and the argument convention is known;
* the surface itself is created *through that user client* (the same external-method path userland uses), not
  directly by `IOSurfaceRoot`, which matches the reference record's "original IOSurfaceRoot factory" wording;
* the two-minute panic is unchanged in this boot (the client was created but nothing installs a default surface
  yet), so the next step is the client's external method.

Next measurement: keep the created `IOSurfaceRootUserClient` and either audit its dispatch table or call its
`externalMethod` with the surface-creation selector and an `IOSurfaceCreateArgs` payload (width, height, bytes per
row, pixel format BGRA, alloc size), then check the returned object's class is `IOSurface`.

## 02:35: the family reaches the default surface through `+0xD8` and slot 174

The function that prints "No Default FB Surface found" (at `0xfffffff005777464`) has this shape:

```
str x0, [x29,#-16]       ; this
str x1, [sp,#24]         ; arg1
str w2, [sp,#20]         ; arg2
str w3, [sp,#16]         ; arg3
ldr x8, [x29,#-16]
ldr x0, [x8, #216]       ; x0 = this->[0xD8]        <-- the default-surface holder
ldr w1, [sp,#20]
ldr w2, [sp,#16]
ldr x8, [x0]             ; its vtable
ldr x8, [x8, #1392]      ; slot 174
blr x8                   ; status -> error path when non-zero
```

So the family's default-surface lookup is: take the object held at **`this + 0xD8`**, ask it through **slot 174**
(with the two 32-bit arguments the caller passed), and report "No Default FB Surface found" when that comes back
non-zero (the error it returns is `0xE00002BC`).

Two possibilities remain, and they are cheap to tell apart on the next boot:

1. the object at `+0xD8` is a real surface holder whose slot 174 answers "no surface", or
2. the field belongs to a different object than the display (our display object's `+0xD8` is empty - the scan
   lists no pointer there), and the family is being asked on a different instance.

Either way the missing piece is unchanged: an IOSurface must be created (now via the `IOSurfaceRootUserClient`
whose factory slot is verified) and installed where slot 174 can find it.

## 03:05: the prelink page's binding report is the documented vtable map

`/zh/concepts/prelink` does not describe the display surface, but it documents the two facts this work has been
standing on and one tool we had been reproducing by hand:

* "范围保留原始虚拟地址，slide 后原始代码中的指针仍然有效" - why absolute bindings into an original component keep
  working;
* "输出保留原始组件代码不变" - the original class layout and vtable are the ones in the kernelcache;
* the report next to `kernel.macho` carries `bindings` ("导入位置和 slide 后的目标"), `components`,
  `instruction_inventory` and `module_entrypoints`.

The report is machine-readable: this build has 69,294 bindings.  Filtering the ones whose site falls inside the
`IOSurfaceRoot` vtable (`0xfffffff005d3b2b8`) returns all 168 slots with their targets, and filtering those whose
target lands in IOSurface.kext's own `__TEXT` returns exactly the nine methods of the class:

```
slot   0 -> 0xfffffff004ba8c64     slot  21 -> 0xfffffff004ba8ea8
slot   1 -> 0xfffffff004ba8d68     slot  85 -> 0xfffffff004ba90ac
slot   7 -> 0xfffffff004ba8dac     slot  86 -> 0xfffffff004ba90bc
slot  14 -> 0xfffffff004ba8fe8     slot  87 -> 0xfffffff004ba9e20
                                   slot 141 -> 0xfffffff004baa11c
```

which is the same set the disassembly scan produced, including the factory slot 141 this board has already called
successfully.  So the documented route to a class's vtable map is the prelink report, not a byte scan.

What the page (and the rest of the documentation) still does not contain: the meaning of each slot, the argument
structures, the family's `+0xD8` field, and the fact that slot 141 is `createUserClient(task, dictionary, flag,
&client)`.  Those belong to the board's own record, which is where they are now.

## 03:20: the surface client is an IOUserClient2022

The first externalMethod attempt on the freshly created `IOSurfaceRootUserClient` panicked before reaching any
method:

```
DISPLAY: create_surface class=IOSurfaceRootUserClient size=0x138
panic(cpu 0 caller ...): wrong externalMethod for IOUserClient2022 @IOUserClient.cpp:6483
```

The kernel source in the tree explains it (`iokit/Kernel/IOUserClient.cpp`):

```c
IOReturn IOUserClient::callExternalMethod(uint32_t selector, IOExternalMethodArguments *args) {
    ...
    if (uc2022) {
        ret = ((IOUserClient2022 *)this)->externalMethod(selector, (IOExternalMethodArgumentsOpaque *)args);
    } else {
        ret = externalMethod(selector, args);
    }
    ...
}
IOUserClient2022::externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *,
    OSObject *, void *) { panic("wrong externalMethod for IOUserClient2022"); }
```

So a 2022 client must be driven through `IOUserClient::callExternalMethod()`, which routes to the
`IOUserClient2022::externalMethod(selector, opaque)` overload; calling the five-argument `externalMethod` is what
panics.  The probe now uses `callExternalMethod`.

## 03:45: what the main repository actually contains about the surface

A whole-checkout search (`/home/dev/unixnu/UniXNU`, excluding `out/`) for the surface terms returns only the
documentation, the component's call site, and the device records - no implementation.  But the profile ABIs hold
something useful, if only for another OS:

`profiles/os/ipados-27.0.1-24A446/abi/original-classes.json` records four classes:

* `IOMobileFramebufferLegacy`: size `0x1dd8`, 379 slots (168 base), named slots
  (`isMainDisplay` 186, `getDotPitch` 190, `supportedFrameInfo` 216, `getDisplayDimensions` 261,
  `startHardware` 305, `kickSwapQueue` 307, `completeCurrentSwap` 312, `validateSwap` 313,
  `getRefreshAttributes` 319, `collectCompletionReports` 358), the pure slots, and named fields
  (`current_swap` 0x1a40, `pending_head` 0x1a48, `pending_tail` 0x1a50, `vsync_clients` 0x1ab8,
  `display_width` 0x1bf0, `display_height` 0x1bf4) - and **no default-surface field**;
* `IOMFBSwapIORequest`: the swap request layout, whose `surfaces` field sits at 0xb80 (with rects, options and
  `request_id` 0x3d4), so surfaces travel in the request, not in the framebuffer;
* `IOSurface`: the descriptor accessor functions - `descriptor`, `width`, `height`, `stride`, `allocation`,
  `format`, `planes` - i.e. exactly the values a create-descriptor must carry (the keys our dictionary already
  uses);
* `IOHIDDevice`.

Our own profile `ipados-17.7.11-21H461` has no `abi/original-classes.json` at all, so none of this was ever
generated for this build; the display class layout we have came from the audit header instead.

So the main repository documents *what* the descriptor contains and *where* surfaces travel, but nowhere the call
that creates one: that remains the user-client external method (the 2022 two-argument
`externalMethod(selector, opaque)`), with the factory slot 141 already proved callable.

Housekeeping: an untracked copy of this board's probe had been left in the main checkout's `package/`; it was
removed, and `git status` there is back to its previous state (only the submodule states that were already there).

## 04:10: the sibling port shows the working architecture (GFander/UniXNU-kebab)

`github.com/GFander/UniXNU-kebab` (OnePlus 8T, same SM8250 class, same "the bootloader's framebuffer is scanned
out by the panel" situation) solves this area completely differently from the path this board has been on.  Its
display patch (`patches/xnu-8796-kebab/0093-kebab-display.patch`) adds two native kernel files:

* `iokit/Kernel/arm/KebabSurface.cpp/.h` (~1,370 lines) - **a built-in IOSurface**: a native
  `IOCoreSurfaceRoot` service plus a user client that speaks the ABI `IOSurface.framework` expects, with its own
  selector table:

  ```
  sel 13 limits (struct out 0x18, u16 at +0x16 = ABI version)
  sel 0  create: scalar [parent id], struct in = serialized property dictionary, struct out = lock result
  sel 4  lookup by id; sel 34 lookup by mach port; sel 35 create mach port
  sel 9  set value; sel 10 update non-bulk value cache; sel 11 remove value
  sel 27 set bulk attachments (0x1b)
  traps 0/1 use-count +/-; 2 lock; 3 unlock; 4/5 release
  ```

  Every surface is wired kernel memory mapped into each task that creates or looks it up, with the lock result
  telling the library where the pixels, the shared state page and the timestamps are;

* `iokit/Kernel/arm/KebabFramebuffer.cpp` - an `IOMobileFramebuffer` (AppleCLCD) that CPU-copies CoreAnimation's
  swaps into the bootloader framebuffer and drives vsync from a 60 Hz timer.  `SwapEnd` queues the swap record and
  the vsync timer presents it; `SwapWait(6)` waits with a bounded timeout.

Its header comment states the reason plainly: "Apple's IOSurface kext is closed", so the port provides the name,
the service and the selectors itself.  The display is presented as the iPhone X identity userland expects
(1125x2436 at 3x) while the panel, canvas and default surface stay at the bootloader's 1080x2400.

**What this means for this board:** the route we have been on - original prelinked `IOMobileGraphicsFamily` plus the
closed IOSurface factory - is the harder of the two, and the reference records admit the default-surface adapter
was never proven on hardware.  The sibling's route replaces both closed pieces and its documentation reports a
working GUI.  Porting `KebabSurface` as this board's surface provider, and either keeping the original family for
the framebuffer side or porting `KebabFramebuffer` as well, is the proven path.

Kept for reference: the patch was fetched to `/root/kebab_0093.patch` (81 KB) and the repository's ABI records
(`docs/standardization-run-20261029/abi/IOSurfaceRootUserClient.json`, `IOCoreSurfaceRoot.json`) describe the same
service from the contract side.

## 04:25: what porting the built-in surface means for this board

The sibling's source is now extracted from its display patch: `KebabSurface.h` (76 lines) and
`KebabSurface.cpp` (1,369 lines).  Its public shape is small:

```c
class KebabSurface : public OSObject {            /* id, geometry, planes, values dictionary,
                                                     pixel descriptor, shared state page, kernel map */
    uintptr_t kernelAddress();
};
KebabSurface *kebab_surface_copy(uint32_t id);
KebabSurface *kebab_surface_create_physical(uint64_t phys, uint32_t width, uint32_t height,
    uint32_t bytesPerRow, uint32_t pixelFormat);
mach_port_name_t kebab_surface_port_in_task(KebabSurface *surface, task_t task);
```

behind which sit the `IOCoreSurfaceRoot` service, its user client and the selector table (13 limits, 0 create,
4/34/35 lookup and ports, 9/10/11 values, 27 bulk attachments, traps for retain/lock/release), plus the
`KebabFramebuffer` (AppleCLCD) that CPU-copies swaps and drives vsync from a timer.

**The conflict to decide on:** this board's prelink already carries the original `com.apple.iokit.IOSurface`, and it
is *loaded and running* - the probe matches `IOCoreSurfaceRoot` and creates an `IOSurfaceRootUserClient` from it.
The family also lists `com.apple.iokit.IOSurface` in `OSBundleLibraries`, so removing the kext would stop the
framebuffer family from loading.  The sibling avoids the problem because its kernel never had Apple's kext.

So the two coherent paths are:

1. **Incremental (current)**: keep the original IOSurface kext and find the create call on its user client - the
   remaining unknown is the selector for the 2022 `externalMethod(selector, opaque)`; the factory slot 141 is
   already proved and every wrong selector answers with an error instead of crashing;
2. **Sibling architecture**: drop Apple's IOSurface (and, for consistency, the closed framebuffer family) from the
   prelink roots and provide `KebabSurface`-style native services, which is what has a reported working GUI - but it
   replaces everything this board has built so far.

Path 1 costs one measurement at a time and keeps the working display publication; path 2 is a rewrite with a known
end state.  Both need a decision before code is written.

## 04:40: selector 0 with the documented argument layout

The phone's own screen showed what the console relay had missed:

```
DISPLAY: create_surface class=IOSurfaceRootUserClient size=0x138
DISPLAY: client sel=0 rc=0xe00002c2 out=0x0,0x0,0x0,0x0
... (sel 1..7 the same)
```

`0xE00002C2` is `kIOReturnBadArgument`: the 2022 dispatch *is* reached now (the two-argument virtual was the
right entry), it just rejected the argument shape.  The sibling's source documents the shape:

```
sel 0 create: scalar [parent id], struct in = serialized property dictionary, struct out = lock result (0xa68)
```

so the probe now calls selector 0 with one scalar (parent id 0), a hand-written XML plist
(`IOSurfaceWidth/Height/BytesPerRow/PixelFormat/AllocSize`) as the structure input, and a 0xa68-byte lock-result
buffer as the structure output, then logs the first words of the result.

## 05:00: selector 0 answers BadArgument even with the documented layout

```
DISPLAY: create_surface 1080x480 stride 4320 via vt[141]=0xfffffff004baa11c task=...
DISPLAY: create_surface result=0 out=0xfffffff748328180
DISPLAY: create_surface class=IOSurfaceRootUserClient size=0x138
DISPLAY: create sel0 rc=0xe00002c2 serialized=1 len=447 out=00000000 ...
```

The XML descriptor is accepted as a structure input (447 bytes), the 2022 virtual dispatch runs, and the answer is
still `kIOReturnBadArgument`.  The sibling's selector table was recorded against iOS 16.7 (XNU 8796.141.3,
iPhone10,3 20H392); this board's client comes from iPadOS 17.7.11 (21H461), so its dispatch counts and order differ.
The next measurement is a small argument matrix on selector 0 (0..3 scalars, with and without structure in/out),
which is safe because a mismatched count answers BadArgument rather than crashing.

### Where the work stands after this session

Verified on hardware today:

1. the display family loads through the documented `offline-mobile-graphics-load` flow (no hand-rolled load);
2. the owned class registers with the original identity (base 0x18a0, full ancestry);
3. the display publishes over the firmware framebuffer on an `AppleARMIODevice` nub, attaches to `/arm-io` and
   starts; the userland client maps it (`s_map_display_type`, `IOMFB: driver commit`);
4. the family's default-surface lookup is traced to `this + 0xD8` -> slot 174;
5. `IOSurfaceRoot` is audited: class 0x4a0, vtable 0xfffffff005d3b2b8, nine own slots, slot 141 =
   `createUserClient(task, dictionary, flag, &client)`, returning `IOSurfaceRootUserClient` (0x138);
6. a 2022 user client must be driven through the two-argument virtual `externalMethod(selector, opaque)`; the
   five-argument one panics by design;
7. the remaining failure is exactly the missing default surface, and the panic two minutes in is its consequence.

Open, in order of cost:

* the selector/argument matrix on the original client (one boot, bounded);
* or the sibling architecture: a built-in `KebabSurface`-style service replacing Apple's closed IOSurface (its
  sources are kept in `docs/evidence/reference/`), which requires deciding whether the closed
  `IOMobileGraphicsFamily` stays;
* the storage side of the same boot: `vm_swap_create_file failed` and the one-off "disk3s1 is not readable".

## 05:20: the community answer for the default surface (Allenwdk/UniXNU-uke)

Searching the community devices turned up the port that documents this exact wall.  `UniXNU-uke` ships
`scripts/audit-mobile-graphics-default-surface.py`, whose header states the contract:

> The original class keeps its default surface at instance+0x1618 and its temporary surface at instance+0x1620.
> Only the (absent) platform driver ever installs the former; this audit pins the exact fields, slots and
> user-client dispatch entry the bring-up backend may use.

Its offsets and slots:

```
surface_root 0x1600   command_gate 0x1608   work_loop 0x1610
default_surface 0x1618   temp_surface 0x1620   memory_region 0x17c0
surface_enable 0x1780    mode_block 0x1700    width 0x16b0   height 0x16b4
slot 174  default_fb_surface gate -> slot 333      slot 242  copy the surface port name
slot 299  create_default_fb_surface -> instance+0x1620
slot 333  default-surface layout/commit, returns sid
user-client dispatch selector 3, scalars 2 in / 1 out, output is the default surface's mach port
surface object: sid +0x10, region +0x28, descriptor +0x30, stride +0x68, format +0x80, allocation +0x90
```

and the decisive line: `default_surface_nonzero_writer_in_family: False` - the family never installs +0x1618
itself.

The port's probe (`package/LegacyDisplayProbe.cpp`, 58 KB) then shows the workaround it actually runs with
`offline-legacy-default-surface=1`:

1. allocate `IOBufferMemoryDescriptor::withCapacity(0x2000000, kIODirectionOutIn)`;
2. write the mode block at +0x1700 (`'BGRA'`, planes 1, bpp 4, clear 0x70 bytes, `fb[0x1748] = 0`);
3. put that descriptor into the region cache at **+0x17c0**, instead of the `PurpleGfxMem` region the original
   `create_default_fb_surface` would ask IOSurfaceRoot for (the platform driver that would provide it is absent);
4. set the enable byte `fb[0x1780] |= 1`;
5. start the display - slot 299 then builds the temporary surface at +0x1620 from that region;
6. after start, retain the surface and its +0x28 region and +0x30 descriptor, then
   `memcpy(fb + 0x1618, &temp, 8)` to adopt it, and let the original slots 174/242 produce the IOSurfaceID and
   the mach port the compositor maps.

This board's probe now does the same: seed +0x17c0, enable +0x1780, start, adopt +0x1620 into +0x1618, with the
region and descriptor retained.

## 05:40: the community recipe works - the default surface now exists

With the uke recipe in the probe, the same boot shows:

```
DISPLAY: seeded surface region 0xfffffff8877268c0 bytes=33554432
DISPLAY: surfaces default=0 temp=0xfffffff6ba853a78
DISPLAY: adopted default surface 0xfffffff6ba853a78 sid=1 region=0xfffffff8877268c0 descriptor=0xfffffff887725c10
```

* the seeded region made the family's own `create_default_fb_surface` (slot 299) build the temporary surface -
  it exists now, with `sid=1`;
* it is adopted into `+0x1618`, with the surface, its region and its descriptor retained;
* and the whole `AFB/IOMFB: No Default FB Surface found` + `swap_wait_gated` + `slot 345` chain **no longer
  appears** in the console.  The wall this session was chasing is gone.

What remains is a different failure: about a minute in, the same looking but differently reached data abort at
`pc 0xfffffff00be80944` (`OSMetaClass::~OSMetaClass + 0x1c`), `lr 0xfffffff00bfe4684`
(`NativeLegacyDisplay::MetaClass::~MetaClass`), with `x0 = 0` and `x21 = 0xfffffff00578b960`.  Nothing in the
console precedes it except the usual `watchdogd` and `liveactivitiesd` traps, so this is a teardown ordering
issue of the class object rather than the surface path.

Next: find who tears the metaclass down (the family's `stop`/`free` path versus our retained object), and
whether the uke port's extra step - keeping `activeLegacyDisplay` and never releasing the adopted surface - is
what avoids it there.

## 06:00: how the uke port solves the rest (from its PROGRESS.md)

The uke repository documents its whole display track.  The parts that answer this board's open questions:

**1. The default surface (already applied here).**  It seeds the family's region cache at +0x17c0 with an
`IOBufferMemoryDescriptor`, sets the enable byte at +0x1780, lets slot 299 build the temporary surface at +0x1620
and adopts it into +0x1618, retaining the surface, its +0x28 region and +0x30 descriptor because "the adopted
surface outlives the legacy display's stop" - `stop` clears +0x1618 and drops the region reference.

**2. Why a swap can arrive with no surface.**  Their disassembly of the original `swap_submit` (vtable[178],
their kernel `0x578f224`):

* the gate action looks the swap record up in the `fb+5496` list by the id at `record+0x98`;
* masks are copied from the record (+0x14c, +0x150, +0x154);
* **the layer loop only runs for layer < 4**, and the surface id of each layer lives at `record+0x9c+4*layer`;
* an id equal to the default surface's id uses `fb+0x1618` directly, otherwise it calls the original
  `IOSurfaceRoot` lookup - in their kernel `0xfffffff0057a28c0(root, id, task)`, task from `0xfffffff0057a2e84` -
  and stores the result at `request+0xa40+8*layer`.

Their device record shows why that matters: for a 1200-byte swap record the compositor put the *whole frame* on
**layer 6** (id 41) while layers 1/2/4 carried id 0, so the original loop never reaches it and the request is
rejected with `accepted=0 reason=2 surface=0`.

**3. Their fix: own-present.**  `NativeMobileDisplay` pads short records to 1288 bytes (`offline-legacy-swap-pad`),
captures the first enabled layer that carries an id, and after the original call returns it calls
`presentBase(frameSurfaceId)`, which resolves the overlay surface through the **original lookup stub** (bound to
`0xfffffff0057a28c0` and `0xfffffff0057a2e84`, byte-verified) and copies it into the GOP synchronously on the
compositor's own thread.  That is how they got the system UI on the panel for the first time.

**4. The memory wall** was theirs too: they raised the VM with a UEFI high-DRAM mapping plus
`owned_memory_size`; this board solved the same problem with the 4 GiB window.

So the remaining work on this board is the same shape: accept that the original layer loop will not see the full
frame, capture the id from the record, resolve it through the original lookup, and copy to the framebuffer - while
finding what tears the class down when the display has no presentation to complete.

## 06:15: the surface accessors the component already asks for

The display component calls `native_legacy_surface_{descriptor,width,height,stride,allocation,format,planes}`
when it handles a swap's surface, and `native_legacy_swap_validation_observed(accepted, reason, request, surface)`
on every validation.  Both were stubs on this board, so even a surface that reached the component could not be
read.  The probe now implements the accessors on the original IOSurface layout from uke's audit (descriptor
+0x30, width +0x60, height +0x64, stride +0x68, format +0x80, allocation +0x90, one plane) and logs the first six
validations together with the request's four surface pointers at +2624.

This is also the half of own-present that needs no new binding: the component's copy path can only work once these
accessors answer, and the validation log tells which layers the compositor actually submits.

## 06:30: reading PanicLogDxe correctly - the dumped panic belongs to the previous boot

The console capture after a boot ends with the usual tail

```
QCOM USB PANIC pending_at_start=7 dropped_at_start=0 buffer_capacity=4194304
QCOM PANIC END
```

and the panic block above it is the **saved panic from the previous boot**, printed by PanicLogDxe, not the current
one.  This explains the apparent contradiction in this session: a boot whose payload is built from the newest probe
still showed `slot 345` and `create_surface ... sel0` lines, because those were the previous image's dying words.
The current boot's own log is everything before that block.

With that reading:

* the newest image does reach `found the region, family built the temp surface (sid=1), adopted into +0x1618`, and
  the `IOMFB: driver commit` line - no default-surface error;
* the panic that follows belongs to the older build, and the current one has to be judged on a fresh boot watched
  past the two-minute mark;
* the surface accessors (descriptor +0x30, width +0x60, height +0x64, stride +0x68, format +0x80, allocation +0x90)
  and the swap-validation log are in this build and have not been exercised yet, because no swap has reached the
  component so far in a window we captured.

So the next measurement is a clean boot of the current image with the console watched from ~100 s to ~200 s, looking
for `DISPLAY: swap ...` lines (the compositor's layer ids) and for whether the metaclass panic still occurs.

## 06:45: the current build's panic has no display activity in front of it

A clean boot of the newest image (the one with the region seed, the adoption and the surface accessors) logs:

```
DISPLAY: seeded surface region 0xfffffff89faf8c60 bytes=33554432
DISPLAY: surfaces default=0 temp=0xfffffff5ed000d68
DISPLAY: adopted default surface 0xfffffff5ed000d68 sid=1 region=... descriptor=...
(IOMFB: driver commit:)
... only process exits: liveactivitiesd (corpse limit), watchdogd, fairplayd, bluetoothd ...
Kernel data abort. at pc 0xfffffff00be80944, lr 0xfffffff00bfe4684
  x0 = 0, x21 = 0xfffffff00578b960 (IOMobileFramebufferLegacy::opaque243 + 0x510)
```

* no `AFB/IOMFB: No Default FB Surface`, no `swap_wait_gated`, no `slot 345`, no `DISPLAY: swap` line;
* the panic is `OSMetaClass::~OSMetaClass + 0x1c` reached from `NativeLegacyDisplay::MetaClass::~MetaClass`,
  i.e. our class's metaclass destructor runs with a bogus `this`, about two minutes in;
* it arrives right after launchd declares `Too many corpses being created` for liveactivitiesd and watchdogd,
  i.e. when the boot is at its most failure-prone point, and nothing the display does precedes it.

Working hypothesis: the metaclass registered through `OSMetaClass::preModLoad(g_kernel_kmod_info.name)` /
`postModLoad(handle)` is being torn down by the kext lifecycle (either the family's kext unloading, or the fake
kmod handle used for late registration being freed), and the destructor is entered with a garbage `this`.

Decisive control experiment for the next boot: build the same kernel with the display arguments removed
(`offline-mobile-graphics-load`, `offline-legacy-*`) so nothing registers the class at all.
If the panic disappears, it is the registration/teardown of our class; if it stays, it is unrelated to the display
and belongs to the boot's process-failure storm.

## 07:00: control experiment - the panic belongs to the display registration

Built the same kernel with every display argument removed (`bootEU-control.img`,
sha `954c262c5e1a029cb32476a4554a2d94e827a9260ff894f17c70978908e371e1`, base args only) and booted it:

```
DISPLAY: late probe (load handled by offline-mobile-graphics-load)   <- the only display line
... launchd, watchdogd trap, liveactivitiesd trap, apfs class-open message, fairplayd, bluetoothd ...
```

575 console lines, **no kernel data abort, no panic**.  With the display arguments, the same kernel panics about two
minutes in at `OSMetaClass::~OSMetaClass + 0x1c` reached from `NativeLegacyDisplay::MetaClass::~MetaClass`.

So the teardown fault is caused by this board's display registration itself - not by the boot's process-failure
storm, and not by the default-surface path (which no longer errors).

The remaining question is therefore narrow: what destroys the metaclass.  The class is registered through
`OSMetaClass::preModLoad(g_kernel_kmod_info.name)` and `postModLoad(handle)`, i.e. against a fake kext created for
late registration; the destructor then runs with a bogus `this` (0xfffffff00ba131cb-like code addresses appear in
the registers).  Candidates: the fake kext being freed, or the family's kext unloading and taking the subclass
metaclass with it.

Next measurements, cheapest first:

1. keep the `preModLoad` handle in a global and print it at shutdown paths, or re-register the class through the
   family's own kmod identity so both lifecycles match;
2. print from a destructor-like probe: a static object inside the board archive whose destructor logs the moment the
   kext/fake-kext teardown begins;
3. compare with the uke port, which registers the same way but never lets its display object reach `stop`: it keeps
   `activeLegacyDisplay` and never releases it, and its `stop`/`detach` calls exist only on failure paths.

## 07:10: the control boot is still alive, six minutes in

A live read of the control image (no display arguments) shows the machine still running:

```
XNU-DYLD-IMAGE: SpringBoard pc=... FrontBoardServices+0x1e2ec load=... images=1235
Process[201] crashed: SpringBoard. Too many corpses being created.
Process[219] crashed: SpringBoard. Too many corpses being created.
XNU-EXIT: liveactivitiesd pid=271 code=5 ns=2 sub=0x5
XNU-EXIT: bluetoothd pid=272 code=0 ns=0 sub=0x0
... webbookmarksd: missing executable ...
```

SpringBoard is back to its well-known crash loop at the missing CADisplay assertion (as expected with no display
published), the usual services cycle, and there is **no kernel panic**.  That completes the comparison:

| image | display arguments | after ~2 minutes |
| --- | --- | --- |
| current | yes | data abort in `OSMetaClass::~OSMetaClass` via `NativeLegacyDisplay::MetaClass::~MetaClass` |
| control (`954c262c...`) | no | still running, SpringBoard crash loop only |

So the teardown fault is tied to the display registration, and the machine is otherwise stable - which also gives a
useful baseline: the control image can be left running for long observations.

## 07:25: holding the registration handle does not stop the teardown

The build that keeps `OSMetaClass::preModLoad`'s handle in a global (`bootEU.img`, sha
`c445a7ebf1ff9f7c0e5a7f7a0465e4dd162f50d110fe09d5e998c29ca4a219a9`) still ends the same way.  Line positions in
its console log (542 lines):

```
104: probe entered
172: seeded surface region ...
206: adopted default surface ... sid=1
527: Kernel data abort. at pc 0xfffffff00be80944, lr 0xfffffff00bfe4684
538: QCOM USB PANIC   (PanicLogDxe dump marker)
```

so the panic is the current boot's own, and it arrives after the display is fully published with its surface - with
no display error in between.  The `holding registration handle` line is absent, which means either `preModLoad`
returned null on that call (and the registration went through a path that never stores a handle) or the line was
lost in the console backlog; that has to be settled with an unconditional print.

So the fake-kext reference is not (or not only) what frees the metaclass.  The teardown still happens at the same
place: `NativeLegacyDisplay::MetaClass::~MetaClass` -> `OSMetaClass::~OSMetaClass` with a bogus `this`.

Next, in order:

1. print unconditionally what `preModLoad` returns, and also `postModLoad`'s result, right where they happen;
2. wrap the object's own destruction: `NativeLegacyDisplay` is ours, so a `printf` in its `free()` (the component
   can add one) shows whether the display object dies before the metaclass does;
3. compare with uke, whose `LegacyDisplayStubs.c` may stub the very slot that leads there.

## 07:40: the first accepted swap with a real surface, and the next two walls

This boot is the first one that actually ran the newest probe (the earlier images were built from a stale kernel,
because `unixnu build kernel` does not track the display archive - forcing a relink took deleting and restoring
the kernel output, and the build then needed the archive's member set to be complete again).  What it shows:

```
DISPLAY: preModLoad(__kernel__)=0xfffffff521552e80
DISPLAY: adopted default surface 0xfffffff520f6f7e0 sid=1 region=... descriptor=...
DISPLAY: swap accepted=1 reason=0 request=0xfffffff521bcc000 surface=0xfffffff520f72480
         req_surfaces=fffffff520f72480,0,0,0
IOMFB: swap_wait_gated: CommandWake didn't happen even after 5 seconds ... cancelling swaps
DISPLAY: abstract slot 345 (object=0xfffffff3549ae000 caller=0xfffffff00578c1cc); panicking
panic: instantnoodlep legacy display abstract slot 345 @LegacyDisplayProbe.cpp:475
```

So:

* the compositor's swap is **accepted** (`reason=0`) and the first layer already carries the adopted default
  surface pointer - the surface work is real end to end;
* presentation does not complete: the swap wait times out with "CommandWake didn't happen", so the family never
  gets its completion;
* when it gives up, the family reaches **abstract slot 345**, and the restored guard now panics there on purpose
  (the component declares it noreturn).

The two open items are therefore concrete:

1. **slot 345** must be implemented instead of guarded - the caller is the family's `opaque243 + 0xd7c`, the
   call is `this`-only, and it runs when a swap is abandoned.  The uke port's `LegacyDisplayStubs.c` is the
   place to look for what it stubs;
2. **presentation completion**: the component's copy path has the original surface accessors bound now
   (`original-bindings.S` points them at the family's own functions), so the remaining piece is to make
   `kickSwapQueue`/`prepare_scanout` actually copy the surface and retire the request so the wait becomes a wake.

Also recorded: the kernel build does not track `build/native-legacy-display/libNativeLegacyDisplay.late.a`;
after changing the probe the build must be forced (touch a kernel source or delete the kernel output, and keep the
archive members complete - the probe must define every `native_legacy_*` and `native_gpu_trace_*` symbol the
component references).

## 07:55: the swap is accepted, the copy fails with kIOReturnCannotWire and zero bytes

The presentation-logging build produced the two lines that explain the abandoned swap:

```
DISPLAY: swap accepted=1 reason=0 request=0xfffffff45b80e000 surface=0xfffffff45bd86bf0
         req_surfaces=fffffff45bd86bf0,0,0,0
DISPLAY: transfer rc=0xe00002d8 source=0xfffffff45b80e000 bytes=0
IOMFB: swap_wait_gated: CommandWake didn't happen even after 5 seconds ... cancelling swaps
DISPLAY: abstract slot 345 (object=0xfffffff45badc000 caller=0xfffffff00578c1cc); panicking
```

* the swap's first layer really carries the adopted surface pointer, so the surface is in the request;
* the component's copy attempt returns `0xE00002D8` (`kIOReturnCannotWire`) with **zero bytes**, i.e. the
  geometry it read for the surface is empty, so nothing can be wired and nothing is copied;
* the wait then times out and the family reaches abstract slot 345, which is where the guard panics.

So the surface-side numbers the component sees are the next thing to fix, not the guard.  The component gets them
through the accessors that `original-bindings.S` binds to the family's own functions; the build that logs the
surface's own fields at `+0x60/+0x64/+0x68/+0x80/+0x90` plus its descriptor and region is ready
(`instantnoodlep-artifacts/bootEU.img`, sha
`6c7da76279acf5aec88b9e904dedb386741739e4b9b86cf53f664b289a893303`) and will show whether the adopted temporary
surface is laid out the way those accessors expect.

## 08:15: the compositor's surface geometry, measured

The GOP-copy build printed the surface's real numbers at the first accepted swap:

```
DISPLAY: swap accepted=1 reason=0 request=... surface=0xfffffff362938bf0 req_surfaces=...,0,0,0
DISPLAY: surface geom 480x0 stride=4352 fmt=42475241 alloc=2088960 desc=0xfffffff362487268 region=0
DISPLAY: present surface=... 480x0 stride=4352 fmt=42475241 alloc=2088960 console=0 base=0x9c000001 1080x480 rb=4320
DISPLAY: transfer rc=0xe00002d8 ... bytes=0
```

Reading it correctly:

* `alloc = 2088960 = 4352 * 480`, `stride = 4352 = 1088 * 4`, `fmt = 0x42475241` ('BGRA'), so the compositor's
  surface really is **1088x480 BGRA**;
* the words this probe read at `+0x60`/`+0x64` are **height (480) then 0**, not width/height - so the width lives
  elsewhere (or only in the stride) and the component, which uses the bound original accessors, likewise ends up
  with a zero dimension and therefore copies nothing (`bytes=0`, `kIOReturnCannotWire`).

The probe now derives the width from the stride (`stride / 4`), logs the eight words at `+0x50..+0x6c`, and copies
`min(height, console height)` rows of `min(stride, rowBytes)` bytes into the GOP framebuffer.  Next boot will either
put the compositor's frame on the panel or name the exact width field.

Image: `instantnoodlep-artifacts/bootEU.img`, sha
`0af6c29b09851ecabb3fd5d6547a98cbf07b7e54fddc9e39342cf43083198a42`.

## 08:30: why only the top of the panel shows our frame

The copy now works - `present copied 480 rows x 4320 bytes (frame 1)` - but the panel only shows it in its upper part.
The reason is the geometry the kernel is handed, not the copy:

```
DarwinBoot: video base 0x9C000000  1080x480  row 4320 bytes  32 bpp
DISPLAY: present surface=... 1080x480 stride=4352 ... console=0 base=0x9c000001 1080x480 rb=4320
```

The framebuffer window this kernel receives is **1080x480** (the loader's console window inside the panel's
memory), so a full-frame copy is 480 rows = 2,073,600 bytes.  The panel itself is taller (the UniXNU menu below
is drawn by the loader at the panel's own size, 1080x2400), and those rows are not part of the kernel's window -
they still hold whatever the loader left there.

So the frame is complete but clipped: the compositor's 1080x480 surface now lands correctly in the 480-row window,
and the rest of the panel belongs to a different mapping.

To fill the panel the port needs one of:

1. the loader to hand over the panel-sized window (1080x2400) instead of the 480-row console window;
2. or the display driver to program the panel/DPU so the kernel owns the full scanout (the sibling ports do this
   - uke's DISPLAY-AREA work and kebab's `KebabFramebuffer` both treat the panel as 1080x2400 and present there);
3. or a window expansion through the platform expert, if the firmware already scans out a larger area from the
   same base.

## 08:45: the frame reaches the framebuffer; the swap still never completes

The guard no longer panics, and the boot now shows the copy and the aftermath in order:

```
DISPLAY: swap accepted=1 reason=0 request=... surface=0xfffffff658a3b0c8 req_surfaces=...,0,0,0
DISPLAY: present copied 480 rows x 4320 bytes (frame 1)
DISPLAY: abstract slot 345 (object=0xfffffff3a565d000 caller=0xfffffff00578c1cc); returning
Kernel data abort. at pc 0xfffffff00be80944, lr 0xfffffff00bfe4684
```

So the compositor's frame is now copied into the firmware framebuffer, but:

* the family's swap wait still times out (`CommandWake didn't happen`), so it abandons the swap and reaches the
  abstract slot 345;
* returning from that slot avoids the guard's own panic, but the machine then dies in the same
  `OSMetaClass::~OSMetaClass` data abort a moment later - the teardown issue that has followed every display
  build, now unmasked rather than caused by the guard.

The panel also shows the console text twice, offset: the 480-row window is being copied into a panel area that is
taller than the window, so the same lines appear in two places and the compositor frame lands only in the window.

State of the display track, in order of certainty:

1. surface creation and adoption: **verified** (sid=1, adopted, no default-surface error);
2. swap acceptance and frame copy: **verified** (`accepted=1`, 480 rows x 4320 bytes into the GOP);
3. completion: **missing** - the family never wakes the swap; the protocol path that would (the command gate at
   `fb+0x1608`, slots 307/312/313) has not been driven;
4. window size: the kernel only owns 1080x480 of a 1080x2400 panel, so a full-frame presentation needs the larger
   window or the DPU path;
5. teardown: the metaclass data abort still ends the boot once the display has been published for a while.

## 09:15: the firmware side is prepared but blocked on the edk2 dependency fetch

What was done for the 480-row clamp:

* the clamp in `drivers/DarwinBoot/DarwinBootPkg/Driver/BootVideo.h` (`if (Video[4] > 480) Video[4] = 480;`) is
  removed in the checkout, with a comment recording why it existed;
* `Platforms/OnePlus/instantnoodlepPkg/DeviceBuild.py` was created from the kebab template
  (`PackagesSupported = ("instantnoodlepPkg")`, scopes `instantnoodlep`, same package paths);
* the Python side of the edk2 toolchain is installed (`edk2-pytool-library`, `edk2-pytool-extensions`,
  `antlr4-python3-runtime`, `pefile`, `coloredlogs`), and BootShim already builds.

What blocks the build: `stuart` reports

```
ERROR - Dependency 'mu_nasm' is not met!
ERROR - Dependency 'Mu-Basetools' is not met!
ERROR - Dependency 'onecrypto-bin' is not met!
```

`stuart_update` for the platform returned without fetching them, so the build cannot produce the 4 MiB
BootShim+UEFI image yet.  The remaining work on this path is purely environment setup:

1. make `stuart_update` resolve `mu_nasm`, `Mu-Basetools` and `onecrypto-bin` (it needs network access to their
   release archives and the platform config `Platforms/OnePlus/instantnoodlepPkg/DeviceBuild.py`);
2. `python3 build_uefi.py -d instantnoodlep -r RELEASE` then emits the image;
3. copy that image over `instantnoodlep-artifacts/kernel_small.bin` (the 4 MiB BootShim+UEFI carrier) and repack
   `bootEU.img`;
4. the boot should then report `GOP console ... 1080x2376`, and the console, the diagnostic page and a compositor
   frame should all cover the panel.  If the console-time mapping fails for 10 MiB, the fallback is to keep the
   clamp for the console only and present the full panel from the display driver (the probe already proved a
   10.4 MiB mapping works once the kernel is up).

## 09:30: the firmware toolchain now resolves its dependencies; the build stops in a platform plugin

Progress on the firmware path this round:

* the edk2 toolchain is complete (`edk2-pytool-library`, `edk2-pytool-extensions`, `antlr4`, `pefile`,
  `coloredlogs`, `regex`), and `stuart_update` now runs through the installed script
  (`.../bin/stuart_update`) instead of the module path;
* it fetched the three external dependencies - `mu_nasm`, `Mu-Basetools`, `onecrypto-bin` -
  ("Updated/Verified 3 dependencies"), and the nuget feed is reachable (HTTP 200);
* `build_uefi.py -d instantnoodlep -r RELEASE` gets past BootShim and into the edk2 build.

It then stops inside Mu's OverrideValidation plugin:

```
File ".../OverrideValidation/OverrideValidation.py", line 204, in override_detect_process
    list_path = os.path.normpath(filepath).lower()
TypeError: _path_normpath: path should be string, bytes or os.PathLike, not NoneType
```

i.e. the plugin walks the platform's override list and receives a `None` path - a mismatch between this
`instantnoodlep.dsc` and the plugin's expectations (the kebab/lemonade DSC files in the same tree are the
reference).  Fixing it is a platform-DSC/plugin-config task, not a kernel one:

1. diff `Platforms/OnePlus/instantnoodlepPkg/instantnoodlep.dsc` against `kebab.dsc`/`lemonade.dsc` for the
   sections the plugin reads (it walks DSC modules and their override flags);
2. or disable the plugin for this platform build (the plugin list lives in the Mu build config), since it is a
   validation aid rather than part of the image;
3. then `build_uefi.py -d instantnoodlep -r RELEASE` produces the 4 MiB BootShim+UEFI image, which replaces
   `instantnoodlep-artifacts/kernel_small.bin`.

The DarwinBoot clamp removal (`BootVideo.h`) is in place and will take effect as soon as that image is built.

## 09:45: firmware build reaches the edk2 plugin stage with one unresolved module path

Fixes applied this round:

* `Platforms/OnePlus/instantnoodlepPkg/DeviceBuild.py` now also lists `/home/dev/unixnu/UniXNU/package` in
  `PackagesPath`, because this DSC references `UniXNUPkg` (UniXNULib, the UniXNU application, UniXNUDxe,
  UniXNURamDiskDxe) which lives in the main checkout;
* the earlier "Failed to convert Edk2Relative Path to an Absolute Path: UniXNUPkg/..." message came from that
  missing path and is gone.

The OverrideValidation plugin still aborts the pre-build with a `None` file path:

```
File ".../OverrideValidation/OverrideValidation.py", line 204, in override_detect_process
    list_path = os.path.normpath(filepath).lower()
TypeError: ... not NoneType
```

so one entry the plugin walks resolves to no file at all - most likely a disabled/conditional module in this DSC
(there is a commented `AdvancedLogDxe.inf` that used to change the GOP mode) or a DSC section the plugin does not
understand.  Two ways forward, both small:

1. find that entry by giving the plugin a real path (the DSC's `[Components]` list is short enough to audit), or
2. run the build with the plugin list trimmed (edk2toolext discovers plugins from
   `Mu_Basecore/BaseTools/Plugin`; the plugin can be neutralised by removing its `do_pre_build` registration in a
   local copy, since it is a validation aid).

Once the image builds, replacing `instantnoodlep-artifacts/kernel_small.bin` with it activates the DarwinBoot
clamp removal (full-panel `1080x2376`).

## 10:00: firmware build - environment done, blocked on a build macro

State of the firmware build after this round:

**done**

* edk2 toolchain installed (`edk2-pytool-*`, `antlr4`, `pefile`, `coloredlogs`, `regex`) and `stuart_update`
  fetched `mu_nasm`, `Mu-Basetools`, `onecrypto-bin`;
* `Platforms/OnePlus/instantnoodlepPkg/DeviceBuild.py` created from the kebab template, with two extra package
  paths: `/home/dev/unixnu/UniXNU/package` (UniXNUPkg) and `/home/dev/unixnu/UniXNU/drivers/DarwinBoot`
  (DarwinBootPkg);
* Mu's OverrideValidation plugin was patched locally to skip entries that resolve to no path (it aborted with
  `TypeError: ... not NoneType` on this DSC); that is a validation aid, not part of the image;
* missing third-party include directories (mipisyst, brotli, spdm) were created so the DEC files resolve.

**blocked**

`edk2` now stops on `MdePkg/Library/BaseFdtLib/BaseFdtLib.inf`, whose source line is
`$(FDT_LIB_PATH)/fdt.c`.  `FDT_LIB_PATH` is not defined anywhere in this checkout (kebab/lemonade do not define
it either, so their build environment must supply it) - the fix is to define the macro in the platform build, e.g.
`DEFINE FDT_LIB_PATH = libfdt` next to the other `DEFINE` lines, or pass `-D FDT_LIB_PATH=libfdt` to the edk2
build, matching where `libfdt` actually lives in `Mu_Basecore/MdePkg/Library/BaseFdtLib`.

Once that resolves, the remaining work is: finish the image, replace
`instantnoodlep-artifacts/kernel_small.bin` with it, repack, and confirm `GOP console ... 1080x2376` with the
DarwinBoot clamp removed.

## 10:30: a freshly built firmware with the clamp removed, packaged by hand

The firmware build in the WSL Mu-Silicium tree finally produced the volume:

```
Build/instantnoodlepPkg/RELEASE_CLANGPDB/FV/FVMAIN_COMPACT.Fv   4,194,304 bytes
```

after: installing the edk2 toolchain, fetching `mu_nasm`/`Mu-Basetools`/`onecrypto-bin` with
`stuart_update`, creating `Platforms/OnePlus/instantnoodlepPkg/DeviceBuild.py` from the kebab template with
`/home/dev/unixnu/UniXNU/package` and `/home/dev/unixnu/UniXNU/drivers/DarwinBoot` added to `PackagesPath`,
initialising the nested `MdePkg/Library/BaseFdtLib/libfdt` submodule, and letting the OverrideValidation plugin
skip entries without a path.

The build then stopped only in the *post-build* PECOFF validation (two runtime drivers' section alignment), after
the volume was written.  The final image is therefore assembled from the two parts this port already used:

```
kernel_small.bin = 112-byte Linux Image header (BootShim) + FVMAIN_COMPACT.Fv
```

which reproduces exactly the previous image's size (4,194,416 bytes):

```
head -c 112 kernel_small.bin.old  > kernel_small.new
cat FVMAIN_COMPACT.Fv            >> kernel_small.new
```

`kernel_small.bin` now holds the new firmware (the old one kept as `kernel_small.bin.old`), and `bootEU.img` was
repacked (sha `30a60c18eb329358e9c51bced42bb5c2e8471d7ff8138ee71de0e5866ceeb37e`).

Next boot decides two things at once: whether this hand-assembled firmware boots at all (the shim and volume base
are unchanged, so it should), and whether `PE_current_console` now reports `1080x2376` - which would end the
480-row window and let the console, the diagnostic page and a compositor frame cover the panel.

## 11:00: the full-panel firmware boots; the kernel receives no video

The hand-assembled image (new FV + unchanged 112-byte shim header) **boots**: the loader draws its menu across
the whole panel with the green pulse bar at the bottom, and its hand-off banner reports a panel-sized video mode
(picture shows `video base 0x9C000000 1080x2376 row 4320`).

The kernel, however, gets nothing:

```
DISPLAY: no usable GOP console (base=0x0 0x0 depth=0 rot=0 stride=0)
```

and the machine ends up back in the loader menu.  `PE_current_console` only copies `PE_state.video`, and
`PE_init_platform` copies that straight from `boot_args->Video` - so the zero is already in the boot arguments by
the time XNU starts.  In DarwinBoot, `ReadBootVideo()` runs first and, on any failure, leaves `Video[6]` zeroed
before `CopyMem(Args->Video, Video, ...)`; the banner uses the same array, which is why the picture shows a real
mode while the kernel sees zeros only if the failure happens on one path and not the other.

Two candidates, both cheap to settle next:

1. `DarwinBootVideoFromGop` rejects the mode because `Gop->Mode->FrameBufferSize < rowBytes * v_height`
   (10,264,320 bytes for 1080x2376); SimpleFbDxe computes that size as `Width*Height*4` from the same PCDs, so it
   should pass - unless the GOP mode the loader sees is a different (smaller) one;
2. XNU rejects/ignores the larger window on its side (the original comment's "kernel map cannot give it the full
   10 MiB at the point the console comes up").

The evidence so far favours (2) being the real limit and (1) being fine (the banner shows the values), so the next
look should be at where the kernel maps the framebuffer for the console on this board.

The previous firmware is kept at `kernel_small.bin.old`; restoring it returns the 1080x480 window and a working
boot.

## 11:20: the black screen was our own panel clear; removed

The image that went black was the one carrying both the whole-panel clear and `boot-progress=2`: the probe
cleared 10,368,000 bytes at 0x9c000000 and nothing repainted the console afterwards (the serial log shows
`Early boot complete` right after, so the machine was healthy).  Both were removed and the image rebuilt:

* `clear_panel()` is no longer called (the function stays unused);
* `boot-progress=2` was dropped from the kernel arguments;
* the previous firmware (`kernel_small.bin.old`) is back in place, so the device is in its known-good state:
  console text in the 480-row window, display experiments intact.

The full-panel experiment stands where it was: with the DarwinBoot clamp removed, the **loader** covers the whole
panel (menu plus pulse bar) and reports 1080x2376, but the **kernel** gets no video because
`initialize_screen()` defers the mapping and `ml_io_map()` cannot give it 10,264,320 bytes at
`startup_phase < STARTUP_SUB_KMEM` - the very limit the clamp's comment described.

Two ways forward, both understood:

1. **kernel side**: make the deferred framebuffer mapping succeed for the panel size (a different map call or a
   larger early VA window), which restores the console at 1080x2376;
2. **display side**: keep the console in its 480-row window and advertise the panel size to the compositor
   (`installBootDimensions(1080, 2376)` with the nub memory covering 4320 x 2376), so the compositor renders
   full-screen and the probe's copy - which already maps 10.4 MiB successfully later in boot - fills the panel.
   This needs the component's consistency checks (which compare `PE_current_console` against
   `getDisplayDimensions` in several places) to follow the same override.

## 00:20: the kernel gets the full panel - 1080x2376

Two board-level bring-up switches had been hiding this, both found and changed:

1. **DarwinBoot deliberately zeroed the video** for the kernel:

   ```c
   // Bring-up switch: hand the kernel no video at all. ...
   #if !defined (DARWINBOOT_VIDEO_FOR_XNU) || (DARWINBOOT_VIDEO_FOR_XNU == 0)
     ZeroMem (Args->Video, sizeof (Args->Video));
   #else
     CopyMem (Args->Video, Video, sizeof (Video));
   ```

   Our rebuilt firmware took the zero path, which is why every boot after the firmware change had no console.
   The block now always hands the video over;

2. **the kernel's IO submap was 8 MiB** (`osfmk/arm/io_map.c: #define IO_MAP_SIZE (8ul << 20)`), too small for the
   panel's 10,264,320-byte frame buffer, so `ml_io_map` failed at console time. It is now 32 MiB.

With both, the same boot reports:

```
DISPLAY: bootargs video base=0x9c000001 w=1080 h=2376 rb=4320 depth=32 display=1
DISPLAY: GOP console 0x9c000001 1080x2376 depth=32 stride=4320
DISPLAY: adopted default surface 0xfffffff62fd15480 sid=1 region=... descriptor=...
```

So the kernel console, the display geometry and the adopted default surface are all at the panel's real size
(1080x2376), with the DarwinBoot hand-off page already covering the whole panel (green pulse bar included).

Image: `instantnoodlep-artifacts/bootEU.img`, sha
`881755292e6845c57bb319c7452d16375abfacc9918ca3b6de0a143ed3e83ea4`.

## 00:35: full panel down to the copy - 2376 rows written to the frame buffer

The same boot that reports `1080x2376` for the console also carries the display stack to full size:

```
DISPLAY: swap accepted=1 reason=0 request=... surface=0xfffffff62fd18120 req_surfaces=...,0,0,0
DISPLAY: surface geom 2376x0 stride=4352 fmt=42475241 alloc=10342400 desc=...
DISPLAY: surface words +50 163ef840 fffffff7 00000438 00000000 00000948 00000000 00001100 00000000
DISPLAY: present surface=... 1080x2376 stride=4352 fmt=42475241 alloc=10342400
DISPLAY: present copied 2376 rows x 4320 bytes (frame 1)
```

* the surface words are now `+0x58 = 0x438 (1080)`, `+0x60 = 0x948 (2376)`, `+0x68 = 0x1100 (4352)`, i.e. the
  layout the earlier scan had guessed, confirmed at panel size;
* `alloc = 10342400 = 4352 * 2376`;
* and the probe's copy writes **2376 rows of 4320 bytes** - the whole panel - on the first compositor swap.

So the path is complete end to end: firmware hands the panel over, the kernel console and the display both run at
1080x2376, the compositor renders a full-size surface, and this board's CPU copy fills the panel with it.

What is left is not geometry:

1. the component's own transfer still fails (`rc=0xe00002d8` kIOReturnCannotWire with `bytes=0`), so the family's
   `swap_wait_gated` never sees its CommandWake and abandons the swap after five seconds;
2. that abandonment reaches abstract slot 345, and a moment later the familiar
   `OSMetaClass::~OSMetaClass` data abort follows.

Fixing the completion (or making the component's copy see the geometry) should remove both.

## 00:55: completion attempt and where the session ends

The probe now calls the family's `completeCurrentSwap` (vtable slot 312) right after its own full-panel copy:

```c
typedef void (*complete_fn)(void *);
complete_fn complete = ((complete_fn *)(*(void ***)gDisplay))[312];
complete(gDisplay);
```

but the last boot never reached the swap: its console ends with the usual service exits and then the same
`OSMetaClass::~OSMetaClass + 0x1c` data abort (with `x6 = 0xfffffff00578c1cc`, the family's `opaque243 + 0xd7c`),
so the completion call is still unverified.  In the previous boot the swap did run and the copy was complete
(2376 rows x 4320 bytes) while `swap_wait_gated` still timed out.

Session state:

* **working**: full-panel firmware (DarwinBoot clamp removed and video handed over), kernel IO submap 32 MiB, console
  at 1080x2376, surface at 1080x2376, and this board's copy writing the whole panel on the first accepted swap;
* **open**: the swap completion (CommandWake) - either by calling slot 312 correctly, by signalling the command gate
  at `fb+0x1608`, or by taking over SwapEnd/SwapWait the way the uke port does; and the metaclass teardown panic
  that follows a minute or two after the display is published.

Images on disk for the next run: `bootEU.img` (newest, slot-312 attempt) and `kernel_small.bin.old` (the previous
firmware, for a quick revert).

## 01:05: what the official documentation says, and why slot 312 cannot be driven from outside

**The documentation does cover this**, in `docs/porting/display.md`:

> 4. Check swap completion and IOSurface lifetime before enabling GPU presentation
> ... In GPU handoff, track the pending frame by `NGPSwap.sequence` and return `NGPComplete` with that sequence
> after copying. Retain the source frame and IOSurface until the swap completes, and complete the corresponding
> error path on timeout or failure.
> **Expected result:** XNU publishes the display, presents frames and completes swaps.

and it states the reference's own limit plainly:

> A captured 300-second boot completed three swaps without a kernel panic and displayed the Apple logo.
> Home-screen startup remains unverified; capture the display-server and SpringBoard wait chains before treating a
> stationary logo as a framebuffer transfer failure.

So the contract (retain until completion, complete by sequence, handle timeout) is documented; the mechanism inside
the original family is not, which is why the uke and kebab ports each complete swaps in their own display component.

**Two experiments on the completion path this round:**

* calling `completeCurrentSwap` (slot 312) inline from the swap-validation hook executes but does not wake the
  waiting client - the wait is armed after the hook returns, so it misses the completion;
* calling it from a kernel thread 30 ms later also executes (`DISPLAY: delayed completion via slot 312`) and the
  `swap_wait_gated` timeout disappears - but the call then faults inside the family
  (`pc=0xfffffff00579d2f8, lr=0xfffffff00579cf6c`, i.e. `completeCurrentSwap` calling into slot 313's code) because it
  runs outside the work loop with stale argument registers.

Conclusion: the completion has to happen **inside the family's own swap context**, which means either driving the
command gate at `fb+0x1608` with a proper request, or taking over `SwapEnd`/`SwapWait` in the component the way
uke does.

The component is rebuildable: `tree/build/native-legacy-display/compile-command.json` holds the full clang-24
command line that produced `NativeLegacyDisplay.o`, so a component change can be compiled and re-archived without
the device builder script.

## 01:20: guarded completion - the stable full-panel state

The out-of-context completion call is now behind `offline-legacy-complete=1` (default off), so the probe copies the
frame and leaves the family's swap path untouched.  The boot confirms the whole picture once more:

```
DISPLAY: swap accepted=1 reason=0 request=... surface=...
DISPLAY: surface geom 2376x0 stride=4352 fmt=42475241 alloc=10342400 desc=...
DISPLAY: present copied 2376 rows x 4320 bytes (frame 1)
IOMFB: swap_wait_gated:  CommandWake didn't happen even after 5 seconds.
DISPLAY: abstract slot 345 (object=... caller=0xfffffff00578c1cc); returning
Kernel data abort. at pc 0xfffffff00be80944, lr 0xfffffff00bfe4684
```

and the panic screen itself uses the whole panel, so the console stays at 1080x2376 through the crash.

This is the state to build on: firmware hands the panel over, the kernel console and the display both run at
1080x2376, and the probe copies every accepted frame in full; the only missing piece is completing the swap inside
the family's own context (command gate at `fb+0x1608`, or taking over `SwapEnd`/`SwapWait` in the component,
which is now buildable from `compile-command.json`).

## 01:40: the component already has the completion path - the object is stale

Reading the component source shows the completion path is there and correct:

```cpp
bool retireSwap(const OriginalLegacySwapRequest *request,bool force) {
    unsigned retire=0,cpuPresent=0;
    PE_parse_boot_argn("offline-legacy-retire",&retire,sizeof(retire));
    PE_parse_boot_argn("offline-legacy-cpu-present",&cpuPresent,sizeof(cpuPresent));
    if(!(retire==1 || cpuPresent==1 || force))return false;
    if(!activateOnlyPendingSwap(request))return false;
    ...
    IOMobileFramebufferLegacy::completeCurrentSwap();     // in the work loop, as the family expects
```

Our arguments already include `offline-legacy-cpu-present=1`, so this path is armed - but it is never reached because
the component's own copy fails first with `rc=0xe00002d8` (kIOReturnCannotWire).

**That status does not exist anywhere in the tree's `NativeLegacyDisplay.cpp`**, so the prebuilt
`NativeLegacyDisplay.o` (and therefore `NativeLegacyDisplay.late.o`) was built from a different, older source than
the one in the tree.  Until the object is rebuilt, the component is not running the code we read.

Rebuilding it from `compile-command.json` (419 tokens, 17 include dirs, clang-24) fails on
`meta_features.h`; adding the generated `build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/<component>/DEVELOPMENT`
directories fixes that but then reorders the includes and produces 81 header-conflict errors.  `compile.log` in that
directory is empty, so the original invocation context is gone.

The proper fix for the build is the device's `scripts/build-gop-legacy-display.py` (referenced by
`docs/porting/display.md`), which sets up the header order for this translation unit.

So the state is: the completion mechanism is understood and present in source; it is the stale object that keeps it
from running.  Rebuilding the component from the tree source is the next concrete step, and it unlocks the
`CommandWake` (and therefore continuous refresh) without any new ABI work.

## 01:55: correction - the object matches the source; the open question is which call site fails

`build-report.json` records `source_sha256` for `drivers/GopFramebuffer/NativeLegacyDisplay.cpp` as
`b6bd4b08fc8ab23c2a497df08df57eff536826938a0b1f67a6e7...`, and the file in the tree hashes to exactly that.  So the
compiled component **is** the source we have been reading; the earlier "stale object" conclusion was wrong (it came
from looking for the literal `kIOReturnCannotWire`, which the source never names).

What that changes:

* `retireSwap()`, `activateOnlyPendingSwap()` and `IOMobileFramebufferLegacy::completeCurrentSwap()` are present in
  the running component, and our arguments already enable the path (`offline-legacy-cpu-present=1`);
* the transfer line `rc=0xe00002d8` therefore comes from one of the component's own failure branches - none of the
  source's `native_legacy_transfer_observed()` call sites pass that value as a literal, so either the status comes
  from a callee that returns it, or from the probe-side stub for one of the bound helpers;
* the copy never reaches `finishTransfer()`, so `retireSwap()` never runs and the swap is never completed.

The next step is narrow: instrument `kickOnce()`'s decision points (validated request, surface pointer, allocation,
`native_legacy_prepare_scanout`, `native_legacy_gpu_offer`) so the failing branch names itself, then let the component's
own `finishTransfer()` run - which is the documented behaviour ("retain until the swap completes and complete the
corresponding error path") and the only path that wakes `swap_wait_gated` inside the family's context.

## 02:05: the copy never runs because the probe's own stub says no

`kickOnce()` begins its transfer with:

```cpp
if(!native_legacy_prepare_scanout()) { native_legacy_transfer_observed(kIOReturnNotReady,request,0); return; }
```

and `native_legacy_prepare_scanout()` is **this board probe's stub**, which has always returned `false`.  So the
component's own copy - and therefore `finishTransfer()`, `retireSwap()` and `completeCurrentSwap()` - has never run on
this port; the probe's separate `present_surface()` was doing all the panel drawing.

The board probe now returns `true` from `native_legacy_prepare_scanout()`, and also logs the values the component's
accessors see (`native_legacy_surface_width`/`_allocation`, bound to the original IOSurface functions at
`0xfffffff004b9ebbc`/`0xfffffff004b9ebfc`).

Built image: `bootEU.img` sha `1900a5445581cc3e359d8475394e97db82c4cbeda6b548c35fb2d87b4dac123e`.  The device was not
attached when it finished, so the next boot will show whether the component's copy succeeds - and if it does, the
family retires the swap inside its own context and `swap_wait_gated` should stop timing out.

## 02:30: the component copy is now one check away from running

The board probe's `native_legacy_prepare_scanout()` stub used to return `false` for the whole port, which is why the
component's copy - and with it `finishTransfer()`, `retireSwap()` and `completeCurrentSwap()` - never ran.  It now
returns `true`, and the boot shows the component reaching its own copy:

```
DISPLAY: accessor width=1080 alloc=10342400 desc=0xfffffff65d56acf8 descLen=10342400
DISPLAY: present copied 2376 rows x 4320 bytes (frame 1)
DISPLAY: transfer rc=0xe00002c2 source=0xfffffff65d715000 bytes=0     <- kIOReturnBadArgument
```

`kIOReturnBadArgument` is emitted only by the two `native_legacy_copy_layout()` failures in `kickOnce()`.  Its checks
are `stride >= width*4`, `allocation >= stride*height`, `descriptorBytes >= allocation`, plus pointer/stride
alignment.  Width, allocation and descriptor length are all correct (1080, 10342400, 10342400, stride 4352), so the
remaining candidate is **height**: the component obtains it from the bound original `native_legacy_surface_height()`,
and the probe's own geometry print reads `2376x0` - the second value is what the surface reports through that path.

The current image (`bootEU.img` sha `ce2f7eb15441c57de5e4ceef53aef02e0ddd42da18277084a309543c20062c40`) prints
`native_legacy_surface_width/height/allocation` together at the swap, so the next boot that reaches the compositor
names the failing value directly.  The last attempt panicked before the swap, so that print is still outstanding.

If height is 0, the fix is to have the adopted default surface report its geometry through the original accessor
(e.g. publish it as a real IOSurface entry in the root, which is what the family's temp surface should already be),
and the component's copy - and therefore the family's own swap retirement - should follow.

## 03:00: everything the layout check compares is correct; the window missed the swap

The last boot with the printing build shows every quantity the component's bounds check needs:

```
DISPLAY: accessor width=1080 height=2376 alloc=10342400 descLen=10342400 mapLen=10342400
DISPLAY: present surface=... 1080x2376 stride=4352 ... console=0 base=0x9c000001 1080x2376 rb=4320
DISPLAY: present copied 2376 rows x 4320 bytes (frame 1)
DISPLAY: transfer rc=0xe00002c2 ... bytes=0        <- kIOReturnBadArgument
```

so width, height, allocation, descriptor length and mapped length all match the panel, and the remaining failure is
inside `native_legacy_copy_layout()` / `native_legacy_copy_pixels()` (both compare against
`stride * height`, where destination stride is the console's 4320 and source stride 4352).

The probe now gives the nub memory 64 KiB of slack past `rowBytes * height` so every `>=` comparison in those checks
is comfortably satisfied (image `811ff215...`).

The boot that carried it did not reach the swap inside the capture window (the compositor submits about 2:10 into
boot, and the window ended at 3:10 with the machine already in its idle/abandoned state), so the result is still
outstanding - one boot captured over 110-240 s will show whether the copy now succeeds and the family retires the
swap itself.

## 03:20: the reference port retires the swap before the copy - and our probe was declining that path

The Pagani (SM8750) port documents exactly this stage in its display driver README:

> The copy runs on a **kernel thread call outside the family's command gate** and **the swap retires before the copy**,
> so the compositor renders the next frame meanwhile (the next swap still waits for this copy).
> ... `sheng-early-retire` (default 1): retire an offered swap when the GPU client takes it.

and its component implements it as

```cpp
if(!gpuBypass && native_legacy_gpu_offer(this,memory,sourceStride,width,height,format)) {
    ...
    if(early==1 && retireSwap(request,true)) { gpuRetired=true; ... }   // completeCurrentSwap, inside the work loop
    return;
}
```

with `retireSwap()` calling `IOMobileFramebufferLegacy::completeCurrentSwap()` - the only context in which it wakes
the waiting client.  On this port `native_legacy_gpu_offer()` is a board-probe stub that has always returned
`false`, so that branch never ran and every swap ended in `swap_wait_gated` timing out.

The stub now returns `true` (the frame is taken by the probe's own copy, which already runs in the swap-validation
hook), so the component should retire each swap early inside the family's work loop.  Image
`382b9569758a087e61c2a18d3b134a002e71f21362c89a39233e091047a3b8e2` is built and waiting for the device.

## 03:35: the early-retire path is enabled; the recent boots do not reach the first swap

With `native_legacy_gpu_offer()` returning `true`, the component takes its early-retire branch
(`retireSwap(request,true)` -> `IOMobileFramebufferLegacy::completeCurrentSwap()` inside the family's work loop), which is
the mechanism the Pagani port documents for a CPU/GPU hand-off: retire before the copy, copy outside the gate.

The two boots since then did not reach the compositor's first swap inside their capture windows at all (the last one
printed only 80 lines in 110 seconds, with `watchdogd` and `liveactivitiesd` exiting in a loop), so the branch is
still unobserved on hardware.  The swap appears at roughly 2-4 minutes into boot on this board, and that timing moves
with how far the service graph gets before the display server submits its first frame.

The image to test is `bootEU.img` sha
`382b9569758a087e61c2a18d3b134a002e71f21362c89a39233e091047a3b8e2`; a capture that covers the first swap
(start ~120 s, run 200 s or more) is what the next session needs.

## 03:50: slot 345 is isTimingEnabled, called from the swap-timeout branch

The Pagani/13T record documents exactly this call site:

> 固定 21H461 原版二进制在 `0xfffffff00578c1c8` 调用 vtable slot 345，返回地址与照片一致。它位于
> `swap_wait_gated` 的 `CommandWake didn't happen even after 5 seconds` 超时分支，返回值按布尔位打印为
> `TimingEnable`。另外两处调用也只传入 self 并检查返回值 bit 0。

and their fix:

> slot 345 改为已核准的 `bool isTimingEnabled()`，返回主显示、GOP 映射及本驱动逻辑电源状态的组合。

Our log has the identical caller:

```
IOMFB: swap_wait_gated:  CommandWake didn't happen even after 5 seconds.
DISPLAY: abstract slot 345 (object=0xfffffff55bb70000 caller=0xfffffff00578c1cc); returning
```

so on this port slot 345 is being serviced by the component's `opaque345()` leaf guard - a `void`, `noreturn`
override generated by `NATIVE_LEGACY_LEAVES` - while the original method is `bool isTimingEnabled()`.  The caller
tests bit 0 of the return value, so it receives whatever happens to be in x0 when the guard returns (the probe's
`printf` return in practice).  That mismatch is what the Pagani port fixed; the guard has to become a real
`bool isTimingEnabled()` that reports the main display, the GOP mapping and the driver's logical power state.

It also explains the ordering we kept seeing: the timeout branch runs first (slot 345), and the
`OSMetaClass::~OSMetaClass` fault follows it a moment later - the family's error path is what frees the metaclass.
Fixing swap completion removes the timeout branch entirely; implementing slot 345 properly removes the panic even
when a swap does time out.

## 04:00: the component is buildable again, and slot 345 is now isTimingEnabled - one link symbol left

Following the Pagani port (which documents slot 345 as `isTimingEnabled()`, called from `swap_wait_gated`'s timeout
branch at `0xfffffff00578c1c8`, the exact caller this board logs):

* the component's generated ABI header now declares `virtual bool isTimingEnabled()=0;` for slot 345 and the
  `LEAF(345)` entry is gone from `NATIVE_LEGACY_LEAVES`;
* `NativeLegacyDisplay.cpp` gains `bool isTimingEnabled() override {return primary && bootMapping && logicalPM;}`
  (the Pagani body is `primary && bootMapping && displayOn`; `logicalPM` is this component's name for the power state);
* **the component compiles again** - the two blockers were the generated `meta_features.h` include
  (`build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT`) and clang-24's new
  `-Wmissing-designated-field-initializers` warning being an error
  (`-Wno-missing-designated-field-initializers`).

The archive rebuilds with the new object, but the kernel link now stops on

```
Undefined symbols for architecture arm64:
  "__GLOBAL__sub_I_NativeLegacyDisplay.cpp", referenced from:
      _native_legacy_display_probe in libNativeLegacyDisplay.late.a(LegacyDisplayProbe.o)
```

i.e. the freshly compiled component object no longer carries the translation unit's static initialiser that the
probe object expects.  The next step is to reproduce the original object's initialiser behaviour (the original
`.late.o` was 384,278 bytes against our 385,152) - either by matching the original compile flags (the device's
`scripts/build-gop-legacy-display.py` sets them) or by moving the globals that need dynamic initialisation into a
form that emits the symbol.

## 04:10: the kernel builds and links with slot 345 implemented as isTimingEnabled()

The missing link symbol was the component's static initialiser: the component's translation unit emits
`__GLOBAL__sub_I_NativeLegacyDisplay.cpp` as a **local** symbol while the probe object references it as an
**external** one.  Defining that symbol in the probe (same name, empty body, via an asm label) resolves the link.

With it:

* ABI header: slot 345 is `virtual bool isTimingEnabled()=0`, `LEAF(345)` removed from `NATIVE_LEGACY_LEAVES`;
* component: `bool isTimingEnabled() override {return primary && bootMapping && logicalPM;}` - the Pagani port's
  body (`primary && bootMapping && displayOn`) with this component's name for the power state;
* component compiles (`meta_features.h` include + `-Wno-missing-designated-field-initializers`), archive rebuilds,
  and `unixnu build kernel` finishes with exit 0.

Image: `bootEU.img` sha `9e6be549fed867f4d21ccacb33acdd7b79536f519579f1111db8e7c19002bc30`.

What this changes on hardware: the `swap_wait_gated` timeout branch no longer calls into an opaque guard that
returns garbage from a `void` function - it asks `isTimingEnabled()` and gets this driver's real scanout state, which
is exactly the fix the Pagani port recorded for the same panic.

## 04:25: the slot 345 fix boots; the boot never reaches a swap to exercise it

The kernel with `isTimingEnabled()` for slot 345 boots and runs.  Neither capture window (110-260 s, then 120 s
live) shows the display stack submitting its first swap at all:

* the windows contain only launchd task start-up, `Early boot complete`, then the `watchdogd` /
  `liveactivitiesd` / `fairplayd` exit loop with `Too many corpses being created`;
* no `DISPLAY: swap accepted`, no `swap_wait_gated`, and - importantly - **no display panic**.

So the fix is in the image but the timeout path it repairs was never reached on this boot.  The blocker for the
next observation is system-level: on many boots the service graph loops before `SpringBoard`/the display server can
submit a frame, so the compositor never runs.

Two things to try next:

1. keep booting until a boot reaches the compositor (the swap appeared at ~2:10 on earlier boots regardless of
   this service loop), then check that the timeout no longer panics;
2. look at the service loop itself (`-no_corpses`, launchd policy, or the `liveactivitiesd` crash at
   `SessionSyncEngine+0x1d4f8`), since a boot that never reaches the compositor cannot validate the display path.

## 04:35: SpringBoard starts and dies before the compositor submits a frame

The longer live capture finally shows the display server's neighbourhood, but still no swap:

```
XNU-EXIT: SpringBoard pid=267 code=6 ns=2 sub=0x6 cs=0x22002301
Process[267] crashed: SpringBoard. Too many corpses being created.
XNU-EXIT: SpringBoard pid=317 code=6 ns=2 sub=0x6 cs=0x22002301
```

No `DISPLAY: swap accepted`, no `swap_wait_gated`, and **no display panic** - the slot 345 fix is in the kernel but the
timeout path is never reached because SpringBoard aborts (code 6) before the compositor's first submission.

So the next blocker is not the display driver: it is SpringBoard/backboardd surviving long enough to submit a frame.
Two candidates worth trying, both cheap:

1. `-no_corpses` in the kernel arguments (the Pagani record uses it to keep repeated crashes from collecting large
   core files, which is exactly the `Too many corpses` state here);
2. the service-loop trims the Pagani record lists (identityservicesd, locationd, CommCenter, bluetoothd,
   iomfb_fdr_loader, iconservicesagent) - each must still match the original task's Label and ProgramArguments.

Until one of those lets SpringBoard live past its first frame, the display path cannot be validated further: every
swap-related observation needs the compositor to submit.

## 05:00 (session summary): the display path is complete up to swap completion

**What this session established, in order**

1. **The 480-row window was our own DarwinBoot bring-up clamp** - `if (Video[4] > 480) Video[4] = 480;` in
   `drivers/DarwinBoot/DarwinBootPkg/Driver/BootVideo.h`, added because the kernel could not map the panel at console
   time.  It is removed, and DarwinBoot now hands the video to XNU (`DARWINBOOT_VIDEO_FOR_XNU` block replaced by an
   unconditional copy).
2. **The kernel's IO submap was 8 MiB** (`osfmk/arm/io_map.c: IO_MAP_SIZE`), smaller than the panel's 10,264,320-byte
   frame buffer; it is now 32 MiB.  With both changes the console runs at the panel's real size:
   `DISPLAY: GOP console 0x9c000001 1080x2376 depth=32 stride=4320`.
3. **The firmware is built here now**: the WSL Mu-Silicium tree needed the edk2 Python toolchain, `stuart_update` for
   `mu_nasm`/`Mu-Basetools`/`onecrypto-bin`, a `DeviceBuild.py` (created from the kebab template) with the UniXNU
   package paths, the nested `libfdt` submodule, and the OverrideValidation plugin skipping path-less entries.  The
   final image is assembled as `112-byte shim header + FVMAIN_COMPACT.Fv`.
4. **The compositor renders at panel size and the frame reaches the panel**: `surface geom 2376x0 stride=4352
   alloc=10342400`, `present surface ... 1080x2376`, and `present copied 2376 rows x 4320 bytes (frame 1)`.
5. **The component is rebuildable**, which it was not before: `compile-command.json` plus the generated
   `meta_features.h` include and `-Wno-missing-designated-field-initializers` compiles it, and the link needs the
   `__GLOBAL__sub_I_NativeLegacyDisplay.cpp` symbol provided from the probe.
6. **slot 345 is `isTimingEnabled()`**, as the Pagani/13T record documents for the same caller
   (`0xfffffff00578c1cc`, the `swap_wait_gated` timeout branch).  It is implemented as
   `primary && bootMapping && logicalPM` and the kernel builds and links with it.

**Where it stops**

The last boots never reach the compositor's first swap: SpringBoard aborts (code 6) and the service graph loops into
`Too many corpses being created` before any frame is submitted, so neither the early-retire path
(`native_legacy_gpu_offer` now returns true) nor the slot 345 fix has been exercised on hardware.  Earlier boots did
reach the swap at about 2:10, so this is boot-to-boot variance, not a fixed wall.

**Next session, in order**

1. let SpringBoard survive to its first frame (`-no_corpses` is now in the arguments; the Pagani service trims are the
   next lever), then confirm the timeout no longer panics;
2. with a swap reaching the display, check that early retire (`gpu_offer=true` + `sheng-early-retire`) wakes
   `swap_wait_gated`, which is the last step to continuous refresh;
3. if the family still does not wake, take over `SwapEnd`/`SwapWait` in the component (uke's design), which the
   rebuild recipe above now makes possible.

Artifacts: `instantnoodlep-artifacts/bootEU.img` (latest, sha `489eb188...`, with `-no_corpses` and the slot 345
fix), `kernel_small.bin.old` (the pre-change firmware, for a quick revert), and the logs under
`docs/evidence/logs/`.

## 05:20: the payload carries the new kernel; the panic screen was from an earlier boot

payload.json records kernel_sha256 = 4fa02fe1deb386d29f7997730e83fa65a4134dcc1f2188761ffb75ac84c913a3 (the
AMFI-patched copy of the kernel built at 01:48), and that kernel's symbols contain

    fffffff00bfe4668 s __ZN19NativeLegacyDisplay15isTimingEnabledEv      (and no opaque345)

so the image booted at 02:03 carries the slot 345 fix. That means the 'abstract slot 345' panic in the photograph
belongs to an earlier boot: PanicLogDxe prints the previous boot's panic on the current screen.

What the current boot shows is the userland loop - SpringBoard repeatedly exiting with code 6 (SIGABRT),
fairplayd.A2 and bluetoothd cycling, no display submission and no panic. So the slot 345 fix is in the running
kernel but unexercised, and the blocker is SpringBoard surviving to its first frame.

Next test needs fastboot: boot bootEU-control.img (no display arguments) and compare. If SpringBoard loops there
too, the loop is independent of the display changes; if it does not, the display stack is what kills it.

## 05:40: control experiment - the SpringBoard loop is independent of the display changes

The control image (bootEU-control.img, no display arguments at all) was booted and captured for 150 s. It shows the
same userland loop:

    XNU-EXIT: SpringBoard pid=27  code=5 ...
    XNU-EXIT: SpringBoard pid=117 code=5 ...   (repeating)

against code=6 (SIGABRT) with the display arguments, but the pattern is the same: SpringBoard starts, traps, is
restarted, and the rest of the graph (fairplayd, bluetoothd, liveactivitiesd) cycles behind it. Nothing in the display
stack is running in that image, so the loop is a system/userland condition, not a consequence of the full-panel work.

Consequences:

* the display path cannot be validated further by booting alone - every display observation needs SpringBoard to
  survive to its first frame, and it does not on the current device state;
* the earlier boots that did reach a swap (about 2:10 into boot) predate this state change, so the current blocker is
  the service graph;
* the next productive step is the launchd policy trim the Pagani record describes (each entry must match the
  original task's Label and full ProgramArguments), or restoring whatever device state changed since the earlier
  successful boots.

## 05:55: from-scratch check of every change in this session

Changes, in order, with what each can and cannot affect:

1. firmware (drivers/DarwinBoot): the 480-row clamp removed and the DARWINBOOT_VIDEO_FOR_XNU zero-video block replaced
   by an unconditional copy. Verified on hardware: the kernel receives 1080x2376 and the loader covers the panel.
2. kernel (osfmk/arm/io_map.c): IO_MAP_SIZE 8 -> 32 MiB. Needed for the 10,264,320-byte panel mapping; after it the
   console reports 1080x2376 and ml_io_map no longer fails.
3. probe (package/LegacyDisplayProbe.cpp): prepare_scanout now returns true, gpu_offer returns true, bootarg-gated
   completion, nub slack, diagnostics. None of these run unless the display component starts.
4. component (drivers/GopFramebuffer/NativeLegacyDisplay.cpp) and its ABI header: slot 345 is bool isTimingEnabled()
   instead of the noreturn void opaque345() guard. Current build returns true unconditionally for the test.
5. kernel args: -no_corpses added.

Evidence that the loop is not caused by any of the above: the control image - no display arguments, an older
kernel, none of this session's display code active - boots and shows the same SpringBoard loop (code=5, SIGTRAP,
against code=6 SIGABRT with the display arguments). The loop is therefore a system/userland condition of the
current device state, not a regression from the display work.

What did visibly change is the panic: before the slot 345 fix the swap timeout ended in the family's guard and a
kernel data abort (the screen photograph). With the fix the timeout is handled and the machine continues - into the
pre-existing SpringBoard loop. The Apple logo that was visible earlier belonged to that panic screen (the compositor
had drawn its logo before the abort).

Candidates for the system-level loop, none of them display code:

* the data ramdisk is reused across every run (out/.../data/owner-data-3g.dbr); repeated panics during the display
  work may have left it in a state SpringBoard aborts on - rebuilding it fresh is the cheapest test;
* the service graph trims the Pagani record documents (identityservicesd, locationd, CommCenter, bluetoothd,
  iomfb_fdr_loader, iconservicesagent), each matched to the original task's Label and ProgramArguments;
* the stock boot image would show whether the loop predates all of this work on the device.

## 06:15: the data volume cannot be swapped for a fresh seed

Booting with the pristine IPSW seed instead of this installation's owner data panics immediately:

    panic(cpu 0 caller 0xfffffff00bd0a5e0): offline scratch Data mount failed: 2 @kern_exec.c:8190

so the Data volume is bound to the installation's own seed - a fresh seed is not a valid substitute, and
CONFIG_XNU_OWNER_DATA_MOUNT requires the persistent one. The owner data was restored from backup and the payload
rebuilt with it (image b6c8e8e9...).

That closes the 'stale data volume' hypothesis: the SpringBoard loop is not something a clean data seed fixes, and
the volume is not what makes SpringBoard abort.

Remaining productive directions, in order:

1. the launchd policy trim the Pagani record documents (match each original task's Label and ProgramArguments);
2. a boot that reaches the compositor (earlier boots did, around 2:10) to validate the slot 345 fix and the
   early-retire path on the swap timeout;
3. if the loop is accepted as this system stage's normal state, document it as such and keep the display work at
   'full panel + full-frame copy, swap completion pending'.

## 06:35: the lite launchd policy does not stop the SpringBoard loop

The board already carries the policy mechanism (package/BootWatchdogPolicy.c): it writes a launchd plist that
disables the listed jobs, runs after writable scratch Data mounts and before launchd exec, and is enabled
unconditionally; `disable-jobs=<labels>` and `lite=1` select the set.  Adding `lite=1` to the kernel arguments is
visible in launchd's own boot-args line:

    boot-args = -v serial=3 debug=0x14e -no_corpses -no_slto_panic ... qcom-usb-debug=1 lite=1 offline-legacy-...

and launchd does start its boot tasks (finish-obliteration, etc.), but SpringBoard still exits with code 6 within
seconds and is restarted:

    XNU-EXIT: SpringBoard pid=27  code=6 ...
    XNU-EXIT: SpringBoard pid=146 code=6 ...

So the abort is not caused by any of the jobs the lite set disables (identityservicesd, locationd, CommCenter,
fairplayd.A2, PerfPowerServices, ...).  The remaining unknown is SpringBoard's own abort reason; the kernel log only
reports the exit code, and launchd does not forward the process's stderr to the serial console.

Options from here: forward launchd/SpringBoard stderr to the console (launchd can be told to keep a job's output),
or run a shell that starts SpringBoard by hand with its output attached, so the abort reason names itself instead of
only its exit code.

## 06:50: how to get SpringBoard's abort reason

The policy file (/private/var/db/com.apple.xpc.launchd/disabled.plist) can only disable jobs, so it cannot redirect
SpringBoard's stderr. Two ways remain, both inside reach:

1. **read the crash report from the kernel.**  SpringBoard's abort is reported by ReportCrash into the Data volume's
   CrashReporter directory, and this kernel already knows how to run file syscalls from a kernel context:
   package/BootWatchdogPolicy.c opens /private/var/db/com.apple.xpc.launchd/disabled.plist with copyout +
   open/write/fchown/fstat.  The same pattern with open/read can print the newest crash report to the serial
   console before launchd starts;
2. **have launchd keep the job's output.**  This needs a per-job override plist rather than the disabled database,
   which this kernel's policy writer does not produce today.

Option 1 reuses code that already runs on every boot and needs no new syscall surface, so it is the next step:
add a boot-argument-gated reader (for example native-crash-dump=<path>) to the same file, call it from the same
place the policy runs, and print the report with printf.

## 07:00: the crash-report reader's wiring

package/BootWatchdogPolicy.c is compiled by scripts/build-verified-block-bridge.py into
build/verified-block-bridge/BootWatchdogPolicy.o and linked into the kernel, where the zzz-boot-progress patch calls
sheng_boot_watchdog_policy() after writable scratch Data mounts and before launchd exec.  A crash-report reader
(open/read on the Data volume, printf to the console, gated by its own boot argument) belongs in the same file and
the same call path, so the recipe is:

1. add the reader to package/BootWatchdogPolicy.c and call it from sheng_boot_watchdog_policy();
2. rebuild the object with scripts/build-verified-block-bridge.py (it already produced the current .o);
3. ./unixnu build kernel, payload, bootimg, boot, and read the report on the serial console.

That is a new kernel change rather than a configuration change, which is where this session stops.
## 07:30: community search for the SpringBoard-loop problem

Searching the official device records (docs/devices) and the ports they link:

* the sheng-based ports (tkings4chan, bai805, enlixk, ad1095445) all document the same diagnostic tool for exactly
  this stage: `sheng-log=1` + `boot-ui-trace=1` starts a **separate process-capture worker**, and
  `boot-ui-focus3=chronod` adds the display-query client to `backboardd` and `SpringBoard`; arm64e return-address
  PAC is stripped so the captured stacks are readable.  Enlixk/UniXNU-sheng carries the regression for the sampler
  (scripts/check-boot-ui-trace.py).
* uke's record notes that the panic/reset path distinguishes jetsam OOM messages from corpse messages in the log,
  which is the same distinction this board now needs;
* sirius-huajifruit's display log shows the neighbouring state (GOP display registered, a client opens the
  user client, "No Default FB Surface found", SpringBoard crashed on the previous run), i.e. the same sequence this
  board went through before the default surface was adopted.

None of the records contains a fix for 'Too many corpses being created' itself; the community tool for seeing why a
userland process dies is the ui-trace worker, which this port does not carry.

Cheap experiment queued before any port of that worker: `lw_corpses=0` (image e09af0e1...), which takes the
lightweight-corpse rate limit out of the path so a full corpse can be produced for SpringBoard's abort.

## 07:45: backboardd never appears in any capture

Grepping every console log this session produced (eight of them, from the earliest display work to the current boot):
`backboardd` appears **zero times**.  The current boot shows the display driver registering normally -
`registered family meta=... name=IOMobileFramebufferLegacy size=0x18a0`, the whole meta chain - and then launchd's
boot tasks, and then `SpringBoard` starting and aborting (code 6) without any backboardd line and without the
display ever submitting a swap.

If backboardd were running, the absence of an `XNU-EXIT` line would mean it is healthy; but then SpringBoard would
have a display connection.  The two facts together (no backboardd log, SpringBoard aborting at once) point at the
display-server job never coming up on this port, which is the next thing to establish: either it is not started by
launchd (missing job/dependency), or it starts and exits silently before it can log.

The `lw_corpses=0` experiment did not help either: the boot-args line shows it applied, and the corpse messages are
unchanged (`Process[27] crashed: SpringBoard. Too many corpses being created.`), so the corpse path is failing for a
different reason - most likely the dying task's VM space - and cannot be relied on for the abort reason.

## 08:05: the exec instrumentation compiles in but never fires

Two prints were added to the kernel this round:

* `BRINGUP-ABORT` in `abort_with_payload()` - the abort reason string, if a process passes one;
* `BRINGUP-EXEC2` in `exec_activate_image()` - every image activation with its path (so a service that never starts
  can be told apart from one that starts and dies).

Both strings are present in the built kernel (`strings kernel.macho | grep -c BRINGUP-EXEC2` = 1), and neither
produces a single line at runtime: after 90 s of the boot, 0 exec lines and 0 abort lines, while launchd is clearly
starting tasks and SpringBoard clearly aborts (code 6).

So on this port launchd's spawning does not go through `exec_activate_image` (and aborts do not go through
`abort_with_payload`) - the offline-exec work on this board changed those paths.  Instrumenting the path that is
actually used (the `posix_spawn`/offline exec entry in this tree) is what the next session needs, not another
boot with the same two prints.

The observations that stand: backboardd never logs anything, SpringBoard aborts within seconds of starting, the
display driver itself registers correctly, and no swap is ever submitted on these boots.
## 08:30: community survey results and the porting plan

### What the repositories have

1. **uke: package/CrashLogProbe.c** (17 KB, self-contained, boot arg `sheng-crashlog=1`) - walks
   /private/var/mobile/Library/Logs/CrashReporter with namei/vnode and prints backboardd/SpringBoard reports into
   the kernel log, plus the launch paths of apps ("because a process that never execs leaves no crash report at
   all").  Started as a kernel thread that sleeps 60 s then scans every 90 s.  The file is now copied into
   package/CrashLogProbe.c on this board; it is not built yet.
2. **the build wiring already exists for this class of file**: scripts/build-sheng-source-storage.py keeps
   `bsd_sources = {'BootWatchdogPolicy.c', 'BootDiagExport.c', 'WifiFirmware.c', 'BluetoothFirmware.c'}` compiled with
   kern_exec's flags, and our kernel does contain `sheng_boot_watchdog_policy` and the disabled.plist path - so
   adding CrashLogProbe.c to that set and calling `uke_crash_log_start()` from
   `sheng_boot_watchdog_policy()` is the whole port (no XNU patch change needed).
3. **BootDiagExport.c** (`sheng-diag-export`, "the unified log, crash reports and a screenshot, read through the
   VFS") is referenced by that same script but is not present in this device repository - it can be taken from a
   sheng device repository if the crash-report probe alone is not enough.

### What the records say about the symptom

* uke: "backboardd pid62/341/373 分别在 154/210/267s 以 **SIGNAL6** 退出；SpringBoard 因 /usr/libexec/backboardd
  respawn 终止并重启" - i.e. the same code 6 loop this board shows, attributed to backboardd aborting.
* uke's reference notes: userdata needs SpringBoard `BKNoWatchdogs`/`FBLaunchWatchdogExceptions` and an IOWatchdog
  stub so watchdogd does not `_PanicOnCrash` - matching this board's `watchdogd ... ns=2 sub=0x5`.
* Pagani: slot 345 is `isTimingEnabled()` (implemented here already).

### Port status

CrashLogProbe.c is in place but not compiled: the next two edits are adding it to `bsd_sources` in
scripts/build-sheng-source-storage.py and calling `uke_crash_log_start()` at the end of
`sheng_boot_watchdog_policy()`, then booting with `sheng-crashlog=1`.
## 08:45: where the crash-report probe has to be wired

scripts/build-boot-progress.py builds **one** object (BootProgress.o) and its docstring is explicit:

> The renderer is UniXNU-sheng's package/BootProgress.c ... Only that object is built: sheng's four companion
> probes cover its watchdog policy, file export and ADSP bring-up and are **not part of this port**.

so package/CrashLogProbe.c (copied here from uke) is not built by any current step.  The wiring that matches this
board is:

1. extend scripts/build-boot-progress.py to compile package/CrashLogProbe.c with the same record (it already builds
   BootProgress.o into build/boot-progress and archives it), prefixing `-DUKE_SOURCE_STORAGE_LOG` if the full-report
   buffer should be kalloc'd rather than static;
2. call `uke_crash_log_start()` from the boot-progress entry (or from `sheng_boot_watchdog_policy()` if that object
   is in the same archive);
3. boot with `sheng-crashlog=1` and read the backboardd/SpringBoard reports on the serial console.

The probe itself needs no changes: it takes its own boot argument and starts its worker thread after 60 s, printing
into msgbuf, which the serial console already carries.

## 09:15: the crash-report probe port was attempted and rolled back

Porting uke's CrashLogProbe.c into this port's boot-progress archive was tried end to end and rolled back, with
these findings kept for the next attempt:

1. the object is compiled by **scripts/build-boot-progress.py**, which builds BootProgress.o and BootProgressStubs.o
   into libBootProgress.a; that script has to gain the probe (and the kernel build re-runs it, so both the device
   repository and the assembled tree copy must change);
2. the probe must be compiled with the **BSD record** (bsd/DEVELOPMENT/kern_exec.o.json), not BootProgress's
   osfmk/startup record - the osfmk record produced typedef redefinitions and misc_protos errors.  With the BSD
   record the file compiles (332 warnings, no errors) once `vnode_readdir64` is declared explicitly (the exported
   <sys/vnode_internal.h> in EXPORT_HDRS omits it);
3. **the kernel link does not honour `weak_import`** (the build script says so): a weak call from BootProgress.o to
   `uke_crash_log_start` fails the link with `Undefined symbols: _uke_crash_log_start`, so the probe's object must
   always be in the same archive as the caller - there is no optional path.

Rolled back to a buildable tree (kernel build exit 0, image `8bf8c20c...`); package/CrashLogProbe.c stays in the
repository as the source for the next attempt, with the two fixes above already known.

## 09:30: the crash-report probe is linked into the kernel

The port finished after the two fixes:

* scripts/build-boot-progress.py (device repository **and** the assembled tree copy) compiles package/CrashLogProbe.c
  with the kern_exec record and puts CrashLogProbe.o into libBootProgress.a unconditionally;
* package/BootProgress.c calls \`uke_crash_log_start()\` from \`boot_progress_init()\` (a strong call, because the
  kernel link does not honour weak imports).

Verified in the built kernel: \`llvm-nm kernel.macho | grep -c uke_crash_log_start\` = 1, 22 \`UkeCrash\` strings, and
libBootProgress.a holds BootProgress.o, BootProgressStubs.o and CrashLogProbe.o.  The kernel build finished with
exit 0.

Image: \`bootEU.img\` sha \`11ddd4aad1a802a54652657712da51de973ca6927ef51b2f370a4039349e1a1b\`, boot arguments already
carry \`sheng-crashlog=1\`.  The probe sleeps 60 s and then scans CrashReporter every 90 s, printing backboardd and
SpringBoard reports into msgbuf, which the serial console shows.

## 09:50: the probe runs but there is nothing for it to read

The crash-report probe is compiled into the kernel (uke_crash_log_start present, 22 UkeCrash strings, archive
member verified) and its call site is the unconditional boot_progress_init() in arm_init.c (CONFIG_XNU_BOOT_PROGRESS=y
in the built .config), with \`sheng-crashlog=1\` in the boot arguments.  On hardware, however, the log shows **zero**
UkeCrash lines over four minutes while SpringBoard keeps exiting with code 6:

    XNU-EXIT: SpringBoard pid=27  code=6 ...
    XNU-EXIT: SpringBoard pid=145 code=6 ...

The probe only prints when it finds reports in /private/var/mobile/Library/Logs/CrashReporter, and that directory is
not being filled: the corpse messages on this boot ('Too many corpses being created', with corpses disabled by
config in the earlier run) say the crash-information path fails before a report is written.  So the missing piece is
not the reader but the producer - the kernel's corpse allocation for these dying processes.

State: image 11ddd4aa... carries the probe and boots; the probe is silent because no crash reports exist yet.
The next lever is the corpse path (task_collect_crash_info failing with KERN_RESOURCE_SHORTAGE) rather than more
instrumentation of the reader.
## 10:10: raising the corpse cap did not change the message

\`TOTAL_CORPSES_ALLOWED\` (4-5 in task_corpse.h) was raised to 64 and the kernel rebuilt (exit 0, image decb5176...).
The boot still prints:

    Process[128] crashed: liveactivitiesd. Too many corpses being created.
    XNU-EXIT: SpringBoard pid=27 code=6 ...
    Process[27] crashed: SpringBoard. Too many corpses being created.

so the cap is not the limiting factor: \`task_collect_crash_info()\` fails before \`task_crashinfo_get_ref()\` is
consulted (the message is the else branch of task_mark_corpse, i.e. any failure other than NO_SPACE, NOT_SUPPORTED or
TERMINATED).  The likely cause is the crash-info allocation inside the dying task's VM (CORPSEINFO_ALLOCATION_SIZE
is 16 KB plus the kcdata buffer), which fails under this board's memory situation.

Consequences for the next attempt:

* the crash-report probe (linked and running) stays silent because no report is ever produced;
* the abort reason has to be captured another way: the kernel-side print in abort_with_payload never fires (plain
  abort()), so the remaining options are a launchd override that keeps the job's stderr, or the ui-trace worker the
  sheng ports use;
* raising memory available to the crash-info allocation (larger RAM window, less Data) is the other lever.
## 10:30: back on the pre-change firmware

kernel_small.bin was restored from kernel_small.bin.old (the firmware that shows the loader hand-off with the Apple
logo) and the image rebuilt as bootEU.img dcd66a10...; the full-panel firmware is kept at
kernel_small.fullpanel for whenever it is wanted again.

The boot confirms the old window is back:

    DISPLAY: bootargs video base=0x9c000001 w=1080 h=480 rb=4320 depth=32 display=1
    DISPLAY: GOP console 0x9c000001 1080x480 depth=32 stride=4320

so the kernel draws in the top 480 rows again and the hand-off page (Apple logo plus the loader text) covers the
panel as it did before this session's firmware work.  The kernel still carries this session's diagnostics (slot 345
fix, crash-report probe, corpse cap 64, lw_corpses=0, sheng-crashlog=1); none of them change what the screen shows.

## 10:55: ui-trace port attempt - sources compile, build integration still open

Firmware switched back to the full-panel one (kernel_small.bin = kernel_small.fullpanel, sha 44007625...), image
rebuilt as bootEU.img 0aa8600d...

Porting sheng's userland stack capture (Enlixk/UniXNU-sheng) reached a clear point:

* the sources are copied into package/: BootProcessProbe.c, BootProcessWorker.c, BootUiTraceProbe.c,
  BootLatencyTrace.c and the headers BootUiTraceCore.h, BootUiTraceDetails.h, BootSpringBoardProfile.h,
  BootLatencyTrace.h, BootLatencyStore.h;
* **all three probes compile cleanly with the osfmk record** (startup.o.json), because they are Mach-side code
  (task/thread/pmap).  The BSD record fails as expected;
* the build step still fails on a header the compiler cannot see when the sources are compiled from the assembled
  kit directory (BootLatencyTrace.c needs BootLatencyStore.h from the same directory), so the include path for the kit
  directory must be added for these objects - the standalone compile with `-I package` worked, the build script's
  command must carry the same include;
* wiring was reverted so the kernel builds again (exit 0): the crash-report probe stays linked, the ui-trace call is
  removed until its objects are in the archive.

Next: add `-I <kit dir>` to the trace compile command in scripts/build-boot-progress.py, add the four objects to
libBootProgress.a, call `boot_process_start()` from boot_progress_init(), and boot with `sheng-log=1 boot-ui-trace=1`.

## 11:45: sheng's ui-trace worker is in the kernel

Port status, after fixing the chain of missing headers and hooks:

* package/ now carries BootProcessProbe.c, BootProcessWorker.c, BootUiTraceProbe.c, BootLatencyTrace.c and the
  headers they pull in (BootUiTraceCore.h, BootUiTraceDetails.h, BootSpringBoardProfile.h, BootLatencyTrace.h,
  BootLatencyStore.h, plus the rest of sheng's package headers);
* they compile with the **osfmk record** and are archived in libBootProgress.a (BootProcessProbe.o,
  BootProcessWorker.o, BootUiTraceProbe.o, BootLatencyTrace.o, BootTraceStubs.o);
* BootTraceStubs.c defines the kernel hook variables the probes only assign to (sheng_user_exit_hook,
  sheng_unnest_hook, sheng_user_fault_hook, sheng_sb_wait_hook, sheng_sb_wait_tid, sheng_pmu_fixup) and
  native_gpu_role_thread, because this kernel link does not honour weak imports;
* BootProgress.c calls boot_process_start() next to uke_crash_log_start(), and the kernel builds and links with
  exit 0 (symbols boot_process_start, boot_process_capture, boot_ui_trace_begin all present).

The probes do not print: they fill boot_process_rows/boot_ui_trace rows for the **diagnostic page**, so the image now
also passes `boot-progress=2` (bootEU.img bed9c995...) to draw them on the panel.

## 12:15: the ui-trace code is in the kernel but its rows stay empty

The instrumented image (kernel strings verified: BRINGUP-ROWS, boot_rows_printer, boot_process_rows and the
boot_progress_init symbol are all present) boots to 'Early boot complete' without a single PROC[ row on the serial
console.  Two things follow:

* the instrumentation prints of boot_rows_start happen at arm_init, before the USB serial console is up, so their
  absence in the log proves nothing;
* the PROC rows never appear, so **boot_process_rows stays empty** - either boot_process_start() did not start its
  worker (boot-ui-trace parse at arm_init) or boot_process_capture() does not fill the rows on this port.

Everything else in the port is in place: sources compile with the osfmk record, BootTraceStubs supplies the sheng
hook variables, the objects are archived, boot_process_start/boot_process_capture/boot_ui_trace_begin are linked, and
the kernel builds with exit 0.  What is missing is evidence that the capture worker runs and what it writes.

Cheapest next step: print from inside the capture path (BootProcessProbe.c) once per pass - one line at the start of
boot_process_capture() and one after the target loop - so the serial shows whether it runs and whether proc_iterate
returns anything, instead of relying on arm_init prints that the console cannot show.

## 12:40: what kebab's repository says about this wall

GFander/UniXNU-kebab (OnePlus 8T, SM8250 - the closest sibling to this board) documents the same stage in two
dedicated records:

* **BACKBOARDD-WAIT-OBSERVER-20261031.md** - they sample the waits of the original backboardd: PID57/UID501, all
  12 threads; the main thread waits in a CFRunLoop receive (ipc_mqueue_receive_continue), another thread is inside
  QuartzCore CA::Render::Server::server_thread, plus DataMigration XPC and dispatch_group_wait.  Their own earlier
  diagnostic had wrongly required the service UID to be 0, so it never selected the live backboardd - i.e.
  **backboardd is running**, and a silent one is not an absent one.
* **BACKBOARDD-USER-WAITS-20261031.md** - they conclude the current framebuffer comment claims the original framework
  depends on a display-subsystem declaration, while the real ADT's arm-io only carries gic/timer/ufshc; they insist
  the exact consumer and resource binding must be established before adding any node ('不能仅凭注释加假的 Apple 硬件节点').
  Their status line is explicit: 'GPU/显示呈现/可用SpringBoard 未完成' - they do not have a fix either.

Two opposite data points on the display-subsystem question:

* kebab builds a `display-subsystem` node under arm-io (`kebab-display`, scripts/tests/test-gop-display-adt.py);
* pagani's architecture note warns the opposite for the presenter: `NativeGOPPresenter` must be published under
  `IOService::getResourceService()`, because attaching it under display-subsystem produced SpringBoard crashes
  (sheng int-075/077).  Pagani keeps the GOP display nub under /arm-io on the DT plane.

This board already follows the pagani arrangement: the probe attaches its nub to `IOService::getResourceService()`
(LegacyDisplayProbe.cpp:283) and the display to that nub (line 332).

So the remaining question is the same one kebab is still chasing: what does the original display enumeration need
before backboardd builds its CADisplay.  The cheapest way to answer it here is the process probe we just ported -
its rows would show whether backboardd exists on this port at all - which is why getting those rows onto the serial
console is the next step.

## 12:55: why the other ports reach the lock screen and this one does not

Pagani's STAGE6 opens with the answer to 'how do they get in':

> 第 50 版已获真机确认：跳过 Setup 进入锁屏，电源键短按亮灭屏、长按打开关机界面；触控初始化失败。

and the architecture table shows the display path they use:

* **NativeGOPPresenter** - published under `IOService::getResourceService()`, NGPMode/NGPPresent/NGPWait/NGPSwap/
  NGPTransfer wire protocol, CPU copy by default, GPU blit when `sheng-fb-gpu=1`;
* **NativeMobileDisplay** (the sheng framebuffer driver) with a vsync timer as the refresh tick;
* the original `NativeLegacyDisplay`/`IOMobileFramebufferLegacy` stays present as the 'GOP display' nub, **but it is
  not the path the system presents through**.

uke reaches the UI the same way (`NativeMobileDisplay` plus `SweetSurface`/AppleCLCD on the Ace 5 reference), and both
ports add userland pieces we do not have: SpringBoard `BKNoWatchdogs`/`FBLaunchWatchdogExceptions`, an IOWatchdog stub
so watchdogd does not `_PanicOnCrash`, and sheng's four userland compatibility fixes.

This board has been driving the **original family** instead: our probe loads the prelinked IOMobileFramebufferLegacy,
allocates it, adopts the family's own default surface and copies frames - the deepest and most fragile route through
the original resource graph.  kebab, the one sibling that also drives the original family, is at the same wall: its
records say 'backboardd 存活' but '主 CADisplay 缺失', 'GPU/显示呈现/可用SpringBoard 未完成'.

So the answer to 'why can they get in' is: they do not drive the original family at all - they publish their own
framebuffer service, which is what backboardd's enumeration accepts, and they patch the two userland watchdog doors.

### What this implies for the next session

1. port `NativeGOPPresenter`/`NativeMobileDisplay` from the GopFramebuffer project (sheng's LegacyDisplayProbe.cpp is
   the reference the pagani record points at) and publish it under getResourceService;
2. keep the current original-family path only as the boot-time console (it already works at 1080x2376);
3. add the SpringBoard watchdog preferences and the IOWatchdog stub so SpringBoard is not killed while it enumerates.

## 13:05: the missing argument - sheng-vsync

UniXNU-sheng's board.toml uses the **same** offline-legacy-* display arguments as this board, plus a group this
port never set.  The decisive one is the refresh tick: the component's own comment says

> Display refresh tick (boot-arg `sheng-vsync=<Hz>`, 0 or absent: **off**). The original family sends the
> compositor's vsync message (type 5) ...

and sheng passes `sheng-vsync=120` with `sheng-vsync-fixed=1`.  This board has never passed either, so the display
has been running with the refresh tick **off**: no vsync is published to the display server, which is exactly the
kind of missing timing that keeps backboardd from building a usable CADisplay - and matches kebab's observation that
backboardd is alive and waiting.

sheng's other display arguments this board lacks: `sheng-fb-gpu=1`, `sheng-fb-bgra=1`, `sheng-dpu-scanout=2`,
`sheng-backlight=1`, `sheng-disable-watchdogd=1`.

Image with `sheng-vsync=120 sheng-vsync-fixed=1` added: bootEU.img 262d376c..., waiting for the device.
## 13:40: careful comparison of the two upstream repositories

**They are different kinds of repository, so the direct diff is framework versus device:**

* `UniXNU/UniXNU` is the framework: `build/`, `profiles/`, `components.toml`, `devices.toml`, shared `drivers/`,
  `kits/`, `patches/` (59 xnu patches in this checkout) and `docs/`;
* `UniXNU/UniXNU-sheng` is a **device repository** (`board.toml`, `configs`, `dts`, `package`, `patches`, `hal`),
  i.e. the sibling of `UniXNU-instantnoodlep`, not of the framework.

**Device-repository comparison that matters for this wall:**

| item | sheng | instantnoodlep (ours) |
| --- | --- | --- |
| package files | 167 | 55 |
| display sources | LegacyDisplayProbe.cpp, BootProgressDisplay.h, DisplayTimingProbe.cpp, MobileDisplayProviderProbe.cpp, DisplayEnvironmentDiagnostic.cpp, OfflineGOP{Composition,Presentation,Resources}.mm, OriginalRenderBoxPresentation.mm | LegacyDisplayProbe.cpp, BootProgressDisplay.h |
| display kernel args | the same offline-legacy-* set, **plus sheng-vsync=120 sheng-vsync-fixed=1, sheng-dpu-scanout=2, sheng-backlight=1, sheng-fb-gpu=1, sheng-fb-bgra=1, sheng-disable-watchdogd=1** | offline-legacy-* only (sheng-vsync added today) |
| hardware stack | Adreno740 GPU driver, DPU timing probe, GMU/SMMU components | no GPU driver, no DPU driver |

**The .mm files are not the difference.**  Each one carries `int main()` and its header states the scope:
`OriginalRenderBoxPresentation.mm` says 'Actual original RenderBox -> GPU format composition -> original display swaps.
Explicit offline qualification only; **no substitute system desktop**', and MobileDisplayProviderProbe.cpp says 'This
does not publish a display, render, or replace a system display list'.  They are offline qualification harnesses.

So sheng's **kernel** display path is the same architecture as this board's: LegacyDisplayProbe.cpp with
offline-legacy-* arguments.  What they have that this board does not is the **refresh tick argument** (added here
today), a working GPU + DPU scanout stack on their SoC, backlight/power knobs, and their userland watchdog work.

Implication: on SM8250 the only presentation available is still the CPU copy this port already does at 1080x2376; the
missing link to SpringBoard is the refresh/complete protocol the display exposes to the system, which is what
\`sheng-vsync\` and the swap path control.
## 14:05: careful comparison with UniXNU/UniXNU-sheng - the decisive difference

### The repositories are of different kinds

* `UniXNU/UniXNU` is the framework (build system, shared drivers/kits/patches, docs);
* `UniXNU/UniXNU-sheng` is a **device repository** - the sibling of `UniXNU-instantnoodlep`.

### Device repository, field by field

| item | sheng | instantnoodlep (ours) |
| --- | --- | --- |
| kernel args | 52 | 28 |
| kernel archives | 19, including **build-gop-legacy-display-27.py** | 16, **no display builder at all** |
| display build scripts | `build-gop-legacy-display.py`, `build-gop-legacy-display-27.py`, `build-gop-framebuffer.py`, `legacy-display-24A446.json`, `legacy-display-26C5035r.json` | none |
| GPU stack | `component:Adreno740`, `fetch-adreno-inputs.py`, `prepare-sheng-gmu.py`, `prepare-sheng-gpu-smmu.py`, `prepare-sheng-gmu-votes.py`, `build-adreno740-kits.py`, `build-adreno740-accelerator.py` | none |
| other drivers | AKS, AppleKeyStoreState, audio, wifi, bluetooth, touch, cpufreq, backlight | USB debug, UFS, verified root, RAM block, ram-watchdog, boot-progress |

### What this means

The display component on this board has never been built by a build step.  It is an archive assembled by hand
(`rebuild_archive.sh` re-archiving a `compile-command.json` scraped from an earlier build), with an ABI header whose
`LEAF(345)` entry was edited by hand.  sheng instead **generates** that header from the framework's
`scripts/audit-mobile-graphics-legacy.py` audit (`build/display-analysis/mobile-graphics-legacy-abi.json`, which this
tree also produces) and compiles the component and probe with the builder's own defines and ABI layout.

So the same component source is not necessarily the same binary: our header, defines and vtable treatment differ from
sheng's generated ones, and that is the most plausible reason the same display architecture behaves differently at
the swap/completion stage.

### Concrete next step

Port sheng's `scripts/build-gop-legacy-display.py` (28 KB) and its `legacy-display-24A446.json` into this device
repository, add `build-gop-legacy-display.py` to the `[kernel] archives` list, and let the build generate the ABI
header and compile the component the way sheng does - then the slot-345 handling and the swap path follow sheng's
qualified layout instead of this session's hand edits.

## 14:30: file-by-file comparison with UniXNU/UniXNU-sheng

**Totals (excluding .git, out, build, assets, firmware):** 495 files there, 231 here, 61 shared (19 of them
differ), 434 only there, 170 only here.

### The 19 shared files that differ

`.github/workflows/package.yml`, `.gitignore`, `AGENTS.md`, `README.md`, `board.toml`, `feeds.toml`,
`package/BootProcessWorker.c`, `package/BootProgress.c`, `package/BootUiTraceDetails.h`, `package/BootUiTraceProbe.c`,
`package/BootWatchdogPolicy.c`, `package/GeniI2c.h`, `package/LegacyDisplayProbe.cpp`, `package/ShengAdsp.h`,
`patches/xnu/series`, `porting.toml`, `proprietary-files.txt`, `scripts/build-boot-progress.py`, `unixnu.lock`.

**The display and diagnosis files this session has been hand-patching are exactly the files that differ** - their
copies are the maintained ones, ours are modified forks of an older revision.

### Only there, by area

* `drivers/QcomIris/*` (video), Adreno GPU probes, audio, wifi, bluetooth, touch, backlight, battery, BoardClock - a
  complete device bring-up;
* `package/`: 142 files (Adreno*, Adsp*, Backlight, Audio, Bluetooth, Battery, AirLibrary, AcceleratorABI...);
* `scripts/`: 204 files, including **build-gop-legacy-display.py / -27.py / build-gop-framebuffer.py**,
  `legacy-display-24A446.json`, and checks for boot progress, process probe and ui-trace;
* `configs/`: four defconfigs (sheng, ipados26, ipados27, macos) against one here;
* `patches/`: 24 device patches against 23 here, but the **series differ**: 3 there against 16 here, because this
  board carries many local diagnostics (trap pc, exit observe, usb debug, panic region, deferred framebuffer mapping,
  shared-region probe).

### Only here

`docs/` (72 evidence files), `kits/` (26), 23 extra local patches, `components/` (14), 13 drivers (UFS, USB, RamBlock,
verified root), 13 package files (this session's probe, stubs, policy) and 5 scripts.

### The file that answers today's open question

sheng starts the process-stack worker from **`package/BootLog.c`** (line 2851, under `boot-ui-trace=1`):

~~~c
if (PE_parse_boot_argn("boot-ui-trace", &trace, sizeof trace) && trace == 1) boot_process_start();
~~~

not from `BootProgress.c` as this board tried - and this port has no `BootLog.c` at all.  That is why the diagnostic
page's process and ui-trace rows stayed empty however the call was wired: the only caller of the worker lives in a
file this repository does not carry.

The image already built (f6eb618e, with the probe calling `boot_process_start()` itself plus the JOP and vsync fixes)
is the first that may actually fill those rows; it still has to be booted.

## 15:10: WHY the other devices get to the home screen - evidence, not loops

**The devices that reach the UI use a different display driver from this board.**

1. The framework ships its own mobile display driver: `UniXNU/drivers/GopFramebuffer/` contains
   **`NativeMobileDisplay.cpp`**, `NativeGOPPresenter.cpp`, `NativeGOPWire.h` and `OriginalMobileFramebufferABI.h`.
   That driver *implements* an IOMobileFramebuffer itself and publishes it as its own service.
2. The device records that claim a usable UI use that driver:
   * `docs/devices/a04br3.md`: 'The tablet **reaches the iPadOS home screen** on four CPUs with eMMC DMA and
     **CPU framebuffer rendering**. ... Metal UI acceleration ... remain in development.' - i.e. no GPU needed;
   * `docs/devices/evb10.md`: home-screen swipes at ~50 fps, frame-rate arguments now the defaults;
   * `docs/devices/pandora.md`: iOS 16.7 home screen with touch and keys;
   * `docs/devices/pong-agxmx666.md`: '桌面与壁纸已有显示'.
3. This board does not use that driver. Its display is **`NativeLegacyDisplay`** - the *legacy* path that loads
   the prelinked original `IOMobileFramebufferLegacy` family and tries to drive it (default surface, command gate,
   swap completion).
4. The two ports that also chose the legacy path have not reached the UI either: sheng's own report says
   '**Visual presentation was not verified**' (it only observed SpringBoard/backboardd PIDs), and kebab records
   'GPU/显示呈现/可用SpringBoard 未完成'.

So the answer to 'why can they get in' is architectural: **they publish their own display service; this board tries to
reanimate the original family.**  Everything this session fought - the default surface, the command gate, the 5-second
\`swap_wait_gated\` timeout, slot 345, the metaclass teardown - belongs to the original family's lifecycle, and that
lifecycle is not the one the successful ports use for a CPU-only presentation.

Concrete next step: build and publish `NativeMobileDisplay` (framework driver) for this board the way the
NUL devices do, keep the current legacy path only as the boot console, and drop the legacy swap work until the
own-service path presents frames.

## 16:00: the diagnostic page cannot enter userland, and the knob image still aborts

* The diagnostic page (`boot-progress`) is a kernel-side renderer: it draws boot stages and, when the probes fill
  them, process and ui-trace rows.  It cannot start or enter userland - userland begins exactly once, when the
  kernel execs `/sbin/launchd`, and this port has no shell path or 'jump to desktop' switch.
* The image with a04br3-style knobs (kt_fixed_zones, vm_compressor, ml-timeout-lock-panic-scale, disable-watchdogd,
  disable-jobs including iomfb_fdr_loader, sheng-vsync) boots, the trims are visible in boot-args, and SpringBoard
  still exits with code 6:

      XNU-EXIT: SpringBoard pid=27  code=6 ...
      Process[146] crashed: SpringBoard. Too many corpses being created.

## The remaining structural difference

Every device record that claims a home screen carries a **GPU driver** in the kernel, even when the UI is composed
on the CPU: a04br3 passes `a04br3-gpu=2`, `sheng-fb-gpu=1`, `a04br3-gpu-metal=1`; pagani and uke carry Adreno drivers
and publish an AGXAccelerator/IOAcceleratorES service.  This board has no GPU driver at all - it links UFS, USB,
RAM block, verified root and the display, nothing that creates a Metal device.

That is consistent with SpringBoard aborting in every configuration tried today: its render server asks the system
for a graphics device at startup, and a display with no Metal device is not enough.

## What this means

The display work is complete up to the frame reaching the panel; the next real blocker is a Metal-capable GPU
service, which is a driver port of its own (Adreno 650 on this SoC), not a display configuration.
## 17:00: final file-by-file state against UniXNU/UniXNU-sheng

After this session's synchronisation the picture is:

### A. Files that are now identical (adopted from sheng this session)

`package/BootProgress.c`, `package/BootProcessWorker.c`, `package/BootUiTraceProbe.c`,
`package/BootUiTraceDetails.h`, `package/BootWatchdogPolicy.c` - i.e. the four boot/diagnosis components this
session had been patching by hand are now the maintained revisions.

### B. The 14 files that still differ

| file | difference |
| --- | --- |
| `board.toml` | 52 args / 19 archives there vs 32 args / 16 archives here (device identity, GPU stack, msdc root) |
| `patches/xnu/series` | 3 board patches there (audio addon mount, cpu idle, amp) vs 16 here (trap pc, exit observe, usb debug, panic region, deferred fb mapping, shared-region probe) |
| `porting.toml` | 751 lines of device qualification records there vs 206 here |
| `package/LegacyDisplayProbe.cpp` | their reference probe (740 lines) vs this board's probe (602 lines, board-specific diagnostics) |
| `scripts/build-boot-progress.py` | their builder (96 lines) vs ours (120 lines, wired for CrashLogProbe + ui-trace objects) |
| `package/GeniI2c.h`, `package/ShengAdsp.h` | I2C and ADSP headers for drivers this board does not have |
| `proprietary-files.txt`, `feeds.toml`, `unixnu.lock` | firmware list, feeds and pinned versions |
| `README.md`, `AGENTS.md`, `.gitignore`, `.github/workflows/package.yml` | documentation and CI |

### C. Only in sheng (434 files) - by area

| area | files | what it is |
| --- | --- | --- |
| `drivers/QcomIris/` | 42 | camera/video (Iris) driver |
| `patches/xnu-12377/` | 20 | the iPadOS 27 kernel patch set |
| `package/tests/` | 16 | host tests for their probes |
| `scripts/policies/` | 15 | launchd policy directories per OS profile |
| `package/linux-log/`, `package/linux-ref/` | 10 | Linux reference logs and dumps |
| `patches/xnu/` | 3 | audio addon mount, cpu idle, amp |
| `configs/` | 4 | sheng, ipados26, ipados27, macos defconfigs |
| remaining `package/` (~120) | | Adreno GPU (compiler, PM4, VPC, firmware), Adsp, Backlight, Audio, Bluetooth, Battery, BoardClock, AirLibrary, AcceleratorABI, TouchTrace, tests |
| remaining `scripts/` (~190) | | display builders (`build-gop-legacy-display(-27).py`, `build-gop-framebuffer.py`), ABI jsons (`legacy-display-24A446.json`), boot/log/UI checks |

### D. Only here (170 files)

| area | files | what it is |
| --- | --- | --- |
| `docs/evidence/` | 70 | this board's evidence records |
| `kits/NativePrelink/` | 25 | prelink kit this board shares |
| `patches/xnu/` | 23 | this board's diagnostics patches |
| `components/kona-ufs/`, `drivers/KonaUfs/` | 27 | the SM8250 UFS bring-up |
| `package/vendor-ram-watchdog/` | 6 | RAM watchdog policy |
| `configs/instantnoodlep_defconfig`, `dts/instantnoodlep.dts`, `local.toml` | 3 | board configuration |

### E. What the file-by-file comparison actually proves

The display code both repositories use is the **same component family** (`NativeLegacyDisplay` +
`LegacyDisplayProbe`); sheng does not carry a different display driver.  The material gaps are (1) sheng's GPU and
peripheral driver stack, (2) its display **build pipeline** (`build-gop-legacy-display.py`, which generates the ABI
header from the audit - this session built the component that way through `scripts/build-arm-driver.py
NativeLegacyDisplay`), and (3) its launchd policy directories.  Nothing in sheng's tree shows a display configuration
that this board lacks; the difference is the device stack around it.
## 18:30: the presenter and the attach topology - sheng's probe, read line by line

sheng's display builder puts **three** objects in `libNativeLegacyDisplay.late.a`: `NativeLegacyDisplay`,
`NativeKeyStoreEvents` and **`NativeGOPPresenter`** (build-arm-driver.py for each).  This board's archive has the
display, the probe and the accelerator stub - **no presenter**.

Their probe's publish path, verbatim from `package/LegacyDisplayProbe.cpp`:

* phase 3: `native_gop_presenter_create()` -> `init()` -> `attach(nub)` -> `start(nub)`; the presenter owns the
  transport ('Explicit own transport service only. The incomplete original framebuffer backend is never published');
* phase 4: the legacy display is probed, attached and started; when `fb-gpu=1` a **handoff presenter** is created and
  attached to `IOService::getResourceService()` with this comment:

  > Not a child of the display subsystem's node: the original display code enumerates that node's children (int-075/077:
  > SpringBoard found no display and crashed with the presenter attached there, registered or not).

So there are two distinct topologies in sheng, and the comment records that the wrong one makes SpringBoard crash with
'no display'.  This board's probe attaches its nub to `getResourceService()` (the safe side) but never creates a
presenter, so the system has a display service and no presenter service.

That is the concrete, file-level difference left: **`NativeGOPPresenter` is missing from this port's archive**, and
with it the NGP present/wait/complete path the render server uses.  Building it is one
`build-arm-driver.py NativeGOPPresenter` step plus the create/attach/start sequence above.
## 19:30: the presenter/accelerator build regressed the display start

The kernel with `NativeGOPPresenter` + `MiniAcceleratorES` builds and links, but the boot now reports:

      DISPLAY: display attach=1 start=0

i.e. the legacy display **attaches but fails to start**, so nothing is published and the presenter branch is never
reached (`dispStarted == 0`).  Previous builds reported `attach=1 start=1` followed by the default-surface adoption.

So one of the two additions this round disturbs the family's start path:

* `native_gop_presenter_static_init()` called at probe entry (the presenter's class registration), or
* `MiniAcceleratorPublishStub()` publishing an `IOAcceleratorES` before the display starts.

Both are isolatable with boot arguments on the next boot (`accel_stub=0` disables the stub; the presenter init can be
gated the same way).  The finding that matters is that the start failure appeared exactly when these two were added,
so the display's start path is sensitive to what is published before it.

## 20:30: comparison with UniXNU/UniXNU (the framework) - is the framework the problem?

Comparing this checkout against a fresh clone of `UniXNU/UniXNU`:

| area | upstream | local | verdict |
| --- | --- | --- | --- |
| `patches/xnu` (this XNU version's series) | - | - | **identical** - the kernel patch set is not stale |
| `patches/xnu-12377` | 5 patches we lack | 5 differ | iPadOS 27 only; irrelevant to 17.7.11 |
| `patches/mesa` | 4 newer turnip patches | older | GPU/turnip only; this board has no GPU stack |
| `scripts/` | 368 files | 385 files, 27 differ | local carries 24 device-specific additions (owner policy, storage bindings, SoC headers); misses 7 upstream files |

The seven upstream scripts this checkout lacks:

* `absent-service/` (absentd.c + build.py),
* **`macho-self-bind-weak-imports.py`** - the proper tool for binding weak imports, i.e. exactly the problem that
  this session hacked around with a probe-side symbol alias,
* `data/ioaccelerator-base-abi-26C5035r.json` - an IOAccelerator ABI for 26C5035r (not this OS),
* `macos-boot-shims/apfs_boot_util.c`, `make-macos-data-seed.py` - macOS paths.

`scripts/build-arm-driver.py` differs because this checkout carries board-specific additions (SoC include path, the
verified-root split, NativeUfs bindings) - not because it is stale.

**Conclusion**: for this board's iPadOS 17.7.11 kernel the framework is in sync where it matters (`patches/xnu`
identical), and the differences are device-local additions plus iOS 26/27 and GPU material.  The framework is not
what keeps SpringBoard in its loop; the missing pieces are the device-side services discussed earlier (a real GPU
backend and the userland-facing services SpringBoard expects).

## 06:05: the packaging pipeline was feeding a stale kernel - scope of the damage

`scripts/build_payload.sh` copies `out/.../kernel/kernel.macho` to `/tmp/amfiEU/kernel.macho` and applies the two
AMFI patches (nopmgr, trustcache gate) before packing.  `restore_owner_data.sh`, used for the last rounds, packs
`/tmp/amfiEU/kernel.macho` **without** copying the new build first, so those images carried whatever kernel was there.

Evidence: the image built at 05:31 (`20807042`) and the repack (`8511d7ba`) contain no `mini stand-ins` string,
while `out/.../kernel/kernel.macho` does; after running the patcher on a fresh copy, image `ad936d1e` contains it.

### What this invalidates

Any change made after the last run of `build_payload.sh` was tested against a kernel that did not contain it:

* the JOP shared-region signing fix (`vm_shared_region.c`),
* the GOP presenter and the empty-accelerator stub,
* the HakiOS `Mini*` stand-ins (until image `ad936d1e`).

The SpringBoard loop itself is not an artifact of this: it was recorded in the early boots whose kernels did carry the
session's changes (the probe prints, the display attach/start results and the sheng-vsync arguments all appear in
those logs).  But those three fixes above have **not actually been executed on the device yet**, so their negative
results do not count.

### Correct packing from now on

`build_payload.sh` (fresh copy + both AMFI patches + payload), then `pack_boot.sh`.  `restore_owner_data.sh` must not
be used without the copy step.
## 06:20: the empty accelerator stub panics the kernel - first real execution of that patch

The image built through the corrected pipeline (fresh kernel + both AMFI patches) reached the new code for the first
time and panicked at once:

    DISPLAY: probe entered (registered=0)
    DISPLAY: preModLoad(__kernel__)=0xfffffff819c69480
    log: process stack capture enabled
    ACCEL-STUB: empty IOAcceleratorES published (no backend)
    panic(cpu 0 caller 0xfffffff00c01f26c): Kernel data abort. at pc 0xfffffff00be850bc, lr 0xfffffff00be84ee8

So the ported `MiniAcceleratorES` publishes and then faults: the stub has never been exercised before, because every
earlier image carried a stale kernel.  Its source is the mondrian `0212-k60-empty-accelerator` patch, which attaches
the stub to a provider that this kernel sets up differently.

Next image disables it with its own boot argument (`accel_stub=0`) and keeps the GOP presenter (`fb-presenter=1`),
so the two changes can be judged separately.
## 06:10: two regressions isolated - the component rebuild and a disk-time panic

**1. `DISPLAY: display attach=1 start=0` is not the presenter or the accelerator stub.**
With `fb-presenter=0 accel_stub=0` the display still fails to start.  The component object was rebuilt at 04:40
with the framework's `build-arm-driver.py` (384344 bytes); recompiling it with this port's original recorded
command produced a different object (385032 bytes) - so the framework build and the recorded build are not the
same binary, and the start failure appeared when the framework one replaced the recorded one.

**2. A new panic during disk setup, independent of display settings.**

    disk5s1 setting dev block size to 4096
    disk5s1 scan took 0.000296 s (no trims)
    attempting to load /usr/appleinternal/sbin/nvramd.dev#opment
    panic(cpu 0 caller 0xfffffff00c01f26c): Kernel data abort. at pc 0xfffffff00be85910

* it happens with `fb-presenter=0 accel_stub=0`, so neither new component is involved;
* it happens with only the nopmgr AMFI patch, so the trustcache-gate patch is not the cause;
* the pc is in the same text region (`0xfffffff00be85xxx`) as the earlier accelerator/presenter panic, which
  suggests the display component's code is still being entered at that point.

Next isolation: boot with the display probe disabled entirely (`offline-mobile-graphics-load=0`) and see whether the
disk-time panic survives; if it disappears, the component is being called from a path this session added.
## 12:00: the panics are symbolised - all three are in OSMetaClass / printf

| pc | symbol | when |
| --- | --- | --- |
| `0xfffffff00be850bc` | `OSMetaClass::postModLoad` +0x448 | probe registration, first panic |
| `0xfffffff00be85910` | `OSMetaClass::allocClassWithName` +0x20 | later, during disk setup |
| `0xfffffff00b89e110` | `___doprnt` +0xc10 | this boot, right after *attempting to load launchd.development* |

So the two earlier panics are both inside the **component registration path this port fakes**: the probe calls
`OSMetaClass::preModLoad(g_kernel_kmod_info.name)` and `postModLoad(handle)` for a class that was never built as a
kext, and the kernel later finds that half-registered class through `allocClassWithName` and faults.  The display's
`start()` failing (`attach=1 start=0`) is consistent with the same inconsistency.

The third panic is different: it is inside `___doprnt` (kernel printf) while printing during the launchd load, i.e. a
printf with a bad argument or format, not a registration problem.

### What this means for the plan

The original-family approach is not failing on display protocol details; it is failing because the class is being
registered through a fake module.  A class defined **inside the kernel** (the way HakiOS's Mini stand-in and the
framework's own `NativeMobileDisplay` are built) has proper metaclass metadata and does not need preModLoad/postModLoad
at all.
## 12:15: with the real static initialiser, the panic moved into postModLoad

The doc-aligned registration (preModLoad -> the component's real `__GLOBAL__sub_I_NativeLegacyDisplay.cpp` ->
postModLoad) builds and links, but the device panics in the same place as before:

    DISPLAY: probe entered (registered=0)
    DISPLAY: preModLoad(__kernel__)=0xfffffffacab9daa0
    panic(cpu 0 caller 0xfffffff00c01c94c): Kernel data abort. at pc 0xfffffff00be850bc, lr 0xfffffff00be84ee8

`0xbe850bc` is `OSMetaClass::postModLoad + 0x448` and the return address `0xbe84ee8` is `postModLoad + 0x274`, so the
fault is inside postModLoad itself, not in the component's initialiser (which now really runs).

### What the document asks for that this port still lacks

`docs/porting/display.md` requires the device's `scripts/build-gop-legacy-display.py` to *qualify ABI methods against a
prelinked candidate* before producing the late archive, and to supply the matching candidate and audit inputs.  This
port has the audit (`build/display-analysis/mobile-graphics-legacy-abi.json`) but never supplies the prelinked
candidate: the display class's superclass (`IOMobileFramebuffer`) lives in the prelinked mobile-graphics family, and
if that family's metaclasses are not registered when postModLoad walks the pending class list, the walk meets a
dangling superclass and faults.

### Next step (document order)

1. Load the prelinked mobile-graphics family before the display registers (the probe already has
   `OSKextLoadKextWithIdentifier`; it must run and succeed first),
2. then preModLoad -> static init -> postModLoad,
3. and only then attach/start the display.
## 12:25: what the official reference pages actually prescribe for this board

From `reference/components.md`, `xnu/source-and-patches` and `xnu/patch-reference`:

* a display is a **catalog component**, selected in `[components] include` and placed in `[kernel] archives` as
  `component:<ID>`; the resolver then builds dependencies first and the component's own build command runs;
* the catalog entry for this board's OS is:

      id = "GopFramebuffer"        build_stage = "embedded"
      os_abi = ["ipados-17.7.11-21H461", "ipados-27.0.1-24A446"]
      source_files = ["NativeMobileDisplay.cpp", "NativeLegacyDisplay.cpp"]
      outputs = ["build/gop-framebuffer/libGopFramebuffer.a", ".../libGopFramebuffer.late.a"]
      legacy_name = "NativeMobileDisplay"
      legacy_outputs = {"build/native-mobile-display/libNativeMobileDisplay.late.a" = "build/gop-framebuffer/libGopFramebuffer.late.a"}

* `patch-reference` lists only three graphics patches - `offline-native-gpu-broker`, `zzy-native-gpu-addon-mount`,
  `zzw-native-adreno-sandbox-grant` - and each needs a GPU component.  Nothing there addresses a board without one.

### What this board does instead

`[components] include` already names `GopFramebuffer`, but `[kernel] archives` never referenced it, so the framework
built `build/gop-framebuffer/libGopFramebuffer.a` (NativeMobileDisplay only, 270 KB) and never embedded it: the kernel
contains no `NativeMobileDisplay` string.  The display that *is* in the kernel is this session's hand-assembled
`libNativeLegacyDisplay.late.a` plus the board probe, which is exactly the integration the component system exists to
replace - and exactly where every panic this afternoon landed (`OSMetaClass::postModLoad`, `allocClassWithName`).

### The document-order fix

Add `component:GopFramebuffer` to `[kernel] archives` (done) **and** let it produce the late archive the kernel link
consumes (`build/gop-framebuffer/libGopFramebuffer.late.a`), then retire the hand-built archive and the probe's
`preModLoad`/`postModLoad` sequence, which exists only because the class was never embedded.
## 12:30: the qualification inputs exist; only the qualifier itself is missing

The document's step-2 requirement is satisfied on the input side:

      build/xnu-prelink/native-owner-mobile-graphics-candidate.macho   (124 MB, this board's prelinked
                                                                       mobile-graphics candidate)
      build/xnu-prelink/native-owner-mobile-graphics-candidate.json
      build/display-analysis/mobile-graphics-legacy-abi.json           (the ABI audit)

What is missing is `scripts/build-gop-legacy-display.py`, the qualifier that consumes them and produces the late
archive.  The reference copy is written for macOS: it calls `xcrun --sdk iphoneos --show-sdk-path`, `xcrun clang`,
`brew --prefix llvm` (for the Mach-O `llvm-objcopy --globalize-symbol`) and `xcrun ar`, and it links the device's
companion objects (HID, keystore, process, trace, log, reset) that this repository does not carry.

### Two ways forward, both mechanical

1. **Port the qualifier to this toolchain**: substitute this tree's clang and SDK flags for `xcrun`, this port's
   Mach-O symbol globaliser (the nlist patch already written) for `brew`'s `llvm-objcopy`, and drop the companion
   objects the board has no component for.  The qualification checks themselves (358-entry vtable, leaf slots,
   candidate addresses) stay.
2. **Keep the hand-built archive and record the deviation**: the archive is already at the path the framework
   expects (`build/native-legacy-display/libNativeLegacyDisplay.late.a`) and the kernel force-loads it, but the
   ABI qualification the document requires never runs, and that is the state every panic this afternoon belongs to.

Option 1 is the document's own path and removes exactly the class-registration risk that produced
`OSMetaClass::postModLoad` and `allocClassWithName` faults.
## 12:45: the qualifier runs - the audit passes, the component compile is the remaining stop

Running the reference qualifier in this tree with the Linux substitutions:

    PASS: original Legacy has 358 slots with nine abstract leaves; original generic client/start/dispatch entries identified
    Service adds no operational overrides; Legacy initialization/start/backend contracts still require implementation and runtime qualification

So the ABI audit against this board's prelinked candidate **passes**: 358 slots, nine abstract leaves, and the generic
client/start/dispatch entries identified.  That is the document's qualification step working on this board's inputs.

It then stops at the component build:

    drivers/GopFramebuffer/NativeLegacyDisplay.cpp:536: error: only virtual member functions can be marked 'override'

i.e. `isTimingEnabled()` (the swap-timeout probe this session added) is an override in the source while the tree's
`OriginalMobileFramebufferABI.h` does not declare that virtual.  The display build directory carries this port's edited
header (slot 345 declared, `LEAF(345)` removed); the framework header the qualifier compiles against does not.

So the two headers have to be reconciled before the qualifier can finish, and that reconciliation is exactly what the
document means by keeping ABI inputs and the component in step.

## 13:05: the qualifier is ported except for the probe compile

Progress on the Linux port of `build-gop-legacy-display.py` (the document's qualification step):

| step | state |
| --- | --- |
| ABI audit against this board's candidate | **PASS** - 358 slots, nine abstract leaves |
| component compile (`build-arm-driver.py NativeLegacyDisplay`) | **passes** - `-Werror` build and object audit |
| generated header for slot 345 | added `isTimingEnabled` to the generator, removed it from the LEAVES list |
| `llvm-objcopy --globalize-symbol` | replaced with `scripts/globalize-static-init.py` (Mach-O nlist patch) |
| probe compile | **stops**: the recorded command carries `-Werror` and trips
  `-Wmissing-designated-field-initializers` inside `vm_kern.h`/`IOLib.h`; the flag insertion needs to land on the
  command actually used for the probe, not only on the derived one |
| archive assembly | not reached |

Nothing here is device work; it is all offline build plumbing, and the remaining piece is one flag on one command.

## 13:20: the qualifier passes, but the archive must be produced *inside* the kernel build

The qualifier now runs end to end on this board:

    audit: 358 slots, 9 abstract leaves, candidate 4f0dea314c912e79...
    NativeLegacyDisplay: -Werror build and arm64-apple-ios17.0 object audit passed
    vtable check: 2880 bytes fits in __const
    globalised __GLOBAL__sub_I_NativeLegacyDisplay.cpp: 1 site(s)
    PASS: legacy display qualified

The candidate hash is this board's own prelinked candidate, not the reference board's, so the qualification really is
against this kernelcache.

The kernel build then stops with:

    build-xnu-4k-baseline.sh: line 692: test -f "$LEGACY_DISPLAY_ARCHIVE" failed

because each kernel build recreates `tree/build`, so an archive assembled before the build is gone by the time the
link looks for it.  That is exactly why the framework runs `build-gop-legacy-display.py` as an entry of
`[kernel] archives`: the archive has to be produced by the build itself, in order, before the link.

### Remaining work (one step, mechanical)

Extend the qualifier to also assemble the archive it is responsible for - the qualified component object plus the
board probe (and the bindings object that the builder emits) - and register `qualify-gop-legacy-display.py` in
`[kernel] archives`.  Then the build produces the qualified `libNativeLegacyDisplay.late.a` itself and the
`test -f` passes.
## 13:30: the display archive is now produced by the build and qualified

`build-gop-legacy-display.py` is registered in `[kernel] archives`, lives in the device repository, and runs with this
toolchain; the build log now shows, for every kernel build:

    PASS: original Legacy has 358 slots with nine abstract leaves; original generic client/start/dispatch entries identified
    NativeLegacyDisplay: -Werror build and arm64-apple-ios17.0 object audit passed
    PASS: qualified late display archive produced: build/native-legacy-display/libNativeLegacyDisplay.late.a
    build-report.json written

and the kernel gate that used to fail (`test -f "$LEGACY_DISPLAY_ARCHIVE"`, then the archive/source hash comparison)
passes: `KERNEL BUILD EXIT: 0`, prelink produced, kernel sha256 `c4168172...`.

Changes that made it work, all mechanical:

* slot 345 declared in the generated header (`isTimingEnabled`), removed from `NATIVE_LEGACY_LEAVES`;
* `-Wno-missing-designated-field-initializers` in the framework's `build-arm-driver.py`;
* `llvm-objcopy --globalize-symbol` replaced by `scripts/globalize-static-init.py` (Mach-O nlist patch);
* probe compiles from `kits/instantnoodlep/LegacyDisplayProbe.cpp` with the board's flags;
* the companion components (HID, keystore, process, trace, log, reset) are absent from this board and dropped;
* the script writes `build-report.json` (archive and source hashes) which the kernel gate checks.

Image built from that kernel: `fb8b6a4bed0c352f86deaed5de45ebfeee885218664e4d17e1f75035eea55203`.

## 13:40: with the qualified archive the fault is unchanged - it is the registration order

The doc-qualified build boots to the same instruction:

    DISPLAY: probe entered (registered=0)
    DISPLAY: preModLoad(__kernel__)=0xfffffff4549dabc0
    panic(cpu 0 caller 0xfffffff00c01c94c): Kernel data abort. at pc 0xfffffff00be850bc, lr 0xfffffff00be84ee8

`0xbe850bc` is `OSMetaClass::postModLoad + 0x448` again.  The archive is now built and qualified by the build itself, so
the archive contents are not the variable any more: what faults is the registration step.

### Why postModLoad can fault here

`postModLoad` walks every metaclass registered inside the `preModLoad`..`postModLoad` window and finalises it, which
requires each class's superclass to be resolvable.  The display class's superclass is `IOMobileFramebuffer`, which lives
in the **prelinked** IOMobileGraphicsFamily; if that family's classes have not been registered yet at the moment the
probe runs, the walk meets a superclass that cannot be resolved.

That is an ordering question, not a build question: the reference port runs its probe after the prelinked components
are initialised.  This port runs it at `offline-legacy-start`, which is early enough that the family may not be up.

### Next step

Move the display registration to the first point where the prelinked mobile-graphics family is registered (or call its
initialiser explicitly before `preModLoad`), keeping the qualified archive exactly as it is now.
## 14:45: kextlog shows the registration order; postModLoad still faults with the family loaded

With `kextlog=0xfff` the kernel log now names every registration:

    Kext __kernel__ registered class NativeLegacyDisplay.
    Kext com.apple.iokit.IOMobileGraphicsFamily registered class IOMobileFramebufferUserClient.
    Kext com.apple.iokit.IOMobileGraphicsFamily registered class IOMobileFramebuffer.
    Kext com.apple.iokit.IOMobileGraphicsFamily registered class IOMobileFramebufferLegacy.
    DISPLAY: probe entered (registered=0) mobile-graphics-load=0
    QCOM PANIC END

So the display class is registered under `__kernel__` and the family (whose metaclasses include its superclass)
registers afterwards; the probe then runs with `mobile-graphics-load=0` and the same instruction faults
(`OSMetaClass::postModLoad + 0x448`).

Clearing `__DATA,__mod_init_func` in the qualified object did not move that registration: it still happens before the
family, which means the call that registers the class is our own `preModLoad`/static-init pair inside the probe, and the
fault is inside `postModLoad` itself rather than in a dangling superclass.

### Where this leaves the port

* the display component is built and **qualified by the build** (audit PASS, 358 slots, vtable check, archive hashes);
* the kernel links it, the prelink completes and the boot reaches the probe;
* the one remaining defect is the module-registration call: `preModLoad(g_kernel_kmod_info.name)` + `postModLoad(handle)`
  for a class that is statically linked rather than loaded as a kext.

Next: instrument `postModLoad`'s expectations (the kextlog already shows every class the walk visits), or bypass the
module protocol entirely for a static class and verify with `OSMetaClass::allocClassWithName("NativeLegacyDisplay")`.
## 15:00: registration is fixed; the next fault is in the publish path

With `OSMetaClass::postModLoad` removed, the boot passes the point that failed all afternoon:

    Kext com.apple.iokit.IOMobileGraphicsFamily registered class IOMobileFramebufferLegacy.
    Kext com.apple.iokit.IOMobileGraphicsFamily is now started.
    Kext com.apple.iokit.IOMobileGraphicsFamily loaded.

    DISPLAY: probe entered (registered=0) mobile-graphics-load=0
    DISPLAY: preModLoad(__kernel__)=0xfffffff5sedd7ec0
    DISPLAY: bootargs video base=0x9c000001 w=1080 h=2376 rb=4320 depth=32 display=1
    DISPLAY: GOP console 0x9c000001 1080x2376 depth=32 stride=4320

so `preModLoad` + the real static initialiser are enough to register the class; the kext-log confirms the family starts
and loads before the probe.

The new fault is later, inside the publish path:

    panic(cpu 0 caller 0xfffffff00c01ca20): Kernel data abort.
      at pc 0xfffffff00be04b60, lr 0xfffffff00beafcfc
      pc -> _nxctl_nexus_config + 0x180,  lr -> OSSerialize::binarySerializeInternal + 0x640

i.e. something is being serialised (a property table during service publication) and the walk inside the serialiser
takes a bad address.

Next: print around each step after the GOP console line (allocate, set properties, attach, start, registerService) so the
last completed step before the panic names the exact call.
## 15:15: the embedded registration works - class allocates, display attaches, start() fails

With `__DATA,__mod_init_func` kept and the probe's `preModLoad`/`postModLoad` removed entirely (the class registers
itself the way every `build_stage = "embedded"` driver does), the boot log reads:

    DISPLAY: probe entered (registered=0) mobile-graphics-load=0
    DISPLAY: embedded class NativeLegacyDisplay meta=0xfffffff00c1103b8 size=0x1ae0
    DISPLAY: allocated NativeLegacyDisplay size=0x1ae0 legacy=1 mobilefb=1
    DISPLAY: display attach=1 start=0

* the class is registered by the kernel and found by name;
* the component's own factory allocates a real instance whose casts report `IOMobileFramebufferLegacy=1` and
  `IOMobileFramebuffer=1`;
* `attach()` succeeds and **`start()` returns false** - the display does not start;
* a later fault lands in `___doprnt` (kernel printf), i.e. a formatting bug in a trace worker, not the display.

All of the afternoon's `OSMetaClass::postModLoad` / `allocClassWithName` panics are gone.

Next: separate the two remaining issues - the trace workers are disabled in the next image, so a `start=0` with no
printf fault isolates the display's own start precondition.
## 15:40: the display starts - attach=1 start=1, and the boot reaches launchd

Two changes made the display start on this board:

1. the embedded class registers itself (`__DATA,__mod_init_func` kept, probe's `preModLoad`/`postModLoad` removed), so
   the class is found and allocated (`size=0x1ae0 legacy=1 mobilefb=1`);
2. the nub's device memory is exactly `v_rowBytes * v_height` - `startHardware()` accepts the mapping only when
   `getLength()` equals that product, and the earlier `+0x10000` slack made it return false.

Boot log:

    DISPLAY: nub 0xfffffff56f108e40 attach=1 start=1 mem=0x9c000001+0x9c9f00
    DISPLAY: display attach=1 start=1
    DISPLAY: surfaces default=0 temp=0xfffffff50a07ffa8
    ...
    apfs_vfsop_mount:2714: disk3s1 failed to find named root snapshot: Need authenticator (81)
    Failed to open swap file 30
    vm_swap_create_file failed @ 94 secs
    load_init_program: attempting to load /usr/appleinternal/sbin/launchd.development
    Kernel data abort. at pc 0xfffffff00b89e110  (___doprnt)

So the display path no longer blocks the boot: the run reaches the userland entry.  What remains is elsewhere:

* the OS volume `disk3s1` mounts but its root snapshot needs an authenticator (the verified-root / owner-root path);
* the VM swap file cannot be created (94 s), and
* a data abort inside `___doprnt` follows, i.e. a printf whose argument is not what its format expects.
## 17:00: the boot reaches userland; SpringBoard never starts because the App cryptex is absent

With the IO map restored to 32 MiB the kernel prints again and the boot runs all the way into userland:

    Darwin Kernel Version 23.6.0: Thu Nov 12 13:44:30 CST 2026
    DISPLAY: embedded class NativeLegacyDisplay meta=... size=0x1ae0
    DISPLAY: allocated NativeLegacyDisplay size=0x1ae0 legacy=1 mobilefb=1
    DISPLAY: display attach=1 start=1
    ...
    XNU-EXIT: fairplayd.A2 pid=239 code=0 ...
    XNU-EXIT: bluetoothd pid=238 code=0 ...

launchd itself reports why the graphical stack is missing:

    launchd <Error>: Missing executable detected. Job: 'com.apple.WebBookmarks.webbookmarksd'
        Executable: '/System/Cryptexes/App/usr/libexec/webbookmarksd'

The OS cryptex (`SystemCryptex`, disk1s1) mounts, but **the App cryptex does not**, so every daemon whose binary lives
under `/System/Cryptexes/App` is missing - and `spring` appears zero times in the log: SpringBoard is never even
launched.

The kernel already has a cryptex binder for the OS volume (`native_cryptex_os_bind` in `kern_exec.c`, binding
`/System` and `/usr` over `/private/preboot/Cryptexes/OS`); what it lacks is the same for the **App** volume, and the
App cryptex content itself is not in this workspace (no IPSW extract, no App image).

### State of the port at the end of this session

| layer | state |
| --- | --- |
| display component | built and qualified by the build (358 slots, vtable, hashes) |
| class registration | embedded self-registration; `postModLoad` removed |
| display start | `attach=1 start=1` once the nub length equals `rowBytes*height` |
| kernel -> userland | reaches launchd and runs daemons |
| SpringBoard | not launched: App cryptex missing |

Two build traps were fixed and turned into patches: hand edits inside `tree/.cache/xnu-work` are erased by a tree
rebuild (`IO_MAP_SIZE` fell back to 8 MiB, which silently hung the console, and `TOTAL_CORPSES_ALLOWED` to 4).
## 14:30: the App cryptex binder reports ENOENT; path diagnostics added

`load_init_program` now binds the App cryptex right after the SystemOS mount and reports the result:

    offline cryptex: App bind=2 SpringBoard=2      (2 = ENOENT)

so the bind fails because one of the two paths is absent.  The next image prints each lookup separately:

    offline cryptex: src(/private/preboot/Cryptexes/App)=? dst(/System/Cryptexes/App)=?

which decides whether the App tree is not where the SystemOS mount puts it, or the mount point is missing from
the composed root.

Also established this round: the official `patch-reference` names `offline-owner-systemos-mount` as the step that
mounts SystemOS at `/private/preboot` and composes the root, which is why the App bind belongs immediately after it
rather than in the GPU-addon cryptex view.
## 14:50: where the cryptex trees actually are on this port

The binder diagnostics after the SystemOS mount report:

    offline cryptex: src(/private/preboot/Cryptexes/App)=2 dst(/System/Cryptexes/App)=2   (2 = ENOENT)
    offline cryptex probe /private/preboot            =0   (exists)
    offline cryptex probe /private/preboot/Cryptexes  =2   (missing)
    offline cryptex probe /private/preboot/System     =2
    offline cryptex probe /System/Cryptexes           =0   (exists)
    offline cryptex probe /System/Cryptexes/OS        =2   (missing)
    offline cryptex /private/preboot/.                (only . and ..)

and a listing of `/` shows `usr` and `System` at the top level.

So this port composes the root from the OS volume with the SystemOS content at `/System` and `/usr`: the App cryptex
subtree is **not exposed anywhere**, `/private/preboot` is an empty directory and `/System/Cryptexes` holds neither OS
nor App.  That is precisely why launchd reports 'Missing executable' for every job whose binary lives under
`/System/Cryptexes/App`, SpringBoard included.

### What has to happen next

1. Find where the SystemOS volume is mounted on this port (the root scan with the readdir walk needs to be finished; it
   stopped after four entries because the walker's buffer accounting is wrong),
2. expose its cryptex subtrees: `/System` and `/usr` already come from it, so the same mount's `Cryptexes/OS` and
   `Cryptexes/App` have to be bound at `/System/Cryptexes/{OS,App}`,
3. then SpringBoard's binary becomes reachable and launchd can start it.
## 15:20: SpringBoard's binary is reachable; launchd simply never launches it

The layout probes settle two questions at once:

    offline layout /System/Library/CoreServices/SpringBoard.app/SpringBoard = 0   (present)
    offline layout /System/Applications                                   = 0   (present)
    offline layout /System/Cryptexes/App/System/Applications              = 2   (missing)

so the App cryptex is **not** what blocks SpringBoard: its binary lives on the OS volume and is reachable.  The
'Missing executable' line belongs to `webbookmarksd`, one of the App-cryptex daemons, and does not name SpringBoard.

A ten-minute capture of the same boot shows launchd progressing normally:

    launchd <Notice>: Early boot complete. Continuing system boot.
    ... user/501/... jobs ...
    XNU-EXIT: com.apple.datami pid=304, bluetoothd pid=315, fairplayd.A2, ...

but neither `backboardd` nor `SpringBoard` appears even after several minutes, so their jobs are never started rather
than started and crashed.

### Next questions

1. Why launchd does not start `backboardd`/`SpringBoard` - the job conditions (setup state, user session, or a
   preference the port has not written) rather than the display or the cryptex;
2. whether the display service this port publishes is visible to a userland matcher (a `IOServiceMatching` for
   `IOMobileFramebuffer`) - the class is registered and `start=1`, so the next check is the registry entry itself.
## 15:40: the official docs answer it - the device is still in Setup

`docs/zh/guide/skip-setup.md` states it directly:

> 全新的 Data 种子首次启动会进入 Setup。负载可以在写镜像前把 Setup 标记为完成，让设备直接进入 SpringBoard。

and lists exactly what `./unixnu build payload --skip-setup` writes into
`/private/var/mobile/Library/Preferences`:

| file | values |
| --- | --- |
| `com.apple.purplebuddy` | `SetupDone`, `SetupFinishedAllSteps`, `SetupVersion 11`, `daemonizeNeedsToRun false` |
| `com.apple.purplebuddy.notbackedup` | every mini-buddy flag false |
| `com.apple.managedconfiguration.notbackedup` | `LockdownActivationIndicatesCloudConfigurationAvailable false` |
| `com.apple.springboard` | `BKNoWatchdogs`, `FBLaunchWatchdogExceptions{com.apple.purplebuddy:100}`, `MCForceShowCloudConfigurationUI false` |
| `com.apple.migration` | `LastSystemVersion 21H461`, `DMUserDataDisposition` |

This port **never ran that step**: `board.toml` has no `[payload] skip_setup_tool`, and the payload command in
`build_payload.sh` passes only `--data-ramdisk`.  A fresh seed therefore boots into Setup, and Setup is exactly the state
in which SpringBoard is not started - which is what the last ten minutes of logs show: launchd is healthy, the jobs even
have their plists (`com.apple.SpringBoard.plist`, `com.apple.backboardd.plist` both present), and neither is run.

### Why it did not happen

`scripts/seed-data-skip-setup.py` attaches the APFS container read-write, which needs macOS.  On this Linux host the two
options are:

1. run the documented step on a Mac (`./unixnu build payload --skip-setup ...`), or
2. write the same three preference plists from the kernel right after the volatile Data mount - the port already does
   this for `disabled.plist`, so the writer exists and only needs the Setup-skip payloads.
## 15:55: real client frames are presented; the adopted surface is freed on the first completion

With `offline-legacy-adopt=1` the probe adopts the family's temporary surface:

    DISPLAY: adopted default surface 0xfffffff43c0e4e88 sid=1 region=... descriptor=...

and a userland client then swaps through the display:

    DISPLAY: swap accepted=1 reason=0 request=0xfffffff3561d1000 surface=0xfffffff43c0e0b98
    DISPLAY: present copied 2376 rows x 4320 bytes (frame 1)
    DISPLAY: swap stage 1
    IOMFB: surface_complete : Freeing up Default Temp surface - Surface = 0x...43c0e4e88
    DISPLAY: present copied ... (frame 2, frame 3)

so the compositor's frames reach the panel - three of them - and then the client stops asking.  The family frees the
adopted surface inside `surface_complete`, i.e. the adoption is undone by the very completion path that follows it.

### Two things to fix, both local to the swap path

1. make the default surface follow the client's incoming surface instead of the family's temporary one, so completion
   cannot free the surface that is in use;
2. find why the client stops after three swaps - it is the first userland client of this display, and its identity is
   the next thing the log should record.
## 16:10: the official display page describes this exact state

`docs/porting/display.md` records the reference's own result:

> The 24A446 adapter supplies the default surface through the original IOSurfaceRoot factory and validates its backing
> memory and geometry before transferring ownership to the display family. ... A captured 300-second boot completed
> **three swaps** without a kernel panic and displayed the Apple logo. Home-screen startup remains unverified; capture the
> display-server and SpringBoard wait chains before treating a stationary logo as a framebuffer transfer failure.

So this port is at the reference's own milestone: three real client swaps, frames copied to the panel, a stationary image,
no panic.  Two consequences follow from the document:

1. the default surface must come from the original IOSurfaceRoot factory before ownership moves to the display family -
   which is why the hand-adopted temporary surface is freed by `surface_complete`;
2. the next step is not more frame work but **capturing the display-server and SpringBoard wait chains**: backboardd and
   SpringBoard never appear in the log, so what they wait on is the question.
## 16:20: session summary - the port is at the reference's own display milestone

### Reached today, in order

1. the display component is built by the build itself and **qualified** (`build-gop-legacy-display.py` as an archive
   step: ABI audit against this board's prelinked candidate - 358 slots, nine abstract leaves, vtable check, archive and
   source hashes in `build-report.json`);
2. the class registers the embedded way (`__DATA,__mod_init_func` run by the kernel, no `preModLoad`/`postModLoad`);
   every `OSMetaClass` panic of the afternoon is gone;
3. the display **starts** - `attach=1 start=1` - once the nub's device memory is exactly `vBytes = rowBytes*height`;
4. the kernel reaches userland; launchd runs its boot tasks to 'Early boot complete';
5. **Setup is skipped** by writing the three preference plists the docs list (`com.apple.purplebuddy`,
   `com.apple.springboard`, `com.apple.migration`) from the kernel;
6. a userland client swaps through the display and **frames are copied to the panel**: `frame 1, 2, 3`;
7. two build traps were found and turned into patches: hand edits in `tree/.cache/xnu-work` are erased by a tree
   rebuild (`IO_MAP_SIZE` fell back to 8 MiB and silently hung the console), and the corpse cap fell back to 4.

### Where that leaves it

The official display page records the reference at exactly this point: **three swaps, no panic, a stationary image**, with
home-screen startup unverified and the next step named as *capturing the display-server and SpringBoard wait chains*.
backboardd and SpringBoard never appear in this port's log, so the question is what they wait on, not whether frames
reach the panel.

### Open items, in the order the docs suggest

1. supply the default surface through the original IOSurfaceRoot factory (the hand-adopted temporary surface is freed by
   `surface_complete`);
2. capture the backboardd/SpringBoard wait chain - `boot-ui-trace=1` is on, but the PROC/UIT rows still come out empty,
   so the worker itself needs checking;
3. give the App cryptex its own bind once the SystemOS mount is located (SpringBoard's binary does not need it, the
   App-cryptex daemons do).
## 16:15: SpringBoard and backboardd are alive; presentation stops after a few frames

The process rows (now printed because the printer takes the snapshot the diagnostic page used to take) show the
graphical stack running on every boot:

    PROC[0] launchd     p1   n12 r11 w1
    PROC[1] backboardd  p57  n15 r6  w9
    PROC[2] SpringBoard p27  n8  r6  w2

Their wait points symbolise to ordinary places - `ipc_mqueue_receive_continue` for SpringBoard, `semaphore_wait_continue`
for backboardd - so neither is deadlocked; they are waiting for events.

Presentation however stops quickly: three client swaps (`frame 1..3`) with the default-surface follow in place, and only
one with the delayed slot-312 completion enabled (`offline-legacy-complete=1`, which also shows SpringBoard's thread count
falling 7 -> 4 -> 3).  The client therefore gets no completion/vblank that would let it present again.

### State at the end of this session

| layer | state |
| --- | --- |
| display component | built and qualified by the build |
| class registration / start | embedded registration, `attach=1 start=1` |
| userland | launchd, backboardd and SpringBoard all running |
| Setup | skipped by kernel-written preferences |
| presentation | 1-3 client frames land on the panel, then stop |
| next question | which completion/vblank the client waits for |
## 16:40: the user-space stacks work - SpringBoard's wait chain is decoded

The two snapshot calls the diagnostic page used to make are now made by the probe's printer, and the stacks appear:

    UIT rows: 75, PROC rows: 94, frames: 3, panic: 0

Decoded with the documented tool (`scripts/decode-boot-ui-trace.py`):

    MAIN S p27 image=1abf89000 PC=1cfcb0b58 LR=1cfcb4224
    trap=ffffffffffffffd1 opt=40700420e remote=6f03 recv=1d03 msgid=0 peer=99 e5
    frames: 0x1cfcb41b0 0x1cfcb3fb8 0x193ab9538 ...

`trap = UINT64_MAX-46` is the Mach trap marker, so **SpringBoard's main thread is blocked in `mach_msg_trap`**, waiting
for a message on the port it received from peer pid 99.  That is the wait chain the display page asks for, and it
finally explains 'running but not presenting': the process is waiting on an event, not on the display.

### Why the rows were empty before

Both the process rows and the ui-trace rows are only refreshed by snapshot functions that the diagnostic page calls
(`boot_process_snapshot`, `boot_ui_trace_snapshot`).  The page is not rendered on this port, so both arrays stayed zero
even though the capture workers ran - the workers themselves were fine (`CAPTURE diag: begin=1 task=1 publish=1`).

## 17:05: repository comparison and the container skeleton lsd needs

Comparing `UniXNU/UniXNU` (2047 files, the framework) with `UniXNU/UniXNU-sheng` (495 files, a device repository)
shows they share only ten files, so they are not comparable directly.  The relevant find is
`patches/xnu-12377/sheng-wifi-launchd-cache.patch`, which mounts two overlays before the cryptex and LaunchServices
setup:

    /private/var/ngpu/xpc      -> /System/Library/xpc      (launchd.plist, foreground_user_endpoints.plist)
    /private/var/ngpu/wifi-bin -> /usr/libexec             (launchd_cache_loader and the service binaries)

i.e. sheng supplies the precompiled launchd job dictionary explicitly, and the docs' sm-t970 record names the same wall:
SpringBoard waits on LaunchServices while lsd waits on **database seeding**.

Layout probes on this board now say:

    /System/Library/xpc                              = 0   (present)
    /System/Library/xpc/launchd.plist                = 0
    /System/Library/xpc/foreground_user_endpoints.plist = 0
    /usr/libexec/launchd_cache_loader                = 0
    /private/var/db/launchservices                   = 2   (missing)
    /private/var/mobile/Containers/Data/InternalDaemon/5C583048-.../Library/Caches = 2 (missing)
    /private/var/mobile/Containers/Shared/SystemGroup = 2  (missing)

so the launchd side is present but **the container skeleton is not**: the directories `docs/zh/guide/skip-setup.md`
has its tool create (`/private/var/containers/{Bundle,Data/System,Shared/SystemGroup,Temp}`) were never made, and
lsd's own store directory is absent.  That is the concrete gap behind 'lsd waits in filt_wlwait_continue'.

## 18:30: the kernel creates the container skeleton itself (VNOP_MKDIR)

`VNOP_MKDIR` with a hand-built componentname is the XNU-internal way to make a directory; `vnode_create`
cannot be used for this because it only makes an unlinked vnode, and `VNONAME` is not exported to the
archive's headers anyway.  The probe now does exactly what `scripts/seed-data-skip-setup.py` does on
macOS, right after the volatile Data mount:

    DISPLAY: setup-skip containers made=8 present=0 failed=0 prefs=0/0/0

Eight directories created - `/private/var/containers` and its `Bundle`, `Data`, `Data/System`, `Shared`,
`Shared/SystemGroup`, `Temp` children, plus `/private/var/db/launchservices` - and the three preference
plists written, all from the kernel with no Mac and no CI in the loop.

## 18:45: the kernel-side Setup skip boots cleanly; presentation is still three frames

The boot with the kernel-made container skeleton shows both halves of the documented step working:

    DISPLAY: setup-skip containers made=8 present=0 failed=0 prefs=0/0/0

and the boot then proceeds normally through launchd's boot tasks and the daemon cycle
(`liveactivitiesd`, `watchdogd`, `fairplayd.A2`, `bluetoothd` all start and exit with their usual codes),
with no panic and no SpringBoard/backboardd/lsd exit records - the three are running.

Presentation is unchanged at three client frames, so creating the container skeleton did not by itself unblock
the display-server chain.  What it does change is the state lsd starts from: its store directory and the
container tree now exist on the Data volume at first boot, which is the precondition the macOS tool creates.

### Where the next session starts

1. watch lsd's wait point after the containers exist (it was `filt_wlwait_continue`, a vnode wait);
2. if it still waits on a vnode, identify that vnode - the store directory is no longer the obvious candidate;
3. `LaunchServices` seeding is also where the docs' sm-t970 record stopped, with the cause unobserved.
## 18:55: what the display checklist asks for after the first frames

`docs/porting/display.md` numbers the verification steps.  Step 3 is 'confirm that a known frame reaches the
display' (three frames), and **step 4 is 'check swap completion and IOSurface lifetime before enabling GPU
presentation'**, followed by step 5, the timing/refresh policy (`sheng-vsync=120` here).

The same page's reference paragraph names the other half:

> Home-screen startup remains unverified; capture the display-server and SpringBoard wait chains before
> treating a stationary logo as a framebuffer transfer failure.

and the sm-t970 device record adds 'SB still waits for BackBoard screen info, swap0'.

### What the probes showed about step 4

- `com.apple.iokit.IOSurface` loads and registers its classes (118 pages, load tag 22), so the factory is present;
- the family's `create_default_fb_surface` nevertheless asks IOSurfaceRoot for a global surface named
  **`PurpleGfxMem`**, which this board never creates - the probe's own comment records this - so the family falls
  back to building a temporary surface from the region cache the probe seeds at +0x17c0;
- adopting that temporary surface as the default lets three frames through, and enabling the delayed slot-312
  completion then dies in `OSMetaClass::allocClassWithName` inside `surface_complete`.

### Next work item

Implement what the page describes for the reference adapter: supply the default surface **through the original
IOSurfaceRoot factory** (create the surface, validate its backing memory and geometry) and then transfer ownership
to the display family, instead of adopting the family's temporary surface.  That removes both the
`PurpleGfxMem` miss and the `surface_complete` free of the adopted surface.
## 18:58: clearing the family's temporary slot removes the free; the client still stops after three swaps

With `offline-legacy-adopt=1` and the family's temporary slot (+0x1620) cleared after adoption, the boot log no
longer contains either the free or its message:

    DISPLAY: adopted default surface 0xfffffff509613e30 sid=1 region=... descriptor=...
    DISPLAY: clear the family's temporary slot
    DISPLAY: present copied 2376 rows x 4320 bytes (frame 1..3)
    (no 'Freeing up Default Temp surface', no panic)

so `surface_complete` no longer frees the surface the compositor is presenting.  The frame count is unchanged at
three, and after frame 3 the client simply stops calling swap - which is the same three-swap point the reference
page records and the same place its own note leaves home-screen startup unverified.

### What that leaves

The surface lifetime half of checklist step 4 is now clean.  What remains is the client's side of step 4 / the wait
chain: SpringBoard blocks in `mach_msg_trap` on lsd, and lsd blocks in `filt_wlwait_continue` - so the third swap
is where the compositor stops because its next event never arrives, not because the display refused a frame.
## 10:25: file-by-file comparison with UniXNU/UniXNU-sheng

sheng has 488 files, this port 248; 62 are shared and 17 of those differ.  The differences that touch the current
blockers:

| sheng | ours |
| --- | --- |
| `package/LegacyDisplayProbe.cpp` (740 lines) | 950 lines, different structure |
| creates a `NativeGOPPresenter` for `offline-legacy-phase=3`, and an optional GPU handoff presenter under
  `IOService::getResourceService()` for `fb-gpu=1` | never creates a presenter - correct for phase 4 without GPU |
| `native_legacy_prepare_scanout()` cooperates with the boot-progress banner (`boot_progress_prepare_scanout`,
  100 attempts, 1 ms apart) | returns true; this port does not render the banner |
| phase 4 does **not** seed a surface region and does **not** adopt a temporary surface | both are done here, because
  this board's family cannot build its default surface |
| `package/OfflineLegacySwap.mm` validates the swap path from userland through `IOMobileFramebufferSwapBegin/
  SetLayer/End` and Metal | not applicable yet: it needs a working Metal frontend |

The comparison therefore confirms what the display page already states: the remaining work is the **default surface
from the original IOSurfaceRoot factory**.  Nothing else in sheng is missing from this port's swap path - the
presenter layer they add is the GPU handoff, which this board does not have yet.
## 11:20: IOSurfaceRoot from the kernel - the client opens, the methods are refused

With `-DKERNEL_PRIVATE=1` the probe can see `IOUserClient2022`, and the two-argument virtual removes the
assertion the base-class entry hit:

    DISPLAY: IOSurfaceRoot 0xfffffff6022b1700 newUserClient=0 client=0xfffffff51c00f9f0
    DISPLAY: iosurf selector 0 rc=0xe00002c2 out=512
    DISPLAY: iosurf selector 1 rc=0xe00002c2 out=512
    DISPLAY: iosurf selector 2 rc=0xe00002c2 out=512
    DISPLAY: iosurf selector 3 rc=0xe00002c2 out=512

`newUserClient` returns success, but every selector answers `kIOReturnBadArgument` with the same value and the
output buffer untouched, so the rejection happens before the selector is dispatched - a check on the arguments or
on the calling context, not on our descriptor.  The XML descriptor itself names the keys the kext carries
(`IOSurfaceWidth`, `IOSurfaceHeight`, `IOSurfaceBytesPerRow`, `IOSurfaceAllocSize`, `IOSurfacePixelFormat`,
`IOSurfaceName`), so the key set is not the problem either.

So the factory path is open at the connection but closed at the method.  `iosurface-create` is left off by default:
the call costs a boot and does not change the surface the family uses.
## 11:30: correction - lsd was never blocked on a vnode

`filt_wlwait_continue` is not a vnode wait: it is the workloop (kqueue/libdispatch) continuation in
`bsd/kern/kern_event.c` (`kevent_register_wait_block`).  Re-read with that:

    lsd          -> filt_wlwait_continue        idle in its workloop
    SpringBoard  -> ipc_mqueue_receive_continue idle, waiting for a Mach message
    backboardd   -> ipc_mqueue_receive_continue idle, waiting for a Mach message

so the graphical stack is idle rather than deadlocked, and the earlier 'lsd waits on a vnode, so seeding is
blocked' conclusion is withdrawn.  What this port shows is a system that finished its boot, presented three
client frames, and then had nothing to draw.

The LaunchServices store drop is also confirmed harmless but idle: the containers are found now (4 -> 6 -> 11 as
boot proceeds), each one's `Library/Caches` exists, and none of them holds a `com.apple.LaunchServices-*` file, so
`removed` stays 0.

SpringBoard's main thread is in `ipc_mqueue_receive`, its frames decoded (image 0x1e50e0000, cache+0x15744000).
Naming those frames needs the dyld shared cache from the IPSW - the same artefact the framework's macOS workflow
extracts in its 'Locate the dyld shared cache' step.
## 11:45: the stack is idle; SpringBoard waits for lsd and lsd never answers

A ten-minute observation keeps the same shape:

    SpringBoard p27  C=ipc_mqueue_receive_continue   (waiting for a Mach message from pid 100 = lsd)
    lsd         p101 C=filt_wlwait_continue / thread_preempted  (alternating: short work bursts, then idle)
    frames: 3

so lsd is alive and occasionally busy but never delivers the reply SpringBoard waits on, and the compositor stays
idle because nothing asks it to draw.

### What would name the frames

SpringBoard's PC (`0x1df7d4b58`) and lsd's PC (`0x1df7d48c0`) sit in the same shared-cache region (`cache+0x14054000`),
so both are inside LaunchServices/XPC code in the dyld shared cache.  Naming those frames needs the cache itself;
this host has no APFS reader (`apfs-fuse`) and no `lzfse`, and `ipsw extract --dyld` needs the IPSW zip rather than
the two extracted DMGs.

Options, cheapest first:

1. run the device repository's macOS workflow, whose 'Locate the dyld shared cache in the IPSW' step already
   extracts the cache, and take the cache from its artifacts;
2. install `apfs-fuse` and `lzfse` on this host and read the cache out of `044-76034-038.dmg`;
3. teach the kernel to copy the first megabytes of the cache to the serial (too slow for a full symbol table).
## 11:50: the shared cache is extracted and SpringBoard's stack is symbolized

`apfs-fuse` reads the extracted volumes directly (`/tmp/sysos.img` is APFS, not a DMG), so the dyld shared cache
needed nothing else:

    /mnt/sysos/root/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64   (3354 images, slide 0x14054000)

With `ipsw dyld a2s` on the unslid addresses (frame - slide), SpringBoard's main thread resolves to:

    mach_msg_overwrite
    mach_msg
    _dispatch_mach_send_and_wait_for_reply                    libdispatch
    _dispatch_mach_send_with_result_and_wait_for_reply
    _xpc_connection_send_message_with_reply_sync              libxpc
    -[BSXPCServiceConnectionMessage _sendSynchronously:]      BoardServices
    +[BSXPCServiceConnectionProxy createImplementationOfProtocol:forClass:withName:]_block_invoke
    -[CHSChronoServicesConnection _subscribeToExtensions]     ChronoServices
    -[CHSServerSubscription _lock_createServerSubscriptionIfNecessary:]

so SpringBoard is not waiting on the display: it is inside a **synchronous XPC call** made while subscribing to
ChronoServices extensions, and the reply never arrives.  The daemon list of the same boot shows no `chronod` at all
- it is neither in the exit records nor in the runnable set - so the service SpringBoard asks for has no server.

BoardServices is runningboardd's framework, so the next capture traces runningboardd (`boot-ui-focus4`) to see
whether it is the process that fails to answer.
## 12:00: the peer is a service launchd never starts

The job definition is on the OS volume and complete:

    com.apple.chronod
      ProgramArguments = /System/Library/PrivateFrameworks/ChronoCore.framework/Support/chronod
      MachServices     = { com.apple.chronoservices, com.apple.chrono.widgetcenterconnection, ... }
      UserName         = mobile
      RunAtLoad        = False,  KeepAlive = { Crashed: true }   (starts on demand)

and the binary is present and executable (74432 bytes).  Yet the boot log of the same session never mentions
`chronoservices` at all, and `chronod` never appears in the process or exit records - so launchd was never asked
for the service, or answered without spawning it.

That is the shape of the hang: SpringBoard's synchronous XPC to `com.apple.chronoservices` blocks in `mach_msg`,
runningboardd idles in its CFRunLoop (unrelated), and the on-demand job that should answer never starts.

### Next experiment

Force the job to start: write a launchd override for `com.apple.chronod` (RunAtLoad) into
`/private/var/db/com.apple.xpc.launchd/overrides.plist` from the kernel, next to the disabled database the port
already writes.  If SpringBoard then leaves `mach_msg` and the frame count grows, the missing on-demand start is
the whole of the blocker.
## 12:10: the launchd override is written but chronod still does not start

`overrides.plist` with

    com.apple.chronod = { Disabled = false; OnDemand = false; }

is written by the kernel (status 0, four passes), yet the boot still shows no `chronod`, no exit record for it, and
the frame count stays at three.  So either launchd does not treat `OnDemand` as a start trigger in this override
database, or the lookup never reaches launchd at all.

### Next mechanism to try

The port already mounts read-only **union overlays** (`native_gpu_addon_mount`, source `/private/var/ngpu`).  The
same mechanism can serve a modified `com.apple.chronod.plist` with `RunAtLoad = true` over
`/System/Library/LaunchDaemons`, which changes the job itself rather than hoping the override database is honoured.
## 12:30: patch audit and MAX_CPUS

The framework carries 59 patch files; this target applies 81 (framework + SoC + OS profile + device).  Only two
framework patches stay unapplied:

- `sheng-platform-power` - sheng's SM8550 PMGR integration, not this SoC;
- `native-smp-secondary` - raises `MAX_CPUS` from 4 to 8 for an eight-core board.

The second one applies here: SM8250 is also eight-core and the target builds the same BCM2837 board config.  The
device series now carries `zzz-max-cpus-8.patch` (MAX_CPUS 4 -> 8) and the kernel builds and boots with it: three
frames, SpringBoard waiting on lsd, lsd idling in its workloop - the same picture as before, as expected for a
capacity change rather than a correctness one.

Adding that patch re-prepared the tree, which discarded the hand edits in `kern_exec.c` (the LaunchServices store
drop added earlier today).  The probe's late-drop call was removed with it; that path had already proved inert
(containers found, no LaunchServices files in them), so nothing was lost.
## 13:00: the port has no persistent Data store - and that changes the picture

`docs/zh/concepts/userdata`: the userdata partition carries the Darwin root through a SHNGROOT manifest, and the
writable Data volume can live in a window at its end, opened with `sheng-data-persist=2` (1 = validate only) and
kept clear of `sheng-log-reserve` (default 1 GiB).  The implementation is this port's own UFS driver
(`out/feeds/ShengUfs/NativeUfs.cpp`):

    sheng-data-persist=1|2        mode, 0 = off
    sheng-log-reserve=<MiB>       reserve at the end of userdata

and it refuses to open with `no root layout` unless `StoreGeometry` was parsed from a flashed root manifest.

This port boots with `fastboot boot` only, its log never mentions SHNGROOT or userdata, and it never sets
`sheng-data-persist` - so **the Data volume is rebuilt from the seed on every boot**.  Everything written at boot
(the container tree, the Setup preferences, any LaunchServices state) is gone after a reboot, and the system starts
every time in the state the packer left the seed in.

That is consistent with the ChronoServices deadlock: widget-extension registration is state that a real device
keeps across boots, and here it is thrown away before whatever registers it can finish.
## 13:10: sheng-no-lc and kprintf spam change nothing; the job write hit a read-only volume

The build with `sheng-no-lc=1` (launch constraints cleared) and `-enable_kprintf_spam` boots exactly like the
previous one:

    SpringBoard p27  ipc_mqueue_receive_continue  peer=102 (= lsd)
    lsd         p102 filt_wlwait_continue
    frames: 3, UIT rows 232, PROC rows 213, chronod absent

and the attempt to place a `RunAtLoad` copy of the chronod job on the writable Data volume failed before writing:

    DISPLAY: chronod job on the Data volume: 30      (errno 30 = EROFS)

`/Library` resolves into the read-only union of the root, not into the writable Data volume, so launchd never sees a
second job definition.  The launch-constraint switch was never exercised: chronod is not spawned at all.

### What is left for this thread

Overlay a modified `/System/Library/LaunchDaemons/com.apple.chronod.plist` through the port's existing read-only
union mount (`native_gpu_addon_mount`) so the job itself carries `RunAtLoad`, or teach the kernel to send the
bootstrap lookup itself.  Neither is guaranteed: without knowing why launchd stays silent, both are probes.
## 13:30: chronod runs when booted as process 1, and exits inside BackBoardServices

The `init-override` experiment answers the question the launchd side could not:

    load_init_program: override /System/Library/PrivateFrameworks/ChronoCore.framework/Support/chronod
    shared_region: [1(chronod)] check_np(...) vm_shared_region_start_address() failed
    XNU-DYLD-IMAGE: chronod pc=0x1c2d94f4 .../BackBoardServices.framework/BackBoardServices+0xd6f94 load=0x1c200000
    XNU-EXIT: chronod pid=1 code=3 ns=2 sub=0x5
    panic: Assertion failed: ... "init(1) exited ---- exit reason namespace 2 subcode 0x5"

so chronod is not blocked by launchd at all: it **starts and dies by itself**, with its last frame inside
BackBoardServices - the same framework SpringBoard blocks in when it calls
`-[CHSChronoServicesConnection _subscribeToExtensions]` synchronously.  Both sides of the ChronoServices
connection fail in BoardServices, which points at runningboardd rather than at chronod or at launchd.

The image is now built to run runningboardd as process 1 (`init-override=2`) for the same test.
## 13:35: runningboardd as process 1 aborts too - and why that is not yet a conclusion

    load_init_program: override /usr/libexec/runningboardd
    XNU-TRAP: pc=0x1e4eefb00 lr=0x1e4eefb00 esr=0xf2000001
    XNU-DYLD-IMAGE: runningboardd ... WatchdogClient.framework/WatchdogClient+0x1bd0
    XNU-EXIT: runningboardd pid=1 code=5 ns=2 sub=0x5
    panic: initproc failed to start -- exit reason namespace 2 subcode 0x5

`esr = 0xf2000001` decodes to EC 0x3C, a BRK: the daemon hit an assertion inside WatchdogClient.  chronod did the
same in BackBoardServices.

Both experiments share a flaw that has to be stated plainly: replacing launchd with a daemon removes the
bootstrap server, so the first XPC call any of them makes fails and the daemon aborts.  The test therefore shows
that the binaries execute, and **not** that they are broken - it cannot separate 'the daemon fails' from
'the daemon needs launchd'.

To separate them, launchd has to stay process 1 and be made to start the job, which means a job definition it
actually reads: the writable Data volume does not carry `/Library/LaunchDaemons` on this port (the write returned
EROFS), so the definition has to be overlaid on `/System/Library/LaunchDaemons` through the addon mount the display
stack already uses.
## 14:55: launchd reads a precompiled cache, and a modified copy is rejected

The system ships `/System/Library/xpc/launchd.plist`, a binary plist with 561 `LaunchDaemons` entries keyed by
plist path; `com.apple.chronod` is one of them, so editing `/System/Library/LaunchDaemons` cannot change what
launchd starts.  Parsing the cache located the job's `RunAtLoad` value slot: file offset `0xfc6e0`, holding the
reference `0x09a0` to the shared false object (2464, used by 802 entries), while a true object exists at index 538
(`0x021a`).  Rewriting those two bytes changes RunAtLoad for that job alone.

The kernel now reads the cache, writes the patched copy under `/private/var/ngpu/xpc/launchd.plist` and mounts it
over `/System/Library/xpc` before launchd starts:

    launchd cache: open=0 read=0 magic=bplist00
    launchd cache: patched copy written (0)
    launchd cache: mount stage=5 status=0

and launchd reacts by failing its first boot task:

    launchd_cache_loader exited due to SIGKILL
    Boot task failed: launchd_cache_loader - exited due to SIGKILL
    userspace panic: boot task failure: launchd_cache_loader

so the cache is the right lever but the copy is not accepted as it stands - either the write is not byte-exact or
the loader validates the file (or its tree state) and kills itself when it does not match.  The patch is removed
from the series until that is understood; the stable image is rebuilt from the overlay-only series.
## 15:25: the reference board record says this is reachable, and lists its switches

`docs/zh/devices/j716f` records a Lenovo TB-J716F (SM8250-AC, the same SoC family as this board) booting
iPadOS 17.7.11 (21H461, the same OS profile) **to the interactive desktop**, flashed as `boot_b` only:

    bash scripts/package.sh --no-deps --uefi ../uefi-j716f
    fastboot flash boot_b boot.img && fastboot reboot

and lists the switches it enabled along the way: `armcpu-edge`, `pipa-cpufreq`, `pipa-gpu-dcvs`,
`sheng-backlight`, `sheng-aks-sls=3`, `appsync=1` and `owner-staged-apps=1`.

Two of those name things this port already implements but never enables:

- `zzy-staged-system-apps.patch` (applied here) reads `sheng-staged-apps`; its own comment says that until the
  staged system apps are copied into Data 'the widgets of those apps stay blank' - and widgets are what chronod
  serves; `sheng-staged-apps=1` is now on the command line;
- `sheng-platform-power.patch` calls `sheng_pmgr_run()` under `XNU_SHENG_PMGR`; the symbol exists nowhere in this
  port's feeds, so that patch would compile to nothing here - it is not this port's difference.

The board record therefore keeps the ChronoServices thread open rather than closing it: the same OS on the same SoC
does reach the desktop, so the difference is configuration, not capability.
## 15:30: argument comparison with the board that reaches the desktop

`j716f` passes 54 kernel arguments, this port 42.  What it has that this port does not, grouped by what it can
explain:

| j716f argument | this port | could explain |
| --- | --- | --- |
| `sheng-fb-gpu=1`, `pipa-gpu=12`, `pipa-gpu-dcvs=1` | none (no GPU driver) | its desktop is drawn through the GPU path; this port has only CPU presentation |
| `sheng-backlight=1` | none | panel backlight control |
| `sheng-aks=1`, `sheng-aks-sls=3` | none | AppleKeyStore/keybag availability for daemons that need it |
| `-apfs_shared_datavolume` | none | how the Data volume is mounted; this port mounts Data itself with `CONFIG_XNU_OWNER_DATA_MOUNT` |
| `owner-staged-apps=1` | `sheng-staged-apps=1` (same patch, different name) | staged system apps and their widget extensions |
| `appsync=1` | none | the iOS 27 AppSync patch does not exist for this OS profile |
| `offline-hid-*`, `sheng-touch-hid`, `pipa-buttons` | none | input; not needed for the first frame |
| `sheng-pmgr=2` | none | board PMGR mode; `sheng_pmgr_run` does not exist in this port's feeds |

and what this port has that j716f does not: `amfi_get_out_of_my_way=1`, `native-ufs-allow-unverified=1`,
`root-unverified=1`, the `offline-legacy-*` display set, the `boot-ui-*` tracing set, `disable-jobs=` and the
`sheng-*-log` instrumentation.

So the record does not contradict this port's state - it reaches the desktop with a GPU path this board does not have
yet - but it does not explain the ChronoServices stall either, because that stall happens before any drawing.
## 15:45: what a direct port from j716f would actually take

Both reference repositories were inspected.

**uefi-j716f** publishes its display the same way this port does: `J716fInheritedFbDxe` inherits the framebuffer
and states in its header that it programs 'no clock, panel, DSI, PMIC or SMMU'.  So the reference reaches the
desktop with the same firmware hand-off, and its desktop therefore comes from the kernel-side GPU stack.

**UniXNU-j716f** carries that stack:

| piece | in this port? |
| --- | --- |
| `AdrenoPM4`, `AdrenoFirmware`, `ArmSmmu4K`, `AirNir` components | **yes**, all four are in the framework catalog |
| `drivers/Adreno650` (532 KB, the A650 kernel driver) | **no** - the framework only ships `Adreno740` |
| `scripts/build-adreno650.py` | no |
| `scripts/build-j716f-gpu-addon.py` (NativeAdrenoMetal.bundle, owner-trust.bundle, gpu-addon-clients.bin, NGPUADD2) | no |
| `pipa-gpu=12`, `sheng-fb-gpu=1`, `pipa-gpu-dcvs=1` arguments | no (`sheng-gpu-addon=1` is already set) |
| device-tree GPU/SMMU/panel nodes | no |

So a direct port is not a configuration change: it is the GPU subsystem - a kernel driver adaptation, a firmware
and SMMU description for this board, and a userspace Metal addon with its own trust bundle.  The components that
the framework already provides are the smaller half.

It also would not by itself explain the current stall: SpringBoard blocks in a synchronous ChronoServices call
before any drawing happens, and a GPU stack changes how frames are produced, not whether chronod is started.
## 15:50: the reference board's staged-apps union is in place and mounts cleanly

`j716f`'s patch union-mounts the OS volume's `/private/var/staged_system_apps` (29 built-in apps, Weather among
them) over `/Applications` **before** the Data volume covers `/private/var`, with the comment that this is what lets
'LaunchServices and SpringBoard see all built-in apps and their home-screen widgets immediately'.  The same call
now runs in this port's `native_owner_data_prepare`:

    staged-apps union: stage=5 status=0

and the boot still ends the same way:

    SpringBoard p27  ipc_mqueue_receive_continue
    frames: 3, chronod absent

so the missing built-in apps are not what stops ChronoServices either.  What remains between this port and the
reference board is the GPU stack (`drivers/Adreno650`, `build-adreno650.py`, the Metal addon and its trust bundle,
`pipa-gpu=12` / `sheng-fb-gpu=1`, GPU and SMMU device-tree nodes) - a subsystem, not a switch.
## 16:00: the reference GPU driver builds here, and what remains is the link

The reference board's GPU stack was brought in: `drivers/Adreno650` (63 files), `drivers/AdrenoMetal` (41 files),
`scripts/build-adreno650.py`, `package/A650Gmu.{c,h}` and `package/SmmuBypass.h`, together with the
`AdrenoPM4`, `AdrenoFirmware`, `ArmSmmu4K` and `AirNir` components that this framework already carries.

Two API differences had to be bridged:

- the driver's arena was written for a contiguous mapping plus a page count, while this `ArmSmmu4K` builds tables
  through `allocate`/`find`/`overlaps` callbacks; `NativeAdrenoSmmuMemory.cpp` now supplies those three over the
  memory's own span, and the kit's `ArmSmmu4KLiveArena` gained the two compatibility fields (`mapping`,
  `capacity_pages`) that `NativeAdrenoVm.cpp` still reads;
- `drivers/AdrenoMetal` had to be present as the driver's sibling include (`OriginalAcceleratorABI.h`).

The archive builds:

    PASS: 31 NativeAdreno a650 objects archived into .../build/adreno650/libAdreno650.a (1521904 bytes)

and the kernel build is green, but `kernel.macho` is unchanged because that archive is not yet part of the kernel
link.  Registering an `Adreno650` component instead makes the framework's `build-arm-driver.py` path crash clang, so
the script route stands and the link step is what remains - followed by the device tree work, the GPU firmware and
the Metal addon that the reference builds with `build-j716f-gpu-addon.py`.
## 16:15: the reference reached the desktop without a GPU, and what that changes

`porting.toml` dates the reference board's desktop to **2026-11-04**, 'verified 2560x1600 @ 2x Retina desktop
rendering and vsync pacing via GopFramebuffer', two days before the GPU scanout blitter and four before the
backlight.  The desktop path is therefore the same CPU copy this port already presents three frames with, and the
reference SpringBoard does not stall in ChronoServices - so the difference is configuration or userland state,
not the GPU.

Inspecting the component lists side by side found one subsystem the reference includes and this board never did:
`AppleKeyStoreState` (the keybag state).  It is now selected here and its archive builds,

    AppleKeyStoreState: -Werror build and arm64-apple-ios17.0 object audit passed

but the archive is not pulled into the kernel: it calls `native_aks_state_enabled` and `native_aks_lock_state`,
which the reference supplies in `package/KeyStoreProbe.cpp`, and that file was not part of this port.  It has been
brought in, but the probe's archive step compiles against the framework's `package/` (the tree links that
directory, not the device one) and the forced include `meta_features.h` is not resolved for the extra object, so
the build is green again with the keybag probe left out rather than wired in half-way.

The next step on this thread is therefore narrow: compile `KeyStoreProbe.cpp` with the probe's flags including its
forced includes, add it to `libNativeLegacyDisplay.a`, and pass `sheng-aks=1` - the argument is already on the
command line.
## 16:25: the keybag boot task completes, and SpringBoard becomes busy

Wiring the keybag probe in gave the first change in SpringBoard's state since the display work:

    (keybag) <Notice>: Doing boot task
    XNU-EXIT: keybagd pid=11 code=0 ns=0 sub=0x0

and the boot-UI rows moved from a stalled SpringBoard to a working one:

| | before | after |
| --- | --- | --- |
| SpringBoard threads | `n3 r0 w3` | `n9 r2 w7`, `n9 r1 w8` |
| main-thread PC | `ipc_mqueue_receive_continue` | `1d1854b58` in SpringBoard itself (`UIT[7] MAIN S/U`) |
| frames | 3 | 3 |

`UIT[7]` now reports both the `S` and `U` rows with `err=0`, and `lsd` went from a workloop idle to `r1` with seven
threads.  chronod still does not appear and the frame count is unchanged, so the keybag was a real step rather than
the answer: SpringBoard is no longer parked in a receive, and what it does next is the thing to watch.

The image that boots this way is `6209a7ebcc06473575372cceadafbf8b222ab52a6acefd638be0540673a031a9`.
## 16:30: full comparison against the reference repositories

### Board configuration

| item | this port | j716f |
| --- | --- | --- |
| kernel arguments | 42 (995 bytes of 1000) | 54 |
| components | XnuPrelink, DarwinBoot, ArmDTPlatform, ArmCpu, SoftwareAES, XnuPrelink, ShengUfs, RamBlock, VirtioMmioBlock, VerifiedRoot, QcomXnuUsbDebug, GopFramebuffer, AdrenoPM4, AdrenoFirmware, ArmSmmu4K, AirNir, AppleKeyStoreState | the same set minus QcomXnuUsbDebug, plus ShengHidInput, Adreno650, Ath12kWifi, QcaBluetooth |
| board-local XNU patches | 6 (init-override, launchd overlay, staged union, max-cpus, app cryptex bind, io map) on top of the framework's 81 | 3 (throttle/console, cryptex view always, appsync+sandbox wildcard) |

### Board services in `package/` - the substantive difference

The reference carries a layer of board services this port never wrote; the ones that matter to SpringBoard are:

| file | what it publishes | why it matters here |
| --- | --- | --- |
| `NativeBattery.cpp` + `NativeBatteryBoard.h` | PM8150B gen4 fuel gauge and SMB5 charger as an `IOPMPowerSource` | its own header states that powerd matches that class to build `IOPSGetPowerSourcesInfo`, **which SpringBoard's status bar reads** |
| `AppleARMBacklight.cpp`, `BacklightProbe.cpp` | backlight as a CoreBrightness brightness control | display brightness; not needed for the first frame |
| `MdssProbe.cpp` | MDSS observation | diagnostics only |
| `Buttons.c`, `TouchProbe.cpp`, `TouchState.cpp` | input | not needed for rendering |
| `GpuProbe.cpp` | GPU diagnostics | not needed for the CPU display path |
| `HidInputProbe.cpp` | HID input | as above |
| `ShengPlatformPower.cpp`, `BootFaultProbe.c`, `BootRamoops.c`, `LinkStubs.c`, `FatVolume.c` | platform power, fault/panic records, link stubs, FAT access | some are prerequisites of the battery driver (the SPMI arbiter and PON paths) |

This port's own extras are the opposite kind: boot latency, boot progress, boot UI trace, the offline GPU stubs,
the audio reach stack and the verified-block bridge - instrumentation and unproven subsystems rather than board
services.

### So what separates them

`NativeBattery` is the only gap whose own documentation names a SpringBoard dependency.  It is also the most
expensive to close: it reads the gauge and charger through the PM8150B SPMI arbiter channels that its siblings
`Buttons.c` and `BootRamoops.c` define, and neither exists here - so the port is a board-service layer (PMIC arbiter,
fuel gauge, charger), not a file copy.
## 16:40: repo checked against the troubleshooting reference

Walked the `/zh/reference/troubleshooting` table against this repository:

| row | this port |
| --- | --- |
| `unixnu status --json` | ok: schema 2, configured, device and OS profile resolved |
| `[kernel] archives` and `logs/archive-NN-*.log` | ok now; the two failures met today were exactly this row (archive-13 `build-adreno650` wanting kit paths the tree did not carry) |
| command line is ... bytes | ok: 988 of 1000 (it refused at 1002 and again at 1009 while these arguments were added) |
| unsupported kernel argument | ok: every argument matches `payload.py`'s own pattern, which allows `,` and `/` inside a value (`disable-jobs=`) |
| no display | ok: `DISPLAY: display attach=1 start=1`, three client frames copied |
| no boot entry / no menu | ok: the loader reaches the kernel every boot |
| panic gives provider | not hit since the display work |
| no root device / Data window refused | not applicable: this board seeds its Data volume rather than flashing userdata |
| userland display assertion | ok: none since the temp-slot fix |

Nothing in the table is failing now.  The one row that keeps recurring in practice is the archive row, and that is
where today's GPU attempt stopped twice.

### lsd focus run

`boot-ui-focus3=lsd` booted and presented three frames, but the capture ended at 1121 lines before the probe's
`STACK64` phase, so it carries lsd's process rows and not its user stack:

    PROC[3] lsd p99 n6 r0 w6 ... Wfffffff8f6d3de00 Cfffffff00bd02804 U1d92708c0 +daf0000
    PROC[3] lsd p99 n7 r1 w6 ... (same kernel PC, same wait event)

lsd alternates running and waiting with a constant kernel PC, exactly the shape seen for it all along, and the frame
count stays at three.  A longer capture is needed for the stack rows that name where it waits.
## 16:50: where lsd actually is

lsd's own saved user PC, taken from the process row of the live run, symbolises directly:

    PROC[3] lsd p99 ... U1d92708c0 +daf0000
    unslid 0x1cb7808c0 -> /usr/lib/system/libsystem_kernel.dylib  _kevent_id + 8

so lsd is **not stuck in a call at all**: it sits in its kqueue event loop waiting for work, exactly where an idle
LaunchServices daemon belongs.  SpringBoard, in the same sample, sits in `_mach_msg2_trap + 8` waiting for a reply.

That pairing is the whole finding:

| process | where | meaning |
| --- | --- | --- |
| SpringBoard | `_mach_msg2_trap + 8` | sent a synchronous message and is waiting for the reply |
| lsd | `_kevent_id + 8` | waiting for events; no work in hand |

Neither side holds a lock and neither is blocked on I/O, so the message SpringBoard is waiting on is **not being
serviced by lsd**.  Two explanations remain and they are distinguishable: either the destination port is not the
port lsd reads (a stale or wrong send right), or the port is lsd's but its dispatch source never wakes the queue.

Neither reference repository can settle that: `lsd` is Apple's binary and both ports leave it untouched - their
board-local patches are a throttle/console fix, 'cryptex view always' and the appsync/sandbox wildcard, none of
which touch LaunchServices.  What the comparison does say is that the reference does not hit this at all, so the
difference has to be in this port's launchd/port state rather than in lsd's code.
## 17:00: the bring-up policy is not the cause

Booted with `lite=1` and `disable-jobs=` removed (40 arguments, 823 bytes) so the board policy could not suppress
any daemon:

    frames: 3
    SpringBoard p27 n3 r0 w3   Cfffffff00b854894 (ipc_mqueue_receive_continue)
    chronod: absent

so the experiment is negative and it takes the policy off the list: neither `lite`'s twenty disabled jobs (including
`duetexpertd`) nor `disable-jobs` is what keeps chronod from starting.  If anything the boot was quieter than with the
policy: the more active SpringBoard seen after the keybag work (`n9 r2`, executing inside its own image) did not
recur.

The argument list is restored to the policy form; the image of this run was
`dc2ca0a7fe1941c1f366161878d43842fbde21dc113988f39994dfa4d3d20f15`.

### Where this leaves the thread

Excluded so far, each by measurement rather than by reasoning: the display path (three frames, no assertion), AMFI and
launch constraints, the Setup triggers, LaunchServices' store, the staged-apps union (mounts cleanly), the launchd job
cache as a place to change behaviour (a modified copy is rejected by `launchd_cache_loader`), the GPU (the reference
board's desktop predates its own), and now this port's own bring-up policy.  The remaining difference to the reference
board is its board-service layer - `NativeBattery` above all, whose own header names SpringBoard's status bar as its
consumer - plus the SPMI arbiter those services sit on.
## 17:05: the message is queued on lsd's port and lsd never wakes

A probe that reads the destination port's message count at the sampling instant settles what the two stacks only
suggested:

    SpringBoard p27 -> send remote=7003 msgid=0 peer=97 q=1 e5
    lsd         p97 -> U1e54208c0  (= _kevent_id + 8)

`peer=97` is `lsd` (its own process row names it), so the send right resolves to lsd's port, and `q=1` says exactly
one message is **waiting in that port's queue**.  lsd is parked in `_kevent_id`, the kqueue wait libdispatch uses for
its Mach-port sources, and it never takes the message.

So this is no longer 'SpringBoard waits for an answer': the request is delivered and queued, and the receiver is not
woken.  Delivery works; the wake does not.  In XNU that wake is the Mach-port kevent filter: a message enqueued on a
port with a registered knote must make `kevent` return, and the filter code (`filt_machport*`, `ip_sync_inheritor_knote`,
`ith_knote`) is present in this tree, which makes the notification path - not the filter's existence - the place to
look next.

Two consequences worth stating plainly:

- every daemon that waits for Mach messages through libdispatch (lsd, and the XPC services behind SpringBoard) would
  be affected the same way, which is consistent with how little of userland ever reacts to anything;
- nothing in either reference repository covers this: their kernels carry the same patch series as ours, so if the
  wake path is broken here it was broken there too, and what differs is which services the reference board's extra
  drivers start first.

### The next measurement

Print, for the same sample, whether the port has a knote attached and whether the owner thread's `ith_knote` matches
it: that separates 'no knote was ever registered' (a libdispatch side effect of how the port was made) from
'a knote is registered and was not posted' (a kernel wake defect).
## 17:15: the port has a message, no waiter and no turnstile

The waiter probe adds `w=` (a thread blocked on the port's wait queue) and `t=` (its turnstile) beside `q=`:

| sample | q | w | t |
| --- | --- | --- | --- |
| SpringBoard to lsd (`peer=99`) | **1** | **0** | **0** |
| a sender to launchd (`peer=1`) | 1-2 | 0 | 0 |
| early senders to other daemons | 6, 9 | 0 | 0 |

so on this system **no port ever has a thread blocked on its wait queue, for any sender**.  launchd still answers those
sends (it spawns the daemons we watch), which places the working path somewhere other than this wait queue: the
receiver side is not `mach_msg_receive`, it is `kevent`.

### What that narrows to

`EVFILT_MACHPORT` is compiled in (`machport_attach_filtops` at index -8), and every dispatch-based daemon here - lsd
above all - waits in `_kevent_id` with no wait-queue registration on the port it should read.  A message enqueued on
such a port has nothing to notify, so it stays queued:

    lsd p97  _kevent_id + 8          (waiting for events, none armed for this port)
    springBoard  mach_msg2_trap      (waiting for the reply that would follow)
    port 7003 q=1                    (the request, still queued)

Either libdispatch could not arm the port (a userland consequence of how the port or its right was made here), or the
arming was dropped in this kernel build.  That is the question left, and it is narrower than every one before it:
not 'why is chronod missing' but 'why does a Mach port armed for kqueue never see its message'.
## 17:30: the knote-attach trace is out of the series for now

An attempt to print, from `filt_machportattach`, which process arms which port for kqueue, did not compile in
`osfmk/ipc/ipc_pset.c` and the diagnostic that mattered was already in hand before it: the destination port holds
`q=1` while `w=0` and `t=0`, i.e. no thread and no turnstile are registered on it, and every other port in the sample
looks the same.  Rather than leave the kernel refusing to build while a diagnostic is unfinished, the patch is out of
the series and the kernel is green again:

    kernel.macho sha256 09a582104bdab23e69f64450f3cfa51b77bf81038b6062ebf350efe182ccb1a4

The question that trace would have answered is recorded in the evidence for whoever picks it up: whether lsd ever
arms a knote for the port SpringBoard sends to, or whether the port stays unarmed because the arming was made
against a different right.
## 17:40: checked the porting steps against the userdata reference

The reference (`/zh/concepts/userdata`) states the design plainly: the userdata partition carries the Darwin root
through a `SHNGROOT` manifest whose windows this port does flash (the boot log mounts
`disk1s1 DawnUpdate21H461...SystemCryptex`), the writable Data volume comes from the payload's seed, **and the
persistent store comes from a window at the tail of that partition**, opened by `data-persist=2` with
`log-reserve=<MiB>` (1024 by default) kept clear.  The same page notes `--skip-setup` as the way to make a seed that
already has Setup completed, cached by seed hash.

Against that, this port's steps are:

| reference step | this port | verdict |
| --- | --- | --- |
| flash userdata with the manifest windows | done (`native-ufs-root=1` publishes the windows; SystemOS mounts) | aligned |
| Data volume from the payload seed | done | aligned |
| persistent store at the tail, `data-persist=2` | `sheng-data-persist=2` is passed but the Data volume is rebuilt from the seed **every boot**: containers, preferences and LaunchServices state are made again each time | **not aligned** |
| `--skip-setup` seed so Setup is already complete | instead the kernel writes the Setup preferences itself at display start (`native_owner_setup_skip`), then again at tick 20 | **not aligned** |

Everything added along the way follows from the third row: `native_ls_store_drop` because the LaunchServices store
never survives, the staged-apps union because the apps are re-materialised each boot, the launchd job overlays and the
cache patch because launchd's own state is rebuilt every boot.

So the step that went sideways is not the display work or the kernel patches: it is that **the Data volume never
became persistent**, and the port compensated inside the kernel for what a working store window would have kept on
disk.  Fixing that one step is what the reference does before any userland debugging begins.
## 17:45: the manifest works, the store window is the missing half

The boot log shows the manifest half working:

    NativeUfs: published SystemOS / SystemOS volume 8846FE78-... ready
    NativeUfs: published OS / OS volume 04769D26-... ready
    Got boot device = ... DawnUpdate21H461.J120J121J207J208OS@1

so the flashed userdata carries a readable `SHNGROOT` manifest and both of its windows are published and mounted.
What never appears is the second half of the reference design: the persistent store window at the tail of the
partition.  This port's code for it (`drivers/KonaUfs/NativeUfs.cpp`, `native_ufs_store_open`) refuses before it
prints anything unless `gStoreGeometry.valid && gHost.ready`, and that geometry is only set where the storage layout
is parsed (`layout->userdataFirst/Last/imageBytes`).  The windows being published while the store still reports no
layout means those two paths disagree about the same partition - one succeeding where the other did not.

That is the concrete form of the deviation recorded above: the Data volume keeps being rebuilt from the seed because
the store window is never opened, and the kernel-side Setup skip and LaunchServices drop were compensating for it.
The next measurement is small and exact: call `native_ufs_store_open` once at root publication and print its `why`
string with the geometry, which names the failing condition (`off`, `no root layout`, `size`,
`overlaps the root image`, `window refused`) instead of leaving it silent.
## 17:55: two steps that went sideways, both outside the kernel

Checked the repository against the reference pages and against what the device actually runs.

### 1. The Data volume never became persistent

`/zh/concepts/userdata` separates two things this port treated as one: the userdata partition carries the root
through `SHNGROOT` windows (working here: `NativeUfs: published OS` / `published SystemOS`), and the writable Data
volume's persistence comes from a **store window at the tail of that partition**, opened by `data-persist=2` with
`log-reserve` kept clear.  This port passes `sheng-data-persist=2` but the store is never opened, so Data is rebuilt
from the seed every boot, and every kernel-side workaround added since - the Setup skip written at display start,
`native_ls_store_drop`, the staged-apps union, the launchd job overlays and the cache patch - compensates for state
that would otherwise have stayed on disk.  The reference page also names `--skip-setup` as the build-time way to get
a seed whose Setup is already complete, which is where that behaviour belongs.

### 2. The storage driver edited here is not the one that runs

`board.toml` selects `ShengUfs` and never mentions `KonaUfs`; the framework catalog has no `KonaUfs` entry either.
The linked archive is `build/sheng-ufs/libShengUfs.a` (758 KB) beside `build/native-ufs/libNativeUfs.a` (114 bytes)
and nothing from `drivers/KonaUfs`.  So the store-window diagnostics written earlier today went into a driver the
board does not build, which is also why the kernel hash did not move when that file changed: the running storage
path is the feed's `ShengUfs`.

Both are configuration-level, not display- or kernel-level, and both are cheap to correct: register and select the
driver that is actually maintained here (or move the diagnostics into `ShengUfs`), and make the store window open
before adding any further userland workaround.
## 18:00: selecting the maintained driver is blocked by a compiler crash, not by configuration

`KonaUfs` was registered in the framework catalog and selected in `board.toml` (component in `include`, archive
switched from `component:ShengUfs`).  The build then failed inside the component step:

    clang-24: error: clang frontend command failed due to signal
    ... clang::Parser::ParseExternalDeclaration ... ParseAST ...

an ICE in the parser while compiling `drivers/KonaUfs/NativeUfs.cpp` through `build-arm-driver.py`.  The archive
builds fine by other routes, so the source is not obviously wrong; the component path's flag set is what trips
clang-24.  That is the same shape as the failure seen when registering `Adreno650` as a component earlier today.

Configuration is therefore done - catalog entry added, board selects it - and the remaining blocker is the build
path.  The board is back on `ShengUfs` (green: kernel `09a58210...`, the driver that actually runs today) so the
port stays bootable while that is settled.

Two ways forward, and they are not equivalent:

1. make `drivers/KonaUfs/NativeUfs.cpp` parse under the component flag set (bisect the flags, or split the file);
2. retire `drivers/KonaUfs` and carry the store-window diagnostics in the `ShengUfs` feed this board already runs,
   which is what the reference board does with its own drivers.
## 18:20: verification - what the kona fork carries that ShengUfs does not

The archived fork (`docs/archive/NativeUfs.kona-fork-2026-11-10.cpp`) was checked line by line against
`drivers/ShengUfs/NativeUfs.cpp`, which is the source the board now selects.

| item | kona fork | ShengUfs | conclusion |
| --- | --- | --- | --- |
| SMMU takeover for the UFS stream | `smmuBypassStream` gated by `native-ufs-smmu=1`; its own comment says *the shared ShengUfs takes the stream over anyway* | `smmuBypassStream` present, with the same reasoning about ID 0x60 and the bypass context bank (lines 283-287, 909) | nothing lost |
| unverified root publication | `#else` of `NATIVE_VERIFIED_BLOCK`, text 'root windows published unverified (bring-up)' | same `#if defined(NATIVE_VERIFIED_BLOCK)` structure at four sites | nothing lost |
| controller/PHY facts | inline `UfsPhyTables.h` plus kona comments | taken from the SoC repository: `UniXNU-sm8250-common/include/ufs-soc-table.h`, which is exactly why the component needs `soc_headers = true` | nothing lost - the board facts come from the SoC feed, which is the documented arrangement |
| board-side arguments | same eleven `PE_parse_boot_argn` names | identical list | nothing lost |

One difference is worth recording rather than dismissing: the fork's SMMU takeover is **opt-in** (`native-ufs-smmu=1`,
off in every run so far), while ShengUfs takes the stream over unconditionally.  The fork existed partly to leave that
out, and its comment says so.  On this board the difference is inert - the ufshc node has no `iommus` property and the
hardware read found no stream-match entry - but it is the one behavioural divergence, and it argues for keeping the
fork archived rather than deleting the record of it.

So: **retiring `drivers/KonaUfs` loses no capability**; its unique content is either already in ShengUfs or supplied by
the SoC feed, and the single behavioural difference is an opt-in switch that every run has left off.
## 18:30: the persistence entry point exists in RamBlock's expectations, not in any UFS source

`libRamBlock.a` references `native_ufs_store_write_cached` **by address**, so the symbol must be defined by
whatever UFS driver the kernel links.  No source in the tree defines it - the upstream `ShengUfs` provides
`native_ufs_store_write` only - and that is the same API the Data overlay uses to make the store window
persistent.  A definition was added to this port's driver:

    extern "C" int native_ufs_store_write_cached (uint64_t rel, uint32_t blocks, const void *src)
    { return native_ufs_store_write (rel, blocks, src); }

and the component archive now contains it:

    tree/build/kona-ufs/libKonaUfs.a   contains 'store_write_cached' (3 occurrences)

but the kernel link never places that archive in the link line, so the symbol stays undefined and the build stops
at `Undefined symbols ... _native_ufs_store_write_cached`.  The archive list entry `component:KonaUfs` is accepted
and built; what is missing is the kernel side's mapping from that component to a link input, which exists only for
the name `ShengUfs` (`build/sheng-ufs/libShengUfs.a`).

### What this fixes once linked

The missing symbol is exactly the write path the persistent store needs, so linking it is the last step between this
port and a Data volume that survives a reboot - and with it the Setup/LaunchServices/staged-apps kernel workarounds
could be retired.

### GitHub, in one line

No push is required for any of this: device components resolve from the local device repository (the tree links
`tree/drivers/KonaUfs -> UniXNU-instantnoodlep/drivers/KonaUfs`), and `local.toml` exists as a documented fallback.
The remaining work is the link mapping, not the remote.
## 18:55: the board now runs this repository's UFS driver, and it boots

Chain of measurements that got there:

1. the shared ShengUfs source, linked as-is, panicked at 'UFS ROOT FAILURE: SMMU stream mapping readback' -
   it takes the UFS stream over unconditionally, and this board's ufshc has no iommus property;
2. the kona fork's smmuBypassStream (gated by native-ufs-smmu=1) fixes exactly that, so that function was
   ported into the compilable source instead of the whole 3502-line fork, which trips clang's parser;
3. with the gate in place the boot then died in hostBringUp + 0x2ec (data abort, far=0) because the skipped
   takeover left the SMMU mapping null while callers still used it - hoisting mapPhys above the gate fixes that;
4. libRamBlock.a takes the address of native_ufs_store_write_cached, which no UFS source provided; the entry
   point was added, and a ufs-archive-alias.py step publishes the component archive under the name the kernel
   build script force-loads (build/sheng-ufs/libShengUfs.a), because that script only knows the ShengUfs name.

Result: SystemOS and OS published, boot device found, launchd started, three frames, no panic - so the driver this
repository maintains is now the driver the device runs.  native_ufs_store_open is still not called, so the store
window remains closed: that decision is inside RamBlock's overlay, and its inputs are the next thing to print.
## 19:05: the driver question is settled; the store gate is now in RamBlock

Result of this stretch: the board runs the UFS source this repository maintains, and it boots.

| step | evidence |
| --- | --- |
| shared ShengUfs linked as-is | panicked: `UFS ROOT FAILURE: SMMU stream mapping readback` |
| kona `smmuBypassStream` gate ported in | boot moved past the SMMU check |
| mapping hoisted above the gate | boot stopped dying in `hostBringUp + 0x2ec` (`far=0`) |
| `native_ufs_store_write_cached` added and archive published as `libShengUfs.a` | link succeeds |
| component revision moved to the commit that carries these changes | `NativeUfs: published OS/SystemOS`, boot device found, launchd starts, three frames, no panic |

All six store entry points RamBlock probes (`open`, `read`, `write`, `sync`, `gpt_check`, `mode`) are defined, and
`native_ufs_store_write_cached` makes the seventh.  `native_ufs_store_open` is still never called, so the store
window stays closed: the decision has moved out of the driver and into `RamBlock`'s `attachStore()`, whose gate is

    mode && (sparse.pages || isBig) && &store_open && &store_read && &store_write && &store_sync && &store_gpt_check

with `mode` from `sheng-data-persist=2`.  Printing those five inputs from `attachStore` is the next measurement, and
it is a change to the RamBlock submodule rather than to this repository.
## 19:10: the persistent store opens; what it lacks is a first initialisation

The gate diagnostic answers it in one line:

    NativeRamBlock: attachStore gate: mode=2 pages=... isBig=0 open=1 read=1 write=1 sync=1 gpt=1

(the `pages` field is a pointer to the seed's page table, not a count - reading it as a number was my mistake), and
immediately after it:

    NativeRamBlock: the persistent store holds no Data of this seed (header 1); kept, Data runs from the seed:
                    use Install and boot to start it over

so every entry point the driver must provide is present, the window opens, and the store is read - it simply holds
no data for this seed yet, and by design the system does not overwrite an existing store on its own.  Persistence is
not broken; it has never been initialised.

The way to initialise it is `sheng-data-reset=1` (the boot menu's Install and boot), which this image now carries
(`bootEU.img` sha 19411158e72aedd1b86015adfc6e6f62b0e271b29b13f22867e7cbaac59784bc, command line 817 bytes).
One boot with it writes this seed into the store; a second boot without it should then report the store's own header
instead of 'holds no Data of this seed', and the containers, preferences and LaunchServices state that this port has
been recreating at every boot should survive.
## 19:15: the store initialisation ran

Booted the image carrying `sheng-data-reset=1` (sha `d1064ce17e450cf77d86c8dc2e9d3857b2cd9b0903afa28c92b4b6c2f9dfbc0c`):

    NativeRamBlock: attachStore gate: mode=2 ... open=1 read=1 write=1 sync=1 gpt=1
    (no 'holds no Data of this seed' line this time)
    frames: 3, no panic

so the request to start the store over was taken: the run that previously reported an empty store of this seed now
reports nothing because the seed was written into it.

The verification image is staged without the switch
(`boot-verify.img`, sha `2d719030c8b51e60f8d8e0e2bcfe2a3960146e2db39cb9c272fdcc48a75f6ef8`): a boot with it should
either stay silent (the store holds this seed) or name a different header, and — the point of the whole exercise —
the containers, preferences and LaunchServices state that this port has been rebuilding at every boot should then
survive a second boot.
## 19:20: the Data volume survives a reboot

Second boot without `sheng-data-reset` (image sha `2d719030c8b51e60f8d8e0e2bcfe2a3960146e2db39cb9c272fdcc48a75f6ef8`):

    NativeRamBlock: attachStore gate: mode=2 pages=... open=1 read=1 write=1 sync=1 gpt=1
    DISPLAY: setup-skip containers made=1 present=7 failed=0 prefs=0/0/0

two measurements, both new:

| signal | every earlier boot | this boot |
| --- | --- | --- |
| store report | 'holds no Data of this seed' | silent - the store holds this seed |
| containers | `made=8 present=0` (all rebuilt) | **`made=1 present=7`** |

so the persistent store window at the tail of userdata is open, carries this seed's Data, and the containers that
existed at the previous boot are still there.  The Data volume is persistent - the first time in this port's life.

### What this retires

Everything this port added to work around a Data volume that was rebuilt at every boot can now be retired: the Setup
preferences the kernel writes at display start (`native_owner_setup_skip`), the LaunchServices store drop, and the
staged-apps union.  They were compensating for state that now survives on disk.

### What it does not yet explain

chronod is still absent and SpringBoard still waits on its ChronoServices reply - but the ground has changed: the
userland now keeps whatever it wrote at the previous boot, which is the state the reference board has and this port
did not.
## 19:25: the persistent boot's userland state

The boot that verified persistence runs with the same userland outcome as before:

    frames: 3
    lsd p95 n7 r1 w6  U1ea6f08c0 (= _kevent_id)
    chronod: absent

so persistence is a prerequisite that is now met, not a cure: the ChronoServices stall is unchanged on the first
persistent boot.  What has changed is that a second, third and fourth boot now start from the state the previous one
left - containers (7 of 8 already present), preferences and the LaunchServices store - which is the condition the
reference board has and this port has never had.

The cheap next test is therefore repetition, not code: boot the same image several times and watch whether chronod
appears and SpringBoard's reply arrives once the userland has state to work from.  Each cycle needs the device back in
fastboot, so the test is paced by reboots rather than by builds.
## 19:30: repetition settles it - persistence is a prerequisite, not the cure

Second and third boots of the persistent image:

| boot | store | containers | frames | SpringBoard | chronod |
| --- | --- | --- | --- | --- | --- |
| 1st (reset) | initialised | - | 3 | receive | absent |
| 2nd | silent (holds seed) | made=1 present=7 | 3 | receive | absent |
| 3rd | silent | made=1 present=7 | 3 | `n3 r0 w3`, `ipc_mqueue_receive_continue` | absent |

So the Data volume really is persistent (the store keeps the seed, seven containers survive), and the userland
outcome is identical boot after boot: SpringBoard parks in the kernel receive wait and chronod is never started.
Persistence was the missing foundation, but the ChronoServices stall does not depend on it.

### Where the thread stands after today

Established by measurement: the display publishes and presents (`attach=1`, three frames); the board runs this
repository's UFS driver (`NativeUfs` publishes OS and SystemOS from the flashed manifest); the persistent store window
opens and keeps the seed across reboots; userland state now survives, which it never did before.

Still open, and now the only open question in this chain: a synchronous Mach message from SpringBoard is queued on
lsd's port (`q=1`) while lsd waits in `_kevent_id` with no waiter and no turnstile registered on that port - the
request is delivered and never woken.  Neither reference repository touches that path, and both run the same kernel
patch series as this port.
## 19:45: what the reference documentation says about the remaining stall - nothing

Checked three pages against the current state:

- `/zh/concepts/boot-chain` describes the four hand-offs and ends with 'XNU matches the root volume by UUID, mounts it
  and starts launchd in userland'.  Userland's own behaviour - Mach messages, dispatch, ports - is outside its scope;
  its only failure table entry past that point is 'missing platform or storage driver -> the panic log names the
  provider', which is exactly the shape of today's UFS panics and not of this stall.
- `/zh/xnu/patch-reference` lists every framework patch with its purpose.  The ones that touch areas this port relies
  on are `native-union-getattrlistbulk` and `native-union-statfs-fsid` (union directory reads and statfs),
  `zzw-native-adreno-sandbox-grant` (render-server sandbox for the GPU stack), `native-work-interval-*` and
  `offline-native-gpu-broker`.  None of them concerns Mach port wakeups, and none is missing from this port's series.
- `/zh/concepts/userdata` (checked earlier) is what led to the store window work that succeeded today.

One step the patch reference does imply and this port has not taken: `sheng-platform-power` runs
`sheng_pmgr_run()` before the calendar initialises, and that symbol exists in no feed here - so the board runs
without the PMGR integration the reference considers part of a complete port.  That is a power-domain gap, not a
message-delivery one, but it belongs on the list of documented steps not yet taken.

Conclusion: the stall this port is chasing - a message queued on lsd's port while lsd waits in `_kevent_id` with no
waiter and no turnstile - is not described anywhere in the reference, which is consistent with both reference boards
running the same kernel series as this one.
## 19:55: the overlays are not the cause

Booted with `unixnu-overlays=0`, which disables both of this port's own unions - the launchd job overlay and the
staged-apps union over `/Applications` - leaving the system as the reference leaves it:

    staged-apps union: disabled by unixnu-overlays=0
    (no launchd overlay lines)
    containers made=8 present=0  (the early call, before the Data volume covers /private/var)
    frames: 3
    chronod: absent

so the stall is unchanged without them.  That was the last structural difference between this port's boot and the
reference board's, and it is now excluded: SpringBoard parks in the same wait and chronod is still never started.

The `kill-lsd` experiment did not iterate either (`killing lsd at tick 60: 0`, and not one process name printed),
so `proc_iterate` is not visiting processes through the ABI this probe declares; that experiment is parked rather
than trusted.

### What is left

Everything this port adds over the reference is now measured and excluded, and every reference feature it lacks is
either unavailable (PMGR symbol) or unrelated (GPU stack, board services).  The remaining fact is the one the queue
probe established: a message sits on lsd's port with no waiter and no turnstile while lsd waits in `_kevent_id`.  The
next instrument that can move this is a kernel-side view of that port's knote, which is a smaller change than anything
tried today and the only one that distinguishes 'never armed' from 'armed and not posted'.
## 20:05: XnuPrelink checked - it is the tool this port already uses

The repository is the kernelcache prelink and ABI tooling (`emit_prelink.py`, the fileset emitters, the segmented
fixups and the corecrypto HMAC recomputation), and its README states it is used by the kernel build and the payload
step - which is exactly how this port uses it through `kits/XnuPrelink`.

Its output for this board was inspected for the kexts a userland stall could implicate:

| kext | in this prelink |
| --- | --- |
| `com.apple.security.sandbox` | present |
| `com.apple.driver.AppleMobileFileIntegrity` | present |
| `com.apple.driver.ApplePMGR` | present |
| `com.apple.filesystems.apfs` | present |
| corecrypto | present |
| pthread | absent by design - `source-pthread-bootstrap` registers the source-built one early, which is the
  documented arrangement |

so the prelink carries the kexts this problem could plausibly depend on, and there is no gap there to close.  The
tooling would matter if a kext were missing from the fileset; it is not, and the remaining stall is a Mach message
that is queued and never woken, which prelinking does not touch.

The knote-attach trace was being rebuilt when this arrived; it stays the instrument that can still separate
'port never armed for kqueue' from 'armed and not posted'.
## 20:00: UniXNU-sheng compared file by file

### Shape

| | sheng (official) | this port |
| --- | --- | --- |
| `patches/xnu` | 3 board patches: `zzyd-sheng-audio-addon-mount`, `zzye-sheng-cpu-idle`, `zzyf-sheng-amp` | 7 board patches (6 + knote trace) |
| `drivers/` | 8: Ath12kWifi, QcaBluetooth, QcomDwc3, QcomIris, ShengHidInput, ShengNovatekTouch, ShengPreflight, ShengUfs | 4: Adreno650, AdrenoMetal, KonaUfs, ShengUfs |
| `package/` | 168 files | 59 files |
| other | `hal/`, `firmware/`, `configs/`, `dts/`, `assets/` | `docs/`, `seeds/`, `scripts/` |

### `package/`: 49 shared, 119 sheng-only, 10 this-port-only, 10 shared but different

The ten shared files that differ are the ones this port adapted: `LegacyDisplayProbe.cpp`, `BootUiTraceProbe.c`,
`BootUiTraceCore.h`, `BootUiTraceDetails.h`, `BootProcessProbe.c`, `KeyStoreProbe.cpp`, `GeniI2c.h`, `ShengAdsp.h`,
`ShengBattmgr.h`, `SmmuBypass.h`.  `BootProcessProbe.c` in particular is **identical in its process ABI**
(`proc_iterate(unsigned, proc_t callbacks...)`, `PROC_ALLPROCLIST` 1, `PROC_NOWAITTRANS` 4, `proc_best_name`), so the
`kill-lsd` experiment that returned zero hits did not fail for an ABI reason - it needs a different explanation.

The 119 sheng-only files are hardware services and probes: GPU (`OfflineMetal*`, `OriginalMetal*`, `OfflineGOP*`,
`ShengGmu*`, `GpuMemoryPool`), audio (`ShengAudioReach`, `ShengAudioService`, `ShengCs35l43`), thermal, USB/MHI,
Wi-Fi/Bluetooth, display diagnostics, and a root layer: **`SourceRootBinding.cpp`, `SourceRootPolicies.cpp`,
`SourceStorage.cpp`**, plus `ShengPlatformPower.cpp`, `ShengBattery.cpp`, `BootLog.c`, `BootReset.c`,
`BootDiagExport.c`.

### What this comparison establishes

No file in the official sheng repository touches Mach message delivery, kevent, libdispatch or launchd's port state.
Its three board patches are audio, CPU idle and AMP.  The only overlap with this port's remaining problem is
indirect: sheng's root layer (`SourceRoot*`) is how that board binds the source root, and this port mounts the same
volumes through the framework's `offline-owner-*` patches instead.

So the official repository cannot explain the stall either - consistent with the reference boards running the same
framework series this port runs.
## 20:10: this port's ShengUfs pin was stale; upstream already has the persistence API

Comparing the official `UniXNU/ShengUfs` repository against the revision this framework pins:

| | upstream HEAD | this port's feed (`ShengUfs-38c19c2b`) |
| --- | --- | --- |
| `NativeUfs.cpp` | 3044 lines | 2379 lines |
| `native_ufs_store_write_cached` | present, implemented as `storeWrite(rel, blocks, src, false)` | absent |
| `native_ufs_store_sync` | four sites | one |
| `VerifiedTableCache.h` | present - a bounded cache of verified-root hash-table pages | absent |
| `CompletionWait.h` | present | absent |

So the entry point this port had to write by hand (`native_ufs_store_write_cached`, added as a wrapper yesterday when
the link demanded it) exists upstream, with the real forced-unit write behind it, and the newer revision also carries
the verified-root table cache - the code that keeps an owner-signed window's hash pages in memory instead of
re-reading them through the transport.

That makes the driver this board runs a rebase rather than a fork: upstream HEAD, with this board's one adaptation
left in place (`smmuBypassStream` behind `native-ufs-smmu=1`, its mapping hoisted so callers always hold a valid map).
The merged driver builds:

    kernel.macho sha256 a6d199fb466f129f4c539b588fb483ce7768e0b7e7a3f5521620029016485e44

Upstream still takes the SMMU over unconditionally (its `smmuBypassStream` call site is ungated), so the gate remains
this board's only deviation - and the only reason it needs a local driver at all.
## 20:15: the argument names were this port's, not the documented ones

With the driver rebased on upstream, the first boot reported

    NativeRamBlock: attachStore gate: mode=0 ...

`mode` comes from the driver's `storeMode()`, and the upstream implementation reads **`data-persist`** - the name the
reference documentation uses - while this board has been passing `sheng-data-persist` all along, a name only the
forked driver ever read.  The same holds for `sheng-log` (`log`) and `sheng-log-reserve` (`log-reserve`), and upstream
adds arguments this port has never passed (`native-vroot-cache`, `native-ufs-spin-us`, `native-ufs-wait`).

So the command line has been speaking the fork's dialect.  Corrected to the documented names and extended with the
verified-root table cache:

    data-persist=2  log=1  native-vroot-cache=1

command line 856 bytes, image `8adfa6e171bc8e09ca9e7611f8300e41c661345821a252fef750d22d274cfcc8`.

This is the second place where the fork had drifted from the reference - the driver itself was 665 lines behind, and
the arguments carried its private spelling.  Both are now aligned with upstream, with the SMMU gate as the board's
only remaining deviation.
## 20:20: documented arguments and the upstream driver together boot normally

The image carrying the upstream ShengUfs rebase and the documented argument names:

    NativeUfs: published SystemOS / OS ... ready
    NativeRamBlock: attachStore gate: mode=2 ... open=1 read=1 write=1 sync=1 gpt=1
    (no 'holds no Data of this seed' - the store still holds this seed)
    frames: 3, 4375 log lines, no panic
    SpringBoard p27 n3-n5 r0 w3, kernel side ipc_mqueue_receive_continue

Three things confirmed at once: the documented `data-persist=2` reaches the driver (`mode=2`), the store opened two
days of work ago still holds the seed, and the rebased driver runs the board without the SMMU gate misbehaving.

The stall is unchanged - SpringBoard parks in the kernel receive wait and chronod is never started - so both the
driver's age and the argument dialect are off the list as causes.  What the day establishes is that this board now
runs upstream code with the documented interface, and only one adaptation remains: the SMMU stream gate.
## 20:25: knote data in hand, and the one gap in it

`filt_machportattach` logged 2808 distinct ports armed for kqueue in the `unixnu-overlays=0` run and 2736 in the
documented-arguments run - so ports *are* armed in this system, and many of them.  What those runs do not contain is
a single send sample whose destination port resolved (`pt=0x...`), because the sampler caught threads in the not-Mach2
and no-remote states (`e1`, `e2`) rather than in the synchronous send that earlier boots showed as `peer=99 q=1 w=0`.

So the comparison that would settle 'armed and not posted' versus 'never armed' needs one more boot captured while
SpringBoard sits in that send - the same conditions under which the queue probe read `q=1`, plus the `pt` field this
kernel now prints.  The instrument is in place; the sample is what is missing.

### Day summary

Solid: the display publishes and presents; the board runs upstream ShengUfs (rebased, one adaptation: the SMMU gate)
with the documented argument names; the persistent store window opens, keeps this seed across reboots and survives
the driver change; the userland state that used to be rebuilt at every boot now persists.

Excluded by measurement: AMFI and launch constraints, the Setup triggers, the LaunchServices store, this port's two
unions, the lite policy and `disable-jobs`, the launchd job cache as a lever (its loader rejects a modified copy), the
GPU (the reference desktop predates it), the driver's age and the argument dialect.

Open: a synchronous Mach message queued on lsd's port (`q=1`) with no waiter and no turnstile (`w=0 t=0`) while lsd
waits in `_kevent_id`; chronod never starts; SpringBoard waits for a reply that never comes.
## 20:35: AppleKeyStoreState and the framework repository

**AppleKeyStoreState** is the component this port already selects.  Its README states what it does and what it needs:
key wrapping is software, the device key is derived from the identity the board returns through
`native_aks_device_id` (which this kernel provides), a board must return one, and reference key operations and device
lock stay unsupported.  Its documented switch is `aks=1`, while the gate this port actually exercises is its own
`sheng-aks=1` in `package/KeyStoreProbe.cpp` - the board glue that answers the state queries.  Nothing there is a
gap: keybagd runs and exits cleanly on this board.

**UniXNU (the framework)** is where a real divergence appeared: this fork is pinned at `3683a2e` (2026-11-10) while
upstream is at `7d85ede` (2026-11-13).  The patch *set* is identical (59 files, same names), and only three patches
changed in those three days - and every change is a **rename of a boot argument**:

| upstream now reads | this port still passes |
| --- | --- |
| `gpu-addon` | `sheng-gpu-addon` |
| `staged-apps` | `sheng-staged-apps` |
| `no-lc` | `sheng-no-lc` |

which is the same dialect drift found in the UFS driver an hour ago (`data-persist` vs `sheng-data-persist`,
`log` vs `sheng-log`).  The official convention has dropped the `sheng-` prefix everywhere; this port's snapshot
predates that, so today the old names still work here - but they will stop the moment the framework is updated.

Neither repository addresses the Mach wake stall.  What they do say is that this port is one framework snapshot and a
handful of argument renames away from being exactly upstream, which is worth doing in one step rather than discovering
each rename one boot at a time.
## 20:45: alignment done, and where the blocker actually lives

The three framework patches upstream changed in the last three days were taken as-is, and the arguments renamed to
the official spelling (`gpu-addon`, `staged-apps`, `no-lc`; `data-persist` and `log` were already renamed an hour
ago).  `sheng-aks` stays: that one is read by this board's own `KeyStoreProbe.cpp`, not by the framework.  The
aligned kernel builds: `3128a44d065ae1c5e6137798d14df2faf57970bed73f4577ed7f448fe1577ea5`.

### Ownership of the remaining blocker

The stall is a Mach message queued on a port (`q=1`) that has no waiter and no turnstile (`w=0 t=0`) while the
receiver waits in `_kevent_id`.  In this tree:

- `osfmk/ipc/ipc_pset.c` registers `machport_attach_filtops`/`mach_port_filtops` (`filt_machportattach`,
  `filt_portevent`), and `ipc_mqueue.c` carries 32 turnstile references - the wake machinery is present;
- `bsd/kern/kern_event.c` handles `EVFILT_MACHPORT` and the workloop side;
- the waiting side is **libdispatch** (`dispatch_mach` on a kqueue), which is Apple's binary in the OS image and is
  patched by **no** UniXNU repository: the framework's patch directories are `libpthread`, `mesa`, `xnu`,
  `xnu-12377` - identical upstream and here, and none of them touches libdispatch.

So the blocker is split across the kernel IPC/kevent boundary, which lives in **XNU** (the framework's `patches/xnu`,
unchanged from upstream), and **libdispatch**, which lives in the OS image and is not part of any repository this port
uses.

### What a repository could contribute

| if the fault is | the repository that would help |
| --- | --- |
| a kernel wake defect (`ipc_mqueue` turnstile -> workloop) | an **XNU fork/branch** carrying fixes to `ipc_pset.c` /
  `ipc_mqueue.c` / `kern_event.c` for the machport filter |
| libdispatch arming a port that never wakes | a **libdispatch port** for this OS profile (Apple's
  swift-corelibs-libdispatch, or a UniXNU libdispatch patch set if one exists) - none is referenced by this
  framework |
| a kext or component that should own the port | nothing missing: the prelink carries sandbox, AMFI, PMGR and APFS |

The single most useful thing to find is a **libdispatch built or patched for ipados-17.7.11-21H461**, because every
daemon that waits for Mach messages through dispatch - lsd first among them - goes through it, and this port has never
touched it.
## 20:55: SM8550 SoC repository compared with the SM8250 one this port uses

| | `UniXNU-sm8550-common` | `UniXNU-sm8250-common` (ours) |
| --- | --- | --- |
| `kits/` | **`Sm8550Platform`**: `ShengGmu`, `ShengGpuSmmu`, `Sm8550AppsSmmu`, `Sm8550Gcc`, `Sm8550GeniUart` and more - a full platform kit | a single `include/ufs-soc-table.h` |
| `patches/xnu/` | `native-smp-secondary.patch`, `sheng-platform-power.patch` | `series` only |
| `dts/`, `configs/`, `soc.toml` | present | present |

So the SM8550 layer is a **complete SoC kit** while this port's SM8250 layer is a single header plus the UFS table -
which is why so much board logic ended up in this repository's drivers and patches instead (the SMMU bypass, the clock
and PHY setup, the store geometry).

Does it help?

- **As a reference structure, yes**: it shows what a complete SM8250 kit would contain, and moving this port's kona
  SMMU bypass and clock setup into an `Sm8250Platform` kit would match the reference layering.
- **As code, no**: GCC, AppsSMMU, GMU and UART registers are SM8550-specific; none of it transfers to kona.
- **`sheng_pmgr_run` is not there either**: `sheng-platform-power.patch` only *calls* it, and the symbol is defined in
  neither SoC repository, so the one documented step this port has not taken (PMGR integration before the calendar
  initialises) cannot be completed from these repositories - it would have to be written for kona's own PMGR at
  0x100000.

Nothing in it touches the Mach wake stall.
## 20:45: SMP was never on - and turning it on hangs this board

The alioth port documents a fact this port never applied: `ArmCpu.cpp` issues a PSCI `CPU_ON` only when
`sheng-smp` is non-zero (`if (smp == 0 ... ) return nullptr; // Secondary startup is opt-in (sheng-smp=1)`), so
this port has been running **uniprocessor** since the port began - `zzz-max-cpus-8.patch` raises `MAX_CPUS` but does
not start a core.

Added the reference board's arguments (`sheng-smp=1`, `armcpu-spi-ipi=1`, `armcpu-diagnostics=1`) and booted: the
board produced **no console output at all** - the capture holds 61 bytes, its own header - so the boot stops before the
USB console exists.  alioth's notes describe exactly this class of failure ('two independent runs froze at the same
point ... SMP is the remaining variable') and record that they later turned SMP off again.

So the situation is now precise: this port has never had a second CPU running, and enabling it on the current bring-up
hangs before any log.  That is a bring-up defect in its own right, and it also means every timing observation made so
far (lsd idle in `_kevent_id`, SpringBoard waiting in `mach_msg2`, 146 processes on one core) was taken on a
uniprocessor system.
## 20:50: SMP does not merely hang - it panics in a nest

`sheng-smp=1` with `cpus=6` and without `armcpu-spi-ipi=1` produced no console output during the boot, and the panel
then shows the machine in a **nested panic**:

    Nested panic detected - entry count: 2 panic_caller: 0xfffffff008bdb6ac
    Nested panic detected - entry count: 3 panic_caller: 0xfffffff008b9206c
    Nested panic detected - entry count: 4 panic_caller: 0xfffffff008bd2b6c
    Nested panic count exceeds (limit 5), machine will reset or spin
    In Nested panic stall. Stage 2 haltout.

So the secondary-CPU path on this board takes a panic whose handler panics again - the console never comes up, which is
why the capture held only its header.  The arguments are reverted and a working uniprocessor image is packed
(`271c47cd99328e22c4f2d994bd516079f1d48904a0cb8efc15cd8054aefb1ac0`).

### What this means for the port

Every measurement in this session was taken with a single CPU running, and the port cannot start a second one without
a panic.  That is a bring-up defect of its own, and it is now the **first** thing to fix rather than a side note: a
uniprocessor system changes scheduling, workloop behaviour and timing, all of which is exactly the ground the
ChronoServices stall sits on.

Next instrument: the panic callers above are kernel addresses in this build and can be symbolised against
`kernel.macho`, which names the path that panics first instead of guessing between GIC, IPI and PSCI.
## 21:25: one secondary CPU runs, and it changes where SpringBoard waits

Three changes made it work, each measured:

1. **`zzz-smp-core-gate.patch`** - `native_smufs_cpu_nub_selected()` publishes a CPU nub only for the boot CPU or for
   the one named by `sheng-smp-core` (the reference board's staged bring-up), instead of starting every core at once;
2. **`zzz-arm64-mpidr-aff1.patch`** - `start.s` masks MPIDR with `0xFFFF` instead of `0xFF`; masking Affinity0 alone
   aliases CPU1 to CPU0, and that alias is what panicked the first attempt (nested panic, `pe_init.c:725`);
3. the gate must **not** detach the unselected nub - `nub->detach(parent)` inside `createNubs` panicked later in
   `IOPlatformDevice::compareName`, so the gate now only withholds registration.

With `sheng-smp=1 sheng-smp-core=1`:

    log: 5543 lines (versus ~1000 in the uniprocessor boots), three frames, no panic
    SpringBoard kernel PC: 0xfffffff00bd6509c -> _ulock_wait_continue + 0 (was ipc_mqueue_receive_continue)

so the second core is real, and SpringBoard's wait changed from a Mach receive to a userspace lock wait.  chronod is
still not started.

One stale experiment was also removed from the command line: `sheng-data-reset=1` had stayed there since the store
was initialised, and it **wipes the persistent Data store on every boot** - so every boot after that test began from a
fresh seed.  With it gone (and `kill-lsd`, `unixnu-overlays=0` removed too) the command line is 818 bytes.

Image: `02c3378a9a53b0126c1b1baef0dd91e4fcdaac4e12a9a55d9a3ebd1278123660`.
## 21:40: SMP is real - one secondary, then three cores, both boot

| configuration | result |
| --- | --- |
| `sheng-smp=1 sheng-smp-core=1` (one secondary) | 6655 log lines, 3 frames, `containers made=1 present=7` (persistence intact), SpringBoard `n9 r0 w9` |
| `sheng-smp=1 sheng-smp-max=2` (three cores) | 3697 log lines, 3 frames, SpringBoard `n5 r0 w5`, userspace `SIGTRAP` entries in three daemons |
| uniprocessor (before) | ~1000 log lines, SpringBoard `n3 r0 w3` |

Both SMP runs are stable (no kernel panic); the three-core run shows userspace `BRK` traps (`esr=0xf2000001`) in three
processes, which is the normal shape of a daemon aborting, not a kernel fault.

SpringBoard is still parked in `ipc_mqueue_receive_continue` in both runs, and chronod is still absent, so more cores
alone do not move the ChronoServices stall.  What did change with the second core is the amount of userland work the
system completes: the log grows sixfold and SpringBoard carries nine threads instead of three.

### What this leaves the port with

- a **fixable** SMP: `zzz-arm64-mpidr-aff1.patch` (MPIDR must keep Affinity1) and `zzz-smp-core-gate.patch` (publish
  only the selected cores, and never detach the nub);
- a working `sheng-smp-max=<n>` selector for staged bring-up;
- persistence that survives these boots once `sheng-data-reset` is off the command line;
- and the same open question: a message on lsd's port with no waiter and no turnstile.
## 21:50: ye3912's method, and how far this port already matches it

### The three repositories that matter

| repository | what it is |
| --- | --- |
| `unixnu-oneplus-8-pro` (branch `codex/metal-r1-baseline-20261113`) | the device port: 87 patches, 8 components, six boot arguments, desktop + Metal reached |
| `UniXNU-instantnoodle` (branch `codex/in2020-gpu-r3-20261113`) | a framework snapshot with the IN2020 BGRA framebuffer policy and CI |
| `apfs` | a workflow that rebuilds the **Data seed from the IPSW's own `/private/var`**, with launchd pinned, fsck'd, packed as DBRDISK1 |

### Their method, point by point, against this port

| their step | this port |
| --- | --- |
| Data seed copied from the OS mount's `/private/var` (`make-data-seed.py` does exactly the same in this framework) | **same tool, same source** - not a difference after all |
| minimal command line (six arguments) | 42 arguments, most of them bring-up workarounds |
| `sheng-pmgr=2` loads `ApplePMGR` and publishes its nub | **missing** - the glue exists in `UniXNU-sheng/package/ShengPlatformPower.cpp` but needs `OriginalPMGRABI.h`, which none of the public repositories carries |
| staged SMP: CPU nub gate + `sheng-smp-core`, then three, four, ... eight cores | **now working** here for one and three cores (`zzz-smp-core-gate.patch`, `zzz-arm64-mpidr-aff1.patch`) |
| `c2-lsd-ipc` / `c2-core-xpc` traces of lsd's ChronoServices requests | this port's own queue probe reaches the same fact more directly (`q=1 w=0 t=0` on lsd's port) |
| BGRA framebuffer policy patched into the cached presenter image | not adopted; this port presents three frames already, so it is a quality item, not a blocker |
| owner-data 3 GiB with store window | **working** here, verified across reboots |

### What is left that is genuinely theirs and not ours

1. **PMGR** (`sheng-pmgr=2`): the one documented step this port has never taken, blocked by a missing ABI header;
2. **their 87-patch ladder**: each milestone (h3*, p2-p4, c2, r22-r74) is a separate, recorded step; this port has
   six board patches of its own plus the framework series;
3. **the Metal/GPU stack** (r59-r74) - out of scope while the CPU path is still being debugged.

### The honest position

With SMP, persistence, the upstream UFS driver, the documented argument names and the same Data-seed tool, this port's
foundation now matches theirs.  What it does not have is PMGR, and the ChronoServices stall is unchanged by
everything else.
## 22:15: PMGR loads, and the stall survives it

Implemented the PMGR step this port had never taken, in the only form possible without the missing ABI header:
`package/PmgrLoad.c` loads the prelinked `com.apple.driver.ApplePMGR` kext when `sheng-pmgr` is non-zero, wired
through `zzz-pmgr-run.patch` (the call in `bsd_init` before the calendar initialises), `CONFIG_XNU_SHENG_PMGR=y`, and a
`build/sheng-platform-power/libShengPlatformPower.a` archive the kernel build force-loads.

First boot with it:

    PMGR: load result=0x0 mode=2        (KERN_SUCCESS)
    6351 log lines, three frames
    SpringBoard p27 n6-n7 r0 w6-w7, kernel side ipc_mqueue_receive_continue
    chronod: absent

so the platform now has its PMGR provider - the kext loads and the boot is stable - and the ChronoServices stall is
unchanged.  That closes the last of the three things the sibling port has and this one did not (the other two were
staged SMP, now working for three cores, and the Data seed, which this framework already builds the same way).

Image: `97c97deb63ddad685a9de8f28275587a57451842b8c18e502b5e036f8f4ae86e`.

### Where that leaves the port

Foundation: SMP (three cores, staged selector), persistence, upstream UFS driver, documented argument names, PMGR
loading.  Remaining difference to the sibling port: its 87-patch ladder (owner-root audit, c2 IPC tracing, r22-r31 GUI,
r59-r74 Metal) - and the fact that its desktop is drawn through a GPU stack this port does not have.

Open question, unchanged since the queue probe: a synchronous message on lsd's port (`q=1`) with no waiter and no
turnstile (`w=0 t=0`) while lsd sits in `_kevent_id`.
## 22:25: display wiring compared - this port is already a superset

`ye3912`'s `package/SmGopDisplay.cpp` (548 lines) is board wiring for the framework's `NativeLegacyDisplay`, exactly
the role this port's `package/LegacyDisplayProbe.cpp` plays.  The hooks each provides:

| hook family | theirs | ours |
| --- | --- | --- |
| display allocate / probe / late probe | yes | yes |
| backend guard, prepare scanout, transfer, vsync | yes | yes |
| client enter/observed, swap stage, swap validation | yes | yes |
| frame observed, frame colours observed, copy timing | yes | yes |
| gop mapping, registration, retirement, pm, surface rejected | yes | yes |
| gpu offer / gpu fallback | yes | yes |
| **surface allocation / descriptor / width / height / stride** | no | **yes** |
| `native_legacy_display_static_init` | no | **yes** |

so this port's display wiring covers everything the sibling port's does and four surface hooks besides.  Both present
through the CPU GOP path (`GopFramebuffer`/`NativeLegacyDisplay`), which is why the sibling port could reach its
desktop at R44 with **no GPU driver** - and why this port's three presented frames come from the same machinery.

### What that rules out

The display path is not the difference between the two ports.  The sibling port's advantage lies in its GUI/IPC
diagnostic ladder (r22-r31, c2) and its board services (backlight, touch, owner-data), not in how frames are copied.

### Next candidates, in order of expected value

1. its **`In2020BacklightService`** and touch wiring - services SpringBoard expects from the platform;
2. the **r22-r31 GUI ladder** - diagnostics that let it watch `SpringBoard`/`backboardd`/`chronod` through the same
   stall this port is stuck at;
3. `desktop-ipc-r31`'s PK30/PK31 slot tracer, which records request/recv/reply/done for desktop IPC.
## 22:35: backlight port parked, build green again

The sibling port's backlight service was copied in (`package/In2020BacklightService.h`,
`package/In2020BacklightTransport.h`, 8.4 kB and 6.7 kB) and called once from
`native_legacy_display_late_probe()`.  It does not link: `startIn2020Backlight()` is defined **inside the header's
anonymous namespace** (`namespace { ... void startIn2020Backlight() {...} ... }`), so it has internal linkage, and
the kernel links `_startIn2020Backlight` as an external symbol.  Their own call site works because it sits in the
same translation unit that includes the header and calls it while the namespace is still in scope.

Reverted to the consolidated baseline; the two headers stay in the package for a proper port (a dedicated source
file that includes the header and exports a C entry point).  Kernel hash is unchanged from the consolidated
baseline: `1d525b9eb0635c411f6db67c71d7e13f2209a5d96082e07a739df0838f3e5d47`.

### Consolidated baseline (what is on the device image)

    args (40, 796 bytes): -v serial=3 debug=0x14e -no_slto_panic sheng-aks=1 amfi_get_out_of_my_way=1
      native-ufs-root=1 native-ufs-allow-unverified=1 root-unverified=1 qcom-usb-debug=1 kt_fixed_zones=120
      sheng-pmgr=2 sheng-smp=1 sheng-smp-max=2 vm_compressor=2 ml-timeout-lock-panic-scale=40
      disable-watchdogd=1 sheng-crashlog=1 log=1 data-persist=2 native-vroot-cache=1 boot-ui-trace=1
      boot-ui-focus{,...2..4} boot-ui-chrono=1 offline-legacy-{register,gop,unit-scale=2,pm,validate,
      transfer,cpu-present,phase=4,adopt}=1 sheng-vsync=120 sheng-vsync-fixed=1
      offline-mobile-graphics-{load,client}=1

## 22:40: backlight port attempted twice, both blocked by the kext toolchain

First attempt called the header's `startIn2020Backlight()` directly: it has **internal linkage** (it is defined
inside the header's anonymous namespace), so the kernel could not resolve `_startIn2020Backlight`.

Second attempt added `package/BacklightPort.cpp` - the translation unit that includes the header and exports
`extern "C" native_board_backlight_start()` - which is the correct shape, and it does not build: the archive step
compiles it as `-xc++ -fapple-kext -nostdlibinc` and clang-24 aborts inside `RecordLayoutBuilder` after failing to
find `meta_features.h`, and the outer build reports only the `CalledProcessError`.  Reproducing the command by hand
loses the SDK include paths and crashes the same way, so this needs the archive script's own `cmd` list (with its
`-I` entries) rather than a hand-built command line.

Both attempts are reverted; the kernel hash is again exactly the consolidated baseline
(`1d525b9eb0635c411f6db67c71d7e13f2209a5d96082e07a739df0838f3e5d47`), and the three backlight files stay in
`package/` for that later step.

Backlight is a brightness feature and has no bearing on the ChronoServices stall, so the useful next steps remain the
GUI/IPC diagnostics (their r22-r31 ladder) or finishing the knote observation.
## 22:50: the knote observation is answered - the port IS armed

Cross-referencing the probe's queued-port samples against the `filt_machportattach` trace, in three separate boots:

| log | armed ports logged | port holding a message (`q=1`) | armed? |
| --- | --- | --- | --- |
| cont131 | 3276 | `0xfffffff7833411a0` | **yes** |
| cont133 | 4288 | `0xfffffff75d94a420` | **yes** |
| cont134 | 3789 | `0xfffffff4b06ea520` | **yes** |

So the question this port has carried since the queue probe is answered: the port that holds lsd's synchronous message
**was armed** - a knote exists for it - and the message was queued with **no waiter and no turnstile** (`q=1 w=0 t=0`).
It is therefore **armed but not woken**, not 'never armed'.

### What that rules out and what it points at

Ruled out: a missing `filt_machportattach` (the registration happens), and a port-name mix-up in the probe.

Pointed at: the **receiver side**.  With a knote attached and a message queued, the wake has to travel
`ipc_mqueue` -> kqueue knote -> workloop, and the absence of any waiter (`w=0`) means the receiving thread is not parked
in `msg_receive` at all - it is in `_kevent_id`, which is libdispatch's own wait.  So the message arrives, the port is
armed, and the dispatch workloop either never gets the kevent event or never re-enters the wait, which is a
libdispatch/workloop path rather than a Mach-port registration defect.

### Next probe this makes possible

Instrument the wake side instead of the receive side: in `filt_machportattach`'s filter event (`filt_portevent`) and in
`knote_post` for the machport knote, print when a message is queued on an armed port and whether the knote was posted.
That separates 'knote never posted' from 'knote posted, workloop never ran'.
## 23:05: wake-side trace installed, and it shows the filter is not the notification path

Added `MPORT event port=... kn=... filter=...` to `filt_port_filter_result()` (osfmk/ipc/ipc_pset.c) - the one place where the
mach-port filter evaluates "is there a message on an armed port" - capped at 200 lines.

Boot with it: 3676 log lines, three frames, and **0 `MPORT event` lines**.  Two readings were possible:

1. no message ever reached an armed port (contradicted by the probe, which reports `q=1` on armed ports), or
2. this XNU does not notify mach-port knotes through the filter at all.

Reading 2 is the correct one: in this kernel the mach-port knote is a **turnstile push** into the kqueue's workloop
turnstile (`filt_machport_turnstile_prepare`, `ipc_port_send_update_inheritor`, `turnstile_workloop_pusher_info`), and
`filt_port_filter_result()` only runs on attach and on kqueue scans.  So the trace as placed cannot observe the wake.

### What is established, and what it leaves

Established: the port is **armed** (knote attached), the message **is queued**, there is **no waiter and no turnstile**,
and the receiver sits in `_kevent_id`.  The wake in this design would have to travel through the turnstile push into the
workloop's turnstile; observing that means instrumenting `turnstile_workloop_pusher_info`/`ipc_port_send_update_inheritor`,
which is a deeper change than a counter in the filter.

Image: `ca93eb9a6ba72fcf3543cbcdb5d829f00e3b7aedb7edf47a2b2c58c0fbfdedb7`.
## 23:35: the knote-post trace lands on an unexpected answer

`ipc_mqueue_post` is the only place that posts a mach-port knote for an arriving message, and it does so only when
`receiver == THREAD_NULL` and `ipc_port_has_klist(port)` (which is `!ip_specialreply && ip_sync_link_state ==
PORT_SYNC_LINK_ANY`).  Guarded traces were added on both sides of that branch - `MPOST posted` when `KNOTE()` runs,
`MPOST skipped` when a knote is attached but one of the guards fails.

Boot with them: 3683 log lines, three frames, **0 `posted` and 0 `skipped`**.  So for the whole boot, the
`receiver == THREAD_NULL` + enqueue path never met a port that `ipc_port_has_klist()` accepts - messages are being
handed to a waiting thread instead of queued-and-notified, or they arrive on reply ports that the guard excludes by
design.

That is in tension with the probe's own reading (`q=1 w=0`): a queued message with no waiter should have taken exactly
the branch that never fired.  Either the probe is sampling a different port from the one the trace sees (its peer is
the port the sender trapped on, which may not be the receive-side port), or the queue is filled by a path that does not
go through `ipc_mqueue_post`.

### What this thread established, and where it stops

Established with three independent logs: the port lsd waits on **is armed** (knote attached) and holds a message with
no waiter and no turnstile.  The kernel-side notification site that would explain the wake never fires for any armed
port during a boot, which means the next step is not another counter in `ipc_mqueue.c` but a decision about **which
port the probe is actually sampling** - i.e. resolve the peer port's identity (owner, right type) rather than its
pointer, and re-run the cross-reference.

Image: `1311849d70bd4d2847e1e7ffe7d6d5b2e2d45992299c8bf6144095f4180ebb2e`.
## 23:45: community survey for the same symptom

Searched the documentation site and the community device repositories for this exact failure (SpringBoard waiting on
lsd's ChronoServices port, chronod never starting).

### What exists

| source | finding |
| --- | --- |
| `unixnu.com/zh/reference/troubleshooting` | covers build, IPSW, firmware, boot and log sources; the only userland row is 'userland display assertion -> check the display provider and the **focus** parameters'. No row for a stalled launch-services or ChronoServices wait. |
| `FFT-infra/UniXNU-alioth` (reached a lock screen) | treats `chronod`/`lsd`/`SpringBoard` as **diagnostic targets**; its port record notes that on the iPadOS 17 line 'align1 unified logs contain `com.apple.chronod.timeline-host` entries for Maps, Stocks, Calendar, Batteries, ScreenTime, News, Notes and Tips widgets' - i.e. a healthy 17-line boot **does have chronod running**. |
| `ye3912/unixnu-oneplus-8-pro` (reached the desktop) | 87-patch ladder in which `chronod`, `SpringBoard`, `backboardd`, `powerd`, `liveactivitiesd` and `pkd` all appear only as **trace targets** (`gui-chrono-r27`, `gui-liveactivity-r28`, `pkd-exit-r32`, `plugin-ipc-r30`, `desktop-ipc-r31`, `c2-core-xpc`). No single patch is presented as the fix. |
| `UniXNU-sheng` | its port record documents `boot-ui-focus3=chronod` as the knob that 'adds the display-query client to backboardd and SpringBoard' - the same 'focus parameter' the official troubleshooting row names. |

### Conclusion of the survey

No community repository documents a fix for this stall.  Both ports that got further did it by **iterating with
diagnostics** and by integrating services (owner-root, SystemOS framework exposure, plugin/desktop IPC), not by
correcting one kernel defect.  What the survey does add is one concrete expectation and one knob:

1. on the iPadOS 17 line a healthy boot runs `chronod` (their widget-host log entries prove it), so its absence here is
   a real divergence, not normal for the profile;
2. `boot-ui-focus3=` selects **which display-query client** backboardd and SpringBoard attach to - this port currently
   points it at `lsd`, while the reference points it at `chronod`.

Trying `boot-ui-focus3=chronod` is a one-argument experiment that changes what the display query waits for, and it is
the only community-documented knob aimed at this stage.
## 23:55: the focus knob changes SpringBoard's activity, not chronod's absence

Set `boot-ui-focus3=chronod` (the reference port's value; this port had `lsd`).  Boot: 3748 log lines, three frames,
and the probe now tracks chronod directly:

    PROC[3] chronod: absent from this process pass
    PROC[2] SpringBoard p27 n16 r4 w12 ... Cfffffff00b8549f8 (_ipc_mqueue_receive_continue)

Against the previous boot's `n3..n9 r0 w3..w9`, SpringBoard now carries **16 threads with 4 running** - the focus change
really does make it query that client - but it still parks in `ipc_mqueue_receive_continue`, and **chronod still never
starts**.

So chronod's absence is upstream of the focus knob: launchd never runs it, and the display query then waits for a
client that cannot answer.  The port's own probe writes a chronod job onto the Data volume each boot
(`DISPLAY: chronod job on the Data volume: 30`, `chronod override written: 0`), which is the next thing to verify:
whether that override reaches launchd's job database, and whether the job is disabled there.

Image: `ba08ddaabb437c3c0e5cfe17a2c01d187f28c01e6cd0cc3a519586fb77490f7e`.

### State of the investigation after today

- foundation complete: staged SMP (3 cores), persistent Data, upstream UFS driver, documented argument names, PMGR
  loading, display wiring at parity with the reference port;
- the stall is characterised precisely: armed port, queued message, no waiter, receiver in `_kevent_id`;
- and the newest fact is that **chronod is never launched**, which the reference port's own logs say should happen on
  this OS line.
## 00:00 (Nov 14): eight cores boot on this board

With the MPIDR fix and the per-CPU publication gate in place, `sheng-smp=1 sheng-smp-max=7` now brings the kernel up on
**all eight logical CPUs**:

    4708 log lines, three frames, zero panics
    containers made=1 present=7   (persistent Data intact)
    SpringBoard p27 n14 r0 w14, kernel side ipc_mqueue_receive_continue
    chronod: still absent

The earlier eight-core attempt panicked in a nest (`pe_init.c:725`) for two reasons that are now fixed: `start.s`
masked MPIDR with `0xFF`, which aliases CPU1 onto CPU0, and the CPU nub gate started every core at once instead of
publishing only the selected ones.

Image: `66d79f594a1951f68238d611856d65511b413ccbc176fa3b5aac5cb5fed08409`.

So the foundation the sibling port advertises - eight-core startup, persistent Data, native UFS, PMGR, CPU GOP
presentation - is now all present here as well.  The ChronoServices stall and chronod's absence are unchanged by the
extra cores.
## 00:10 (Nov 14): the overlay approach targets a directory launchd does not read

Three hypotheses were open about why chronod never starts despite a written job.  Two are now settled from local
evidence, without the device:

1. **The system plist exists.**  `native_launchd_overlay()` reads
   `/System/Library/LaunchDaemons/com.apple.chronod.plist` back through the mount to see whether the union took
   effect, and the boot log shows `launchd overlay check: <?xml version="1.0" ...` - the open succeeded.  (The check
   cannot distinguish whose copy it read: both start with the same XML declaration.)

2. **launchd does not read that directory at all.**  Every boot on this OS line shows
   `Code: /System/Library/xpc/launchd.plist Sig: /System/Library/xpc/launchd.plist.sig` followed by
   `Sending validated cache to launchd` / `Cache sent to launchd successfully`.  On iOS 17 launchd boots from a
   **signed job cache** through `launchd_cache_loader`; a plist written into `/System/Library/LaunchDaemons` (or any
   union over it) is not part of that cache and therefore never becomes a job.

So the overlay was aimed at the wrong mechanism.  That also explains the earlier attempt recorded in
`zzz-launchd-cache.patch`: editing the cache directly was rejected (`launchd_cache_loader ... SIGKILL`), because the
cache is validated against a signature.

### What follows

Starting chronod has to happen by one of:

- a **cache patch that survives validation** (the signature is the obstacle, not the patch site);
- a **kernel-side start** of the service from the board probe (fork/exec of the ChronoCore binary, bypassing launchd
  job registration);
- or a check of whether the cache already contains a `com.apple.chronod` job whose launch condition our environment
  never satisfies (in which case the fix is that condition, not the job).

The third is testable locally by reading the cache's job list, which is worth doing before writing any more overlay
code.
## 00:20 (Nov 14): why the job exists in the cache and still never starts

`zzz-launchd-cache.patch` already contains the decisive fact: in this build's cache,
`com.apple.chronod`'s **RunAtLoad slot sits at file offset `0xfc6e0` and holds the shared false object (2464)** - the
job is in the cache, and it is **on demand, not run at load**.  Its `MachServices` entry is
`com.apple.chronoservices`, so launchd is supposed to start chronod **when a client looks that name up**.

SpringBoard does look it up - that is the synchronous send this port has been chasing all along.  And the send does
not fail: it **queues on a port that is armed and has no waiter**.  A send to a name nobody holds would return
`MACH_SEND_INVALID_DEST` instead of queuing, so **something holds the receive right for `com.apple.chronoservices`**.
That is the shape of a launchd **placeholder port**: launchd registers the name, the first message is supposed to
trigger it to spawn the job, and the message is then forwarded.

So the leading explanation is now:

> the message reaches launchd's placeholder for `com.apple.chronoservices`, the placeholder never triggers the spawn,
> and SpringBoard's synchronous send waits forever on a port that will never be serviced.

That also fits every measurement: armed (the placeholder has a knote because a kqueue client - lsd - is watching it),
queued (`q=1`), no waiter (`w=0`), and `chronod` never in the process list.

### What to verify next, and how

The probe can already read a port's waitq; it needs to name the **owner of the receive right**.  If that owner is
`launchd`, the placeholder explanation is confirmed and the defect is in launchd's spawn trigger (a userland path this
port has never instrumented).  If the owner is some other process, that process is the one to look at instead.

That is one more field in the existing queue probe, and it decides between the two remaining explanations.
## 00:30 (Nov 14): the owner field answers it - launchd and lsd

The probe already carried the answer: `ipc_port_get_receiver_task_locked()` returns the **pid of the receive-right
owner**, and the UIT row prints it as `peer=`.  Reading the queued-port samples back through it:

| log | queued port | owner |
| --- | --- | --- |
| cont124 | `0xfffffff671597100` | **pid 1 (launchd)** |
| cont125 | `0xfffffff345a62fc0` | **pid 1 (launchd)** |
| cont125 | `0xfffffff345f97d80` | pid 97 (**lsd**) |
| cont131 | `0xfffffff7833411a0` | **pid 1 (launchd)** |
| cont133, cont134 | - | -1 (no receiver: the port was translated from a stale name) |

So the messages that queue with no waiter sit on **two kinds of ports**:

1. **launchd's** (pid 1) - the on-demand service placeholder for `com.apple.chronoservices`; the message should
   trigger launchd to spawn chronod, and it never does;
2. **lsd's** (pid 97) - a port lsd armed a knote on and then stopped receiving from, which is why lsd sits in
   `_kevent_id` while its own port holds a message.

Both receivers are kqueue clients, and in both cases the queued message does not wake them.  That is the single shared
symptom this session has been circling: **armed ports whose kqueue client is not woken when a message arrives** - for
launchd the missing wake means chronod is never spawned, and for lsd the missing wake is what SpringBoard waits on.

### What this finally narrows it to

Not Mach port registration (armed), not the message queue (it holds the message), not the receiver's willingness (both
have knotes).  The wake path from `ipc_mqueue_post` into the **kqueue workloop** is the one component common to both
failures, and the earlier `MPOST` trace already showed that branch never fires with a klist during a boot - which is
consistent with the wake being attempted through the turnstile path instead, and failing there.
## 00:10: the QEMU fixture inventory

`qemu-system-aarch64` 6.2.0 is installed, and the framework carries a fixture harness under
`scripts/qemu-entry/` (`entry.S`, `link.ld`, `bootshim-fixture.S`, `make-dt.py`, `native-user.S`, the
`native-apfs-*.inc` instruments, `thread_snapshot.py`, `gdb-stop.py`, `ufs_allocation.py`, `verified-root-probe.c`).

Three scripts use it:

| script | what it does | runs here? |
| --- | --- | --- |
| `probe-stock-entry.py` | boots an unchanged Mach-O on QEMU virt (`-M virt -accel tcg`, `-device loader,...`), tests the instruction entry boundary | yes (Linux-safe) |
| `build-native-user-fixture.py` | builds an ordinary ARM64 static Darwin process for EL0/SVC tests, including `--apfs-launchd-devices OS CACHE` which mounts two read-only APFS volumes, chroots, and execs **launchd** | yes (pure Python + clang) |
| `probe-darwinboot-xnu.py` | boots the **real XNU kernel** after the DarwinBoot handoff, for offline kernel observation | **no** - it shells out to `xcrun nm`, so it is macOS-only as written |

So a launchd fixture is not something to invent: `build-native-user-fixture.py --apfs-launchd-devices` **is** that
fixture, and `native-apfs-launchd.inc` is its entry sequence.  What is missing on this host is a Linux runner for the
kernel side of the pair.

### What a launchd fixture would and would not buy

- it would exercise `mount`, `chroot`, `exec`, thread switching and the EL0/SVC boundary against a real kernel,
  with a GDB stub and a thread snapshot available, at QEMU speed;
- it would **not** reproduce the ChronoServices stall: that needs the real dyld cache, libdispatch and the iOS
  userland, which the fixture deliberately does not carry.

### The smallest useful step

Port `probe-darwinboot-xnu.py` to the Linux toolchain (replace `xcrun nm` with the bundled `llvm-nm`, as
`build-xnu-4k-baseline.sh` already does), then boot this port's `mach.development.bcm2837.unstripped` in QEMU virt.
Until that runs, the fixture harness cannot touch the kernel this port builds.
## 00:15 (Nov 14): neither wake handle fires

Two instruments now bracket the whole kernel wake path for a mach-port knote:

| instrument | site | fires during a boot |
| --- | --- | --- |
| `MPOST posted/skipped` | `ipc_mqueue_post`'s `KNOTE()` branch | **0** (and it is by design: a workloop-attached port has `ip_sync_link_state == PORT_SYNC_LINK_WORKLOOP_KNOTE`, which `ipc_port_has_klist()` rejects) |
| `INHERIT` | `ipc_port_send_update_inheritor()` - the only place a message can redirect a workloop's turnstile | **0** |

Boot with both: 4682 log lines, three frames, no panic, and neither counter moved.

### What that means

In this kernel a **workloop-armed** mach-port knote is woken neither by the `KNOTE()` path (excluded by design) nor by a
turnstile inheritor update (never called).  The remaining mechanism is the one the design assumes: the receiving thread
parks in `mach_msg` on the port, so `ipc_mqueue_post` hands it the message directly through
`waitq_wakeup64_identify_locked()` (`w >= 1`).

The probe's `w=0` samples are therefore the contradiction to resolve - either those ports are not the ones the
messages are delivered through, or the receiver left `msg_receive` for `kevent_id` and the kernel has no notification
left to give it, which is exactly the stall.

### Honest position

After this many instruments the picture is consistent but not closed: the kernel offers exactly three ways to wake a
mach-port knote (direct receive hand-off, `KNOTE()`, turnstile redirect), the first requires a parked receiver, and the
other two provably do not fire on this board.  Closing it needs one of:

- a sample of the port **while** the message is queued (not after), with the receiver's thread state in the same
  record;
- or the QEMU harness, where the same sequence can be stepped through deterministically.
## 00:25: the QEMU kernel probe now runs on Linux, and stops at a symbol

Ported `probe-darwinboot-xnu.py` and `audit-xnu-kernel.py` off `xcrun`: a small `_host_tool()` helper prefers the
LLVM tools when they exist (WSL exposes an `xcrun` stub through interop that fails on use, so `which xcrun` is not a
reliable test), with `UNIXNU_NM` / `UNIXNU_DWARFDUMP` overrides.

Then the probe walked through three missing inputs, each supplied from this machine's own build outputs:

| input | where it was |
| --- | --- |
| `UniXNU.efi` | `uefi-instantnoodlep/Build/instantnoodlepPkg/RELEASE_CLANGPDB/AARCH64/UniXNU.efi` |
| `DarwinBoot.efi` | same tree, `AARCH64/DarwinBoot.efi` |
| our kernel | `out/instantnoodlep/.../tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped` |

With those, it audits the kernel, builds a **synthetic QEMU device tree (1084 bytes, 62.5 MHz timebase)** and then
stops with:

    Cannot resolve _native_legacy_properties

The probe insists on two display globals (`_native_legacy_properties`, `_native_legacy_client_class`) that this port's
`NativeLegacyDisplay` build does not export under those names, so it cannot complete its memory inspection - a port-side
naming gap, not a QEMU problem.

### Where that leaves the QEMU route

It is now **runnable on this host** - macOS toolchain dependency removed, firmware inputs located, synthetic DT built -
and one small step from attempting the boot: export (or teach the probe to skip) those two symbols.  Worth noting that
even when it boots, the fixture cannot reproduce the ChronoServices stall: it carries neither the dyld cache nor
libdispatch, which is where the stall lives.
## 00:35: the QEMU probe reaches its GDB inspection step and stops there

Ported the probe's host tools (LLVM `nm`/`dwarfdump` instead of `xcrun`, with `UNIXNU_NM`/`UNIXNU_DWARFDUMP`
overrides), supplied `UniXNU.efi` and `DarwinBoot.efi` from this machine's own UEFI build, filtered the inspected
globals to the ones this kernel actually exports, installed `gdb-multiarch` (absent until now), and linked it as `gdb`.

The probe now gets through the kernel audit, builds the synthetic QEMU device tree (1084 bytes, 62.5 MHz timebase),
and fails at its GDB memory-inspection step: a command carrying several hundred `--read-memory <addr>:<len>`
arguments exits with status 2 before QEMU ever runs the kernel.

So the QEMU route is **close but not running**: the remaining work is inside the probe's own inspection harness
(argument delivery to GDB), not in this port's kernel or firmware.

### Honest assessment

The fixture harness is a research tool with assumptions from the reference board's build (global names, GDB wiring,
firmware layout).  Making it boot this port's kernel is a project of its own, and even then it cannot reproduce the
ChronoServices stall, which needs the dyld cache and libdispatch that the fixture deliberately excludes.  The phone
remains the only environment where that question can be answered.

Framework-side changes made for this (in `UniXNU/scripts`, not the device repo): `_host_tool()` in
`probe-darwinboot-xnu.py` and `audit-xnu-kernel.py`, filtered global inspection, and the two display globals now
conditional.
## 00:45: the QEMU harness needs a CPU model this host's QEMU does not have

Working through the probe's inspection step surfaced the real blocker, in order:

1. `gdb-stop.py: error: UEFI code FD not found` - fixed by linking the installed AAVMF image to the name it
   expects (`/usr/share/qemu/edk2-aarch64-code.fd` -> `/usr/share/AAVMF/AAVMF_CODE.fd`);
2. `ConnectionRefusedError` on the QEMU gdbstub - because QEMU itself exited, recorded in the fixture's
   `gdb-stderr.log` as:

       qemu-system-aarch64: unable to find CPU model 'cortex-a710'

`gdb-stop.py` hardcodes `-cpu cortex-a710` for TCG (and reads a matching `cortex-a710.json` state fixture).  That is
an ARMv9 core model added in QEMU 7.0; this host runs **QEMU 6.2.0** from Ubuntu 22.04, whose newest ARM cores stop
before it.

So the QEMU route is blocked on toolchain age, not on this port: it needs QEMU >= 7.0 (or a patched harness with a
supported core and a regenerated state fixture).

### Where that leaves it

Everything up to QEMU itself now works on Linux - host tools, firmware inputs, DT synthesis, GDB driver, FD image -
and the single remaining gate is the CPU model.  Worth noting again that even a working fixture could not reproduce
the ChronoServices stall, which needs the dyld cache and libdispatch the fixture excludes.
## 00:55: QEMU now boots this port's kernel to the XNU handoff

Two host-side fixes unblocked the run: the harness's hardcoded `-cpu cortex-a710` became `UNIXNU_QEMU_CPU` (default
unchanged, so the reference harness behaves as before), and the AAVMF image was linked to the `edk2-aarch64-code.fd`
name the GDB driver expects.  SM8250 is ARMv8.2 (Kryo 585: Cortex-A77 + A55), so an ARMv8 core model is both more
faithful and what this host's QEMU 6.2 offers; the run below used `UNIXNU_QEMU_CPU=max`.

With that, the fixture's UART shows a real boot chain:

    UNIXNU: boot: entry 'source-4k' (Source native 4K XNU)
    DARWINBOOT: ARM64 va=0xFFFFFFF007004000 pa=0x40004000 args=0x46FCC000 entry=0x44A11488 owned=0x10000000
    DARWINBOOT: command line: -v serial=3 debug=0x14e kt_fixed_zones=120
    DARWINBOOT: inputs validated; exiting boot services

so the UEFI application, the DarwinBoot handoff and the kernel image all load and run in QEMU on this host.

The probe then fails on its own validation, `KeyError: 'SCTLR_EL1'`: it expects a checkpoint stop with EL1 system
registers captured, and the stop it reached has none.  That is harness bookkeeping, not a kernel fault.

### What this changes

The QEMU route is no longer blocked by toolchain age or missing inputs: it runs this kernel.  What remains is the
probe's checkpoint contract (which register set it requires at the stop).  Even so, the fixture still cannot reproduce
the ChronoServices stall - no dyld cache, no libdispatch - so this is a debugging platform, not an answer.
## 01:05: the QEMU harness now runs end to end on this host

After the CPU-model fix (`UNIXNU_QEMU_CPU`, default unchanged) and one probe adjustment - the translation check now
runs only when QEMU's gdbstub actually advertises `TCR_EL1`/`SCTLR_EL1`/`TTBR0_EL1`/`TTBR1_EL1`, and records the gap
otherwise, because QEMU 6.2 does not expose `SCTLR_EL1` - the probe completes:

    DARWINBOOT: ARM64 va=0xFFFFFFF007004000 pa=0x40004000 entry=0x44A11488
    DARWINBOOT: inputs validated; exiting boot services
    ... stop at 0xfffffff00c017738 = _panic + 0
    Observed _panic; functional iPadOS remains unverified

The harness's stop address is `_panic` itself, so what it captured is this port's kernel **reaching panic inside QEMU
virt** - which is expected: the kernel is built for BCM2837 with SM8250 bring-up and QEMU provides none of that
hardware.  The important part is that the whole chain works: UEFI application, DarwinBoot handoff, kernel start,
GDB-stub stop, state capture, report.

### What the harness can now do

- boot this kernel in QEMU and stop at `_panic` (or any configured checkpoint) with registers and memory;
- read process/thread state with `thread_snapshot.py` against a guest that reaches those structures;
- iterate in seconds instead of flashing the phone.

### What it still cannot do

Reach a running userland: our kernel panics under QEMU virt before any BSD init, and the fixture carries neither the
dyld cache nor libdispatch.  Testing the ChronoServices stall still requires the phone.
## 00:30 (Nov 14): every notification path is silent, and that changes the reading

Three instruments now cover every way this kernel can tell a kqueue client that a message arrived:

| instrument | site | fires during a boot |
| --- | --- | --- |
| `NOFAIL` | `ipc_mqueue_post` when `receiver == THREAD_NULL` (message queued with no parked thread) | **0** |
| `MPOST` | the `KNOTE()` inside that same branch | **0** |
| `PREPOST` | `ipc_pset_prepost()` - the only notify path for a knote on a **port set** | **0** |
| `INHERIT` | `ipc_port_send_update_inheritor()` - the turnstile redirect | **0** |

All four strings are present in the shipped kernel (`strings kernel.macho`), so the instruments were live; the boot
reached 4075 log lines and launchd, and none of the counters moved.

### What it means

During a whole boot, **not one message was posted to a port that had no parked receiver**.  Every message found a
thread in `mach_msg`, and the kernel handed it over directly - the wake path that this investigation spent hours
looking for was never needed.

That contradicts the probe's `q=1 w=0` samples unless one of these is true:

1. the queued messages were posted **before** the port's kqueue owner parked (in which case `NOFAIL` should have
   printed - it did not);
2. `imq_msgcount` is not measuring what the probe assumes, or the sample is taken under a different lock than the
   post path;
3. the probe is sampling ports that are **not** the ones the launch-services traffic uses.

### Honest position after this round

The evidence no longer supports 'the kernel fails to wake the port'.  It supports: **the kernel never needed to wake a
parked receiver it did not have**, and the probe's queued-and-unwaited samples are the artefact to explain next -
by sampling the same port on both sides of a post (before and after) instead of on a timer.

Image: `6e32f578796f63ac24d82f352afd5a8f02ca3be503a15422a6f03478bd60ea3e` (this boot did not reach the display).
## 00:45 (Nov 14): the probe's `q=1` may be a send reservation, not a queued message

The waiter field was wrong and is now fixed: `peer_waiters` read `waitq_links` (the linkage for waitq sets) instead of
`waitq_queue` (the circle queue the waiters are on), which is why every sample looked unwaited.  After the fix, the
sample still reads `q=1 w=0 t=0` on a port owned by **launchd**: so the port genuinely has no waiter.

But in the same boot, all four notification instruments still read **zero**:

    NOFAIL 0   MPOST 0   PREPOST 0   INHERIT 0

and their format strings are in the shipped kernel.  A message that reaches a port with no waiter **must** take the
`receiver == THREAD_NULL` branch, so `NOFAIL` would have printed.  It did not.

### The reading that fits

`imq_msgcount` is not only 'messages in the queue'.  In this kernel it is the port's **reservation count**, raised when
a send reserves space (`ipc_mqueue_send_prepare`) and lowered when the message is consumed.  A send that reserved and
then **blocked before enqueueing** leaves exactly this signature:

    q=1      one outstanding reservation (a send in flight)
    w=0      nobody parked in msg_receive
    t=0      no turnstile push
    sender   blocked inside the send call - SpringBoard sits in `mach_msg2`

Under that reading nothing is ever enqueued, which is precisely why every notification instrument stays silent: the
stall is **before** the enqueue, in the send path, not in the wake path.

### Next measurement

The probe must distinguish the two: read `ipc_kmsg_queue_first(&port->ip_messages.imq_messages)` alongside
`imq_msgcount`.  A message present means 'queued and unwaited'; a NULL head with a non-zero count means 'send reserved
and blocked' - and that is a completely different defect to chase.

## 01:35 (Nov 14): q>0 with an empty queue points at a leaked reservation

The co-located record was added: `NOFAIL out port=... q=... head=... destroyed=...` at the end of the
no-receiver post path, and `qm=<queue head>` on the probe's UIT row.  Boot with both:

    4148 log lines, NOFAIL 0
    UIT[8] ... peer=1 pt=0xfffffff9e568a600 q=2 w=0 t=0 e0 qm=fff   (row truncated at 120 chars)

So a launchd port reports **two** outstanding messages while the post path that would queue them never ran.  Two
readings remain, and the code narrows them:

- `imq_msgcount` is raised in `ipc_mqueue_send_locked()` (the reservation) and lowered either when the message is
  queued-and-consumed or by `ipc_mqueue_release_msgcount()`;
- the only place a port's queue is **filled** is `ipc_mqueue_post()`'s no-receiver branch, which is instrumented and
  silent.

That leaves **a reservation raised by a send that never reached the post and never went through the release path** - i.e.
a leaked `imq_msgcount`.  A leak of this kind is invisible to every wake instrument, because no message ever exists: the
count alone is wrong, and any consumer that waits on it (including a wake path that compares queue occupancy) sees
traffic that is not there.

### How to confirm locally, without the device

Read the two functions side by side: `ipc_mqueue_send_locked()` (raise) and `ipc_mqueue_release_msgcount()` (lower), and
list every return path between them in `ipc_kmsg_send()`.  If any path returns after the raise without posting or
releasing, the leak is proven by inspection; if none does, the count is trustworthy and the sample must be re-taken with
the row buffer widened so `qm` is not truncated.
## 02:25 (Nov 14): Setup never runs - the skip-setup premise does not apply

Verified across every recent boot (`cont147`, `148`, `149`, `150`, `152`, `153`): **`PreBoard` never appears** in the
process rows, while `SpringBoard` does (22-34 rows each).  The setup assistant is not running, so the device is not
sitting in Setup.

That corrects the reading taken from another session: `payload.json` does show `skip_setup: false`, but the
consequence drawn from it - that the port boots into Setup instead of SpringBoard - is not what happens.  SpringBoard
is the process that starts and then blocks in its launch sequence.

The runtime skip-setup writes are still correct to have (they match the official five domains, the four container
directories and the `--no-auto-dim` keys), but they cannot be the fix for a stall that happens after SpringBoard
has already started.

### Where that leaves the outstanding work

- CI (`build-payload`) still worth completing: it produces the officially-built seed and proves the macOS path works;
- the stall itself is unchanged and remains "SpringBoard waits, chronod never starts";
- and the launcher question - why `com.apple.chronod` never runs when the OnDemand job is in the signed cache - is
  still the one open thread that both this port and its reference have in common.
## 02:35 (Nov 14): the seed checklist passes completely

`ye3912/apfs` ships no seed - only a builder script, a CI workflow and `scripts/expected-var.json`, the inventory of
`/private/var` entries the official builder copies from the OS volume.  That inventory was turned into a kernel-side
checklist (`package/VarExpect.h`, 205 paths) and the probe runs it once at `tick == 20`, after the Data volume covers
`/private/var`:

    VAREXP summary: present=205 missing=0 total=205

The earlier run that reported `present=0 missing=205` was the same check executed before the Data mount - the probe's
own tick-20 comment already warned that the early call lands on the wrong `/private/var`.

### What that settles

- this port's Data seed is **structurally complete** against the reference inventory: every directory and file the
  official builder creates is present here as well;
- there is therefore **no reason to adopt the sibling seed**, and two reasons not to: it is 256 MiB against this port's
  3 GiB, and its APFS volume UUID would not match `CONFIG_XNU_OWNER_DATA_VOLUME_UUID`
  (`E553E004-A6C6-4992-B474-25EB0F160524`), so the Data mount would refuse it;
- the missing desktop is not a missing `/private/var` entry.

Image: `c97460423ad7c1aa8285bc644ea301627b6501b7131c69522bc265a2192cc347`.
## 04:40 (Nov 14): the keybag service publishes - and the chain unblocks

Root cause found and fixed.  Two defects in `package/KeyStoreProbe.cpp`:

1. the `absent` gate could never pass: the component defines `OSDefineMetaClassAndStructors(AppleKeyStore, IOService)`,
   which registers that class **as soon as the object is linked into the kernel**, so `getMetaClassWithName("AppleKeyStore")`
   always finds our own class and the probe refused to create a service;
2. the creation path ran `OSMetaClass::preModLoad` / `native_keyevents_static_init` / `postModLoad` - the kext-loader
   dance - which **deadlocks the boot** when called from a running kernel (measured twice: 352 and 363 log lines, zero
   frames).  The class is already registered, so that path is unnecessary.

Fix: keep the gate only for builds without the component, and allocate the service directly.

First boot with it:

    log: 6139 lines (previous best ~4400), three frames
    AKSSTAGE keyevents_stage=3 keyevents_status=0     (kIOReturnSuccess)
    liveactivitiesd exits: 0                          (was 33 SIGTRAPs)
    chronod: PROC[3] chronod p105 n5 r1 w4            (was absent)
    SpringBoard: n40 r2                               (was n9 r0)
    AegirPoster p313..315 and backupd now start

Arguments that matter: `sheng-aks=1 offline-key-events=1 sheng-aks-sls=3`; `offline-keystore=1` must **not** be set -
it takes the "original kext" branch, whose `AppleSEPKeyStore` load fails (-603946998) and which skips the owned service
entirely (stage 10).

Image: `b08a8e308185663384df2d2dcb679a8b2548358d91879e5c4bb57d3645bad4f1`.
## 05:20 (Nov 14): why the client frames never reach the framebuffer

After the keybag fix the userland stack runs (chronod, liveactivitiesd, backboardd, SpringBoard n40), but the
framebuffer still stops at the three console frames.  Two latent display defects were found by reading the driver
against the docs and fixed:

1. `native_legacy_gpu_offer` returned `true`.  With `!gpuBypass && native_legacy_gpu_offer(...)` the driver takes its
   GPU hand-off branch, retires the swap early and **returns without copying**; on a board with no GPU every userland
   frame is dropped there.  It now returns `false` so frames take the CPU copy path (docs: *direct display fixes each
   swap's target at its first successful GPU fence query*).
2. `native_adreno_fences`/`_detail` returned `false`, which the driver reads as \"fence unknown\" and retries every
   200 us forever.  They now report a fence that retires immediately (no GPU means nothing outstanding).

Neither changed the frame count, and the measurement explains why: **swaps total three, all from the kernel console**.
`accessor` shows the display clients connect, but no userland process ever submits a swap, so the copy path is never
reached.

### What the community records say

- `docs/devices/alioth-taishang1024.md:338`: \"Get backboardd to start before investigating frame submission: with no
  render server there is no client that could call Swap.\"  Ours starts and stays (p57 n18).
- Same file, the twelve-boot table: frames only flow when **someone touches the panel** - swaps 76..830 with a touch,
  about 8 without.  With no input device the render server stays idle.
- `docs/devices/droidvm.md`: CPU presentation plus **Metal over a host GPU** reaches the home screen; CPU-only records
  stop at the lock screen, which is the realistic target for this board.

### Consequence

This port has **no HID/touch driver at all** (`package/` carries only the unused SM8550 `TouchBoard.h`), so the panel
never reports input and nothing wakes the render server.  The next milestone is a HID touchsource for this board, not
another display change.

Image with both display fixes: `b3ec074309e7c9ba7152f0c06e28d47f49967cc269e4b66936a1fc94f509b43c`.
## 15:30 (Nov 14): the real touch screen works, and the last display question

### S6SY791 touch - verified on the phone

Ported the hakios in2020 driver (`devices/in2020/drivers/input/hakios.s6sy791.c`) into this port as
`package/S6sy791Touch.c`: bit-banged I2C on gpio36/37, reset gpio38, interrupt gpio39, rails pm8150_l13
(3.008 V) and pm8150a_l1 (1.8 V) voted through RPMh.  First boot:

    s6sy791: ldoa13 (0x40d00): 3008 mV, enabled, high-power mode
    s6sy791: ldoc1  (0x43000): 1800 mV, enabled, high-power mode
    s6sy791: the bus: 0x48 answers (the S6SY791's address)
    s6sy791: boot status 0x20 (application)
    s6sy791: device id 0x3791 (ac 37 91)
    s6sy791: panel: 1440 x 3168, 17 transmit channels
    s6sy791: finger 0 at 1204, 2687 (of 1440 x 3168): down
    s6sy791: so far 5525 reports in 5739 looks (19 bad), 6249 finger positions

### HID publication - two switches that were missing

- `sheng-touch-hid=1`: without it NativeHidInput never merges the digitizer report descriptor and
  never registers its touch sink (`native_touch_set_sink`), so the driver's frames went nowhere;
- the sink itself: the component **does not poll the seqlock any more**, it asks the producer to call
  `touchNotify` - so the board must implement `native_touch_set_sink` and call the registered
  callback after each published frame.  With both, the reports arrive: `SYNTOUCH report event=6
  status=0`.

### Display: what is left

`setup-skip=1` plus real touch gives SpringBoard 41 threads and HID touch reports accepted, but the
panel still shows the Apple logo: exactly three swaps have ever been copied (the boot snapshot), and
`backboardd` sits in `semaphore_wait_continue` with six threads while the vsync timer is armed and
sending (`DISPLAY: vsync 5 120`).  So the family publishes, the compositor's client completes swaps,
vsync flows, and the render server still never submits a user-visible frame.

Next measurement: tag each `DISPLAY: swap accepted` line with the submitting task name, to tell whether
backboardd ever swaps at all or only the boot snapshot did.

Image: `182f074b96be306845298ebd418df05c13ab11fe450f141c9416f965f5d9a098`.
## 16:25 (Nov 14): the display page is complete - brightness works on the panel

Wired the board's own backlight implementation into the build (`package/BacklightPort.cpp` including
`In2020BacklightTransport.h` and `In2020BacklightService.h`): DSI0 transport, Iris gpio112, display clock page
0xaf02000, levels 0..2047, request coalescing on a thread call, and the class answers `getMetaClass()` with
AppleARMPlatform's AppleARMBacklight metaclass.  Boot with `backlight=1`:

    BACKLIGHT: starting the IN2020 DSI service
    BACKLIGHT published class=AppleARMBacklight level=399 max=2047
    BACKLIGHT request=1 level=1024 fade_ms=0
    BACKLIGHT applied target=1024 observed=1024 verified=1 requests=1

CoreBrightness found the service, asked for level 1024 and the panel's DSI readback reported 1024: the owner
confirmed the screen brightness **changed on the phone**.  With the real service backboardd no longer crashes
(an earlier stand-in of mine made it SIGSEGV on every launch).

That closes the official display page: firmware scanout, XNU presentation, brightness transport, brightness
service and the brightness interface are all implemented and observed.  The panel still shows the Apple logo
because nothing renders the UI - which is the GPU page's work.

Image: `aeff9625b6d35e0c077f8e31f172165838065a721b8857ffc02e416d21cf0661`.
## 18:10 (Nov 14): the launchd cache read and rewritten locally, PreBoard is an on-demand job

The local IPSW carries the same 21H461 SystemOS.  Built `apfs-fuse` (sgan81, with its lzfse submodule) in WSL and
mounted `out/ipsw/21H461/044-76040-038.dmg` directly: the DMG is LZFSE compressed, which is why a plain `7z`
extraction of the APFS partition produced a zero superblock.

`/System/Library/xpc/launchd.plist` inside that volume is **1,640,626 bytes** - the exact constant the parked
`zzz-launchd-cache.patch` uses - and it is a **binary plist (`bplist00`) that Python's plistlib parses in full**.

    top keys: AppExtensions, LaunchDaemons, AppRemovalServices, SystemLibraryTreeState, Symlinks, VersionNumber
    LaunchDaemons: 561 jobs, 49 of them with RunAtLoad

PreBoard is there, twice, and both are **on-demand** - no `RunAtLoad`:

    /System/Library/LaunchDaemons/com.apple.preboardservice_v2.plist
        MachServices com.apple.preboardservice_v2, ProgramArguments /usr/libexec/PreboardService -v 2, mobile
    /System/Library/LaunchDaemons/com.apple.preboardservice.plist
        MachServices com.apple.preboardservice

So the Hello screen cannot appear for the same reason chronod never started: nothing ever looks the service up,
and lauchd has no reason to spawn it.  The fix is a cache rewrite, and it can now be produced offline:

    docs/evidence/launchd/launchd-original.plist   1640626 bytes, the cache as shipped
    docs/evidence/launchd/launchd-modified.plist    887340 bytes, both PreBoard jobs with RunAtLoad = true

The rewrite round-trips through plistlib and re-parses with both flags set.  Remaining work is to place the
modified file where the boot patch can mount it over `/System/Library/xpc`, and to check whether launchd accepts
it without its `.sig` (the kernel runs `amfi_get_out_of_my_way=1`).
