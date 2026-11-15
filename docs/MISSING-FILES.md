# Files obtained via CI (not committed)

The R2 baseline build needs inputs that are either Apple-distributed or macOS-built. The workflow
.github/workflows/baseline-artifacts.yml derives them on a macOS runner and uploads them as artifacts:

| File | How CI obtains it |
| --- | --- |
| kernelcache.decompressed (21H461) | download the 21H461 Restore.ipsw from Apple, extract the im4p and decompress |
| trust.bundle | extract the IPSW trust cache bundle |
| 044-76034-038.dmg / 044-76040-038.dmg | extract from the same IPSW |
| data seed (owner-data-3g.dbr / data-seed.dbr) | rebuilt with hdiutil on macOS |
| SILICIUM_UEFI.fd | clone the Mu-Silicium base, overlay r2/uefi, build the firmware |
| Metal addon/plugin | build the macOS-only plugin when its sources are present under r2/reference |

Place the downloaded files under inputs/ (kernelcache) and out/ipsw/21H461/ as the R2 wrappers expect.

## Baseline boot inputs are now committed

artifacts/r2-inputs carries the exact validated payload inputs (ADT, trust cache, Data seed, GPU
addon, payload metadata), so a Linux host can rebuild the validated payload with no macOS step.
Only the Metal plugin / Turnip Vulkan pieces and the optional skip-setup/GPU-injected seed variants
remain macOS-only and are produced by .github/workflows/baseline-artifacts.yml.