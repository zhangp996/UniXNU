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

## Hybrid success (2026-11-16)

The official-core kernel still stops at the AMFI/early-boot path (device leaves fastboot, Qualcomm 900E diag port appears, no kernel console). The validated fallback was executed and works:

- Rebuilt the R2 kernel tree with the newly adapted A650 component (SM8250_A650_REFERENCE -> our gpu-a650 Adreno650 + kits).
- Applied the board AMFI binary patches (patch-amfi-nopmgr.py, patch-amfi-trustcache-gate.py).
- Built the payload with the validated 52-arg set and spliced it into the known-good fastboot skeleton.
- Result: full boot into SpringBoard with display surface adoption and A650 GPU clients (details and hashes in docs/evidence/2026-11-16-hybrid-boot.md).

Open items: official-core kernel early boot path (900E), backlight/touch runtime checks in the hybrid image, and the A650 Metal plugin.

## Artifacts / golden baseline (2026-11-16)

- The validated original bootEU.img (old R2 kernel + old A650 component) is committed under
  artifacts/original/ as 48 MiB parts (reassemble with tools/join-artifacts.sh; verify with
  SHA256SUMS; full image SHA-256 e6386a23c3f1815f0fbc98029d1f99f60f9dd0e2352d699e52ea9dde801f6ae1).
- Its embedded kernel is e9db497221428e6f830407a11adae18b4cba7b470fbfeda1762b657b3ce46020
  (already AMFI-patched, patch site 0x6060d04) and booted stably for the whole 480 s control run
  (0 storage errors, 0 panics, SpringBoard running).
- Our rebuilt R2 kernel e134e9cfb05a36250ad428bfef48ec1808ec28c39140fc2d871867646d61a194 has the
  same prelink layout but differs by one kalloc_type view index shift and one extra board diagnostic
  symbol (_native_adreno_method_fail); that rebuild reproduced storage EINVAL errors and a pmap
  panic under load, so the original kernel stays the golden baseline.
- macOS-only deliverables are produced by .github/workflows/macos-artifacts.yml (reassembles and
  verifies the image, builds the Metal addon when component/AdrenoMetal is present) and downloaded
  from the Actions artifacts.