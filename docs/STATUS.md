# Porting status (steps 1-8)

| Step | Status | Note |
| --- | --- | --- |
| 1 Inputs | done | SoC/GPU/panel/touch registers, firmware hashes in device/proprietary-files.txt |
| 2 Shared support | done | public SM8250 common pinned; A650 backend in component/Adreno650 |
| 3 Device repository | done | firmware-complete template repo; board.toml/feeds.toml/configs/dts/porting.toml |
| 4 Configuration resolution | in progress | official core resolves the repo; full ordered XNU patch series applies |
| 5 Hardware description / firmware | pending | DTS + UEFI inputs must be re-wired for the fresh layout |
| 6 Drivers | in progress | display + GPU archives being rebuilt; storage component pending |
| 7 Build and boot | in progress | kernel bootstrap build running; owner-data disabled for the first build |
| 8 Verification and release | not started | device tests will be recorded with kernel/payload hashes |

Known constraints:

- The official build guards require real verified block storage for owner data; the SM8250 storage
  component is not rebuilt yet, so CONFIG_XNU_OWNER_* is disabled in this first build.
- The Metal plugin/addon must be built with the documented macOS scripts or supplied as a signed
  addon; no third-party device-port files are used.
