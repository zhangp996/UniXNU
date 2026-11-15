# OnePlus 8 Pro (instantnoodlep)

UniXNU board repository for the OnePlus 8 Pro IN2020: Qualcomm SM8250
(Snapdragon 865), 8 GiB DRAM, 1440x3168 Samsung DSC command panel, Samsung
S6SY791 touch controller. The port targets the **arm64** iPadOS 17.7.11 profile
(`ipados-17.7.11-21H461`, iPad7,1 / j120ap); the SoC is Armv8.2-A and has no
FEAT_PAuth, so arm64e does not apply.

## Status

| Area | State | Notes |
| --- | --- | --- |
| Configuration | host-verified | `unixnu defconfig` and `unixnu status` resolve the arm64/SM8250/iPadOS layers and 59 XNU patches |
| Firmware and boot | device-verified | menu, 60 s timeout and keys work on the phone; the kernel console now reaches the panel |
| CPU and interrupts | implemented | 8 CPUs at MPIDR 0x000-0x700, GICv3 0x17a00000, 19.2 MHz timer |
| Memory and DMA | implemented | 0x80000000 + 0x200000000 (8 GiB) |
| Storage and root | planned | UFS 3.0 at 0x1d84000; userdata 4 KiB LBA 4133800-30513146 |
| Display | planned | `qcom,mdss_dsi_oppo19811_samsung_1440_3168_dsc_cmd` |
| Input | planned | S6SY791 at I2C 0x48, 17x38, firmware in `proprietary-files.txt` |
| GPU / network / audio / power | planned | no components selected yet |
| Packaging | device-verified | hand-packed boot images (header v2, stock dtb) boot to the menu; seed is a placeholder |

Hardware measurements and the commands that produced them are in
[`docs/evidence/2026-11-09-device-inventory.md`](docs/evidence/2026-11-09-device-inventory.md);
`porting.toml` is the authoritative record.

## Host setup

Run from a Linux or macOS host with the UniXNU toolchain (on Windows use WSL):

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU
git clone https://github.com/zhangp996/UniXNU-sm8250-common   # via feeds.toml
```

## Commands

```sh
DEVICE_DIR=../UniXNU-instantnoodlep
./unixnu defconfig instantnoodlep_defconfig -D "$DEVICE_DIR"
./unixnu fetch -D "$DEVICE_DIR"
./unixnu status --json -D "$DEVICE_DIR"
./unixnu tree -D "$DEVICE_DIR"
```

The build order is kernel, package, firmware, payload, userdata and boot image
per [Building](https://unixnu.com/building). The firmware step needs a
`uefi-instantnoodlep` checkout:

```sh
./unixnu build kernel -D "$DEVICE_DIR"
./unixnu build firmware --uefi ../uefi-instantnoodlep -D "$DEVICE_DIR"
```

## Build inputs

* IPSW: `iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw` (iPad7,1, build 21H461).
* UEFI firmware: not published for this board yet.
* Platform seed `UNIXNU_UNIQUE_CHIP_ID`: required by the payload step, kept
  outside the repository.

## Open work

1. Create the Mu-Silicium kona platform (`uefi-instantnoodlep`) with the
   UniXNU/DarwinBoot DSC and FDF integration.
2. Port the SM8250 UFS stream configuration to this board and add the storage
   component.
3. Add the display path (GOP first) and the S6SY791 touch probe.
4. Fill `[firmware]`, `[bootimg]` and the payload inputs once 1-3 exist.

## References

- [Device repository](https://unixnu.com/porting/device-repository)
- [Device tree](https://unixnu.com/porting/device-tree)
- [Component catalog](https://unixnu.com/reference/components)
- [Porting record](https://unixnu.com/porting/records)
