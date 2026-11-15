# Kernel hand-off probes and the frame-buffer hand-off fix (2026-11-09)

## What the panic screen showed

Image `bootE.img` (payload with `panic_screen = true`, probe built with
`--no-reset`): the menu appeared, the default xnu entry ran, the panel drew the
green DarwinBoot bar, the blue kernel-entry bar and the magenta pre-MMU bar, and
the device then fell into Qualcomm 900E with no further text.

The three bars prove the bootloader hand-off, the MMU-off kernel entry and the
code immediately before the MMU is switched on. The missing text was not proof
that no panic happened: the panic screen reads `vinfo.v_baseaddr`, and that
stays zero until `PE_create_console()` reaches `initialize_screen()`.

## Reset probes

`[payload] entry_probe_reset_delay = 3` with `entry_probe_reset_after = "<symbol>"`
moves the probe from the MMU-enable point to three seconds after the named call.
A warm reset back to Android means the call returned; 900E means the kernel died
at or before it.

| Probe (call #1 in `_arm_init`) | Observation | Reading |
| --- | --- | --- |
| `_arm_vm_init` | device reset by the probe | `arm_vm_init` returned |
| `_kernel_startup_initialize_upto` | device reset by the probe | the KPRINTF startup entries completed |
| `_PE_create_console` | 900E after about six seconds | died in console creation |

## Cause

`initialize_screen()` in `osfmk/console/video_console.c` chooses whether to map
the frame buffer with

    makeMapping = (kernel_map == VM_MAP_NULL) || (0 != (1 & newVideoVirt));

Bit 0 of the handed-off video base marks a **physical** address that the kernel
must map itself; with an even base and the VM up, the value is used as a virtual
address. `DarwinBootVideoFromGop()` passed the raw GOP physical base
(`cont_splash_region@9c000000`, 0x2300000 bytes, panel 1080x2376 at 32 bpp), so
XNU wrote the console into unmapped address 0x9C000000 and the first store
faulted.

The stock device tree also names `dfps_data_region@9e300000` + 0x100000 and
`kboot_uboot_logmem@0x9FE00000`, which bound the free window the firmware volume
uses.

## Fix

`drivers/DarwinBoot/DarwinBootPkg/Driver/BootVideo.h`:

    Video[0] = Gop->Mode->FrameBufferBase | 1;

The firmware volume was rebuilt with the board's own platform
(`build -p Platforms/OnePlus/instantnoodlepPkg/instantnoodlep.dsc -a AARCH64
-b DEBUG -t CLANGPDB -D FD_BASE=0x9e400000 -D FD_SIZE=0x400000 -D FD_BLOCKS=1024`):
4,194,304 bytes, sha256
93dbf6b8ed97720c007a404e3f60f667d4e17c057cc464dc5691e16e47c2482f.

## Result

`bootJ.img` (new FD + panic-screen payload, 107,507,712 bytes, sha256
3906afb7e63cccefe6a86391845d9ba72eb813d4193f22fdf7618f7aa3248cc0) puts the kernel
console on the panel: after the three bars the `-v` kernel output is visible.
The text captured from that screen is the input to the next fix.

## Side observation

The generated ADT exports only `intc` (GICv3) under `arm-io`; there is no uart
node, so `serial_init()` reports no serial console and the panel is the only
console. `boot-progress=1` is a no-op in this build: the kernel has
`kern/boot_progress.h` but no implementation, and `XNU_BOOT_PROGRESS` is not
defined.
## Second round: the probe paints at the tagged address

With `Video[0] |= 1` in the firmware, the first image (`bootJ.img`) reached the
panic screen but reported `Data abort: Alignment fault` with `FAR 0x9C09C9F1`.
That address is `0x9C000000 + 2376 * 4320 / 16 + 1`: the entry probe's own blue
bar paints at `video base + framebuffer size / 16` with `stp`, so the tag bit
turned the write unaligned while the MMU was still off.

`scripts/patch-xnu-entry-probe.py` now clears the low three bits of the base
before painting (`bic x1, x1, #7`). `bootK.img` paints the bars correctly but
still ends in 900E about three seconds after them, with no console text.

## Console init still does not return

With the tagged base and the paint fix, a reset probe placed on
`_initialize_screen` inside `_PE_initialize_console` did not fire (900E about
five seconds after the bars).

The reference port for the same board (`F:\UniXNU-instantnoodlep`, recorded in
its `porting.toml`) reached the same wall and records three things that change
how the probes are read:

* `PE_state.video.v_baseAddr` is the physical frame buffer; drawing through it
  faulted at 0x9c00a8e0, and asking `ml_io_map` for the whole panel panicked in
  `kmem_alloc` (`round_page(1080 * 2376 * 4) = 10264576`, `KERN_NO_SPACE`).
* It draws through `vinfo.v_baseaddr` instead, which it describes as already
  mapped.
* The entry probe's inline assembly is only valid while the identity mapping is
  in place, so a probe that does not fire after the pmap switch is not proof
  that the kernel died there.

## Boot arguments from the reference port

`board.toml` now carries the reference port's arguments for this board:

    kernel_args = ["-v", "serial=3", "debug=0x14e", "-no_slto_panic",
                   "amfi_get_out_of_my_way=1"]

Its record says AMFI panics with "No PMGR?" until the board has a PMGR provider
and that the spinlock timeout panic has to be turned off before the PMGR work,
which matches the three-to-six second gap between the bars and 900E.
`bootN.img` is the image built with them.
## Third round: the AMFI call site is patched and the wall is unchanged

The reference port records that every kernel rebuild needs
`scripts/patch-amfi-nopmgr.py`, which skips AMFI's "No PMGR?" panic. The call
site matched this board's kernel exactly:

    scripts/patch-amfi-nopmgr.py out/.../kernel/kernel.macho --dry-run
    0x198ad04: 0x940069b8 -> 0x17ffffd2 (b 0xfffffff004aaec4c)

`bootP.img` carries that patched kernel (`90ddd6d5...`), the panic screen and
the reference boot arguments. The result is unchanged: three bars, then 900E a
few seconds later. AMFI is therefore not the layer that stops this boot.

## What the timing means

A panic parks the CPU; the SoC watchdog then resets it, and XBL shows 900E. The
few seconds between the bars and 900E are the watchdog, not work being done, so
the fault is at or just after the last confirmed checkpoint: console creation.
`initialize_screen()` is the only code between the KPRINTF startup entries and
the first console draw that can fail this way, and the panic screen cannot report
it because `vinfo.v_baseaddr` is exactly what has not been set yet.
## Fourth round: the A/B rules out both the tag and AMFI

| Image | FD video base | AMFI | Result |
| --- | --- | --- | --- |
| `bootE.img` | physical (untagged) | unpatched | three bars, then 900E |
| `bootJ.img` | physical \| 1 | unpatched | panic screen: alignment fault from the probe's own bar paint |
| `bootK.img` | physical \| 1 | unpatched | three bars, then 900E |
| `bootN.img` | physical \| 1 | unpatched | three bars, then 900E (reference arguments) |
| `bootP.img` | physical \| 1 | patched | three bars, then 900E |
| `bootQ.img` | physical (untagged) | patched | three bars, then 900E |

So neither the AMFI skip nor the physical tag changes where the boot stops: with
an untagged base the console draws into an unmapped virtual address, and with the
tag the mapping that `initialize_screen()` asks for does not happen. The next
question is therefore why the tag path fails here while the reference port
records a mapped `vinfo.v_baseaddr`.

Candidates, in order:

1. `ml_io_map_unmappable` before `STARTUP_SUB_KMEM` goes through
   `io_map()`'s static `virtual_space_start` branch and `pmap_map_bd`. If the
   DarwinBoot window (kernel plus ramdisk) really sits over the splash region,
   `pmap_map_bd` is asked to map pages the kernel already manages.
2. The static virtual window may be too small for a 10 MiB framebuffer when
   `pmap_startup` sized it from the window the firmware handed over.
## Fifth round: why the mapping cannot work that early

The DarwinBoot diagnostics (frame-buffer text added to the driver, plus the
boot-args tag) gave the hand-off layout on the panel:

    kernel pa 0xC007C000    window 0x100000000
    window end 0x1C007C000  args 0xC4044000
    video 0x9C000000  1080x2376  row 4320   entry 0xC32144B8

The frame buffer at 0x9C000000 is below the 4 GiB DarwinBoot window, so it is
outside the kernel's managed memory and `ml_io_map_unmappable` is allowed to
map it. The window is also large, so the static virtual window is not short of
space.

The real cause is ordering. `io_map()` takes its static branch while
`startup_phase < STARTUP_SUB_KMEM`:

    start = virtual_space_start;
    virtual_space_start += round_page(size);
    pmap_map_bd_with_options(start, phys_addr, phys_addr + alloc_size, ...);

`virtual_space_start` is assigned only by `pmap_startup()`, which
`vm_page_bootstrap()` calls during the `STARTUP_SUB_KMEM` entries. On arm64
`PE_create_console()` runs in `arm_init()` right after the
`STARTUP_SUB_KPRINTF` entries, so `virtual_space_start` is still zero:
`ml_io_map_unmappable` returns 0, `vinfo.v_baseaddr` becomes 0, and the first
console store goes to address 0.

The same call is what `check_for_panic_log()` makes for the `pram` region, so
it also returns 0 and the function returns silently: the panic log is never
armed, which is why `PanicLogDxe` found no header and the device still reached
the menu. Apple's iBoot passes an already-mapped virtual frame buffer and never
takes this branch, so the ordering hole is invisible on Apple hardware.

The tag added to `Args->Video[0]` is what selects this branch; without it the
kernel uses the physical address as a virtual address and faults at
0x9C000000 instead. Both variants end in the same watchdog reset.

### Fix to implement

Patch `initialize_screen()` so the mapping is deferred instead of attempted
too early: when `makeMapping` is requested and
`startup_phase < STARTUP_SUB_KMEM`, keep the physical base, leave
`vinfo.v_baseaddr` zero so the console reports "No video" and the boot
continues, and map the frame buffer at the later
`initialize_screen(NULL, kPEAcquireScreen)` call from
`kernel_bootstrap_thread`, where the kernel map and `kmem` exist.

That call currently skips the mapping block because `boot_vinfo` is NULL, so
the patch also has to give it the remembered physical base.
## Sixth round: the deferred mapping patch

`patches/xnu/zzz-defer-framebuffer-mapping.patch` (device series, applied last)
changes `initialize_screen()` in three places:

* when `makeMapping` is requested and `startup_phase < STARTUP_SUB_KMEM`, it
  stores the physical base, width, height, row bytes and depth, clears
  `v_physaddr` and `newVideoVirt` and lets the documented "No video" path run,
  so `PE_create_console()` returns and the boot continues;
* in that path it leaves `gc_acquired` FALSE, so the console is not claimed;
* at its start it turns the later `initialize_screen(NULL, kPEAcquireScreen)`
  call (from `kernel_bootstrap_thread`, after the KMEM entries) into a real
  graphics hand-off: it maps the remembered physical range with
  `ml_io_map_unmappable()` and passes a synthetic `PE_Video` with the mapped
  virtual base, so the normal path copies the geometry and initialises the
  video console.

The patch applies cleanly (`patch -p1 --dry-run`) and the kernel was rebuilt
with it: 51,027,968 bytes, sha256
54c3565b5d84303eebdcd971721c21d4d562eabe99f8b50cebf69e43040f1ed0.

Artifacts for the next hardware run:

| File | sha256 |
| --- | --- |
| `kernel_diag.bin` (BootShim + FD with the docking diagnostics) | shim 112 B + 4 MiB FD |
| `bootT.img` (kernel above + AMFI patch + panic screen + pram region) | 1c06b7829d133df37076c942985b69373ae9971022df82ca04fec3c0169cb4e0 |

Not yet run on the device: the phone was left parked from the previous attempt
and has to be power cycled back to fastboot before `bootT.img` can be booted.
## Seventh round: the kernel boots to bsd_init

Two more device patches, both in `pexpert/arm/pe_init.c`:

* `zzz-arm-panic-region-guard`: `check_for_panic_log()` returns immediately
  while `kernel_map == VM_MAP_NULL`. Without it, adding the `pram` node made the
  first call map the region through `io_map()`'s static branch with
  `virtual_space_start == 0`, i.e. at virtual address 0.
* `zzz-arm-panic-region-late`: `PE_initialize_console()` calls
  `check_for_panic_log()` again once `kernel_map` exists, which is after
  `pmap_startup()`, so the DT panic region is armed for real.

Kernel `a8c5d42b...` (three device patches) + AMFI patch, panic screen and the
480-row frame buffer, booted as `bootX.img`
(`23627207e51bd2c151531bf46deecccc895722b914750fbefdd450b41a848e14`):

**the panic screen drew**:

    failed to mount root device @IOKitBSDInit.cpp:984
    frames fffffff00797c384 fffffff0076e489c fffffff0072c4864 fffffff0074164a4
    pages=0x20000000 000f814c
    free=0x00000000 0f6f51

That is the milestone for the handoff: `arm_init`, `machine_startup`,
`vm_init`, `PE_create_console`, `kprintf`, the console acquisition in
`kernel_bootstrap_thread` and the whole bootstrap up to `bsd_init` all run, and
the deferred frame-buffer mapping succeeded (the panic screen draws through
`vinfo.v_baseaddr`, which only exists if that mapping happened). The kernel now
stops where a port without a storage driver stops: `vfs_mountroot` finds no root
device.

The next layer is storage (UFS), not the handoff.
