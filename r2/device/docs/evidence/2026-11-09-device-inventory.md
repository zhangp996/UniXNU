# OnePlus 8 Pro (IN2020) hardware inventory

Collected 2026-11-09 over adb from serial fa8620b3, Android 13
(IN2020_13.1.0.185(CN01)), bootloader unlocked (verifiedbootstate=orange).
Commands below were run as root through Magisk.

## Identity

| Fact | Value | Source |
| --- | --- | --- |
| Model / product | IN2020 / OnePlus8Pro_CH | getprop ro.product.model |
| SoC | SM8250 (kona v2.1) | getprop ro.soc.model, boot.dtb model |
| RAM | 8 GiB (MemTotal 7,708,100 kB) | /proc/meminfo |
| Kernel | 4.19.157-perf+ aarch64 | uname -a |
| Boot slot | a | getprop ro.boot.slot_suffix |
| Panel | qcom,mdss_dsi_oppo19811_samsung_1440_3168_dsc_cmd | /proc/cmdline msm_drm.dsi_display0 |

## CPUs

8 cores, MPIDR 0x000..0x700. cpu0-3: implementer 0x51, part 0x805 (Kryo 585
Silver, A55 class). cpu4-7: implementer 0x41, part 0xd0d (Cortex-A77)。 （prime 核 MIDR 报 A77，非 X1；4/3/1 属缓存拓扑，见 2026-11-14-cpu-isa-audit.md）
timebase 19.2 MHz (SoC DTSI). No FEAT_PAuth: the SoC is Armv8.2-A, so this
board uses the arm64 profile, not arm64e.

## Memory

DRAM base 0x80000000, size 0x200000000 (8 GiB). /proc/iomem lists the holes
left by the bootloader carveouts; the largest contiguous System RAM runs to
0x27fffffff.

## Storage

| Fact | Value |
| --- | --- |
| UFS host | 1d84000.ufshc (UFS 3.0), PHY 1d87000 |
| userdata GPT entry | first LBA 4133800, last LBA 30513146 (4 KiB LBA) |
| boot_a | sde11, 96 MiB (98304 KiB) |
| dtbo_a | sde17, 8 MiB, 19 overlay entries |
| Android boot image | boot_a.img, header v2, page size 4096 |

GPT read with `dd if=/dev/block/sda bs=4096 count=6`; the header is at 4096
and the entry array at 8192, so the disk uses 4 KiB logical blocks.

## Display

* Panel node: `qcom,mdss_dsi_oppo19811_samsung_1440_3168_dsc_cmd` in
  dtbo_a entry 2 (fragment@39), 1440x3168, DSC command mode.
* MDSS 0xae00000, DSI0 0xae94000.
* Pixelworks Iris bridge present (pxlw,iris) with reset/wakeup/abyp GPIOs.
* TE and reset GPIOs are defined in the panel node of dtbo entry 2.

## Input

* Samsung S6SY791 touch controller at I2C address 0x48 (`s6sy791_19811@48`
  in dtbo entry 2, fragment@71).
* Report geometry: 17 TX x 38 RX, panel coords 0x59f x 0xc60 (1439 x 3168).
* Firmware: /odm/firmware/tp/19811/FW_S6SY791_SAMSUNG.bin,
  sha256 4cfcca35d268c4fad4ac194f8e192e31bb198d56035af8faf7206277df953e77.

## GPU

Adreno 650: kgsl 0x3d00000, GMU 0x3d6a000. Firmware a650_gmu.bin
(df79fa8482fcc7587bb239b9c9e2b24bce4729893b990fe3f82b30da8175b01c) and
a650_sqe.fw (b4c27420fd6a5c6b511b254cc0fad8cdd40eea38ceb3418f895a1084f4460487).

## USB

DWC3 at 0xa600000 (`ssusb@a600000/dwc3@a600000`), androidboot.usbcontroller=a600000.dwc3.

## Not in scope

Cellular modem (SDX55) and cameras are outside the UniXNU 17 profile scope for
this port.
