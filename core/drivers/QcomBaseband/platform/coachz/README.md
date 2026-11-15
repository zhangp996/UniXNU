# Coachz integration

The SC7180 example is by **HoshinoYumeka ([@chiba233](https://github.com/chiba233))**. Its IPA register tables target v4.2. `coachz_modem.c` contains the board's fixed DMA ranges and assumes the apps SMMU is in bypass.

## Startup order

1. Provide modem power, clocks, interconnect votes, reserved MBA/MPSS memory, SMEM/SMP2P, GLINK/QRTR and rmtfs from the SoC/board integration.
2. Call `coachz_modem_configure(qmp_send, send)` after IPA power is available and before starting the modem. Call `coachz_modem_starting()` as modem startup begins.
3. On SMP2P `ipa-setup-ready`, call `coachz_modem_setup()`. Forward QRTR service announcements to `coachz_modem_new_server()` and IPA QMI traffic to `coachz_modem_qmi()`. Poll `coachz_modem_poll()` from the transport worker.
4. Set `/chosen/coachz-baseband` and publish the selected OS's `/baseband` properties and `/product/baseband-chipset`. The reference uses root `compatible = J121AP` for the Mav16 Qualcomm branch while keeping display identity properties unchanged.
5. Call `coachz_baseband_configure(send, del_client)`. The callback sends a QMI body from a local QRTR port to a remote node/port. Forward QRTR NEW_SERVER, DEL_SERVER and replies through the functions in `baseband/QcomBaseband.h`; poll `coachz_baseband_poll()` on the same worker. The reference worker polls every millisecond.
6. Apply the board's XNU service/firmware integration. `patches/coachz-prelink-baseband.patch` targets XNU 10063 after the Coachz Wi-Fi and OS patches: it connects the QRTR hooks, enables the cellular CommCenter job and mounts the Mav16 firmware directories before launchd.
7. Compile with `--coachz`, link the archive into the source kernel, and register its IOKit classes through the selected kernel's class-initialization path before publishing services.

The board packages its Qualcomm modem firmware separately from the Apple Mav16 files used by ABM and BBUpdater. The reference Apple archive has magic `CZBBFW01`; the XNU integration checks its hash, writes changed files under `/private/var/coachz/baseband/Mav16`, and exposes them at ABM's standalone and personalized firmware paths.

## Checkpoints

Check modem startup, QRTR services, IPA INIT_DRIVER/INIT_COMPLETE, AppleBaseband publication, ABM boot, QMUX clients, SIM identity, registration and a data session in that order. The device's factory FSG supplies modem identity and calibration; the reference reads it from the eMMC hardware boot partition and verifies it against the device's stored digest. Firmware packages keep this device-specific input with the board.

`baseband/coachz_bb_data.h` opens the DPM hardware port, negotiates WDA raw IP/QMAPv4 and translates WDS/QOS endpoints from PCIe `{3, 4}` to embedded `{4, 1}`. The activation indication carries service-provisioned and accepted-slot TLVs. Coachz accepts the data-port bindings and registers a valid SIM on LTE roaming with CS/PS attached. The operator and signal bars appear directly after a cold boot. Each ABM channel buffers up to 1,024 frames and logs header fields for its first 32 dropped frames. The bridge observes CommCenter's EF_ICCID and EF_IMSI reads, converts the card values to reading-order BCD and includes them in activation TLVs `0x42` and `0x43`. The SIM matches its carrier bundle and LTE is shown. PRI/GRI segments are acknowledged with the next expected segment number and discarded; FileTransferStatus `0xa001` follows the last one, so CommCenter caches the configuration. BSP `0xb001` returns the BHI chip ID and serial. Cellular IPv4 web loading works through `pdp_ip0`, including after PDP reconnects. EFS writes remain in memory until reboot.

`baseband/coachz_bb_esim.h` connects VINYL Tape to UIM. Card-status replies and indications preserve capability TLV `0x21`; legacy GetCsn returns `0x204`. EID/capability requests read TLV `0x01`, and Tape results use an 11-byte header. Challenge, authentication and download preparation retain the owning client's ISD-R channel. Terminal operations, errors, disconnect and 180 seconds of inactivity close it and restore the original slot mapping. The bridge returns empty recovery records. The host models pass; Coachz shows eSIM in Settings with an incomplete page. Profile provisioning remains unverified.

`coachz_bb_pdp.cpp` registers `pdp_ip0` through `pdp_ip3` for muxes 1 through 4, with MTU 1492. WDS bind, IP-family, start, stop and status replies drive family availability. IPv4 disconnect leaves an active IPv6 context running. Output queues drain through `coachz_modem_data_send`; `coachz_modem_data_receiver` installs the IPA receive callback. Both hooks execute on the QRTR/IPA owner thread. `PF_INET` and `PF_INET6` protocol plumbers let CommCenter reattach a family after unloading the old context; the initial attach uses the same callback. GSI polling also checks the hardware completion pointer for pending transactions. The PDP source is included by `coachz_baseband.cpp`.

The eSIM bridge caches four read-only ES10 responses and clears them after state-changing card commands. Repeated cached queries keep the physical SIM mapping unchanged.

## CommCenter compatibility copy

The pinned CommCenter (iPadOS 17.7.11 / 21H461) blocks the Internet context while Apple's postponement ticket is absent. `tools/coachz_cellular_compat.py` takes the original binary from that OS, checks its SHA-256 and Mach-O UUID, rewrites one conditional branch, re-signs it ad hoc with the original entitlements and writes a matching trust cache. WDS activation, address queries and network results keep the original logic.

```sh
python3 platform/coachz/tools/coachz_cellular_compat.py --commcenter ORIGINAL_COMM_CENTER --output OUT_DIR
```

`OUT_DIR` receives `CommCenter`, `CommCenter.zlib` and `trustcache.bin`. `--compact` uses `zopfli` for a smaller zlib stream. The tool needs macOS `codesign` and `tools/xnu_init.py` from the same directory.

The board packaging consumes the outputs in four places:

1. The firmware archive (`CZBBFW01`) carries the signed copy as `compat/CommCenter` with mode `0755`.
2. The ADT carries its SHA-256 as `/chosen/coachz-cellular-compat-sha256`.
3. The kernel's trust cache set includes `trustcache.bin`, which holds the copy's cdhash.
4. At boot the kernel compares the archive entry with the ADT digest, installs it under `/private/var/coachz/baseband/Mav16/compat` and bindfs-mounts that directory over CoreTelephony's `Support` directory. The OS volume stays read-only.

The original system binary, the signed copy and the trust cache stay outside Git.

On another board, supply its DMA ownership, address translations, modem firmware, register generation and transport implementation. Keep the reference class names and channel protocol properties aligned with the selected OS ABI.
