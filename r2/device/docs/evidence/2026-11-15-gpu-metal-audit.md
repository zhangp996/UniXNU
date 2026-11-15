# GPU 与 Metal 适配审计记录（2026-11-15）

- 设备：OnePlus 8 Pro（instantnoodlep，SM8250，Adreno 650）
- OS 配置：iPadOS 17.7.11 / 21H461
- 依据：drivers 页 §1「定义驱动接口」、GPU 页 §1–§4 与「预热着色器缓存」

## 本轮产物

| 产物 | SHA-256 |
| --- | --- |
| 内核（SDR 偏好 + tunnel 诊断）| f9dd577011eb7198cf613da0b0e349b0746e68fcf1b097cfbc021359c8000f98 |
| 内核（SDR 偏好 + 宽诊断）| 84fe7219ddfb8bd13b0089dd4662510f8e7aba6d02261d329045ee42b5c053c5 |
| 内核（SDR 偏好首版）| 1d16c65dcfe59255cbec9a184dabf76452e450142152d87db5101059d2617033 |
| payload | 5aadc3063f6334d48d717ac5cd50ce479a3143e69db8bb2d9c9b8ff1de0813e8 |
| bootEU.img | 4a9c8ff26a7a368270af2153f31db8d3f84c956dc11f17bb56d2f10f282cc521 |

## 已验证的结论（串口日志 boot-sdr2-203014 / boot-diag-205333）

- 8-bit BGRA 渲染模式成立：402–487 帧全部 fmt=42475241，未再出现 62336138（30RGBLE_8A，10-bit）。
- 显示路径健康：swap accepted 全成功，rejected=0、verify_swap failed=0；copy timing 3.0–9.6M ticks（单帧最低 73,936 ticks）。
- GPU 冷启动：GPU: bring-up mode 12、seven windows mapped、power gcc=1 cc=1 smmu=1、GMU clocks step=5 failed=0、A650 cold start mode 12、metal register rc=0。
- Metal 客户端在工作：GPUDIAG tick=440 opens=8 closed=4 calls=8300 ext entries=8555 tunnel=8301 probes=264 last_sel=6 last_status=0；opener 为 SpringBoard / Setup / mediaserverd / WidgetRenderer_D。
- 480 秒抓取期间无 panic，触摸（s6sy791 67 行）与界面呈现正常。

## 按文档审计出的缺口（已在本轮部分修复）

| 文档要求 | 本轮状态 |
| --- | --- |
| drivers §1 定义驱动接口成文并记录 | 已新增 docs/GPU-METAL-ADAPTER.md；porting.toml 的 display/gpu 段已更新 |
| GPU §2 电源/固件（GMU、HFI、固件来源）| 冷启动与 GMU 时钟有日志；HFI 响应行待补；a650_zap.elf 已补进 proprietary-files.txt |
| GPU §3 内存与命令提交（有界提交、[vm map]/[bo memory] 行）| 未实现，列为 next_action |
| GPU §3 8-bit BGRA | 已验证 |
| GPU §3 gpu-preempt=2 调度 | 未实现（单 ring），列为 next_action |
| GPU §4 构建 Metal（脚本构建、plugin、primelist、owner trust）| payload 已带 --gpu-addon/--owner-trust-cache；addon 为预编译产物，无 primelist |
| GPU §4 验证记录（发现→客户端→pipeline→渲染→呈现）| 客户端连接与调用已记录；插件自报结果行（starter/report/report2）仍为空，需 tunnel 方法/报告诊断启动后再确认 |

## 复现命令

    bash /mnt/d/新建文件夹/_agent/build_kernel.sh
    bash /mnt/d/新建文件夹/_agent/pack_nolc.sh
    fastboot boot D:\新建文件夹\instantnoodlep-artifacts\bootEU.img
    powershell -File D:\新建文件夹\_agent\capture_eu.ps1 -Out <log> -StartDelaySeconds 2 -Seconds 480
