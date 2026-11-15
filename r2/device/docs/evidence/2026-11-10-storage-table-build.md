# Storage table wiring and archive build (2026-11-10)

Route: the upstream `ShengUfs` component with this board's own UFS facts in the SM8250 SoC
repository, decided in `docs/evidence/2026-11-10-storage-route.md`. This record covers the four
changes that make the component build against those facts, the build that verifies them, and the
link wall that stops `CONFIG_XNU_NATIVE_UFS=y` afterwards.

## Changes

| Repository | Change |
| --- | --- |
| UniXNU | `scripts/unixnu/tree.py` links the resolved SoC feed into the assembled tree as `soc/`; `scripts/build-arm-driver.py` adds `-I <tree>/soc/include` for a component whose catalog entry sets `soc_headers`; `scripts/build-sheng-source-storage.py` adds the same include for the arm64e runtime; the `ShengUfs` catalog entry declares `soc_headers = true` |
| UniXNU-sm8250-common | new `include/ufs-soc-table.h`: one kona entry with the host window `0x1d84000+0x3000`, the PHY window `0x1d87000+0xe00`, the GCC branch offsets `0x77010 0x77018 0x7701C 0x77020 0x7705C 0x77064 0x7709C 0x770B8 0x770CC`, `core_reset` id `0x21` in the UFS PHY BCR at `0x77000`, and no apps-SMMU stream |
| ShengUfs | `NativeUfs.cpp` looks the node's `compatible` up in that table and adopts the entry: controller and PHY windows, GCC branches, block reset, SMMU decision. The SM8550 values in `GccUfs.h` stay the fallback, and a single-entry table also covers a root node that names no SoC. The SMMU bypass, the display and the WiFi stream paths are now conditional on the entry |
| this repository | `board.toml` `[kernel] archives` is the complete `profiles/arch/arm64` list plus the storage route; `configs/instantnoodlep_defconfig` is unchanged in the delivered state |

`[kernel] archives` is a replacing array, not a merged one: naming only the three storage
components dropped corecrypto, the AES original-bindings archive, the mockfs fixture and
libpthread from the link, which fails on `IOAESAccelerator`. The list now repeats the whole
architecture list in its order and appends `component:ShengUfs`, `component:VirtioMmioBlock` and
`component:VerifiedRoot`.

## Revisions

| Repository | Commit |
| --- | --- |
| UniXNU | `3d7a87f4a9e935d0e25f1be44918016c29ed3d64` |
| UniXNU-sm8250-common | `2321a836f0140d2549072967e189da1160b61e05` |
| ShengUfs | `1165a34b37af15edaae04c90f24f797942e8d575` |
| UniXNU-instantnoodlep | `9fd5dc7251bcfa753fd15a56d8ba0929d2667895` |

Both sources are local working trees, pinned in the Git-ignored `local.toml` of the device
repository (format: `local.toml.example`); `feeds.toml` keeps the published revisions and
`unixnu.lock` keeps recording them, so a checkout without `local.toml` resolves the feeds.

## Commands

```sh
cd /home/dev/unixnu/UniXNU
export UNIXNU_UEFI=/home/dev/unixnu/uefi-instantnoodlep
python3 ./unixnu tree     -D ../UniXNU-instantnoodlep
python3 ./unixnu defconfig instantnoodlep_defconfig -D ../UniXNU-instantnoodlep
python3 ./unixnu build kernel -D ../UniXNU-instantnoodlep
```

## Result

- `out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/tree/soc` links to
  `/home/dev/unixnu/UniXNU-sm8250-common`.
- `build/sheng-ufs/compile-command.json` contains `-I .../tree/soc/include`.
- `NativeUfs.o` contains both `qcom,sm8250-ufshc` and `qcom,sm8250` (the controller and SoC
  identities of the table entry).
- `build/sheng-ufs/libShengUfs.a` sha256 `1b7fa8d9b94fa23d1381a0e830a1747a733ad24eb10fdb55da971b1e66170ab8`,
  521152 bytes.
- `kernel/kernel.macho` sha256 `a8c5d42b046ea416eb998276e4d00ad0843f12899fb71ab574c9ad9644433ec3`,
  byte-identical to the kernel in the booting `bootX.img`: the storage route changes no code that
  this configuration links.

## The link wall behind `CONFIG_XNU_NATIVE_UFS=y`

`python3 ./unixnu build kernel` with `CONFIG_XNU_NATIVE_UFS=y` builds the archive and stops at the
XNU link with eight undefined symbols:

| Symbol | Source |
| --- | --- |
| `__ZTV20IOBlockStorageDevice` | the storage family base class vtable |
| `_vr_open`, `_vr_verify_block`, `_vr_xnu_crypto` | `kits/VerifiedRoot` sources |
| `_verified_root_key`, `_verified_root_policies`, `_verified_root_policy_count` | the signed policy bundle |
| `_boot_progress_poll` | the boot progress observer |

The VerifiedRoot group has a builder (`scripts/build-verified-root-probe.py`, force-loaded as
`build/verified-root-xnu/libVerifiedRootProbe.a` when `XNU_VERIFIED_ROOT_PROBE=1`) and needs the
signed policy of the selected root images. The remaining wall is the vtable:
`IOBlockStorageDevice` is a class of the prelinked `com.apple.iokit.IOStorageFamily` kext, so a
kernel object cannot link against it. The arm64e path solves this by compiling the pinned
`IOBlockStorageDevice.cpp` into its own archive (`scripts/build-sheng-source-storage.py`); the
arm64/17.7 path has no equivalent yet. That is the next task, and it is why the delivered
defconfig keeps `CONFIG_XNU_NATIVE_UFS` off.

## Verification state

Host: the archive builds with `-Werror`, the kernel links and reproduces the booting image.
Device: not tested in this state; the storage route has not been on the phone since the route was
chosen.
