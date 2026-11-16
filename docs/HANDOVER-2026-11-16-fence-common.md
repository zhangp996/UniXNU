# HANDOVER 2026-11-16 — fence wait 接到公共 present 路径（gpudiag6）

状态：**gpudiag5 真机验证完成 → 根因定位 → 修复落地 → boot-gpudiag6.img 已拼好，等真机验证**。
本文取代 `HANDOVER-2026-11-16-fence-wait.md` 的"待验证"状态；硬约束仍以 `SESSION-BOOTSTRAP.md` 为准。

---

## 1. gpudiag5 真机验证结果（22:45 boot，480s 抓取）

- 主观：**撕裂、闪烁、跳帧仍在**（用户报告）。
- 镜像 sha256 `0e83c0d6…757fc83` 核对一致；fastboot boot OKAY（4.4s），未写分区。
- 日志 `D:\新建文件夹\instantnoodlep-artifacts\gpudiag5-cap.log`（577134 字符，COM14）。

关键证据：

| 项 | 值 | 含义 |
| --- | --- | --- |
| PRESENT copy-ok | 128+（每帧） | 全部帧走 **CPU copy** |
| PRESENT gpu-offer-taken / dpu-flip | 0 / 0 | GPU-offer 与 direct flip 都没走 |
| GpuTraceDisplayFence / gpu_wait_us | 0 / 0 | **fence wait 一次都没执行** |
| DPUDBG | direct refused state=dead0001 mode=2 | direct scanout 被拒（显示 SMMU bank 不可用） |
| DISPLAY frames=128 | late=0 maxus=0 meanus=0 | direct 路径计数全零 |
| copy timing | ticks≈35k–39k / 10264320 B | 每帧全量 copy ~2ms@19.2MHz |
| GPUDIAG | pub=8 tick=440 | fence provider 已 publish（若被查询 detail[2] 应为 2） |

## 2. 根因（代码级，已闭环）

fence wait 链（上游 df4a905 合并）只接在 **direct flip 路径**：

1. `NativeLegacyDisplay.cpp` 的 `gpuWrote()`（fence wait）唯一调用点在
   `if(directOn && !half && !gpuBypass && !latchRequest)` 分支内。
2. 本板 `startDirect()` 因 dead0001 提前返回 → `directOn=false`，且 **fenceTimer 也在该守卫之后才创建**（从未创建）。
3. `native_legacy_gpu_offer` 在编译进的 probe（`package/LegacyDisplayProbe.cpp:868`）是 `return false` 桩 → GPU-offer 永不接受。
4. 于是每帧走 CPU copy（map cacheable → flush dcache → copy → dsb → retire），**读表面前从不等 GPU** → compositor 在 CB 被调度时就 swap，copy 抓到 GPU 正在写的表面 → 撕裂/闪烁。
5. 串口看不到 GpuTrace 事件还有第二层原因：probe 的 `native_gpu_trace_record` 是空桩、`native_gpu_trace_enabled` 返回 0，事件根本不上串口。

交接里"fence wait 在 copy 前等待，gpu_wait_us 应 >0"的预期与代码不符——wait 不在 copy 路径上。

## 3. 修复（GopFramebuffer 子模块 `cc329f1`，port/instantnoodlep）

补丁镜像：`docs/evidence/2026-11-16-fence-common-path.patch`（子模块远端 UniXNU/GopFramebuffer 无推送权限，按惯例以 full-project 镜像为准）。

1. **fence wait 移到公共 transfer 头部**（transfer guard 之后、direct/gpu-offer/CPU-copy 分支之前）：任何路径读表面前都等 `retired >= fenceTarget`；未通过则 `transferred=true` 保持上一帧，200µs fenceTimer 重试（原机制不变）。
2. **fenceTimer 在 start() 无条件创建**（startDirect 拒绝 direct 时也存在）；startDirect 只管 latchTimer；stop()/cancelSwapsAfterTimeout 原有保护不变；gpuWrote 加 fenceTimer 空保护（fail-open）。
3. **串口诊断**（trace 桩是空的，必须 printf）：
   - `FENCEWAIT defer n=… target=… detail2=… waits=…`（前 6 次 + 每 256 次）
   - `FENCEWAIT pass n=… target=… detail2=… wait_us=… max_us=… slow=…`（前 12 次 + 每 128 次）
   - `FENCEWAIT no-provider linked; CPU-only present`（一次性，不应出现）
   - `DISPLAY frames=` 汇总追加 `fence_waits/fence_us/fence_max_us/fence_slow`。
4. host 测试 `Test/check-fence-wait.py` **PASS**（harness 补 fenceDetailLast 成员；gpuWrote 语义未变）。

## 4. boot-gpudiag6.img（可直接核对）

- `D:\新建文件夹\instantnoodlep-artifacts\boot-gpudiag6.img`
- sha256 `8c5754f93492f1d15bd539e3a5854082278f96080469498cb0fb0964854aab69`
- 199782400 bytes；引导参数 **991 B**（≤1000）；**不带 --no-amfi**（AMFI 补丁已打：kernel.macho `10de0997…` → 嵌入 `baf6eef1…`）。
- 内核 strings 含 FENCEWAIT×3、fence_waits 汇总×1（已验证）。
- payload/addon/skeleton 与 gpudiag5 完全相同（dpu5-payload.img + v6 gpuaddon.bin + bootEU.img），**唯一变量是显示 fence wait**。

拼镜像命令（不要加 --no-amfi）：
```bash
cd /home/dev/unixnu/full-project && python3 tools/r2/build_boot_image.py \
  --payload /tmp/dpu5-payload.img \
  --kernel /home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/kernel/kernel.macho \
  --skeleton "/mnt/d/新建文件夹/instantnoodlep-artifacts/bootEU.img" \
  --gpu-addon /home/dev/unixnu/_ref/artifacts/14012210733/gpuaddon.bin \
  --out /tmp/boot-gpudiag6.img
```

## 5. 提交与仓库状态

- GopFramebuffer：`5e1ee73` → **`cc329f1`**（本地提交；父仓 gitlink 已 `git add` 暂存到 cc329f1，**重建前务必保持/重做此暂存**，否则回退）。
- full-project：本文 + 补丁镜像，提交号见回复（远端 zhangp996/UniXNU 分支 full-project）。
- 内核 build EXIT 0（/tmp/kbuildEU.log）；`ensure_touchhid.sh` 报 build-sm8250-touch-hid.py 缺失但**不阻塞**（历史构建同样，触控正常）。

## 6. 真机验证（待人工/授权执行）

设备 fa8620b3，COM14 115200 8N1。**只 fastboot boot，绝不写分区**。

```bash
/d/新建文件夹/tools/platform-tools/fastboot.exe boot "D:/新建文件夹/instantnoodlep-artifacts/boot-gpudiag6.img"
```
发出后立即：
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\新建文件夹\_agent\capture_eu.ps1" -Out "D:\新建文件夹\instantnoodlep-artifacts\gpudiag6-cap.log" -StartDelaySeconds 20 -Seconds 480
```

**核对点**：
- `FENCEWAIT pass … detail2=2` → 真实 fence 接上（=1 锁忙 / =3、4 unknown 则导出没生效）。
- `FENCEWAIT defer` 出现且 waits 增长 → wait 真的在等 GPU；wait_us/max_us 量化等待时长。
- 主观撕裂/闪烁是否消除或减轻。
- 若 detail2=2 且 wait_us≈0 但仍撕裂 → swap 可能早于本帧 rendering 的 ring submit（fenceTarget 捕到旧 submitted），需重新审视 target 捕获时机；转 §7。
- 若 wait 经常 ≥250ms（slow）→ GPU 帧耗时问题，转 GPU 帧诊断。

## 7. 后续（未批准，先测量再改）

- vsync 60 vs 参考板 120：用 dpu-scanout=2/3 测实际刷新率再决定。
- device-memory copy 速率、零拷贝 direct scanout（需显示 SMMU bank，dead0001 未解）。
- **不改 CPU 核心数**（8 核，sheng-smp-max=7 inclusive）。

## 8. 本轮新坑（环境）

- **pwsh 外层双引号会展开 `$VAR`**：wsl bash -lc 里的 shell 变量（$K/$B/$X）会被 pwsh 先吃掉，导致 strings/grep 读空 stdin 出**假阴性**。检查命令一律用字面路径，或把脚本写文件再执行。
- UNC 写 WSL 仍 EPERM：编辑走 cp 出来 → 本地替换 → cp 回去；提交信息用 `git commit -F` 文件中转。
- PAT 已换新（用户 22:43 提供），存 WSL /tmp/.ghtok（600）；收尾删除；旧的需吊销。
