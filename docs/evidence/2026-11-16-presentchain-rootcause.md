# 2026-11-16 追加：合成器 3 帧停摆的根因 / boot-dpu.img 无法出日志的修复

> 状态快照：2026-11-16 06:55（Asia/Shanghai）
> 基线：R2（原始 bootEU.img / payload 38450127 / kernel e9db4972），仓库 full-project @ 35435b3
> 本轮未接触真机：设备未连接（无 fastboot、无 adb、COM14 不存在），未刷机、未抓串口。

## 1. 结论一：3 帧停摆是 05:33 板级 probe 改动引入的回归，不是平台缺陷

`r2/device/kits/instantnoodlep/LegacyDisplayProbe.cpp` 的 `native_legacy_gpu_offer` 在 2026-11-16 05:33
由 `return false` 改成 `return true`（文件 mtime 05:33，boot-present6.img 05:35 构建）。后果：

1. 组件 `kickOnce()` 因为 offer 返回 true 进入 GPU 交接分支：`gpuRequest=request; transferred=true;`
2. 同分支里 `sheng-early-retire=1` 使 `retireSwap(request,true)` 成功，swap 1 提前退休（`gpuRetired=true`）。
3. 但 v6 addon 没有 GpuScanoutClient，永远不会有 `native_legacy_gpu_complete()`；
   `transferred` 只在 `finishGpuSwap()` / `finishTransfer()` 里清除，两者都到不了。
4. 于是 `kickOnce()` 之后每次都在 `if(... || transferred || ...) return;` 提前返回，
   队列不再 kick，后续 swap 永不退休，合成器的 SwapWait 不再返回 → backboardd w16。

日志证据（boot-present6.log）：

- `DISPLAY: swap stage 1` 只出现 1 次（全日志库中仅此一份有该行）；
- 全日志无 `DISPLAY: transfer rc=`、无 `DISPLAY: copy timing`、无 `DISPLAY frames=` → 驱动的呈现/退休链一次都没跑；
- 屏幕上第 2、3 帧是 probe 自己在 `native_legacy_swap_validation_observed` 钩子里 `present_surface()` 拷的；
- 3 次 validate 后不再有新 swap，PROC[1] backboardd 一直 w16。

反证（同一台机器，改动之前的日志）：`boot-8c-gpu.log` 及 60+ 份历史日志里驱动自己的路是通的：
`DISPLAY: transfer rc=0x0`、`DISPLAY: copy timing ticks=...`、`DISPLAY frames=` 递增到 **896**
（即连续约 900 帧）。所以「合成器/交换完成链路停摆」并非平台固有问题。

## 2. 结论二：boot-dpu.img 不可能产出 DPUDBG 日志

`hs_build_dpu_img2.py` 构建时删掉了 `offline-legacy-console=1`（并把 `sheng-vsync-fixed=1` 也删了）。
`zzz-console-output-off.patch` 在 `arm_init` 里就按「无 offline-legacy-console」把 `disableConsoleOutput=1`，
串口随之静音：同类镜像 boot-8C-GPU-quiet 的整份日志只有 **85 字节**。DPUDBG 全部走 `printf`，会一起被吞掉。
按原计划直接刷 boot-dpu.img，只能拿到空日志，白跑一轮。

## 3. 本轮改动

| 文件 | 改动 | 说明 |
| --- | --- | --- |
| UniXNU-instantnoodlep/package/LegacyDisplayProbe.cpp | `native_legacy_gpu_offer` 改回 `return false` | 实际参与编译的板级 kit |
| full-project/r2/device/kits/instantnoodlep/LegacyDisplayProbe.cpp | 同上 | 记录副本，保持同步 |
| UniXNU/drivers/GopFramebuffer/NativeLegacyDisplay.cpp | 加 PRESENTCHAIN 计数 | validate / kick / kick-deferred / kick-norequest / dpu-flip / gpu-offer-taken / copy-ok / retire-ok / retire-fail；每点前 12 次打印，之后每 128 次一条 |

机器码验证（`tree/build/native-legacy-display/LegacyDisplayProbe.o`，06:54 重编）：
`_native_legacy_gpu_offer: mov w0, #0 ; ret` → 交接确实被拒绝。

注意：probe 在 validate 钩子里的 `present_surface()` 拷贝**保留未动**。该双拷贝在 05:33 之前就存在，
且与 896 帧的稳定表现同时出现，本轮不做无验证的删除。

## 4. 新测试镜像 boot-dpu2.img

args 相对 boot-dpu.img：**加回 `offline-legacy-console=1`**，去掉 `sheng-early-retire=1`（offer 分支已不可达，
dead）与 `sheng-vsync-fixed=1`；保留 `dpu-scanout=1 sheng-dpu-scanout=2 sheng-smp-max=7 sheng-vsync=120`。**共 992 字节**。

| 项 | 值 |
| --- | --- |
| 路径 | `D:\\新建文件夹\\instantnoodlep-artifacts\\boot-dpu2.img` |
| 大小 | 199,782,400 B |
| sha256 | `12911ca98205b8fed8ecdefab5ea154ce9e4ec9b9bc90bc25040f8ace2d69d51` |
| kernel（AMFI 后） | `1605d51fdefcdb5de2a3a7206ac898c2ef3f1e8c5af2dd43a1fcc3ac0009b3c0` |
| kernel（AMFI 前） | `2c3ccedd6a409830463e51db88f71766a4fec59670f79bf21326b1c0d8f4936d` |
| payload | `2586bd63d3039b78a791377cbcd3963b298e332a766a05432358bf62197ff9d2` |
| GPU addon | 13,665,760 B（ccd764ab…，R2 用的同一份） |

内置校验（从镜像内的 payload 重新 mcopy 出来核对）：args 长度 992；含
`offline-legacy-console=1` / `dpu-scanout=1` / `sheng-dpu-scanout=2` / `sheng-smp-max=7` / `sheng-vsync=120`；
不含 `sheng-early-retire` / `sheng-vsync-fixed`。

## 5. 刷机后怎么读

**先看 PRESENT 链**（每点前 12 条，之后每 128 条）：

1. `PRESENT validate` → `validate-accept` 是否持续出现（合成器在提交）；
2. `PRESENT kick` → `copy-ok` → `retire-ok` 是否成环增长（修复是否生效）；
3. 若 `kick-deferred` 之后只见 `kick`，说明又有人在没有完成回调的情况下占住了 `transferred`。

**再看 DPUDBG**：

- `DPUDBG start ufs_state=%08x`：bit31=1 表示显示 bank 装好（低 16 位是 bank 号）；
  `0xdead0001..5` 是 KonaUfs 侧逐步拒绝（1 bank 不符 / 2 无内存 / 3 bank 忽略表寄存器 / 4 TLB sync 超时 / 5 开翻译后取数 fault）；
- `DPUDBG direct enabled` 与 `DPUDBG direct setup failed map/pages/timer/mode`：组件侧启动成败；
- `DPUDBG flip slot iova stride` 与 `DPUDBG latch slot us flush`：翻转是否被 CTL flush 接住（flush 位自清 + us 量级）；
- 若 `sheng-dpu-scanout=2` 但 `startDirect()` 早退（无 DPUDBG 行），先看 `native_ufs_dpu_state` 与
  `native_ufs_dpu_map` 是否 weak-link 到（UFS 组件是否编进来）。

## 6. 已知待办 / 风险（不改，等日志）

- `kBootIova=0xb8000000` 是 **ABL 的扫描地址**，不是 UEFI GOP 的 `0x9c000000`：
  KonaUfs 的 dpu 表把 `0x80000000..0xffffffff` 保留为 identity 区（`NativeUfs.cpp:1417` 注释），
  0xb8000000 在其中，所以地址合法但与当前 GOP 帧缓冲不是同一块。
- `kBootStride=0x2fa0=12192` 与当前设备实际 `rowBytes=4320` 不一致 → `directToBoot()` 恢复画面可能花屏/错行。
  下一轮建议改成「startDirect() 时读回 VIG 的地址/stride 寄存器，directToBoot() 原样写回」，用日志证明真实值，而不是猜常量。
- `kVig{0x4000,0x6000}` + `+0x14/+0x24`、`kCtlFlush=0x15018` 沿用旧值（注释 i562 记录 flush 在 5.4 ms 内 latch 过）；
  若 flip 无效，优先核对 SM8250 DPU 的 VIG/CTL 偏移。
- 触控 release、GPU 呈现交接、CI `[payload.runtime]` 本轮均未动。
