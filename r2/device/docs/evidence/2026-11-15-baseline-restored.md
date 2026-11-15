# Baseline restored: the pre-reference build boots to SpringBoard

Date: 2026-11-15
Scope: recover the build that produced the last known-good kernel (the R2-display /
A650 kernel era) after the reference-ecosystem port left the tree unable to link.

## Result

| Item | Value |
| --- | --- |
| kernel.macho | `35e975afea7341958d3b145bec241f9ac76c85bff499492d8d694ab4bc55595f` |
| payload.img | `be17b34e355d87cdd604fc44698b158910bf5a3931c1f6182b5da5ed9ab88f4a` |
| bootEU.img | `bbd3973ca88d7d6563b3856fb87cebb2d8e6763a5692bf13ecea08ebb70ff33b` |
| console log | `D:\\新建文件夹\\logs\\boot-fix-161120.log` (171,858 bytes, 3,191 lines) |

Hardware evidence from that log:

    NativeUfs: published SystemOS / SystemOS volume 8846FE78-... ready
    NativeUfs: published OS / OS volume 04769D26-... ready
    Got boot device = IOService:/NativeUfs/... AppleAPFSContainer/DawnUpdate21H461...
    load_init_program: attempting to load /sbin/launchd
    PROC[0] launchd p1 ... boot-mode set by sysctl: main
    GPU54 mode7 result status=0 ... fw=20010008 ready=1/1 ack=c6 error=0
    GPU55 mode8 result status=0 on=1 ... rbbm=00000001
    GPU56 mode9 result status=0 ... marker=a6500c0d rptr=1b wptr=1b fault=0
    GPU57 mode10 result status=0 zap=1 secure=1 readback=a002 fence=4
    GPU: metal register rc=0
    DISPLAY: display attach=1 start=1, chosen display-scale=2 rotation=0 ok=1
    DISPLAY: present copied 2376 rows x 4320 bytes (frame 1)
    PROC[2] SpringBoard, PROC[3] backboardd
    s6sy791: 291 reports in 1763 looks, 384 finger positions

No panic in that log (the only "gPanicBase" lines are the benign early init notes).

## What had to be repaired

1. **UFS routing**: the board selects `KonaUfs`; `ufs-archive-alias.py` publishes
   `build/kona-ufs/libKonaUfs.a` as `build/sheng-ufs/libShengUfs.a`, which is the archive
   `build-xnu-4k-baseline.sh` links for `XNU_NATIVE_UFS=1`. Do not add a second UFS component.
2. **Archive stage**: `CONFIG_XNU_BUILD_ARCHIVES=y` must be in the defconfig; without it every
   repository archive is silently skipped and the kernel link fails on `\$CRYPTO_ARCHIVE`.
3. **Package visibility**: the assembled `tree/package` is a symlink to the **framework** package
   directory, so board sources (A650Gpu/board glue/firmware) must also live in `UniXNU/package`.
4. **Submodule revisions**: `feeds.update_submodules()` resets submodules to the gitlinks recorded in
   the framework index. Pin the wanted revisions by checking them out and staging the new gitlink
   (`git add <path>`); a plain `git checkout` in the submodule is reverted on the next build.
5. **KonaUfs component**: the catalog id is `KonaUfs` but `build-arm-driver.py` branches on the
   driver class name, so `KonaUfs` must be added to the `NativeUfs`-class branches (storage ABI
   include, VerifiedRoot include, `-DNATIVE_VERIFIED_BLOCK`, `-mgeneral-regs-only`).
6. **Owner policy chain**: `build-owner-policy.py --component os --dmg 044-76040-038.dmg
   --volume-uuid 04769D26-...` and the same for `systemos` with `044-76034-038.dmg` and
   `8846FE78-...`, then `build-verified-root-probe.py --component os --include-systemos`.
   The signing key lives in `UniXNU/build/verified-root-host/keys` and must be copied into the tree.
7. **IOBlockStorageDevice bindings**: `scripts/data/native-storage-21H461-original-bindings.S` is
   compiled into `build/io-storage-bindings/libIOStorageBindings.a` with the **Mach-O**
   `CommandLineTools/usr/bin/{ar,ranlib}`; GNU ar archives are rejected by ld64.
8. **Patch glob**: `build-xnu-4k-baseline.sh` applies `patches/xnu/*.patch` wholesale; untracked
   reference patches copied into that directory conflict with the tracked series and must stay out.

## The panic that was fixed on the way

The first boot of the restored kernel panicked with

    Kernel instruction fetch abort at pc 0xfffffff00c15fb00, lr 0xfffffff00bff4a58

Symbolising the new kernel showed `pc == &_armcpu_spi_mask` and the caller in
`hostEnableInterruptEv` (KonaUfs): the armcpu driver had been pinned to a reference revision that
no longer implements `armcpu_spi_mask(uint32_t, bool)`, and a stop-gap stub defined it as a
**data** symbol, so the call jumped into data. Checking out ArmCpu `696e092` (which still has the
GIC SPI mask function) and staging its gitlink fixed the abort; the symbol is now a
`(__TEXT_EXEC,__text)` function in the linked kernel.

## Still open

`GPU: addon files=-1 written=0 failed=0; extensions stage=0 status=-1; accelerator=0 open=0 service=0`:
the kernel-side GPU stack is green but the userspace GPU addon is not staged/loaded, so Metal
acceleration is not yet exercised. This is the next step.

## Addendum: the swap stall that followed the first successful boot

After the baseline booted to SpringBoard the display froze: no second frame and, five seconds later,

    IOMFB: swap_wait_gated: CommandWake didn't happen even after 5 seconds.
    Seems system in bad state, cancelling swaps
    Kernel data abort at pc ... (vm_object_cache_add)

A diagnostic print in the driver's copy rejection path gave the exact reason:

    DISPLAY reject: w=1080 h=2376 dstStride=4320 dstLen=10264320 srcStride=4352 srcLen=10342400 alloc=10342400 depth=32 rot=0 dst=0xfffffffa12f15001 src=0xfffffffbf9e00000

The GOP base is 0x9c000001: the low bit is a panel flag and the framebuffer itself starts at the page
base (the probe already documents this and maps `v_baseAddr & ~0xFFF`). The driver mapped device memory
index 0 and used the returned virtual address directly, so it carried the same +1 offset; the 4-byte
aligned device stores in `native_legacy_copy_layout` were therefore rejected, the swap never retired and
the family cancelled it.

Fix: record `bootOffset = v_baseAddr & 0xFFF` when the boot mapping is installed and have every scanout
destination go through `scanoutBase()` (mapping VA - bootOffset). Same correction as the probe's map.

Result (kernel 3f72e723, payload f0459f79, 417 KB console log):

    DISPLAY reject: 0
    present copied ... (frame 1/2/3)
    no swap_wait_gated timeout, no bad-state message, no panic
    SpringBoard and backboardd running, 164 UIT render trace lines

The userspace GPU addon is still not staged (`GPU: addon files=-1 ... accelerator=0`); the GPU kernel
stack is green (GPU54-57, metal register rc=0) and the display now runs on the CPU copy path.

## Addendum 2: GPU extension + display together (30RGBLE_8A biplanar support)

With the GPU addon staged (`sheng-gpu-addon=1`, extensions stage 5, `/System/Library/Extensions`
served from the addon), the compositor stopped using 'BGRA' and submitted
`kCVPixelFormatType_30RGBLE_8A_BiPlanar` (0x62336138) surfaces instead, so every swap was rejected
(`reason=4`, `verify_swap failed` thousands of times) and the UI stopped.

Measured geometry of the rejected surface (raw object dump + accessor view):

    width 1080, height 2376, planes 2, stride 1088, allocation 12931072, descriptor length 12931072
    plane 0 (30-bit RGB LE, R in bits 0-9, G 10-19, B 20-29): offset 0, stride 1088*4 = 4352
    plane 1 (8-bit alpha): offset allocation - round_up(1088*2376, 4096) = 10342400, stride 1088

The probe's own surface dump confirmed the layout (`80 01 06 18` repeated = RGB(384,384,384), alpha
plane all 0xff), and the halves add up exactly to the allocation.

Two changes in the display driver:

1. `nativeBiplanarBounds()` replaces `native_legacy_surface_bounds()` for this format: the alpha
   plane is validated with `stride` bytes per row and the RGB plane with `stride*4`; the old helper's
   `stride >= width*4` rule rejected the alpha-plane stride outright (that was the whole failure).
2. `native_legacy_copy_biplanar()` converts the two planes to the BGRA scanout: R8 = (v>>2)&0xFF,
   G8 = (v>>12)&0xFF, B8 = (v>>22)&0xFF, A8 from the alpha plane.

Result (kernel 8e0e787b, console 441 KB):

    swap accepted=1 reason=0, verify_swap failed: 0
    transfer rc=0x0 bytes=10264320 (the BGRA frame)
    present copied 3 frames, SpringBoard running, no panic

The GPU extension is now visible to the system while the display keeps presenting, which is the
combination the userspace Metal plugin needs. The addon's accelerator service is still not opened
(`accelerator=0 open=0 service=0`); that is the next layer.

## Addendum 3: the kernel text console over the compositor frame

After the biplanar path worked the UI animated, but the left part of the panel kept showing kernel log
text that kept scrolling. Three hand-over attempts, in order of discovery:

1. `switch_to_serial_console()` looked right but is a no-op on this board: the implementation only sets
   `cons_ops_index` when `serial_console_enabled` is true and still returns the previous index, so the
   caller sees a success cookie while the video console keeps drawing. The frame counters proved the
   driver kept presenting 128 frames with zero guard failures while the text kept scrolling.
2. `PE_initialize_console(&PE_state.video, kPEReleaseScreen)` is the documented userspace hand-over, but
   it retires the graphics console (`gc_ops.enable(FALSE)`) and with it the firmware framebuffer this
   driver keeps writing: the device aborted straight into 900E right after the fourth swap.
3. `disableConsoleOutput` is the flag `gc_pause`/`gc_enable(FALSE)` set to silence `console_write` without
   touching the console operations or the scanout mapping. The driver sets it once, after four presented
   frames, and blanks the scanout in the same step so the text that was already on the panel disappears.

Result: the panel shows a clean full-screen iOS welcome page; the console log stops at the same frame,
which is expected because `console_write` returns before the QCOM USB capture sees the bytes.
Boot-arg `offline-legacy-console=1` keeps the log for debugging.

