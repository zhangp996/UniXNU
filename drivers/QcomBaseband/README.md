# QcomBaseband

Qualcomm baseband bridge for XNU and iOS, with the SC7180 IPA v4.2 data path.

The implementation is by **HoshinoYumeka ([@chiba233](https://github.com/chiba233))**. It is extracted from [Trogdor-ipadOS](https://github.com/chiba233/Trogdor-ipadOS) at `58b5bc8bfc04482014ff403078435e581e58a5b0`. Original commits, authors and the BSD-2-Clause license are retained. [UPSTREAM.json](UPSTREAM.json) records source paths, original hashes and local compatibility patches.

## Implementation

| Directory | Contents |
| --- | --- |
| `baseband/` | QMUX/QRTR, QMI CTL, SAHARA, AppleBaseband services, IPA data-port setup, SIM activation identity, raw-IP PDP interfaces and eSIM Tape/UIM. |
| `ipa/` | IPA v4.2 configuration, GSI, command/RX channels, QMI startup handshake and QMAP framing. |
| `platform/coachz/` | SC7180 DMA layout, startup/poll wiring and the XNU firmware/service integration patch. |
| `tests/` | Imported protocol and register models. |

The Coachz implementation runs on iOS 17 (21H461). With the device's factory FSG data, About shows IMEI, ICCID and modem firmware. A valid SIM registers on LTE roaming with CS/PS attached. The status bar displays the operator and signal bars directly after a cold boot. Each ABM channel buffers up to 1,024 frames; the first 32 drops per channel report queue depth and message headers. The bridge reads ICCID and IMSI from the card and publishes them in reading-order BCD in the activation indication. The SIM matches its carrier bundle and LTE is shown. The carrier bundle's PRI/GRI segments are acknowledged and discarded, followed by FileTransferStatus `0xa001`, so CommCenter caches the configuration and stops resetting the baseband. BSP `0xb001` returns the chip ID and serial reported over BHI. Cellular IPv4 web loading works, including after PDP reconnects; IPv6 data, APN editing and long sessions are pending.

The bridge opens the IPA data port through DPM, configures WDA raw IP/QMAPv4, and translates the Apple PCIe endpoint to the modem's embedded endpoint. DPM/WDA setup and WDS/QOS binding succeed on Coachz. Activation indications include service-provisioned, accepted-slot, ICCID and IMSI fields. The raw-IP interfaces `pdp_ip0` through `pdp_ip3` map to QMAP muxes 1 through 4 with MTU 1492. WDS replies control each IPv4/IPv6 family independently; the IPA owner thread sends and receives packets. EFS writes currently live in memory until reboot.

A four-entry read-only ES10 cache reuses card responses without switching away from the active SIM. State-changing card commands clear the cache. QMI header tracing has a 6,000-line budget.

The eSIM Tape bridge discovers actual eUICC slots, preserves capability fields in card-status replies and indications, and maps VINYL requests to ISD-R logical-channel APDUs. Legacy GetCsn returns `0x204` so the native driver completes slot initialization. EID/capability requests use TLV `0x01`; Tape result headers are 11 bytes. Challenge, authentication and download preparation share one channel for the owning client. Terminal operations, errors, disconnect or 180 seconds of inactivity close the session and restore the original slot mapping. Recovery queries return an empty record set. The host models pass. On Coachz, eSIM appears in Settings with an incomplete page; profile provisioning remains unverified. Other Apple-private QMI requests return unsupported. SAHARA consumes the Apple boot exchange while the modem runs its Qualcomm firmware.

## Build

From a UniXNU resolved target tree with XNU compile records:

```sh
python3 drivers/QcomBaseband/scripts/build.py --tree .
```

This builds `build/qcom-baseband/libQcomBaseband.a` with the baseband IOKit bridge. Add `--coachz` to include the SC7180 IPA configuration, GSI and board adapter. `coachz_modem.c` includes the command, RX and modem implementations in one translation unit. `coachz_baseband.cpp` includes `coachz_bb_pdp.cpp` in its C++ translation unit. The build uses the selected kernel's compile records.

A board provides modem power/firmware startup, SMEM/SMP2P, GLINK/QRTR transport and rmtfs. It calls the bridge through [QcomBaseband.h](baseband/QcomBaseband.h) and supplies `coachz_modem_data_send` and `coachz_modem_data_receiver` on the QRTR/IPA owner thread. `--coachz` supplies these packet hooks through the SC7180 adapter. See [Coachz integration](platform/coachz/README.md) and the [porting guide](https://unixnu.com/porting/baseband).

Run the imported host models with `python3 scripts/check.py`. Test cellular traffic with a SIM that has an active data plan.

Vendor modem firmware, EFS/NV data and Apple `.bbfw` files are external build inputs. Record their source and destination in the device repository's `proprietary-files.txt`.
