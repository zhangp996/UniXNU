# 会话引导（换对话框时先读这个）

> 本文件是给**新对话里的接手者**看的：它记录那些不在磁盘上的状态（在跑的 run、设备状态、下一步）。
> 技术细节不在这里，在 `docs/HANDOVER-2026-11-16-addon-ci.md`。

## 0. 一句话

把 iPadOS 17.7.11 (21H461) 移植到 OnePlus 8 Pro（instantnoodlep / SM8250 / Adreno 650），
保持 **R2 基线**（原始 `bootEU.img` 骨架），不做 fresh-core 重写；当前主线是**打通 CI 并自建 GPU addon**。

## 1. 先读这两份（其余按需）

| 文件 | 内容 |
| --- | --- |
| `docs/HANDOVER-2026-11-16-addon-ci.md` | 本次全部技术结论：CI 打通、addon 前置条件、真机对照实验、6 个坑、换机流程 |
| `docs/HANDOVER-2026-11-16.md` | 更早的基线交接：R2 定义、8 核、显示/触控修复、产物哈希 |

## 2. 当前实况（接手时先复核，本区会过期）

记录时间：**2026-11-16 19:15 (+08:00)**

| 项 | 状态 | 复核方法 |
| --- | --- | --- |
| 仓库 | `zhangp996/UniXNU` 分支 `full-project`，HEAD `9759adf` | `git -C ~/unixnu/full-project log --oneline -1` |
| 基线 CI | ✅ 已绿（run 43835360031，产物 payload 93.5 MB） | `gh run list -R zhangp996/UniXNU -w macos.yml -L 3` |
| addon CI | 🔄 run **43851478657**（head `ea83575`）卡在 `Build the NativeAdrenoMetal plugin`，已跑 50+ 分钟 | `gh run view 43851478657 -R zhangp996/UniXNU` |
| 设备 | **当前离线**（无 fastboot、无 COM14） | `fastboot devices`；设备管理器看 VID_1209&PID_316D |
| 本地内核 | `119d07ff…`（125,304,832 B） | `sha256sum ~/unixnu/UniXNU/out/…/kernel/kernel.macho` |

> 若 43851478657 已结束：**先看它的日志**再决定下一步；若仍是 in_progress 超过 90 分钟，取消它并查 cargo 是否卡在网络/编译。

## 3. 硬约束（不能破）

1. **只 `fastboot boot`，绝不写分区**。
2. **不提交任何 token / 密钥**；临时推送用 `GIT_ASKPASS`，用完删除。
3. 引导参数 **≤ 1000 字节**（DarwinBoot 限制），`build_boot_image.py` 会打印字节数。
4. 保持 R2 基线，不做 fresh-core 重写。
5. 改 `kits/` 或 `package/` 后，必须同步到 `out/…/tree/`（那是软链，实际改仓库即可，但要确认构建读到的是新文件）。

## 4. 环境事实（路径都要背下来）

| 用途 | 路径 |
| --- | --- |
| 核心仓 | `/home/dev/unixnu/UniXNU`（8.4 GB，20 子模块） |
| 设备仓 | `/home/dev/unixnu/UniXNU-instantnoodlep`（分支 `bringup-2026-11-12`） |
| CI/文档仓 | `/home/dev/unixnu/full-project`（分支 `master` → 远端 `full-project`） |
| 构建树 | `…/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/tree` |
| 本地内核 | `…/out/…/kernel/kernel.macho` |
| boot 骨架 | `D:\新建文件夹\instantnoodlep-artifacts\bootEU.img` |
| 已知可用镜像 | `…\boot-dpu5.img`（sha256 `cb3761be…`） |
| fastboot | `D:\新建文件夹\tools\platform-tools\fastboot.exe`（WSL 无 USB 直通） |
| 串口抓取 | `D:\新建文件夹\_agent\capture_eu.ps1`（COM14, VID_1209&PID_316D） |
| Linux Apple 工具链 | `/opt/unixnu-linux`（`ensure_*.sh` 里硬编码） |

## 5. 下一步（按优先级，命令可直接跑）

**① 等/查 addon CI**
```bash
gh run view 43851478657 -R zhangp996/UniXNU --log | grep -E 'PASS|error|FAILED' | tail -20
```
过了 `Build the NativeAdrenoMetal plugin` 就会自动 `Pack the addon` 并上传 `gpu-addon-ios`。

**② 拿到 `gpuaddon.bin` 后拼镜像（用 dpu5 的参数集，不要用 CI 的 toml）**
```bash
cd ~/unixnu/full-project
python3 tools/r2/build_boot_image.py \
  --payload <payload.img> \
  --kernel ~/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/kernel/kernel.macho \
  --skeleton '/mnt/d/新建文件夹/instantnoodlep-artifacts/bootEU.img' \
  --gpu-addon <gpuaddon.bin> --no-amfi --out /tmp/boot-addon.img
```

**③ 真机验证（务必先开抓取再 boot）**
```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\新建文件夹\_agent\capture_eu.ps1" `
  -Out "D:\新建文件夹\_agent\cap.log" -StartDelaySeconds 0 -Seconds 90
& "D:\新建文件夹\tools\platform-tools\fastboot.exe" boot "<img>"
```

**④ 修 CI payload 的参数问题**（`setup-skip=1`、缺 `boot-ui-chrono`、`smgop=1`）
改 `r2/device/board.toml` 的 args，或拼镜像时用 `--args-add/--args-remove` 覆盖。

## 6. 不要重踩的坑（详见交接文档 §5）

| 现象 | 真相 |
| --- | --- |
| `kernel-bootstrap.log` 里一堆 undefined symbols | **设计内**，引导 pass 故意不带 archive，错误被 `kernel.py` 吞掉 |
| 本地能编、CI 不能 | 本地 `out/` 是增量树，从不走引导 pass |
| dispatch 返回 422 | fork 仓库的 Actions 没在网页启用 |
| dispatch 返回 404 | 工作流文件不在默认分支 `main` |
| 串口只有 `QCOM XNU USB CDC ACM v1` | 抓取开得太晚，或镜像本身挂了 |
| `git apply` 报 `dev/null: No such file` | 补丁是手工编辑的，回退到 `patch -p1` |
| bridge 补丁在 `prime.rs:52` 失败 | 补丁世代与 pin 不一致，需整棵同步 `drivers/AdrenoMetal` |

## 7. 协作约定

- 用**中文**回答，结论先行，命令可直接复制。
- 每次改动都要 **commit + push 到 `full-project`**，并在回复里给出提交号。
- 真机操作（进 fastboot、观察屏幕）需要人来做；先给指令再等确认。
- 长任务用后台 job + 轮询，不要空转等待。

