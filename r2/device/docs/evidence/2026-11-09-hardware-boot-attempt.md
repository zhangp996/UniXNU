# Hardware boot attempt, 2026-11-09

## What was tried

1. `fastboot boot boot.img` (no flash). ABL reported `Booting OKAY`, but the
   device returned to fastboot. The ABL log shows the command loading the
   *current slot* instead of the downloaded image: `Booting from slot (_b)`,
   then `avb_vbmeta_image.c:59 ERROR: Magic is incorrect` for the empty
   `vbmeta_b`, then `HandleActiveSlotUnbootable: Rebooting`. The downloaded
   image never ran.
2. `fastboot flash boot_b boot.img` + `set_active=b`: same empty vbmeta_b
   failure; the bootloader fell back to slot a.
3. `fastboot flash boot boot.img` on slot a: the bootloader showed the OnePlus
   "system destroyed / flash the correct image" screen (AVB refused the
   unsigned image). The firmware never executed.

## Recovery

`fastboot flash boot boot_a.img` with the dump taken before the attempt
(sha256 784ef7a4b658ed883cc444b280ca7e128acdb891ea97c4bb76a6c3e5c282c068,
matching `/dev/block/by-name/boot_a`), then `fastboot reboot`. Android booted
again on slot _a (`sys.boot_completed=1`).

## Consequence

The bootloader verifies boot images even in the orange (unlocked) state. Booting
a custom image needs the verification flags turned off in vbmeta (or a signed
image with the owner key). `fastboot boot` is not usable on this ABL.


## Second attempt (AVB disabled)

fastboot --disable-verity --disable-verification flash vbmeta vbmeta_a.img
rewrote the struct and Android still booted. Then fastboot flash boot boot.img
and reboot: the system-destroyed screen no longer appears, so the bootloader
accepted the image, but the device still returns to fastboot within a minute
(firmware never reaches USB or the framebuffer). The ABL log ring keeps no
firmware-side trace after the jump.

Restored with the same boot_a dump; Android booted on slot _a.

Conclusion: verification no longer blocks execution; the failure is inside the
firmware (SEC/PEI, DTB or DXE init). Observability is the next requirement: the
merged Android DTB is not a Mu-Silicium board tree, and there is no log
transport yet.


## Third round (image format and stub tests)

Tried, each time restoring boot_a afterwards:

* header v1 and v2 (v2 with a 525 KB base DT and with the 940 KB merged DT),
* FD base 0x9F000000 and 0x9FC00000,
* a 32-byte framebuffer stub, raw and gzip, as the boot kernel.

Every image returned the device to fastboot within a minute; the stub never
painted the frame buffer, so the boot chain stops before the image's first
instruction or the stub cannot reach the panel.

The ABL log around the attempts shows the memory map ABL hands over
(0x80000000 + 0x3BB00000, 0x180000000 + 0x100000000, 0xC0000000 + 0xC0000000)
and "Shutting Down UEFI Boot Services", but no record after the jump.

Conclusion: without a firmware-side log the next change cannot be validated.
AdvancedLogDxe and a log sink (screen QR or a persistent block) are the next
requirement; more blind flashing is not useful.


## Fourth round (format matrix) and final state

Tried after the AVB-off attempt, restoring boot_a each time:

* header v1 and v2, merged and base DTB, FD base 0x9F000000 and 0x9FC00000,
* gzip and uncompressed kernel,
* a 144-byte BootShim instrumented to paint the splash frame buffer red before
  copying the FD,
* an AVB hash footer signed with a test key (image padded to 96 MiB),
* a 32-byte bare stub, raw and gzip.

Every image returned the device to fastboot within 20-25 s; the frame buffer
paint never appeared, so the bootloader does not hand control to the image (or
the image faults before its first store). The AVB footer did not change this.

## Phone state

Stock boot_a and stock vbmeta_a (flags 0) restored; Android boots on slot _a
with root intact (sys.boot_completed=1).


## Fifth round (execution bisect)

* BootShim painted the splash frame buffer and then hung at _Start: the device
  stopped enumerating USB and needed a manual power cycle. This proves the
  bootloader does hand control to BootShim.
* A hang placed after the 3 MiB copy to FD_BASE, and a hang at the first SEC
  instruction, both returned the device to fastboot instead. Either the copy or
  the jump to FD_BASE faults, or those two images were not executed.
* Both FD_BASE values (0x9FC00000 and 0x9F000000) behaved the same.

Phone restored to stock boot_a and stock vbmeta_a after every test; Android
boots on slot _a (sys.boot_completed=1).

Next measurement: a single store to FD_BASE followed by a hang, then a 4 KiB
copy followed by a hang, to separate "destination not writable" from "copy loop
or jump fault".


## Sixth round (single store, alternate base)

A single 32-bit store to FD_BASE followed by a hang did not hold the device:
it returned to fastboot within 15 s, at 0x9F000000 and with the earlier
0x9FC00000 setup alike. The one earlier "hang" observation is therefore not
reproducible and cannot be treated as proof that BootShim runs; it may have
been an ABL/SoC stall.

No further blind flashing is planned. Stock boot_a and vbmeta_a are restored
and Android boots on slot _a (sys.boot_completed=1).

The next useful step is a hardware debug channel (UART on the board, or a
known-good Mu-Silicium image for a sibling kona device as a control), not more
boot-image variants.
