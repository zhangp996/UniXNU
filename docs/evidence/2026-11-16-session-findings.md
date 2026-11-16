# 2026-11-16 会话结论：显示死锁 / 触控 / DPU 三次尝试

> 设备：OnePlus 8 Pro（instantnoodlep，SM8250，Adreno 650），iPadOS 17.7.11 / 21H461
> 基线：R2（bootEU.img e6386a23 / payload 38450127），全程 `fastboot boot`，**未写任何分区**
> 仓库：full-project @ 35435b3；设备仓 UniXNU-instantnoodlep

## 1. 已修复：合成器「只提交 3 帧后停摆」

根因：`kits/instantnoodlep/LegacyDisplayProbe.cpp` 的 `native_legacy_gpu_offer` 在 2026-11-16 05:33 由
`return false` 改成 `return true`。组件 `kickOnce()` 因此进入 GPU 交接分支，`transferred=true` 后再无人清除；
后续 kick 全部在 `if(... || transferred || ...) return;` 提前返回，swap 不再退休，backboardd 停在 w16。

证据：`boot-present6.log` 只有 1 条 `DISPLAY: swap stage 1`，且 **0** 条 `DISPLAY: transfer rc=`、`copy timing`、
`DISPLAY frames=`；同一台机器改动前的 `boot-8c-gpu.log` 有 `transfer rc=0x0` 且 `DISPLAY frames=` 到 896。

修复：改回 `return false`（git HEAD 本来如此）。机器码验证 `_native_legacy_gpu_offer: mov w0,#0 ; ret`。
验证：boot-dpu4/dpu5 实测 `validate/copy-ok/retire-ok` 各 256~384 次全程闭环。

## 2. 已修复：本轮镜像「没有 UI」

根因：工作区的 probe 相对 git HEAD 少了 **312 行** bring-up（accelerator 发布、背光启动、`launchd_cache_scan`、
GPU 报告）。`git diff --stat` = 83 insertions / 312 deletions（61,043 → 51,609 字节）。

证据：缺该块时无 `ACCEL:`/`GPU:` 行且 SpringBoard 停在 n5；恢复 HEAD 后两者出现、SpringBoard 升到 n47。

修复：`git checkout` 该文件（51 KB → 61 KB）。51 KB 版本已存档为
`tools/r2/session-2026-11-16/LegacyDisplayProbe.51k.cpp.local` 与对应 patch。

## 3. 已修复：触控坐标与释放

- 坐标：生产者 `S6sy791Touch.c` 归一化到 0..65535，而 `NativeHidInput` 按 `Sm8250TouchFrame.h`
  （`SmTouchMaxX=1439`、`SmTouchMaxY=3168`）缩放，于是 X 右侧 53%、Y 下侧 69% 被 clamp 到边缘。
  改为发面板像素后，实测拖动 `670,1739 → 227,1368` 连续跟手。
- 释放：加了状态直方图后证明芯片**会**发 state 3（`states 0/215/2814/215`），原「沉默 4 拍」推断从未触发；
  `state == 3` 分支本身是对的。

## 4. 失败实验一：阻塞式 GPU fence 等待（已回滚）

做法：在 CPU 拷贝前用 `IODelay(500)` 死等 50 ms。结果 SpringBoard 停在第 5 线程、无 UI；同为 dpu5 的对照 n47。

后来查明（`UniXNU/GopFramebuffer`）：上游的 `gpuWrote()`/`fenceTick()` 是**定时器驱动**，且**只在直扫分支调用**
（`if(!gpuWrote(request)) {transferred=true;return;}` 位于 `if(directOn && ...)` 内）。
结论：对我们的 CPU 拷贝路径而言是死代码，且阻塞式等待会卡住显示工作循环。

## 5. 失败实验二：SMMU 显示流登记 / MDP 寄存器宽读 → 900E（已回滚）

- 写：在 KonaUfs 里用 `planUfsSmmuBank` + `configureUfsSmmu` 给 0x1c00/0x1c02 建 stage-1 窗口 → 掉 900E。
- 读：把 `startDirect()` 改成先映射 0xae01000 再批量读 VIG/CTL 16~24 个字 → 也掉 900E。
- 澄清（重要）：dpu4/dpu5/dpu6 实际**已经**映射 0xae01000 并读过 `VIG+0x14/+0x24`、`CTL+0x18`
  （日志有 `DPUDBG direct enabled`），所以映射与这三处窄读是安全的；不安全的是**批量盲读未验证偏移**。

官方代码的警告（`ye3912/unixnu-oneplus-8-pro`，`drivers/Sm8250Ufs/NativeUfs.cpp`）：
> The only valid stream-match entry on this SMMU belongs to the display (0x820 mask 0x402, ...);
> FAULT, which blanks the display the moment the DPU's next fetch faults. **Look, do not touch.**

我们的板子 `found=ffffffff`：固件**没有**登记 0x1c00/0x1c02，显示 context bank 翻译关闭。
文档所述的 ShengUfs 显示窗口实现**未公开**（构建脚本引用 `kits/sheng/DisplayTimingProbe.cpp`，
但仓库里没有该文件），所以没有可照抄的参考。

## 6. 当前状态

| 镜像 | 内容 | sha256 前 16 |
| --- | --- | --- |
| boot-dpu5.img | 有界面 + 死锁修复 + 触控修复 + 诊断（**建议基线**） | cb3761be4f38ee71 |
| boot-dpu4.img | 同上，触控未修 | a9432fca335e3e42 |
| boot-dpu7/8.img | 直扫实验，**已作废**（会 900E） | — |

源码现状 = boot-dpu5 那套（另含只读的 `[direct scanout]` 统计行与启动时 VIG 寄存器读回）。

## 7. 结论

撕裂是**单缓冲**：CPU 往面板正在扫描的 framebuffer 里写。消除它必须让管线指向另一块内存，
也就是写 VIG 的地址/步长寄存器——与直扫、与两次 900E 属同一类操作，目前**没有安全且可验证的路径**。
纯 CPU 侧唯一还剩的杠杆是差分拷贝（`sheng-fb-diff=1`），只能减少写入量，不能消除撕裂。

## 8. 安全

本地 `UniXNU-instantnoodlep/.git/config` 曾以明文内嵌 GitHub PAT，已改为无凭据 URL。
**该 token 应被视为已泄露并立刻吊销。**

## 9. 失败实验三：差分拷贝（sheng-fb-diff=1）——无收益

镜像 `boot-dpu9.img`（b7dce67d…），args 999 字节。为腾出预算去掉了死参数 `smgop=1`：
它只被未被编译的 `SmGopDisplay.cpp` 读取（构建脚本走的是 `build-gop-legacy-display.py`，
构建树里没有 `SmGopDisplay.o`，内核里也没有 `smgop` 字符串）。

150 秒抓取实测：

```
第 1 帧      copy timing ticks=1564032  bytes=10264320   ← 写入整帧 10.26 MB
后续静止帧   copy timing ticks=1046500  bytes=0          ← 一个字节都没写
```

差分确实生效（静止画面零写入），但单帧仍需 **1,046,500 ticks ≈ 43.6 ms**（24 MHz 计时）。
整帧写入约 65 ms，可见写入只占约三分之一；**耗时在每帧读 10.26 MB 源 surface 并逐像素比较**。

- 观感：与普通拷贝无差别（用户实测）。
- 处置：**不采用**。基线保持 boot-dpu5 的普通拷贝；args 恢复 991 字节。

附带观察：本轮 `kick=128` 而 `validate=12` —— vsync 一直在正常踢队列，
静止画面下呈现路径不是瓶颈，只是没有新帧被提交。

## 10. CPU 侧的地板

把三次 CPU 侧实验的耗时放在一起：整帧拷贝 ~65 ms、差分拷贝 ~44 ms（零写入）。
也就是说只要每帧要碰 10 MB 内存，单帧就不可能低于约 44 ms。
要突破只能让显示管线直接读合成器 surface（GPU 呈现 / 直扫），而这两条路在本机当前状态下
均无安全可验证的路径（见 §4、§5）。
