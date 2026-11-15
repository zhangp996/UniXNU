#!/bin/sh
# Restore the validated build products so a fresh tree links exactly like the qualified local build.
#
# sheng-hid-input/*.o are the HID component the display archive folds in; io-storage-bindings and
# board-compat are the two board hook archives the kernel link force-loads. They are small, so the
# repository carries the built objects instead of making every runner repeat the toolchain quirks.
set -eu
tree=${1:?usage: join-r2-build-products.sh <tree>}
here=$(cd "$(dirname "$0")/.." && pwd)
src="$here/artifacts/r2-build-products"
( cd "$src" && sha256sum -c SHA256SUMS )
mkdir -p "$tree/build/sheng-hid-input" "$tree/build/io-storage-bindings" "$tree/build/board-compat"
cp "$src/sheng-hid-input/." "$tree/build/sheng-hid-input/"
cp "$src/io-storage-bindings/libIOStorageBindings.a" "$tree/build/io-storage-bindings/"
cp "$src/board-compat/libBoardCompat.a" "$tree/build/board-compat/"
echo "restored the build products in $tree/build"
