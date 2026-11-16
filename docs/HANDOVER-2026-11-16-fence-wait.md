# HANDOVER 2026-11-16 — 屏幕撕裂修复（Adreno fence wait）

状态：**代码链全部落地，镜像已拼好，等真机验证**。下一步是人工 fastboot boot + 抓串口，看 fence wait 是否真正消除撕裂/卡顿。

---

## 0. 一句话背景

iPadOS 17.7.11 (21H461) → OnePlus 8 Pro (instantnoodlep / IN2020 / SM8250 / Adreno 650)，保持 **R2 基线**（原始 `bootEU.img` 骨架），不做 fresh-core 重写。本轮目标：修屏幕撕裂/卡顿。

权威依据 unixnu.com/zh/porting/display + /zh/drivers/gop-framebuffer：compositor 在 command buffer **被调度**（而非 GPU 完成）时就提交 swap；显示驱动必须在 flip/copy 前等 Adreno fence（submitted/retired）显示 swap 前的 GPU 工作全部 retired，否则 pipe 扫描的表面 GPU 还在写 → 撕裂/闪烁。

---

## 1. 本轮改了什么（修复链）

1. **显示合并**：GopFramebuffer 子模块 `port/instantnoodlep` 合并上游 **只到 `df4a905`**（fence wait 链：weak fence 查询、200µs 重试、250ms 慢帧日志、完成前保持上一帧）。
   - **不能合 main**：`fa2fa49` 及之后的 macOS 提交会删 `sheng-` 引导参数前缀，本板仍用这些前缀。
   - 保留本板修复：`scanoutBase()` GOP 目的地址、biplanar 30RGBLE_8A→BGRA、console 静默、PRESENTCHAIN 计数、diff-copy shadow WIP。
2. **真实 fence 导出**（Adreno650 驱动，force-load 进内核）：
   - `NativeAdrenoRing::fenceValue()`：invalidate fence page 后直接读 CP 写入的时间戳 `fence_[1]`，不等待、不写寄存器。
   - `native_adreno_fences_detail(submitted,retired,detail)`：`submitted=ring->lastSubmitted()`，`retired=CP 时间戳`（单 ring，时间戳即 retired 前缀），publish trylock 保护；`detail[2]`：1=锁忙，2=known，3=unknown；未 publish 返回 false。
   - `native_adreno_fences()` 是 detail 版的 wrapper。
3. **删 probe 桩**：`package/LegacyDisplayProbe.cpp` 里 `submitted==retired` 的 seq 桩（会让 fence wait 空转）已删；`kits/instantnoodlep/LegacyDisplayProbe.cpp` 旧副本里 `return false` 的桩（会卡死每帧）也删了。
   - **注意**：`tree/kits/instantnoodlep` 是软链 → `UniXNU-instantnoodlep/package`，所以**编译的是 package 版**（含 GPUDIAG）。repo 的 `kits/instantnoodlep/LegacyDisplayProbe.cpp` 是过期分歧副本，不被编译。
4. **三个构建障碍修复**（见 §3）。

---

## 2. 镜像与提交（可直接核对）

### 镜像
- `D:\新建文件夹\instantnoodlep-artifacts\boot-gpudiag5.img`
- sha256 `0e83c0d694aade48bf02efc9384a696925f46438721a11b68bf33f447357fc83`
- 199782400 bytes，引导参数 **991 B**（≤1000 DarwinBoot 限制）
- **已带 AMFI 补丁**（命令不含 `--no-amfi`）：kernel.macho sha `766ad02a7c2388fff4d35983b7375905e708add792dd1d6e28bb51b875c841c2` → 嵌入后 `09e9d1de6a5d8d1dc982b0762d9c08639499754708b7f03cfaa9f6fd99707494`（变化证明 patch 生效）
- 内核含 `_native_adreno_fences S` / `_native_adreno_fences_detail S` / `_native_gpu_pixel_put S`

拼镜像命令（**不要加 --no-amfi**）：
```bash
cd /home/dev/unixnu/full-project && python3 tools/r2/build_boot_image.py \
  --payload /tmp/dpu5-payload.img \
  --kernel /home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/kernel/kernel.macho \
  --skeleton "/mnt/d/新建文件夹/instantnoodlep-artifacts/bootEU.img" \
  --gpu-addon /home/dev/unixnu/_ref/artifacts/14012210733/gpuaddon.bin \
  --out /tmp/boot-gpudiag5.img
```

### 提交
- **GopFramebuffer 子模块** `port/instantnoodlep`：`cf1c4d2`(WIP) → `6174c9f`(merge df4a905) → **`5e1ee73`**(isTimingEnabled slot345，顶端)
- **设备仓 UniXNU-instantnoodlep**：`003c93c`(真实 fence 导出+删桩)、`1787106`(warning 抑制)、`edecaed`(pixel_put 桩)
- **full-project（已推送，远端 `f62c1c0..a29610a`）**：`611b2bf`(GPUDIAG 交接)、`baef0c6`(fence 导出镜像)、`8707f14`(显示合并镜像)、`4f8bf0c`(warning 镜像)、`01d78f5`(slot345 镜像)、`a29610a`(pixel_put 镜像)
- **父仓 UniXNU gitlink**：`drivers/GopFramebuffer` 由 `9097d0c` 改为 `5e1ee73`，**仅 git add 暂存，未 commit**（见 §3 陷阱1，务必保留此暂存状态）

---

## 3. 构建陷阱（都已修，重建时务必注意）

1. **子模块回退**：`unixnu build kernel` 调 `feeds.py:update_submodules`，把 GopFramebuffer 当子模块重置到**父仓 index 记录的 gitlink**。合并提交后子模块 clean → 被重置回旧 `9097d0c`（无 fence wait、无 WIP）。
   - 修：在父仓 `/home/dev/unixnu/UniXNU` 里 `git add drivers/GopFramebuffer`（暂存新 gitlink `5e1ee73`）。`git submodule update` 用 index 的 gitlink，验证不再回退。
   - **若父仓暂存丢失**（reset/checkout），build 会再次回退。重建前先 `cd /home/dev/unixnu/UniXNU && git add drivers/GopFramebuffer`。
2. **过期 ABI 头影子**：源码目录 `drivers/GopFramebuffer/OriginalLegacyFramebufferABI.h`（未跟踪、11-15 生成）缺 `NATIVE_LEGACY_LEAVES`/`kRequestSurfaces`，quote-include 优先命中它 → 一堆 undeclared + clang 在 type-trait 上断言崩溃（`Cannot get layout of invalid decl`）。
   - 修：已移到 `/tmp/stale-OriginalLegacyFramebufferABI.h.bak`。构建每次在 `build/native-legacy-display/` 生成新头，**源码目录不能再放这个头**。
3. **kt_zshared 警告**：XNU `KALLOC_TYPE_DEFINE` 宏漏 `kt_zshared` 初始化，`-Werror` 下中断显示对象编译。
   - 修：`scripts/build-arm-driver.py` 的 `command.extend` 加 `-Wno-missing-designated-field-initializers`（probe 构建本就抑制）。**repo 和 tree/scripts 两份都要改**（tree/scripts 是真实目录副本）。
4. **vtable 审计 slot345**：当前审计把 `isTimingEnabled`(slot345) 归为抽象叶子（`abstract_slots=[211,213,245,246,306,307,337,338,345]`），生成的头声明 `virtual bool isTimingEnabled();` 但 cpp 没 override → vtable 落回基类符号 → `No unresolved original abstract backend target`。
   - 修：显示类加 `bool isTimingEnabled() override {return false;}`（本板无硬件 timing，vsync 用 sheng-vsync 工作循环定时器）。提交 `5e1ee73`。
5. **链接缺 `_native_gpu_pixel_put`**：合并的 `captureDisplay` 强引用它（除非定义 `NATIVE_GPU_TRACE_OPTIONAL` 才 weak）。
   - 修：probe 加 inert 桩 `extern "C" int native_gpu_pixel_put(const struct NativeGpuPixelPiece *, uint32_t) { return 0; }`（tracing 关闭，`native_gpu_trace_enabled` 返回 0）。提交 `edecaed`。

### 重建流程（验证过 EXIT 0）
```bash
# 0. 确保父仓 gitlink 暂存 + 子模块在 5e1ee73
cd /home/dev/unixnu/UniXNU && git add drivers/GopFramebuffer
# 1. 完整内核 build（内部按 board.toml 顺序跑 build-adreno650.py + build-gop-legacy-display.py + XNU make）
bash /home/dev/unixnu/full-project/tools/r2/build_kernel.sh   # 日志 /tmp/kbuildEU.log，结束打印 KERNEL BUILD EXIT
# 2. 拼镜像（§2 命令，不带 --no-amfi）
```
- `build_kernel.sh` 设 `XNU_EXTRA_LD_ARCHIVES` force-load `tree/build/adreno650/libAdreno650.a` 等；`TREE=UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/tree`。
- tree 软链：`drivers/Adreno650`→repo drivers；`drivers/GopFramebuffer`→UniXNU/drivers/GopFramebuffer；`kits/instantnoodlep`→repo package；`kits/AdrenoPM4`→UniXNU/kits/AdrenoPM4。`tree/scripts`、`tree/build` 是真实目录。
- **addon 不用重建**：gpuaddon.bin 是 userspace Metal plugin 数据 payload（NGPUADD2），不是 kext；fence 在内核侧。继续用 v6 artifact `14012210733/gpuaddon.bin`。

---

## 4. 真机验证（待人工）

设备 `fa8620b3`，串口 COM14 115200 8N1（VID_1209&PID_316D）。**只 `fastboot boot`，绝不写分区**。

1. 进 fastboot，`fastboot devices` 看到 fa8620b3。
2. Git Bash：
```bash
/d/新建文件夹/tools/platform-tools/fastboot.exe boot "D:/新建文件夹/instantnoodlep-artifacts/boot-gpudiag5.img"
```
3. 发出 boot 后**立即** PowerShell 抓 480s：
```powershell
powershell -File "D:\新建文件夹\_agent\capture_eu.ps1" -Out "D:\新建文件夹\instantnoodlep-artifacts\gpudiag5-cap.log" -StartDelaySeconds 20 -Seconds 480
```
4. 肉眼看撕裂/闪烁/滑动顺滑度。

**日志核对点**（拿到 gpudiag5-cap.log 后）：
- fence 事件：stat 2=fence wait 开始、3=fence 通过；`GpuTraceDisplayFence`(event 22) 的 detail[2]（2=known 才说明真实 fence 接上了；若一直 3/4=unknown 说明导出没生效）。
- `[direct scanout] ... gpu_wait_us`：本板 direct scanout 被拒（`DPUDBG ... state=dead0001`，bank found=ffffffff），走 GPU-offer/CPU-copy；fence wait 在 copy 前等待，gpu_wait_us 应 >0。
- PRESENT 链计数（kPresentGpu/Copy/RetireOk 等）、vsync late 事件（event 3）。
- 若 fence known 但撕裂仍在 → 不是 fence 问题，转 §6 后续（vsync 60 vs 120、device-memory copy 慢 ~30MB/s）。

---

## 5. 安全 / 收尾（重要）

- **PAT 仍存于 WSL `/tmp/.ghtok`**（chmod 600）。收尾时**删除**：`wsl.exe -u root -- rm -f /tmp/.ghtok`。
- 该 PAT 曾外泄，**提醒用户去 GitHub → Settings → Developer settings 吊销并重建**。
- 推送用一次性 `GIT_ASKPASS`（python heredoc 创建 `/tmp/.ghaskpass.sh`，push 后 `rm -f`），**不写进 .git/config/脚本/文档**。push 命令：
```bash
wsl.exe -u root -- bash -lc "python3 - <<'PY'
import os
open('/tmp/.ghaskpass.sh','w').write('#!/bin/sh\ncase \"\$1\" in\n  *Username*) echo zhangp996 ;;\n  *) cat /tmp/.ghtok ;;\nesac\n')
os.chmod('/tmp/.ghaskpass.sh',0o700)
PY
cd /home/dev/unixnu/full-project && GIT_ASKPASS=/tmp/.ghaskpass.sh GIT_TERMINAL_PROMPT=0 git -c http.postBuffer=1048576 push https://github.com/zhangp996/UniXNU.git master:full-project 2>&1 | tail -3; rm -f /tmp/.ghaskpass.sh"
```
- 远端有个杂散 `master` 分支，删除需用户确认。
- 过期文档待修：`HANDOVER-addon-ci.md:177`、`SESSION-BOOTSTRAP.md:70` 里的 `--no-amfi` 命令（本树 build_kernel.sh 无 AMFI 步骤，拼镜像不能带 --no-amfi）。

---

## 6. 后续（研究 step 2/3，尚未批准）

- 若 fence 生效但仍有卡顿：量化 vsync late 事件 + GPU 帧诊断；用 `dpu-scanout=2`(只读 pipe/CTL/tear-check 报告) / `=3`(rewrite+latch 测量，参考 5.4ms) 测实际刷新率，再决定是否动 `sheng-vsync`（当前 60，参考板 120）。**先测量再改**。
- 零拷贝 direct scanout 需要 sm8250 显示 SMMU bank（ShengUfs/NativeUfs，TCR2 SEP bits17:15=7）；本板 storage 组件拒绝显示 bank（dead0001），暂不可用。
- **不要改 CPU 核心数**（用户明确；8 核，sheng-smp-max=7 inclusive）。
- GPUDIAG 已验证：gate tick>=40 && tick%20==0 + gpu-metal=1；遗留疑点 last_sel=8 e00002c7、mb_maps/mb_calls=0（非阻塞）。

---

## 7. 环境备忘

- Git Bash(Windows) + WSL2 Ubuntu-22.04，repo root 所有 → `wsl.exe -u root -- bash -lc '...'`。
- **wsl.exe 在双引号里会吃掉 `$VAR`/`$(...)`**：外层用单引号，或字面路径/python heredoc。
- UNC Write 到 WSL root 目录 EPERM → 先 Write 到 Windows 工作区，再 `wsl cp /mnt/c/... /tmp/...`。
- Monitor 参数里 MSYS 会转换 /tmp 路径 → 包 `bash -lc`。
- 抓串口必须在 fastboot boot **之后**启动（脚本有 30s 等端口）。
- 代理 http://127.0.0.1:8890；push 用 `-c http.postBuffer=1048576`。
- CI：zhangp996/UniXNU 分支 `full-project`，gpu-addon.yml id 419914995；run 43874073721（GPU addon macOS）上次查仍 in_progress（userspace，与 fence 无关）。