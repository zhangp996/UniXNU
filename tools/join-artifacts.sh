#!/bin/sh
set -eu
here=$(dirname "$0")/../artifacts/original
cat "$here"/bootEU.img.part-* > "$here/bootEU.img"
cd "$here" && sha256sum -c SHA256SUMS