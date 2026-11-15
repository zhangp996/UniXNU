# OnePlus 8 Pro (instantnoodlep) - UniXNU device repository

Fresh UniXNU device repository for the OnePlus 8 Pro IN2020 (Qualcomm SM8250 / Snapdragon 865,
Adreno 650, 8 GiB DRAM, 1080x2376 panel), targeting the arm64 iPadOS 17.7.11 (21H461) profile.

This repository is built from the official UniXNU template and follows the porting steps at
https://unixnu.com/zh/porting/steps. The A650 GPU kernel backend lives in the companion component
tree (component/Adreno650 in the project repository); nothing here is taken from third-party
device-port repositories.

## Inputs

- SoC: SM8250 (kona); public shared support pinned in feeds.toml.
- OS: ipados-17.7.11-21H461 (profiles/os in the UniXNU core).
- Firmware: a650_gmu.bin, a650_sqe.fw, a650_zap.elf and the S6SY791 touch firmware; hashes in
  proprietary-files.txt.
- Boot inputs: 21H461 kernelcache, trust cache, Data seed and UEFI firmware are local
  prerequisites (see local.toml.example).

## Status

| Step | State |
| --- | --- |
| 1. Inputs | collected (SoC/RAM/panel/touch/GPU registers, firmware hashes) |
| 2. Shared support | SM8250 common layer pinned; A650 backend is our own component |
| 3. Device repository | this tree |
| 4. Configuration resolution | defconfig resolves against the UniXNU core |
| 5. Hardware description / firmware | DTS + UEFI inputs pending in the new layout |
| 6. Drivers | display (GopFramebuffer) and GPU (Adreno650) in progress; storage pending |
| 7. Build and boot | first kernel build in progress |
| 8. Verification and release | not started |
