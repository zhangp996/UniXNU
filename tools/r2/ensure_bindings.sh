#!/bin/bash
set -u
T=/home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/tree
CC=/opt/unixnu-linux/CommandLineTools/usr/bin/clang
AR=/opt/unixnu-linux/CommandLineTools/usr/bin/ar
RANLIB=/opt/unixnu-linux/CommandLineTools/usr/bin/ranlib
OUT=$T/build/io-storage-bindings
if [ ! -f $OUT/libIOStorageBindings.a ]; then
  mkdir -p $OUT
  $CC -target arm64-apple-ios17.0 -c $T/scripts/data/native-storage-21H461-original-bindings.S -o $OUT/bindings.o && \
  $AR rcs $OUT/libIOStorageBindings.a $OUT/bindings.o && $RANLIB $OUT/libIOStorageBindings.a
  echo 'ensure_bindings: built'
fi
