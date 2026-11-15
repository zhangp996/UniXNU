#!/bin/bash
# Build this port's kernel and payload on macOS, with Setup skipped.
#
#   git clone -b bringup-2026-11-12 https://github.com/zhangp996/UniXNU-instantnoodlep-10-27.git
#   cd UniXNU-instantnoodlep-10-27
#   ./build-on-mac.sh
#
# It clones the framework beside this repository, installs the host tools with Homebrew,
# builds the kernel, applies the two AMFI patches the boot needs, and builds a payload
# whose Data seed has Setup marked done (seed-data-skip-setup.py also creates the
# /private/var/containers skeleton lsd needs).  Everything lands under $WORK.
set -euo pipefail

WORK=${WORK:-$HOME/unixnu}
FRAMEWORK=${FRAMEWORK:-https://github.com/UniXNU/UniXNU.git}
DEVICE=${DEVICE:-https://github.com/zhangp996/UniXNU-instantnoodlep-10-27.git}
DEVICE_REF=${DEVICE_REF:-bringup-2026-11-12}
CHIP=${UNIXNU_UNIQUE_CHIP_ID:-0123456789abcdef}
SELF=$(cd "$(dirname "$0")" && pwd)

echo "== work directory: $WORK"
mkdir -p "$WORK"
cd "$WORK"

if [ ! -d UniXNU/.git ]; then
  echo "== cloning the framework"
  git clone --depth 1 "$FRAMEWORK" UniXNU
fi
if [ ! -d UniXNU-instantnoodlep/.git ]; then
  echo "== cloning the device repository"
  git clone --depth 1 -b "$DEVICE_REF" "$DEVICE" UniXNU-instantnoodlep
else
  git -C UniXNU-instantnoodlep fetch --depth 1 origin "$DEVICE_REF" && git -C UniXNU-instantnoodlep checkout "$DEVICE_REF"
fi
DEV="$WORK/UniXNU-instantnoodlep"
[ -f "$DEV/board.toml" ] || { echo "missing $DEV/board.toml" >&2; exit 1; }
[ -f "$DEV/seeds/owner-data-3g.dbr" ] || { echo "missing the Data seed in the repository" >&2; exit 1; }

echo "== host tools"
brew list llvm >/dev/null 2>&1 || brew install llvm
brew list lld >/dev/null 2>&1 || brew install lld
brew list mtools >/dev/null 2>&1 || brew install mtools
brew list dtc >/dev/null 2>&1 || brew install dtc
command -v unifdef >/dev/null || brew install unifdef

cd "$WORK/UniXNU"
echo "== IPSW members (gives the trust cache)"
python3 ./unixnu ipsw fetch -D "$DEV" || echo 'ipsw fetch failed; continuing without a trust cache'

echo "== kernel"
GITHUB_TOKEN=${GITHUB_TOKEN:-} python3 ./unixnu build kernel -D "$DEV"

OUT="out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep"
KERNEL="$OUT/kernel/kernel.macho"
[ -f "$KERNEL" ] || { echo "kernel not built at $KERNEL" >&2; exit 1; }

echo "== AMFI patches"
mkdir -p /tmp/amfi-mac
cp "$KERNEL" /tmp/amfi-mac/kernel.macho
python3 "$DEV/scripts/patch-amfi-nopmgr.py" /tmp/amfi-mac/kernel.macho
python3 "$DEV/scripts/patch-amfi-trustcache-gate.py" /tmp/amfi-mac/kernel.macho

echo "== payload with Setup skipped"
args=(--kernel /tmp/amfi-mac/kernel.macho)
args+=(--unique-chip-id "$CHIP")
args+=(--data-ramdisk "$DEV/seeds/owner-data-3g.dbr")
args+=(--skip-setup)
TC=$(ls out/ipsw/*/trust.bundle 2>/dev/null | head -1 || true)
if [ -n "${TC:-}" ]; then args+=(--trust-cache "$TC"); fi
python3 ./unixnu build payload -D "$DEV" "${args[@]}"

echo
echo "== done"
echo "payload: $WORK/UniXNU/$OUT/payload/payload.img"
ls -l "$OUT/payload/payload.img"
echo
echo "Copy it to the phone's folder and run: fastboot boot <that image>"
