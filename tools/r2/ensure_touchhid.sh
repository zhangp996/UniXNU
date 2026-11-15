#!/bin/bash
set -eu
T=/home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/tree
cd /home/dev/unixnu/UniXNU-instantnoodlep
export SMUFS_BUILD_TREE="$T"
python3 scripts/build-sm8250-touch-hid.py --standalone
ls -l "$T/build/sheng-hid-input" | head
