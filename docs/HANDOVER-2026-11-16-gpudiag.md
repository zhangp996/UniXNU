# HANDOVER 2026-11-16（晚）：GPUDIAG 诊断移植与真机回归排查

承接 `SESSION-BOOTSTRAP.md` 与 `HANDOVER-2026-11-16-addon-ci.md`。本文件只记本轮新增事实与下一步，硬约束以 SESSION-BOOTSTRAP §3 为准（只 `fastboot boot`、boot-args ≤1000 B、R2 基线、commit+push 到 `full-project`、中文回复）。

## 0. 结论先行

- GPUDIAG 三处移植已提交并推送（`fc3d4b8`、`b4c7831`），内核编译干净，`verify-baseline` CI 在 `b4c7831` 上 success。
- **根因已确认：镜像漏打 AMFI 补丁**。`build_boot_image.py` 默认跑 `patch-amfi-nopmgr.py`（nop 掉 `"AMFI: No PMGR?"` panic 调用）与 `patch-amfi-trustcache-gate.py`；`--no-amfi` 的前提是"内核已由 build_kernel.sh 打过补丁"，但**本树 build_kernel.sh 根本没有 AMFI 步骤**（HANDOVER-addon-ci.md:177 的说法过期）。本轮三个镜像都沿旧命令带 `--no-amfi` → 未补丁内核 → AMFI 启动即 panic（tick 24–40 之间），GPUDIAG 的第一次触发（tick 40）永远到不了。
- dry-run 证据：未补丁内核在 `0x6064d04` 为 `0x940069b8`（bl panic）、trustcache gate 为 `0x360010a0`（cbz），与补丁脚本 EXPECTED 完全吻合。
- 对照实验（`gpu-metal=0`，boot-gpudiag3）同样 panic ⇒ **排除 GPUDIAG 函数体触发假设**；dpu5 基线镜像当年带补丁故正常。
- 修复镜像 **boot-gpudiag4.img**（`6e30bca7d9b3e458c01b966fe4d7ea869db87a7711488ccd26b86f832ed9ee17`，补丁后内核 `e57a0b1f6347a37bea8d0c9e783f1af3b51723b828e33ba2ae37dc29625ce4d1`，args 991、`gpu-metal=1` 恢复）**真机验证通过**（`gpudiag4-cap.log`，638 KB 连续）：`AMFI: finished: 1 0` 回归；GPUDIAG tick 40→180 连续打印；tick 60 opener=backboardd、tick 180 **opens=8 closed=4 calls=4174 tunnel=4175**（与参考审计 opens=8、tunnel≈calls+1 同形态，参考快照更晚故 calls=8300）；prop=122 name=6 probes=264 marks=11 selbits 全 f；UI 健康（SpringBoard n25/n47、backboardd n38、UIT 行活跃、无 panic）。
- 遗留小异点（非阻塞）：`last_sel=8 last_status=e00002c7`（kIOReturnUnsupported）、`report=[P2 ee00002c7]`——selector 8 的外部方法返回 unsupported，calls 仍持续增长；参考的 report/report2 为空，此处有记录，后续可查 sel 8 对应方法。`mb_maps=0 mb_calls=0`（mode 12 静默成功路径，未走 map-buffer）。
- **规则：本树拼镜像一律不带 `--no-amfi`**（build_kernel.sh 不打补丁）；HANDOVER-addon-ci.md:177 与 SESSION-BOOTSTRAP.md:70 的旧命令待修。
- addon CI run **43869651151**（head `b4c7831`）job `addon` conclusion=**cancelled**（12:46:44Z 起、13:01:47Z 止，约 15 分钟），日志 BlobNotFound；workflow `timeout-minutes: 300`、`cancel-in-progress: false`，故非超时/非并发取消，原因待查（重跑并live看）。

## 1. 产物与坐标

| 项 | 值 |
|---|---|
| dpu5 基线内核 sha | `119d07ff589f3e6ec30ec959121508f78dab75f4f627f3c7bd5fc03efd09dda1` |
| GPUDIAG 内核 sha | `7168ed505511c0becf83c011edf90d3f989a9a6d0405533d638c3d5874834182` |
| boot-gpudiag.img | `507b95565e5654583eea45c271dcff7f88e00d76e5054a6e1b37817812981d6d`（args 991） |
| boot-gpudiag2.img | `ef8f1c543fdab19b69a7f03eac6f8985286c127a23b3cfc61d820ad42e42af9f`（args 991，`sheng-crashlog=0`，仍漏 AMFI 补丁） |
| boot-gpudiag3.img | `b9d81a616ee3e4d46a292933423b4fe2318a753ab432501e8386cd8764e34d9b`（`gpu-metal=0` 对照，仍漏补丁 → 同 panic） |
| boot-gpudiag4.img | `6e30bca7d9b3e458c01b966fe4d7ea869db87a7711488ccd26b86f832ed9ee17`（**带 AMFI 补丁**，`gpu-metal=1`，args 991） |
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

**主假设（已确认）**：漏打 AMFI 补丁（见 §0）。panic 杀死 USB serial TX 造成"断流"；tick 28–36 静默使 panic 行在串口上紧贴 tick-24 PROC 行。
**已排除**：GPUDIAG 函数体触发（gpu-metal=0 对照同 panic）；CrashLogProbe 回放灌爆串口（boot 2 关回放仍断）；GopFramebuffer `9097d0c`"silence text console"（pin 昨天 18:08 已在树里，dpu5 同带；且 escape hatch `offline-legacy-console=1` 在 args 里）；采集脚本/COM 口问题（脚本 AutoFlush、正常 exit、口在）。
**备假设（已无需）**：root 身份重编引入 probe 之外的输入差异——对照与 dry-run 已把差异定位到镜像期补丁，与构建身份无关。

## 5. 下一步（按序）

1. ~~gpu-metal=0 实验~~ 已做：对照同 panic ⇒ 排除函数体假设；根因=漏 AMFI 补丁，boot-gpudiag4 验证通过（§0）。GPUDIAG 移植本身无需再改。
2. 可选跟进：selector 8 的 kIOReturnUnsupported（`NativeAdrenoUserClient` 方法表对照 sel 8）；`mb_maps/mb_calls=0` 是否意味着 Metal backend 的 map-buffer 路径未被客户端使用（mode 12 静默成功的预期行为，待与参考对照）。
3. 采集时机坑：`capture_eu.ps1` 的找口窗口只有 30 s；设备在 fastboot 时无串口 gadget，**先 boot 再挂抓取**（或 StartDelay 覆盖 send+启动），否则 exit 1 "no console device"。
4. CI：重 dispatch gpu-addon.yml（ref full-project），live 盯 STEP 12 之后（self-bind 已修；下一风险 codesign/bundle seal/pack）。cancelled 且无日志的 run 不必再挖。
5. 收尾：删 WSL `/tmp/.ghtok`；**提醒用户吊销本轮聊天里暴露的 PAT**；stray `master` 分支待确认删除；过期文档/注释（`LegacyDisplayProbe.cpp:1375` "Nothing calls native_adreno_publish() yet" 等）待修。

## 6. 环境坑（本轮新增/重申）

- WSL heredoc 里 `\\n` 会塌成真换行：C 串里的反斜杠用 `chr(92)` 拼且**去掉字面换行**（b4c7831 的事故与修法）。
- 内核构建必须 root（`wsl.exe -u root`）：dev 身份会撞 build 目录权限、/tmp 权限、git dubious ownership；但 root 构建的副作用要与 dev 基线对照（§4 备假设）。
- push 用一次性 `GIT_ASKPASS`（用完即删）＋ `-c http.postBuffer=1048576`；token 只放 `/tmp/.ghtok`（chmod 600），不进 repo/文档。
- WSL 内 GitHub API 走 `https_proxy=http://127.0.0.1:8890`。
- 照片判读：panic 屏是 framebuffer 合成画面，行序不可当时间序；串口静默段要先对照 tick 打印表再下"断流"结论。