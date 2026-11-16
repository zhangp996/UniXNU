# HANDOVER 2026-11-16（晚）：GPUDIAG 诊断移植与真机回归排查

承接 `SESSION-BOOTSTRAP.md` 与 `HANDOVER-2026-11-16-addon-ci.md`。本文件只记本轮新增事实与下一步，硬约束以 SESSION-BOOTSTRAP §3 为准（只 `fastboot boot`、boot-args ≤1000 B、R2 基线、commit+push 到 `full-project`、中文回复）。

## 0. 结论先行

- GPUDIAG 三处移植已提交并推送（`fc3d4b8`、`b4c7831`），内核编译干净，`verify-baseline` CI 在 `b4c7831` 上 success。
- **但带 GPUDIAG 的内核在真机上于 tick 40 前后 panic**：`"AMFI: No PMGR?\n" @ConfigurationSettings.cpp:235`。dpu5 基线内核同位置打印 `AMFI: finished: 1 0` 并活到 UI。GPUDIAG 因此**从未在任何抓取里出现**（第一次触发即 panic）。
- 串口"断流"不是主机/采集问题，是 panic 本身杀死 USB serial TX；照片里 panic 块"后面"的 launchd/触摸行是 framebuffer 旧内容＋panic 屏直接绘帧，**照片行序 ≠ 时间序**。
- 下一步第一实验（不用重编内核）：`gpu-metal=1` → `gpu-metal=0` 拼镜像再 boot。gate 会直接 return，不碰任何 driver 符号；若 panic 消失即锁定 GPUDIAG 函数体为触发点。
- addon CI run **43869651151**（head `b4c7831`）job `addon` conclusion=**cancelled**（12:46:44Z 起、13:01:47Z 止，约 15 分钟），日志 BlobNotFound；workflow `timeout-minutes: 300`、`cancel-in-progress: false`，故非超时/非并发取消，原因待查（重跑并live看）。

## 1. 产物与坐标

| 项 | 值 |
|---|---|
| dpu5 基线内核 sha | `119d07ff589f3e6ec30ec959121508f78dab75f4f627f3c7bd5fc03efd09dda1` |
| GPUDIAG 内核 sha | `7168ed505511c0becf83c011edf90d3f989a9a6d0405533d638c3d5874834182` |
| boot-gpudiag.img | `507b95565e5654583eea45c271dcff7f88e00d76e5054a6e1b37817812981d6d`（args 991） |
| boot-gpudiag2.img | `ef8f1c543fdab19b69a7f03eac6f8985286c127a23b3cfc61d820ad42e42af9f`（args 991，`sheng-crashlog=0`） |
| 镜像位置 | `D:\新建文件夹\instantnoodlep-artifacts\`（WSL 侧 `/tmp/boot-gpudiag*.img`） |
| payload / skeleton / addon | `/tmp/dpu5-payload.img`；`/mnt/d/新建文件夹/instantnoodlep-artifacts/bootEU.img`；`/home/dev/unixnu/_ref/artifacts/14012210733/gpuaddon.bin`（v6） |
| 抓取日志 | `D:\新建文件夹\_agent\{dpu5-cap,dpu5-verify-cap,gpudiag-cap,gpudiag2-cap}.log` |
| 设备 | `fa8620b3`，交接时在 **fastboot**；串口 COM14 `USB\VID_1209&PID_316D` 115200 8N1 |
| fastboot | `/d/新建文件夹/tools/platform-tools/fastboot.exe`（不在 _agent 下） |

拼镜像命令（`--args-remove/--args-add` 可做等长替换）：

```bash
cd /home/dev/unixnu/full-project && python3 tools/r2/build_boot_image.py \
  --payload /tmp/dpu5-payload.img \
  --kernel /home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/kernel/kernel.macho \
  --skeleton "/mnt/d/新建文件夹/instantnoodlep-artifacts/bootEU.img" \
  --gpu-addon /home/dev/unixnu/_ref/artifacts/14012210733/gpuaddon.bin \
  --no-amfi --args-remove 'gpu-metal=1' --args-add 'gpu-metal=0' --out /tmp/boot-gpudiag3.img
```

## 2. 本轮提交（均已 push 到 `full-project`）

- `f2e97b3`：补 `r2/core/scripts/macho-self-bind-weak-imports.py`（CI STEP 12 链接后缺它必失败；从 `_refactor-aside/upstream-core/scripts/` 逐字节拷贝）。
- `fc3d4b8`：GPUDIAG 三处移植（见 §3）。
- `b4c7831`：修 heredoc 转义事故（printf 串里 `\n` 变真换行）。
- stray 远端分支 `master` 是我误推产物（与 full-project 同提交），**删除属共享状态操作，待确认**。

## 3. GPUDIAG 移植内容（已进内核）

文件：`UniXNU-instantnoodlep/package/LegacyDisplayProbe.cpp`（镜像同改 `full-project/r2/device/package/LegacyDisplayProbe.cpp`；构建读前者，经 `out/…/tree/kits/instantnoodlep` 软链）。

1. 前向声明：`gpu_metal_diag_report(unsigned tick)`。
2. gate（`probe_rows_thread` 内、tick-24 块之前）：`if (tick >= 40 && (tick % 20) == 0) gpu_metal_diag_report(tick);`
3. 函数体：读 `native_adreno_metal_diag`（55×uint32=220 B；opener[3][17]@220、reader@271、name_reader[17]@322、starter[3][17]@339、report[64]@390、report2[64]@454）、`native_adreno_publish_stats(&closed)`、`native_adreno_published_service()`、`native_adreno_metal_rows[r<6]`，printf 五行 GPUDIAG。
4. 相对参考（`_refactor-aside/device-a650/package/LegacyDisplayProbe.cpp:1420-1491`）**省略**了 fail/unk/GPUVMOPS/tun 四块：对应数组只存在于参考的 `package/BoardGlue.cpp`，本树没有，照抄会链接失败。

**tick 语义（关键）**：`probe_rows_thread` 是 `for(;;){ delay(4000000); tick += 4; … }` → tick≈线程启动后秒数；**tick 28/32/36 无任何打印**（touchscreen=1 关合成触摸、kill-lsd=0）。所以 tick 40 的 panic 在串口上紧贴 tick-24 的 PROC 行出现，中间 16 秒是静默，不是"断在 tick 24"。

## 4. 真机证据链（按可信度）

1. `dpu5-verify-cap.log`（805 KB，连续）：tick-24 PROC 行后 `AMFI: finished: 1 0`（:401），launchd、KNOTE、直到 `XNU-EXIT fairplayd` 正常；无 panic、无 Finger（没人摸屏）。
2. `gpudiag-cap.log`（boot 1）：止于 tick-24 PROC 行（25,828 字符后静默）。
3. `gpudiag2-cap.log`（boot 2，sheng-crashlog=0）：:441 `"AMFI: No PMGR?\n" @ConfigurationSettings.cpp:235` 与 PROC[9] 交错，随后两行 PROC 再静默。**dpu5 的 `AMFI: finished: 1 0` 在两个 gpudiag 抓取里都不存在**。
4. 照片（用户拍屏）：panic 报告块（panic 行／Please go to／UUID／boot-args）出现在屏幕中段，上下是旧日志与触摸行 → panic 屏直接绘 framebuffer 的合成画面；触摸行真身是 `s6sy791: finger %u at …`（`S6sy791Touch.c:693`），**不是 tick 计数**。
5. 两次 boot 结束后设备都回到 fastboot → panic 后复位。
6. 已排除：CrashLogProbe 回放灌爆串口（boot 2 关回放仍断）；GopFramebuffer `9097d0c`"silence text console"（pin 昨天 18:08 已在树里，dpu5 同带；且 escape hatch `offline-legacy-console=1` 在 args 里）；采集脚本/COM 口问题（脚本 AutoFlush、正常 exit、口在）。

**主假设**：tick 40 首次执行 `gpu_metal_diag_report` → `native_adreno_published_service()`（IOService 查找）或 `native_adreno_publish_stats()` 触发 AMFI policy 评估 → `ConfigurationSettings.cpp:235` 读 PMGR 失败 → panic。dpu5 无此调用故 AMFI 正常 finished。
**备假设**：root 身份重编引入 probe 之外的输入差异（ensure_*.sh / submodule / components.toml 现仍 M）。注意 dev 身份构建曾因 `drivers/RamBlock` dubious ownership 与 root-owned build 目录权限失败；root 构建是否更新了 submodule 未证实（GopFramebuffer 现 HEAD `9097d0c` 且仍 M NativeLegacyDisplay.cpp，与昨天 pin 一致）。

## 5. 下一步（按序）

1. **gpu-metal=0 实验**（§1 命令，等长 991 B）：boot + 抓取 120 s。panic 消失 ⇒ 锁定函数体；仍在 ⇒ 转备假设，用"只回退 gate 调用、其余不动"的内核再二分。
2. 若锁定函数体：先只留纯内存读（diag words + rows），去掉 `native_adreno_published_service()`/`native_adreno_publish_stats()` 两个 driver 调用再测；逐步加回定位到具体调用。参考移植里这两调用同样存在但参考树 AMFI/PMGR glue 不同（参考有 BoardGlue 计数数组），不能照搬结论。
3. 采集时机坑：`capture_eu.ps1` 的找口窗口只有 30 s；设备在 fastboot 时无串口 gadget，**先 boot 再挂抓取**（或 StartDelay 覆盖 send+启动），否则 exit 1 "no console device"。
4. CI：重 dispatch gpu-addon.yml（ref full-project），live 盯 STEP 12 之后（self-bind 已修；下一风险 codesign/bundle seal/pack）。cancelled 且无日志的 run 不必再挖。
5. 收尾：删 WSL `/tmp/.ghtok`；**提醒用户吊销本轮聊天里暴露的 PAT**；stray `master` 分支待确认删除；过期文档/注释（`LegacyDisplayProbe.cpp:1375` "Nothing calls native_adreno_publish() yet" 等）待修。

## 6. 环境坑（本轮新增/重申）

- WSL heredoc 里 `\\n` 会塌成真换行：C 串里的反斜杠用 `chr(92)` 拼且**去掉字面换行**（b4c7831 的事故与修法）。
- 内核构建必须 root（`wsl.exe -u root`）：dev 身份会撞 build 目录权限、/tmp 权限、git dubious ownership；但 root 构建的副作用要与 dev 基线对照（§4 备假设）。
- push 用一次性 `GIT_ASKPASS`（用完即删）＋ `-c http.postBuffer=1048576`；token 只放 `/tmp/.ghtok`（chmod 600），不进 repo/文档。
- WSL 内 GitHub API 走 `https_proxy=http://127.0.0.1:8890`。
- 照片判读：panic 屏是 framebuffer 合成画面，行序不可当时间序；串口静默段要先对照 tick 打印表再下"断流"结论。