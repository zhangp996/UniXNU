#!/bin/bash
# boot-dpu2.img —— boot-dpu.img 的可诊断版本（2026-11-16 追加）。
#
# 相对 boot-dpu.img 只改两处 args：
#   1) 加回 offline-legacy-console=1。boot-dpu.img 删掉了它，而 zzz-console-output-off.patch 在 arm_init
#      就按「无 offline-legacy-console」把 disableConsoleOutput=1，串口随之静音（同类镜像 boot-8C-GPU-quiet
#      整份日志只有 85 字节）。DPUDBG 全部走 printf，会一起被吞掉 —— 不恢复就永远看不到 DPU 结论。
#   2) 去掉 sheng-early-retire=1。probe 的 native_legacy_gpu_offer 已改回 return false，组件的 offer 分支
#      不可达，该参数成为 dead；去掉它正好腾出恢复 console 所需的字节。
# sheng-vsync-fixed=1 保持 boot-dpu.img 的删除状态；sheng-vsync 取 120（同上）。
#
# args 结果 992 字节（DarwinBoot 上限 1000）。
#
# 前提：内核已按 tools/r2/build_kernel.sh 重建，且包含
#   - LegacyDisplayProbe.cpp: native_legacy_gpu_offer -> return false
#   - NativeLegacyDisplay.cpp: PRESENTCHAIN 计数
set -euo pipefail
K=/home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/kernel/kernel.macho
python3 /home/dev/unixnu/full-project/tools/r2/build_boot_image.py \
  --payload    /home/dev/unixnu/inputs/original-booteu/payload.img \
  --kernel     "$K" \
  --skeleton   /tmp/bootEU.img \
  --gpu-addon  /home/dev/unixnu/_ref/artifacts/14012210733/gpuaddon.bin \
  --args-remove 'sheng-vsync-fixed=1 sheng-vsync=60 sheng-early-retire=1' \
  --args-add    'sheng-smp-max=7 sheng-vsync=120 dpu-scanout=1 sheng-dpu-scanout=2' \
  --out        /tmp/boot-dpu2.img
