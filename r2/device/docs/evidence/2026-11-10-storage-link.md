# Storage link: past the IOBlockStorageDevice wall (2026-11-10)

The four wiring steps gave a building archive; enabling `CONFIG_XNU_NATIVE_UFS=y` then stopped the
XNU link. This record covers what the link needed, the image it produced, and the one step that is
still open: the phone has not run it.

## What was in the way

| Group | Symbols |
| --- | --- |
| storage family | `__ZTV20IOBlockStorageDevice`, then every base method the subclass repeats in its own vtable (`init`, `getProperty`, `setProperties`, `requestIdle`, `doUnmap`, `doSetPriority`, `doGetProvisionStatus`) |
| owner policy | `vr_open`, `vr_verify_block`, `vr_xnu_crypto`, `verified_root_key`, `verified_root_policies`, `verified_root_policy_count`, `boot_progress_poll` |

The storage family lives in the prelinked `com.apple.iokit.IOStorageFamily` and the kernel link runs
before the prelink, so the class can only come from recorded addresses. The policy group needs the
signed OS/SystemOS policies, which need the root images and the userdata image this board has not
built yet.

## What changed

| Where | Change |
| --- | --- |
| UniXNU `scripts/derive-storage-bindings.py` | new: reads the compiled object's vtable, maps every entry it repeats to the same slot of the original vtable at the pinned prelink address, and writes the absolute bindings |
| UniXNU `scripts/data/native-storage-21H461-original-bindings.S` | new: the 11 bindings (vtable, metaclass, constructor, base D2, seven base methods) |
| UniXNU `scripts/build-arm-driver.py` | archives the bindings next to the driver for `NativeUfs`, refuses a build whose object imports a storage symbol the record does not cover, and compiles the owner-policy variant only when `XNU_VERIFIED_ROOT_PROBE=1` |
| UniXNU `scripts/unixnu/payload.py` | a kernel whose AMFI call site carries the recorded patch (device `scripts/patch-amfi-nopmgr.py`) is accepted for `native-ufs-root=1` without `sheng-pmgr=2`; the check reads the instruction, not the argument |
| ShengUfs `NativeUfs.cpp` | the read timer and its counters moved out of the policy block, so the unverified variant compiles |
| this repository | `configs/instantnoodlep_defconfig` enables `CONFIG_XNU_NATIVE_UFS`; `board.toml` adds `native-ufs-root=1`; `dts/instantnoodlep.dts` enables `&ufs` and `&ufsphy` |

## The bindings are derived, not transcribed

The vtable of the built class repeats every entry it does not override, and each repetition names
the base method. Reading the same slot out of the original vtable gives that method's address. The
four entries the layout does not describe are the anchors recorded from the 21H461 prelink
(vtable `0xfffffff005cb44c0`, metaclass `0xfffffff006add1d0`, constructor `0xfffffff00496ed98`, base
D2 `0xfffffff00496edb8`), the same ones UniXNU's marble-milacy port records for this build.

Checked against this board's own prelinked image before use: the vtable's address point +16 holds
190 entries, all inside `__PLK_TEXT_EXEC`, one of them (the shared stub) fourteen times, and the
seven derived methods land between `0xfffffff00496ee30` and `0xfffffff00496f08c`, right above the
constructor. The build now fails if the driver starts importing a storage symbol the file does not
list.

`D1` and `D0` are deliberately not bound: they are brk traps in the abstract base, and a reference
to them means abstract complete/deleting destruction, which has to fail the link instead.

## Result

| Artefact | Value |
| --- | --- |
| `libShengUfs.a` | sha256 `1b6bce6ec2b709cd2b71a09931026f9ef652194a90c8914f7683ce5e6e61d2b3`, 486920 bytes |
| `kernel/kernel.macho` | sha256 `82e653afbc0de677d5d3aa78ed17bfb9d10610728382b80040470aaf0b34538b`, links with the driver |
| AMFI-patched copy | sha256 `94c76d9f3fda57aaa0bd2c96e77e5832e85007d005c96bb283fb5b5a3bf53dba` |
| `payload/payload.img` | sha256 `bbb7580bc04b71347bb2926bd19ec0e6d4ffa04ab9dd0cfe60ae6f79aa04a387`, 92,274,688 bytes, args `-v serial=3 debug=0x14e -no_slto_panic amfi_get_out_of_my_way=1 native-ufs-root=1` |
| ADT | root compatible `J120AP oneplus,instantnoodlep qcom,sm8250`, 13904 bytes; the UFS nodes stay disabled in the board dts (see below) |
| boot image `bootZ.img` | sha256 `cea799d1a77c2534b7dcc2e7a2eacb7497e152b8a92c8534421d542847b701a5`, 107,507,712 bytes (BootShim + FD `kernel_small.bin`, this payload, `stock_boot.dtb`) |

## The device nodes that had to come back out

Enabling `&ufs` and `&ufsphy` in the board dts exported `ufshc` and `ufsphy` under
`arm-io` and grew the ADT from 13904 to 14400 bytes. With that ADT the kernel stopped right after
the entry-probe bars and never reached its console. The A/B isolates it to the ADT, not the driver:

| Image | kernel | ADT | args | result |
| --- | --- | --- | --- | --- |
| bootX (earlier) | a8c5d42b | without the UFS nodes | without `native-ufs-root` | console, then the root-mount panic |
| bootZ4 | a8c5d42b | with them | with `native-ufs-root=1` | bars, then silence |
| bootZ5 | a8c5d42b | without them | with `native-ufs-root=1` | console, then `failed to mount root device @IOKitBSDInit.cpp:984` |

Same kernel in all three rows, so neither the storage driver nor the extra boot argument is
involved: the two device nodes are. ShengUfs does not need them - it attaches to the platform
expert and reads its windows from `ufs-soc-table.h` - so the board dts keeps them disabled and
carries a comment with this measurement. Why the platform expert stops with them present is
still open; the candidates are the hand-off buffer for the ADT and the exported properties
(`reg` with two ranges, `interrupts` with `interrupt-parent`).

The ADT root compatible is what resolves the SoC at run time: the platform-expert personality reads
it, matches `qcom,sm8250` in the table, and takes the kona host window, the GCC branches and the
no-SMMU decision from that entry.

## Device: the first storage result (2026-11-10 13:20)

Image `bootZA` (kernel `6bc86152`, args `native-ufs-root=1 native-ufs-diagnostics=1 native-ufs-probe=10`).
The personality had to be compiled in first: `KernelConfigTables.cpo` kept the flags of the first
build (without `XNU_NATIVE_UFS`), so every earlier driver image carried the driver with no personality
pointing at it - `build-xnu-4k-baseline.sh` now stamps `PERSONALITY_CFLAGS` and recompiles that object
when the set changes. The kernel strings confirm it: `NativeUfsRoot` is present and the driver initializer is
in `__mod_init_func`.

The driver then reported on the panel:

```
NATIVE_UFS root check
userdata first=3f13a8 last=1d197fa
block0 magic f79c330c 8476b30b ext4magic=8c24 ext4blocks=405 3431726
manifest status=1 windows=0
```

What that proves on the device: the controller hand-off was adopted (firmware request list stopped and
replaced, NOP OUT accepted, the device woken and LUN 0 ready), the DMA buffer reached the controller, the
GPT of LUN 0 parsed, the `userdata` entry matches board.toml exactly (`0x3f13a8` = 4133800 and
`0x1d197fa` = 30513146), and block 0 read back. That is the UFS guide host-access, DMA/commands and
block-read milestones in one run.

`manifest status=1` is `no manifest magic` (RootLayout.h): the partition still holds Android data
because this board has not written a userdata image, so the driver has no root windows to publish and
`vfs_mountroot` still panics. The next step is `unixnu build userdata`: it needs the 6.83 GB IPSW
(the OS and SystemOS disk images are not cached here), writes an ~8 GiB `userdata-root.img` with the
`SHNGROOT` manifest, and prints the `fastboot flash userdata` command for the operator.
## The userdata image (2026-11-10 13:29)

`python3 ./unixnu build userdata -D ../UniXNU-instantnoodlep` built the partition image the driver needs:

| Item | Value |
| --- | --- |
| image | `userdata-root.img`, 9,382,658,048 bytes, sha256 `fd43ee7035ce185ce4215ac93fdc006fd784a54b3b7442e21457c226cbe03093` |
| OS window | 6,077,580,800 bytes at `0x100000`, sha256 `e5634d04a8ac35f024845f583394e0a35deb26a04cb0c00286a8674e25a9a4f2` |
| SystemOS window | 3,303,014,400 bytes at `0x16a600000`, sha256 `b4908cde41d23e6bc05e6af51a2c9eeb00cf0f863a6a18b1600c96a3fd70414f` |
| partition | `userdata`, 108,049,805,312 bytes |
| flash | `fastboot flash userdata instantnoodlep-artifacts/userdata-root.img` |

The digest matches the image UniXNU's marble-milacy port recorded (`fd43ee70`) byte for byte: same IPSW,
same layout, same two windows. The copy on the Windows side is byte-identical to the one in the build tree,
which is what `fastboot` sees, since Windows cannot read the WSL filesystem.
## Why the policy chain is not in this image

`CONFIG_XNU_VERIFIED_ROOT_PROBE` stays off, so the driver compiles its unverified variant: it
publishes the userdata windows as an IOMedia without authenticating each block against an
owner-signed policy. The verified chain needs the signed OS/SystemOS policies built from the root
images and the userdata image (`unixnu build userdata`), which this board has not produced, and the
6.83 GB IPSW is not cached locally. Bring-up comes first: the unverified device is what tells
whether the controller hand-off, the command path and the window layout work on this board.

## Open

`fastboot boot bootZ.img` and read the panel: no device was attached when the image was built
(2026-11-10 12:17, `adb devices` empty). The result belongs in this record and in the storage area's
completion criteria.
