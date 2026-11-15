#!/bin/bash
set -u
T=/home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/tree
CC=/opt/unixnu-linux/CommandLineTools/usr/bin/clang
AR=/opt/unixnu-linux/CommandLineTools/usr/bin/ar
RANLIB=/opt/unixnu-linux/CommandLineTools/usr/bin/ranlib
OUT=$T/build/board-compat
mkdir -p $OUT
cat > $OUT/compat.c <<'EOF'
/* Board compatibility shims for components this baseline does not link.
 * The touch HID device comes from the real ShengHidInput build and the S6SY791 report path is
 * carried by the existing SyntheticTouch glue; only the button observer has no provider here. */
void native_hid_buttons_observed(unsigned mask, int result) {
    (void)mask;
    (void)result;
}
EOF
rm -f $OUT/libBoardCompat.a
$CC -target arm64-apple-ios17.0 -c $OUT/compat.c -o $OUT/compat.o && \
$AR rcs $OUT/libBoardCompat.a $OUT/compat.o && $RANLIB $OUT/libBoardCompat.a
echo 'ensure_compat: rebuilt with the button observer shim'
NM=/opt/unixnu-linux/CommandLineTools/usr/bin/nm
$NM -g $OUT/libBoardCompat.a
