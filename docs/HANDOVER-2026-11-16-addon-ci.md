# 交接文档：CI 流水线打通 + 自建 GPU addon（2026-11-16 晚）

> 上一份：`docs/HANDOVER-2026-11-16.md`（R2 基线、8 核、显示/触控修复）。
> 本文只记录**当晚这一段**：把本地 R2 基线推到 GitHub、把基线 CI 跑绿、新建 `gpu-addon.yml` 自建 addon、以及真机对照实验。

## 1. 目标与范围

| 目标 | 状态 |
| --- | --- |
| 本地 R2 基线推到 `zhangp996/UniXNU` | ✅ 完成 |
| 基线 CI（内核 + payload）跑通 | ✅ **成功**（run 43835360031） |
| 新建 `gpu-addon.yml` 在 macOS runner 上自建 GPU addon | 🔄 推进中（已过 Turnip，卡在 bridge 的 cargo 编译） |
| 用 CI 产物做真机对照实验 | ✅ 完成，结论见 3.3 |

## 2. 仓库与分支状态

```
远端  https://github.com/zhangp996/UniXNU
分支  full-project（CI 用的就是这条）
HEAD  68b87c9
```

本次会话提交（由旧到新）：

| 提交 | 内容 |
| --- | --- |
| `35435b3` | 基线：八核内核、静默控制台、触控释放、显示 present |
| `c9eb934` | 显示/触控：修复三帧停摆、恢复 bring-up block、修正触控帧契约 |
| `bce426b` | 把触控与显示修复同步进 `r2/core` 快照 |
| `de5e0db` / `08d098f` | CI：构建环境诊断步骤（第一版写崩，第二版修好） |
| `83b5e8d` | **CI：修掉 `KERNEL` 赋值里的 bad substitution**（见 5.1） |
| `566e8c4` | 新建 `gpu-addon.yml` |
| `be3e569` … `241a4e9` | addon 流水线逐步补齐（patch 回退、bison、requirements、Rust、metal2vulkan、spirv-tools、LLVM 链接） |
| `309ee61` | `r2/core`：更新 Adreno ABI 头（7,733 → 9,218 字节） |
| `ff94257` | `r2/core`：GPU bridge 及其补丁集、addon 脚本对齐上游 main |
| `0a0f13b` | CI：`rustup target add aarch64-apple-ios` |
| `ea83575` | `r2/core`：整个 AdrenoMetal 树（补 `shaders/scanout_convert.ll`） |
| `68b87c9` | 清理：删除误提交的 `.pyc`，忽略 `__pycache__` |

**注意**：`zhangp996/UniXNU` 是 fork，Actions 需要在网页点一次启用；API 显示 `enabled: true` 也没用，dispatch 会回 422。

## 3. 关键结论

### 3.1 基线 CI 已绿

run **43835360031** 产出：

| 产物 | 大小 | 内容 |
| --- | --- | --- |
| `unixnu-instantnoodlep-payload-1` | 93,540,092 B | 解出 `payload.img` 184,549,376 B（FAT32，卷标 `OP8PRO`） |
| `unixnu-instantnoodlep-logs-1` | 3,902,846 B | 23 个构建日志 |

`payload.img` 内只有三项，**没有内核**（刻意不上传，源自 Apple 内核缓存）：

```
EFI/UniXNU/unixnu.toml                       1,330 B
EFI/UniXNU/Payloads/kernel.macho.gpuaddon   13,665,760 B   ← 仍是旧 v6
EFI/UniXNU/Payloads/kernel.macho.trustcache     61,432 B
```

### 3.2 自建 addon 的全部前置条件（已逐条踩过）

| 依赖 | 固定值 | 说明 |
| --- | --- | --- |
| mesa | `85fda3b36405bcaea8bffc64aa4f2651dc90770d` | 17 个补丁按 `adreno-compiler-inputs.json` 的顺序施加；其中 3 个是手工编辑的，`git apply` 不认 `/dev/null` 前像，需回退到 `patch -p1` |
| bison | **3.8.2**（自建，校验 sha256） | IR3 parser 需要；新版脚本只认 `shutil.which('bison')` 或 `/opt/homebrew/opt/bison/bin/bison` |
| metal2vulkan | **`555b13d5`**（development 分支） | bridge 的 Rust 运行时 |
| Rust | **1.98.0** + `aarch64-apple-ios` std | bridge 交叉编译 |
| LLVM | brew `llvm`（keg-only） | `llvm-as/objcopy/...` 必须软链进 `.cache/mesa-tools/bin` |
| 其它 | `lld`、`glslang`、`shaderc`、`spirv-tools`、`meson`、`ninja` | 缺一个就失败一轮 |
| Python 依赖 | `scripts/requirements-adreno-compiler.txt` | meson 1.9.1 / Mako / packaging / PyYAML / ply |

### 3.3 真机对照实验：CI payload 镜像启动即挂

| 镜像 | 串口字符数 | 结果 |
| --- | --- | --- |
| `boot-dpu5.img`（已知可用） | **181,182** | 正常 |
| `boot-ci-payload.img`（CI payload + 本地内核） | **24**（仅 `QCOM XNU USB CDC ACM v1`） | 挂住；屏幕黑、左上角一个红块；COM14 枚举但端点死 |

**结论：设备和抓取流程都正常，问题在 CI payload 的引导参数集。** 两份 `unixnu.toml` 的差异：

| 参数 | 本地（能进 UI） | CI payload |
| --- | --- | --- |
| `setup-skip` | 0 | **1** |
| `sheng-vsync` | 120 | **60** |
| `boot-ui-chrono` | 1 | **缺失** |
| `sheng-gpu-addon` | 无 | **1** |
| `smgop` / `sm8250-gpu-alioth` | 无 | **1 / 12** |
| `sheng-early-retire` | 无 | **1** |

下次做真机镜像时，应使用 **dpu5 的参数集**，不要直接用 CI 的 `board.toml` 参数。

## 4. `gpu-addon.yml` 结构（13 步）

```
Check out -> Lay out r2/core -> setup-python -> brew 装工具
  -> rustup 1.98.0 + aarch64-apple-ios
  -> Fetch Mesa（pin + 17 补丁，git apply / patch(1) 回退）
  -> Fetch metal2vulkan（pin 555b13d5）
  -> Build bison 3.8.2（下载校验 + configure/make + 软链）
  -> venv + requirements + 软链 llvm-*/ld.lld/glslang/glslc/spirv-val
  -> build-turnip-darwin.py --target ios
  -> build-adreno-metal-plugin.py --target ios --no-replay
  -> make-gpu-addon.py --publish build/gpuaddon.bin
  -> 上传 gpu-addon-ios 产物
```

`--no-replay` 是必须的：否则会要求 host 版 Turnip 做诊断录制，而那是可选项。

## 5. 已知坑（按踩到的顺序）

### 5.1 CI 工作流的 bad substitution（已修）
`macos.yml` 里曾是 `KERNEL` 的赋值把 `${{ }}` 写成了单花括号，内核**构建成功**之后，
AMFI 打补丁那步在 bash 里报 `bad substitution` 退出 1。已改正。

### 5.2 `kernel-bootstrap.log` 里的 undefined symbols 是设计内
`scripts/unixnu/kernel.py` 在全新树上会先跑一次 `XNU_BOOTSTRAP=1` 的引导 pass，
它**故意**不带板级 archive、停在链接，只为留下编译记录（`startup.o.json`），错误会被吞掉。
看到 `_native_legacy_display_probe` 之类的未定义符号**不要**当成失败。

### 5.3 增量树掩盖问题
本地 `out/` 是增量树，从不走引导 pass；只有 CI 的干净树才会暴露 5.2 那条路径。

### 5.4 刷机与抓取的顺序
必须**先开串口抓取、再 `fastboot boot`**，否则启动输出早已错过（本次因此白抓两次，只拿到 24 字符）。
COM14 同一时刻只能被一个进程占用。

### 5.5 WSL 没有 USB 直通
`fastboot` 要用 Windows 侧：`D:\新建文件夹\tools\platform-tools\fastboot.exe`。
设备挂住后 PnP 里只剩 `USB 串行设备 (COM14)`，必须长按电源 10–15 秒复位。

### 5.6 bridge 补丁世代必须与 pin 一致
旧快照里是 `0001`–`0016` 世代，上游已到 `0046` 世代；补丁上下文与 pin 不匹配时
`git apply` 会在 `prime.rs:52` 一类的位置失败。同步时要整棵 `drivers/AdrenoMetal`（含 `shaders/`）。

## 6. 产物与哈希

| 文件 | 大小 | sha256（前 16 位） |
| --- | --- | --- |
| `boot-dpu5.img` | — | `cb3761be…`（已知可用） |
| `boot-ci-payload.img` | 199,782,400 | `a7e9b009b85e6556` |
| CI `payload.img` | 184,549,376 | `8ee10ef25d78c9c9` |
| 本地 `kernel.macho` | 125,304,832 | `119d07ff589f3e6e` |
| `_agent\dpu5-cap.log` | 181,243 | 对照实验的串口证据 |

## 7. 复现命令

### 7.1 触发 CI

```bash
# 基线（内核 + payload）
curl -X POST -H "Authorization: Bearer $GH_TOKEN" \
  https://api.github.com/repos/zhangp996/UniXNU/actions/workflows/macos.yml/dispatches \
  -d '{"ref":"full-project","inputs":{"device_repository":"zhangp996/UniXNU-instantnoodlep-10-27","device_ref":"main","defconfig":"instantnoodlep_defconfig","steps":"kernel payload"}}'

# addon
curl -X POST -H "Authorization: Bearer $GH_TOKEN" \
  https://api.github.com/repos/zhangp996/UniXNU/actions/workflows/gpu-addon.yml/dispatches \
  -d '{"ref":"full-project","inputs":{"target":"ios","xcode":"26.6"}}'
```

> `gpu-addon.yml` 必须同时存在于默认分支 `main` 才能被 dispatch（否则 404），但实际执行的是 `ref` 指向的版本。

### 7.2 拼 boot image

```bash
cd /home/dev/unixnu/full-project
python3 tools/r2/build_boot_image.py \
  --payload <payload.img> \
  --kernel  /home/dev/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/kernel/kernel.macho \
  --skeleton "/mnt/d/新建文件夹/instantnoodlep-artifacts/bootEU.img" \
  --gpu-addon <gpuaddon.bin> \
  --no-amfi \
  --out /tmp/boot-test.img
```

`--no-amfi` 仅当内核已由 `build_kernel.sh` 打过补丁；命令会打印 args 字节数，必须 <= 1000。

### 7.3 刷机 + 抓串口

```powershell
# 1) 先开抓取（90 秒）
powershell -NoProfile -ExecutionPolicy Bypass -File "D:\新建文件夹\_agent\capture_eu.ps1" `
  -Out "D:\新建文件夹\_agent\cap.log" -StartDelaySeconds 0 -Seconds 90

# 2) 立刻刷
& "D:\新建文件夹\tools\platform-tools\fastboot.exe" boot "<img>"
```

## 8. 未完成 / 下一步

1. **addon 最后一公里**：run 43851478657 停在 `Build the NativeAdrenoMetal plugin`（cargo 编译 bridge）。
   失败优先看 SDK/framework 链接与 `build-adreno-metal-bridge.py` 的 fixture 组装。
2. **拿到 `gpuaddon.bin` 后**：用 dpu5 参数集拼镜像（不要用 CI 的 toml 参数），真机验证 GPU 是否比 v6 有改善。
3. **CI payload 参数修正**：`board.toml` 里 `setup-skip=1`、缺 `boot-ui-chrono`、`smgop=1` 这几项要么改对、要么拼镜像时覆盖。
4. 三个老问题：显示撕裂（单缓冲固有）、DPU 直连（显示流未注册）、GPU 用户态。

## 9. 索引

| 路径 | 内容 |
| --- | --- |
| `r2/core/scripts/build-xnu-4k-baseline.sh` | 内核构建；板级 archive 全在 `BOOTSTRAP=0` 分支内 |
| `r2/core/scripts/unixnu/kernel.py` | 引导 pass 的容忍逻辑（195–206 行） |
| `r2/core/scripts/build-turnip-darwin.py` | Turnip 交叉编译；需要 bison >= 3.7 |
| `r2/core/scripts/build-adreno-metal-bridge.py` | Rust bridge；pin 与补丁顺序在此 |
| `r2/core/scripts/data/adreno-compiler-inputs.json` | mesa / bison / 补丁清单 |
| `r2/core/scripts/data/gpu-metal-vulkan-inputs.json` | metal2vulkan pin `555b13d5` |
| `.github/workflows/macos.yml` | 基线 CI |
| `.github/workflows/gpu-addon.yml` | addon CI（本次新建） |
