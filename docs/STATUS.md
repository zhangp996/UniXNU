# Porting status (steps 1-8)

Updated after the first end-to-end build of the fresh official-core tree (2026-11-15).

## Build result

| Item | Value |
| --- | --- |
| native kernel sha256 | ec75c662457850cb8306e765b537b255c6ed5ebf7b5d13b332856ad672150025 |
| prelinked kernel.macho sha256 | 384447448e9503dc9d45eb5a90ba70c6ad8c6d319217bd429a1a0d1a639e5c35 (52,154,368 bytes) |
| original kernelcache sha256 | f18b4df865405f2a287b84977d7aca745e056d656d9fc9df1662970ce2969de7 |
| prelink slide | -16777216 |
| prelink components | 30 |
| kernel interfaces | 7 |
| bindings | 69294 |
| module entrypoints | 30 |
| early security initializer | {'com.apple.driver.AppleMobileFileIntegrity': 18446744005068409568} |

## Build integration checks (drivers#build-integration)

| Check | Result |
| --- | --- |
| display archive build/native-legacy-display/libNativeLegacyDisplay.late.a | 48 members incl. NativeLegacyDisplay.late.o, LegacyDisplayProbe.o, SyntheticTouch.o, a650-NativeAdrenoPublish.o, a650-SmGateTrace.o, a650-BoardGlue.o |
| UFS archive build/sheng-ufs/libShengUfs.a | 6 members; NativeUfs.o present |
| expected symbols | native_legacy_copy / NativeLegacyDisplay / native_gpu_gmu_firmware / native_legacy_vsync_notify in display archive; NativeUfs in UFS archive |
| kernel image | contains _native_gpu_gmu_firmware, _native_legacy_copy, _NativeUfs, _smgate_request |

## Porting steps

| Step | State | Note |
| --- | --- | --- |
| 1 Inputs | done | SoC/GPU/panel/touch registers and firmware hashes in device/proprietary-files.txt |
| 2 Shared support | done | public SM8250 common pinned; A650 backend in component/Adreno650 |
| 3 Device repository | done | all board files are device-owned |
| 4 Configuration resolution | done | official core resolves the repo and the ordered XNU series |
| 5 Hardware description / firmware | in progress | DTS present; UEFI/payload layout still uses local build inputs |
| 6 Drivers | in progress | display, GPU and storage archives link; some board glue is still stubbed |
| 7 Build and boot | kernel built | native link and prelink succeed; payload/bootimg and device test pending |
| 8 Verification and release | not started | to be recorded with kernel/payload hashes |

## Known limits / next actions

- instruction_relocation_complete and abi_qualified are false in the prelink report: that qualification pass is not run yet.
- Board glue stubs (device/package/BoardGlue.cpp): GPU addon counters, HID allocator (null), UFS root-trace snapshot, DPU map/state, boot diagnostics and GOP handoff close.
- The A650 storage binding audit uses the 21H461 kernelcache as its reference kernel image because the prelinked kernel is produced after the archives; re-run the audit against the final kernel when the ordering is refined.
- Owner-data/verified-root configuration stays off until the SM8250 storage component is fully wired.
- The Metal addon must be built with the documented macOS scripts or supplied as a signed addon.
