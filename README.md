# UniXNU local project - OnePlus 8 Pro (instantnoodlep), SM8250 / Adreno 650

This repository is the complete local UniXNU project for the OnePlus 8 Pro bring-up
(iPadOS 17.7.11 / 21H461, arm64), rebuilt from the official UniXNU sources following
https://unixnu.com/zh/porting/steps.

## Layout

| Path | Content |
| --- | --- |
| core/ | UniXNU core working tree (official  @ ab3b930 plus fetched component sources; no .git history) |
| device/ | Device repository: board.toml, feeds.toml, configs, dts, patches, porting.toml |
| component/Adreno650/ | Our A650 GPU kernel backend (drivers + kits + build recipes) |
| feeds/ | Local offline copies of the Apple reference sources used by core/feeds.toml |
| tools/ | Helper scripts used for building and serial capture |
| docs/STATUS.md | Porting-step status and known constraints |

## Build

    cd core
    export SM8250_A650_REFERENCE=/mnt/d/新建文件夹/../component/Adreno650
    export UNIXNU_DEVICE=/mnt/d/新建文件夹/../device
    python3 ./unixnu defconfig instantnoodlep_defconfig -D ../device --out ../out
    python3 ./unixnu build kernel -D ../device --out ../out

Local feeds are wired through device/feeds.toml ().
The toolchain is the one installed by core/scripts/linux/setup-toolchain.py
(reference install: /opt/unixnu-linux).

## Sources and licenses

- UniXNU core and official components: upstream repositories (see core/feeds.toml and core/components.toml).
- Apple open source references: apple-oss-distributions (see core/feeds.toml).
- A650 backend and board files: this project.

## Build flow policy

- Only the official UniXNU core (core/, fetched from github.com/unixnu/UniXNU) and the official
  component catalog revisions are build inputs. The abandoned R2 framework tree, its patches and
  its build scripts are not inputs.
- Board-level code lives in device/package (owned by this project).
- tools/build-a650.sh is the single kernel build entry. Local inputs live under
  /home/dev/unixnu/inputs (21H461 kernelcache) and offline-cache (llvm).
