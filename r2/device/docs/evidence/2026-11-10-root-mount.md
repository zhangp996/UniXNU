# Root mounted, launchd blocked on the dyld cache (2026-11-10)

Continues `docs/evidence/2026-11-10-storage-link.md`. The userdata image is written, the root mounts
on the device, and the boot now stops at the shared cache instead of the mount.

## What the device did, in order

| Image | Change | Panel |
| --- | --- | --- |
| bootZA | personality compiled in, `native-ufs-probe=10` | `NATIVE_UFS root check`, `userdata first=3f13a8 last=1d197fa`, block 0 read back |
| bootZB | userdata flashed, root path only | `failed to mount root device` (publication gated on the owner policy) |
| bootZE | `native-ufs-allow-unverified=1`, diagnostics panic | `UFS Root FAILURE: SystemOS signed volume not found`, `stage=media publication`, a completed READ(10) in the dump |
| bootZG | the unverified build also skips the signed-UUID wait | `rootvp not authenticated after mounting @bsd_init.c:1107` - the root volume mounted |
| bootZH | `root-unverified=1` (`patches/xnu/zzz-root-unverified.patch`) | `initproc failed to start ... Library not loaded: /usr/lib/libSystem.B.dylib ... no dyld cache` |

The UFS stack is therefore complete on the phone through the mount: controller hand-off, DMA,
command path, GPT, both windows, IOMedia publication, APFS on the OS window, and the root vnode
handed to userland. What stops the boot now is the shared cache: `/usr/lib/libSystem.B.dylib` lives
in the SystemOS (Cryptex) volume, and the kernel does not mount it.

## The unverified switches

Both exist only in the unverified compile variant and both are opt-in at boot, so a verified build
behaves exactly as upstream:

- `native-ufs-allow-unverified=1` (`NativeUfs.cpp`): the publisher's identity phase has no signed
  policy to compare window identities against, so it publishes when the operator asks for it.
- `root-unverified=1` (`patches/xnu/zzz-root-unverified.patch`): `bsd_init.c` skips the
  `rootvp not authenticated` panic. Without the argument the check still panics.
- `native-ufs-diagnostics=1` turns the driver's soft root failures into panics that name the stage,
  which is what produced every line in the table above.

## The dyld cache wall

`XNU_OWNER_CACHE_MOUNT` requires `XNU_OWNER_ROOT_TRUST`, which requires `XNU_OWNER_ROOT_AUDIT`, which
requires `XNU_VERIFIED_ROOT_PROBE` or `XNU_SOURCE_STORAGE` - the owner-signed policy chain this board
has not built (`scripts/build-owner-os-policy.py` wants an owner key and a macOS-built
`verified-root.dylib`). The reference port's alternative is board code that mounts the Cryptex volume
without the owner signature.

## USB debug console

Integrated, not yet enumerating:

| Where | Change |
| --- | --- |
| device `feeds.toml` | pins `FFT-infra/QcomXnuUsbDebug` at `ab6e3635`; `local.toml` points at the working tree |
| device `patches/xnu/series` | `zzz-qcom-usb-debug-console.patch` and `-worker.patch` (the module's darwin23 set, which applies cleanly to XNU 10063.141.1) |
| UniXNU `components.toml` | `QcomXnuUsbDebug` entry, built by `build-qcom-usb-debug.py` |
| device `scripts/build-qcom-usb-debug.py` | runs the module's own builder with this tree's `pe_kprintf.o.json` and `soc/Sm8250.h`, then adds the board hooks |
| device `package/QcomUsbBoard.c` | `boot_progress_persist`, `boot_reset_bootloader` (PSCI SYSTEM_RESET), and `qcom_usb_board_check`, which panics with the module status under `qcom-usb-report=1` |
| UniXNU `build-xnu-4k-baseline.sh` | `XNU_QCOM_USB_DEBUG` validates and force-loads the archive |
| `board.toml` | `qcom-usb-debug=1 qcom-usb-ctl=1 qcom-usb-report=1` |

The console's poll worker starts in `bsd_autoconf()`, before the launchd attempt, so the report image
states on the panel why the controller hand-off was refused (phase, error, core result, PHY/VBUS
ownership, hardware parameters). No `VID_1209:316D` device appears on the host yet.
## Console status on the panel

`bootZK` carried `qcom-usb-report=1`, so `qcom_usb_board_check()` panicked with the module status in
`bsd_autoconf()`:

```
QCOM USB DEBUG report
phase=3 error=0 core=0 qcom=0 restore=0 dma_mode=16 diag=0
vbus=1/0 phy=1/0 applied=1 hwparams0=4020400a width=64
dma_pa=c395000 iova=90000000 line=64 freq=19200000
poll=49272 events=0 io_failed=0 dropped=0
buf=4194304/0 high=88 over=0 contended=0
```

`phase=3` is `QCOM_USB_DEBUG_ACTIVE`: the controller hand-off, the private translated DMA window at
the SM8250 IOVA, and the PHY/VBUS ownership all succeeded, and the kernel had already queued 88
bytes for the host. The same boot enumerated on the host as
`USB\VID_1209&PID_316D\QCOMXNUUSBDEBUG` (COM12/COM14), so the device side is done.

## What the host side still needs

- Windows `usbser` cannot open the port: `Open()` fails with `A device attached to the system is not
  functioning`, i.e. its CDC class requests are not answered the way Windows expects. Opening is
  also the point where a killed reader leaves the port locked (`Access to the port is denied`).
- Passing the device to WSL with `usbipd` works mechanically (a fastboot-mode phone was visible in
  WSL as `18d1:d00d`), and WSL already has `fastboot`, `adb` and `cdc_acm`.
- Two usbipd details cost several attempts: `bind --force` unbinds the host driver immediately, so
  the following `attach --hardware-id` cannot find the device - attach by **busid** instead; and the
  busid is the first column of `usbipd list` (`1-1`), not the `VID:PID` column.
- Any `fastboot` command run inside WSL needs `wsl -u root` (no udev rules for the phone), which is
  how `bootZL` was sent over the passthrough.

Once a reader is attached, `usbipd attach --wsl --busid 1-1 --auto-attach` keeps it attached across
the panic-time re-enumeration, and `/dev/ttyACM0` receives the module's replay (the 88 buffered
bytes) plus live output.
### End of the first console day

The remaining gap is only the reader. What was established:

- the WSL kernel has `CONFIG_USBIP_VHCI_HCD=y` and the `vhci_hcd` platform driver exists, but no
  distribution package provides the `usbip` client; usbipd ships a static one at
  `C:/Program Files/usbipd-win/WSL/usbip` and it runs from `/usr/local/bin` inside WSL;
- `usbipd attach --wsl` needs the WSL VM to be running (it exits with *no WSL 2 distribution
  running* otherwise), and its own WSL-side attach did not land: the `vhci` ports stayed at state
  `004` (`VDEV_ST_NULL`) and `usbip list -r 127.0.0.1` reported *no exportable devices*;
- the window is short because the kernel panics about ten seconds after the console enumerates, and
  the panic takes the controller down with it, so the device disappears mid-capture.

Two ways forward, both clean:

1. finish the boot (owner-signed policies, then the dyld cache) so the kernel lives long enough for
   a reader to attach - the console then works as intended, and the reader recipe is known:
   keep a WSL process alive, `usbipd bind --busid 1-1 --force` while the device is present,
   `usbip attach -r 127.0.0.1 -b 1-1` as root in WSL, then read `/dev/ttyACM0`;
2. make the Windows path work instead: the module answers CDC class requests the way `AppleUSBACM`
   expects, and Windows' `usbser` refuses them. Binding the interface with WinUSB and reading the
   bulk IN endpoint directly (libusb) bypasses the class requests entirely.
