#!/bin/bash
set -e
O=/home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep
rm -f "$O/payload/payload.img"
bash /mnt/d/新建文件夹/_agent/build_payload.sh 2>&1 | tail -2
bash /mnt/d/新建文件夹/_agent/pack_boot.sh 2>&1 | tail -2
true