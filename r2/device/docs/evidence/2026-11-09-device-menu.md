# First working firmware boot (2026-11-09)

Command:

    fastboot boot instantnoodlep-artifacts/bootD.img

Image: 107,507,712 bytes, Android boot header v2, page 4096, cmdline
"console=ttyMSM0,115200n8 androidboot.hardware=qcom".

| Part | Value |
| --- | --- |
| shim | compiled from BootShim.S with FD_BASE=0x9E400000, FD_SIZE=0x400000, REQUIRES_KERNEL_HEADER=1; 112 bytes, ARM64 header image_size 0x400000 |
| firmware | SILICIUM_UEFI.fd, 4,194,304 bytes, base 0x9E400000 (window between dfps_data_region and kboot_uboot_logmem) |
| payload | payload.img, 92,274,688 bytes, unixnu.toml [boot] timeout = 60, default xnu |
| dtb | stock boot_a dtb section, 11,028,247 bytes (qcom,msm-id present) |

Observed on the panel:

* firmware reaches the UniXNU menu;
* the menu counts down 60 seconds before the default entry;
* volume up/down move the selection, the power key acts as Enter
  (KonaKeysDxe, PM8150 SPMI GPIO6/GPIO7 and PON);
* selecting xnu shows the DarwinBoot hand-off bar and then the kernel crashes
  into Qualcomm 900E diagnostics, which is the next layer to fix.

The earlier failures came from the boot image layout: image_size must stay
FD_SIZE (0x400000), not shim+FD, and the FD must live at 0x9E400000. A 3 MiB
FD at 0x9FC00000 overlaps kboot_uboot_logmem and never started.
