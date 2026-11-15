# GPU 阶段：参考 ye3912 的 in2020 仓库，移植现状与下一步

日期：2026-11-14。参考账号 `ye3912` 的三个仓库：

| 仓库 | 分支 | 内容 |
| --- | --- | --- |
| `unixnu-oneplus-8-pro` | `codex/metal-r1-baseline-20261113` (c986bfc) | 设备端口 R1 快照：87 个 `sm8250-*` patch（含 r59-r74 Metal 阶梯）、`package/Sm8250Alioth*.cpp`（A650 GPU 驱动）、`support/a650-reference` |
| `UniXNU-instantnoodle` | `codex/in2020-gpu-r3-20261113` (46a4569) | 框架快照；CI 产出 iOS 17 GPU userspace |
| `apfs` | main | Data seed 重建工作流 |

## 已经拿到的现成产物

`UniXNU-instantnoodle` 的 Actions artifact：

| artifact | id | 大小 | 内容 |
| --- | --- | --- | --- |
| `in2020-gpu-r3-ios17` | 14012210733 | 13,563,043 B | `gpuaddon.bin`（NGPUADD2，6 个文件，13,665,760 B，sha256 `ccd764ab…`）+ `adreno-metal-plugin/trust.json` + `in2020-build-inputs.json` |
| `in2020-gpu-r3-diagnostics` | 14012310249 | 43,530 B | .cache/build 诊断 |

`gpuaddon.bin` 内容（`make-gpu-addon.py` 的 NGPUADD2 布局，与本框架一致）：

```
755 22052080 addons/NativeAdrenoMetal.bundle/NativeAdrenoMetal
755 17296128 addons/NativeAdrenoMetal.bundle/libvulkan_freedreno.dylib
644     2310 addons/NativeAdrenoMetal.bundle/gpu-build.json
644      622 addons/NativeAdrenoMetal.bundle/Info.plist
644     2256 addons/NativeAdrenoMetal.bundle/_CodeSignature/CodeResources
644        2 addons/NativeAdrenoMetal.publish
```

trust.json：plugin cdhash `0804e32473d2722dbe869f41040b8fea7cc0ceaa`，turnip cdhash
`234f3a73c72033479ef977c806b8f55c67071998`（daemon `bd53013d…` 不在 addon 里）。

## 本端口已经有的东西（不用重写）

| 环节 | 位置 |
| --- | --- |
| A650 内核驱动 | `drivers/Adreno650/`（`NativeAdrenoAccelerator.cpp` = `AGXAccelerator : IOAcceleratorES`，`NativeAdrenoPublish.cpp` 回答 `MetalPluginName`、服务插件 open type `'AG'`，`NativeAdrenoGmu*`/`NativeAdrenoClient` 等） |
| A650 GMU/ZAP/HFI | `package/A650Gmu.c` + `kits/AdrenoPM4`、`kits/AdrenoFirmware`、`kits/ArmSmmu4K` |
| 构建 | `scripts/build-adreno650.py`（已在 `[kernel] archives`，产出 `build/adreno650/libAdreno650.a`） |
| addon 通道 | framework `zzy-native-gpu-addon-mount.patch`：`/chosen/memory-map/GpuAddon` → 写入 `/private/var/ngpu` → bindfs union 挂 `/private/var/ngpu/addons` 到 `/System/Library/Extensions`，并检查 `NativeAdrenoMetal.bundle/Info.plist` |
| 信任 | `scripts/trust-append-module.py`、`scripts/make-owner-trust-bundle.py`（原 `trust.bundle` sha `7416ca4f…` 与 payload 用的同一个） |
| 插件源码 | `drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm`（本机不能编：framework 的 `build-adreno-metal-plugin.py`/`build-turnip-darwin.py` 走 `xcrun`，需要 macOS + iOS SDK；fakeroot 里没有 Metal.framework） |

## 三个缺口

1. **A650 归档没进内核**：`kernel.macho` 里没有 `_native_adreno_publish`（只有 `native_adreno_fences` 等 display 桩）。
   `build-kernel.sh` 只 force-load `libRamWatchdog.a`；`build-xnu-4k-baseline.sh` 把 `$XNU_EXTRA_LD_ARCHIVES`
   加进链接，所以 `build/adreno650/libAdreno650.a` 需要一条 `-Wl,-force_load,…` 才会进内核。
2. **没有发布调用点**：`native_adreno_publish(vm, submit, ring, …)` / `native_adreno_accelerator_allocate()`
   在本仓库的 `package/` 里没有任何调用者（J716F 的板级代码没跟过来）。参考端口用
   `sm8250_alioth_metal_register()`（`sm8250-metal-addon-r59.patch` 在 `bsd_init.c` 里挂）完成这一步。
3. **插件/turnip 信任**：payload 现在用原始 `trust.bundle`；要换成"原始 + plugin/turnip cdhash"的 owner bundle。

## 两条可行路线

**路线 A（推荐，工作量小）**：把现成的 r3 addon 与 owner trust 接进现有 payload，再把路线 B 的内核发布挂钩补上。
先用已有内核看 addon 是否被写入、`NativeAdrenoMetal.bundle` 是否出现在 `/System/Library/Extensions`，
以及 `backboardd` 是否读到 `MetalPluginName`。

```sh
# 1) owner trust：原始 bundle + plugin/turnip cdhash
python3 scripts/trust-append-module.py out/ipsw/21H461/trust.bundle \
  out/instantnoodlep/.../payload/owner-trust.bundle \
  0804e32473d2722dbe869f41040b8fea7cc0ceaa 234f3a73c72033479ef977c806b8f55c67071998
# 2) payload 带 addon 与 owner trust
unixnu build payload -D ../UniXNU-instantnoodlep --kernel /tmp/amfiEU/kernel.macho \
  --unique-chip-id 0123456789abcdef --trust-cache .../payload/owner-trust.bundle \
  --data-ramdisk .../data/owner-data-3g.dbr --gpu-addon .../gpuaddon.bin
```

**路线 B（把 ye3912 的 A650 发布层移植过来）**：以他们的 `package/Sm8250Alioth*.cpp`（`Sm8250AliothMetal.cpp`
发布、`Sm8250AliothGmu.h`/`Zap.cpp` GMU+ZAP、`Sm8250AliothCold.cpp` CX）为蓝本，或者用本仓库
`drivers/Adreno650` 已有的 `native_adreno_publish()`，在板级 probe（`LegacyDisplayProbe.cpp` 的
`native_legacy_display_probe`/之后的 tick 里）加一个 GPU 发布钩子：

1. `build_kernel.sh` 增加 `-Wl,-force_load,$TREE/build/adreno650/libAdreno650.a`；
2. 在 GPU 上电 + GMU 起来之后分配 `AGXAccelerator` 并调用 `native_adreno_publish(vm, submit, ring, …)`
   （参考 `sm8250-metal-addon-r59.patch` 的 stage 打印与回滚逻辑）；
3. 用他们的阶梯 r59-r74 里与发布相关的三项：`sm8250-metal-addon-r59`、`sm8250-a650-revoke-cpu-mappings-r67`、
   `sm8250-a650-pager-ownership-r71`。

## 现状总结

- 用户态 GPU 件（turnip + 插件）**不用自己编**：r3 artifact 直接可用，且 addon 格式与本框架一致。
- 内核里 A650 驱动的**源码齐全、已能编译**，缺的是"force-load 进内核 + 板级发布调用"这两小步。
- 一旦 `AGXAccelerator` 发布并回答 `MetalPluginName=NativeAdrenoMetal`，backboardd 的 Metal 发现就会加载
  `/System/Library/Extensions/NativeAdrenoMetal.bundle`（extensions union 已由 addon 挂好）。

## 上机结果（2026-11-14 19:26，bootEU.img sha `9090adbd…`）

内核已带 A650 驱动链接（sha `30bc7274…`，payload 里 AMFI 补丁后 `b28d835f…`），addon 为
`gpu-addon-merged.bin`（14,438,888 B，sha `0bb3e82d…`：launchd cache 4 个文件 + GPU 插件 6 个文件），
信任为 `owner-trust.bundle`（sha `b440aad2…`）。

日志（`console-gpu1.log`）确认：

```
launchd cache: mode=1 mount stage=5 status=0 visible=887340 error=0
Using unsecure cache: /System/Library/xpc/launchd.plist
Cache sent to launchd successfully
attempting to load 3 external trust cache modules      <- 第 3 个就是我们加的 owner 模块
DISPLAY: present copied 2376 rows x 4320 bytes (frame 1/2/3)
```

无 panic、无 boot task 失败。但日志里**没有任何** GPU/NativeAdreno/MetalPluginName 输出：
加速器没有被注册，因为没有代码调用 `native_adreno_publish()` —— 这符合预期。

## 下一步：板级 GPU 上电与发布钩子

内核侧已具备（`drivers/Adreno650`）：`AGXAccelerator : IOAcceleratorES`、
`native_adreno_publish()` / `native_adreno_accelerator_allocate()`、`MetalPluginName` 回答、
插件 user client（open type `'A'/'G'`）、`NativeAdrenoGmuContextBringup()`、`NativeAdrenoGpuContextVm()`、
`A650Gmu.c`（ColdStart/BuildStartup）、`GpuPowerBoard.h`（CX/GDSC/时钟）。

要写的板级文件（参考 `_ref/oneplus8pro/package/`，共约 700 行）：

| 参考 | 行数 | 要点 |
| --- | --- | --- |
| `Sm8250AliothCold.cpp` | 191 | CX/GDSC 上电、regs 映射（ADT 里 gpu 节点的 base） |
| `Sm8250AliothZap.cpp` | 108 | ZAP shader 加载（`a650_sqe.fw`）|
| `Sm8250AliothCp.cpp` | 304 | CP 初始化 + ring + 时钟 |
| `Sm8250AliothGmu.h` | 94 | GMU 固件入口（`a650_gmu.bin`）|
| `Sm8250AliothMetal.cpp` | 65 | `sm8250_metal_register()`：`MetalPluginName=NativeAdrenoMetal`、`MetalPluginClassName=NativeAdrenoMetalDevice`、`NativeSandboxGrantProcess=backboardd,SpringBoard,iconservicesagent,…`、`registerService()` |
| `Sm8250AliothPublish.cpp` | 161 | `native_adreno_publish(vm, submit, ring, clock, timer, &info, 31, bits)` + 一次 PM4 自检提交 + `backend_ready` 门控 |

固件来源：`proprietary-files.txt` 已登记 `a650_gmu.bin`（sha `df79fa84…`）与 `a650_sqe.fw`
（sha `b4c27420…`）；`drivers/Adreno650/NativeAdrenoFirmwareDefaults.cpp` 用 weak 常量占位，
构建时要按 `build-adreno650.py` 的方式把真实镜像编进去（否则 `native_adreno_firmware_image_size` 为 0）。

建议顺序：
1. 先在 `LegacyDisplayProbe` 的 tick 探针里加一行诊断打印（addon 计数器、extensions union 的
   stage/status、`native_adreno_firmware_image_size`/`native_adreno_sqe_firmware_image_size`、
   `native_adreno_publish_stats()`、`native_adreno_published_service()`），一次启动就能确认固件是否编入；
2. 再实现 Cold/Zap/Gmu/Cp 四段上电与 GMU 启动，成功后才调 `native_adreno_publish` + `sm8250_metal_register`；
3. `gpu-metal=1` 作为总开关，失败时按 r59 的方式回滚并打印 stage。

## 存档（2026-11-14 19:45）：GPU 交付链路已验证，卡在固件镜像

### 上机验证（`console-gpu3.log`，bootEU.img sha `531787d2…`）

```
GPU: addon files=10 written=0 failed=0; extensions stage=5 status=0; accelerator=0 open=0 service=0
launchd cache: mode=1 mount stage=5 status=0 visible=887340 error=0
Cache sent to launchd successfully
attempting to load 3 external trust cache modules
```

- addon 10 个条目（launchd cache 4 + GPU 插件 6）全部被内核识别；`written=0` 是上一轮已写入
  Data 卷且内容一致，`failed=0`；
- `/System/Library/Extensions` 的 bindfs union 挂载成功（stage 5），
  `NativeAdrenoMetal.bundle/Info.plist` 与密封层 `AGXMetalA10.bundle` 同时可见；
- 加速器未注册（`accelerator=0`），符合预期：发布钩子尚未实现。

### 内核里已有的 GPU 侧符号（kernel `3a450574…`）

`_native_adreno_publish`、`_native_adreno_accelerator_allocate`、`_native_adreno_published_service`、
`_native_adreno_new_user_client`、字符串 `MetalPluginName` / `NativeAdrenoGpuPublished`、
`NativeAdrenoGmuContextBringup`；A650 归档以 `-force_load` 链接，`MiniAcceleratorES.o` 在
`build/adreno650/libAdreno650.a` 存在时自动让路。

### 硬前置条件：A650 固件镜像缺失

`proprietary-files.txt` 登记了 `a650_gmu.bin`（sha `df79fa84…`）与 `a650_sqe.fw`（sha `b4c27420…`），
但**磁盘上没有这两个文件**，树里也没有把它们编成 `native_gpu_gmu_firmware[]` /
`native_adreno_firmware_image[]` 的构建步骤。没有固件 → GMU 微码无法加载、ZAP 起不来 →
`native_adreno_publish()` 无法成功 → 加速器不注册 → 插件不会被 backboardd 加载。

获得方式（待定）：
1. 从原厂 vendor 分区 dump（sha256 可校验）；
2. 从公开 vendor blob 仓库 sparse 取 instantnoodlep 的两个文件。

### 固件到位后的实现清单（约 700 行，参考 `_ref/oneplus8pro/package/`）

| 参考文件 | 行数 | 作用 |
| --- | --- | --- |
| `Sm8250AliothCold.cpp` | 191 | CX/GDSC 上电、时钟、regs 映射（`ml_io_map_with_prot`） |
| `Sm8250AliothZap.cpp` | 108 | ZAP shader（`a650_sqe.fw`） |
| `A650Gmu.c` + `Sm8250AliothGmu.h` | 94 | GMU 微码（`a650_gmu.bin`）与 HFI |
| `Sm8250AliothCp.cpp` | 304 | CP 初始化 + ring + `armcpu_spi_attach` |
| `Sm8250AliothPublish.cpp` | 161 | `native_adreno_publish(vm, submit, ring, clock, timer, &info, 31, bits)` + PM4 自检 |
| `Sm8250AliothMetal.cpp` | 65 | `sm8250_metal_register()`：`MetalPluginName=NativeAdrenoMetal`、`MetalPluginClassName=NativeAdrenoMetalDevice`、`NativeSandboxGrantProcess=backboardd,SpringBoard,…` |

相机可用的验证信号：`GPU: … accelerator=<非0> …`、日志出现 `NativeAdrenoGpu` 服务、backboardd 读取
`MetalPluginName`、SpringBoard 从 `ipc_mqueue_receive` 转为提交帧。

### 本轮产物

| 项目 | 值 |
| --- | --- |
| kernel | `3a450574f963f0db…`（A650 + 探针） |
| payload / 镜像 | `963945e3…` / `bootEU.img` `531787d2…`（199,782,400 B） |
| addon | merged `0bb3e82d…`（14,438,888 B） |
| owner 信任 | `b440aad2…`（3 modules） |
| 计划与证据 | 本文档、`console-gpu1/2/3.log`、`_ref/oneplus8pro`、`gpuaddon-r3.bin` |

## 上机进展 2（2026-11-14 23:2x）：GPU 上电链路跑通，卡在 GMU 内存映射

固件从 `D:\迅雷下载\OnePlus8ProHydrogen_15.H.45_OTA_0450_all_2202142225\images\vendor.img` 用
`debugfs` 抽出（`a650_gmu.bin` sha256 与 `proprietary-files.txt` 完全一致；`a650_sqe.fw` 来自
HydrogenOS 12，哈希与记录值不同；`a650_zap.elf` 布局与参考端口期望的一致），放在
`package/firmware/`，由 `scripts/build-adreno650.py` 生成 `native_gpu_gmu_firmware[]` 等数组编入归档。

参考端口的五段板级胶水（`Sm8250AliothCold/Zap/Cp/Publish/Metal`）连同 `GpuPowerBoardAlioth.h`、
`Sm8250AliothGmu.h`、`Sm8250AliothCpTables.h` 已进入 `package/`；新写的
`package/Sm8250GpuBringup.cpp` 负责映射 7 个寄存器窗口、跑 `gpb_run(mode 3)` + `gpb_gmu_clocks_on`，
再调 `sm8250_alioth_gmu_cold_start(12, &io)` 与 `sm8250_alioth_metal_register()`，由
`LegacyDisplayProbe` 在 tick 24（`gpu-metal=1`）触发。

上机日志（`console-gpu5/6.log`）：

```
GPU: power gcc=1 cc=1 smmu=1 cfg_ahb=00000001>00000001 cx_gdscr=f8222000 hwctrl=80001fe0 cx=1 clocks=1 err=0
GPU: gmu clocks step=5 failed=0 … cx_gmu=00004ff1 ahb=00000001 cxo=00000001 axi=00100001 memnoc=00004221
GPU: A650 cold start mode 12
GPU-DIAG vm-map batch=7 spans=1
GPU-DIAG mapInto[0] iova=4000 bytes=16760832 spans=1 status=e00002ca
GPU54 mode7 result attach=e00002ca step=6 phase=5 routes=2
```

含义：GCC AHB 时钟、GPUCC 可读、**CX 电源域上电**、SMMU 投票时钟、**GMU 四个时钟步骤全部通过**、
SMMU 上下文（分配/编程/路由，routes=2 已校验）都成功；失败在 **Map 步骤**，把第一块 GMU 内存
（icache，16 MiB @ iova 0x4000）写进上下文表时，`ArmSmmu4KLiveUpdateContextBatch` 返回
**7 = ArmSmmu4KStateMismatch**，`NativeAdrenoVm::fail()` 统一转成 `kIOReturnIOError`
（0xe00002ca）。

下一步：在 `ArmSmmu4KLiveUpdateContextBatch` 的 StateMismatch 点打印 `ArmSmmu4KLiveResult`
（descriptor_reads/writes、phase），确认是"表里已有非零项"还是"上下文寄存器镜像与追踪状态不符"；
也可能是本仓库 `drivers/Adreno650`（J716F 血统）与参考端口所用版本在 GMU 上下文布局上有差异。
诊断用 `GPU-DIAG` 打印已在 `NativeAdrenoGmuMemory.cpp`、`NativeAdrenoVm.cpp` 中。

## R2 显示驱动：生成式 ABI 是缺件（2026-11-15 00:25）

结论链（已逐项验证）：

1. 能编译的显示驱动是 support/gop-r20/NativeLegacyDisplay.cpp，sha256 09d7811db8fc6b659e12a05690867326e851f0d11bec7ffbb36b72c33ea035ca；
   scripts/display-tools/build_display.py 用该哈希把它和 NativeLegacyRequest.h（81e89e1f40f2f1d0…）钉死。
   support/UniXNU/drivers/GopFramebuffer/ 那份是 9d3c7681…，即 SOURCE-SNAPSHOT.md 中“被回退成 R1 备份”的版本。
2. LEAF(345) override 的真正原因既不是驱动也不是配方，而是 ABI 头必须在构建时生成：
   scripts/display-tools/audit_display.py 从 kernelcache 的 IOMobileGraphicsFamily 审计出 358 槽、
   9 个 abstract leaf（245, 246, 306, 307, 337, 338, 345, …），再用配方中的 header 模板生成
   OriginalLegacyFramebufferABI.h（21,362 B，181 处 opaque）。仓库签入的 OriginalMobileFramebufferABI.h
   只是占位，没有 opaque345，因此任何“只拷文件”的组合都编不过。
3. 生成方法（已验证）：
     SMGOP_BUILD_TREE=<tree> SMGOP_OUTPUT=<tree>/build/display-audit SMGOP_REFERENCE_DIR=<tree>/scripts \
       python3 <tree>/scripts/audit_display.py
   生成 <tree>/build/display-audit/OriginalLegacyFramebufferABI.h 与 abi.json；
   把头拷到 drivers/GopFramebuffer/OriginalLegacyFramebufferABI.h，配合 R2 驱动源码与我们自己的 build-arm-driver.py，
   即可得到：NativeLegacyDisplay: -Werror build and actual ARM64/iOS 17 object audit passed。
4. 新阻塞点（下一步）：内核链接报 Undefined symbols: IOBlockStorageDevice::…，来自 libShengUfs.a(NativeUfs.o)
   与 libRamBlock.a(NativeRamBlock.o)。这是 Apple IOStorageFamily 的类；显示归档加入内核链接后链接组成变化，
   把这两个归档拉进了内核。方向：让显示归档随 prelinked kext candidate 链接（不进内核），或在内核侧提供 IOStorageFamily 导出符号。

当前工作区已恢复到绿灯配置（原显示驱动 62,033 B + 原配方），上述生成步骤与哈希均已记录。

## 链接组成问题的实测（2026-11-15 00:30）

删除并重建 libShengUfs.a / libRamBlock.a 后，内核链接仍报同样一组未定义符号：
  IOBlockStorageDevice::{gMetaClass, requestIdle, doSetPriority, init, doUnmap, ...}
  referenced from libShengUfs.a(NativeUfs.o) / libRamBlock.a(NativeRamBlock.o)
说明不是陈旧归档，而是 R2 显示归档（依赖原版 IOMobileFramebufferLegacy 的 358 槽位 ABI）加入内核链接后，
链接成员集合变化，把 UFS/RamBlock 归档带进了内核链接；这两个归档依赖 IOStorageFamily（kext），内核无此符号。

下一步两条路：
  A1. 让显示归档不进入内核链接，而是随 prelinked kext candidate 一起链接（保持内核链接集合不变）；
  A2. 在内核侧为 IOBlockStorageDevice 提供 weak stub（与之前 GPU 的 boot_log_export_put /
      native_gop_handoff_call 同一手法），代价是 UFS/RamBlock 在被强制加载时会引用空实现。
推荐 A1：不动内核链接集合，风险最小。

## A1 排查进展（2026-11-15 00:35）

1. tree/build/native-legacy-display/original-bindings.o 定义 194 个符号，其中**没有**任何 IOBlockStorage*/IOStorage* 符号；
   我们的 build-gop-legacy-display.py 里也没有存储相关的桩或绑定（grep 无命中）。
2. 说明 IOBlockStorageDevice 这一组定义来自归档计划里的其他配方（board.toml archives 中的
   component:KonaUfs / ufs-archive-alias.py / component:RamBlock 一带），R2 显示归档改变了归档计划的
   执行顺序/成员，使那组定义不再进入内核链接。
3. 下一步（A1 的落点）：
   a. 对比 R1(旧驱动) 与 R2 两次内核链接命令（logs/kernel.log 的 LD 行），列出 -force_load 的归档清单差异；
      差异项就是被 R2 挤掉的那一组存储绑定，把它补回 board.toml archives 的顺序即可（最小改动）；
   b. 若清单无差异，则对比两次 archive 产物的符号表（llvm-nm --defined-only）找出提供 IOBlockStorageDevice 的归档，
      确认它为何在 R2 构建中未产出该符号；
   c. 兜底：在内核侧为该符号集提供 weak stub（A2），仅当 a/b 无法解释时才用。

当前工作区：绿灯配置（原显示驱动 62,033 B）；R2 驱动源码与生成式 ABI 均在 _ref 与 build/display-audit 中可复用。

## 工作区漂移与恢复指引（2026-11-15 00:40）

R2 显示驱动实验期间对构建树做了多处改动（kits/sheng 布局、build-arm-driver.py 配方、
generated ABI、kits 替换、显示构建目录清理等）。恢复绿论文档时出现新的归档失败：
  PASS: original Legacy has 358 slots with nine abstract leaves
  NativeLegacyDisplay: -Werror build and actual ARM64/iOS 17 object audit passed
  No unresolved original abstract backend target   ← 归档在此退出（root archive 24,808 B 配方）
清除 tree/build/native-legacy-display 与 tree/kits/sheng 后仍复现，说明是构建树状态与仓库配方不一致。

恢复步骤（下一轮先做）：
  1. 重新生成构建树：从设备仓库重新执行 tree 准备（或直接删除
     <out>/.../tree 后重跑 unixnu build kernel，框架会按仓库重新铺开）；
  2. 确认树内 scripts/build-gop-legacy-display.py 与设备仓库一致（24,808 B），
     scripts/build-arm-driver.py 为框架原版（10,068 B）；
  3. 确认 drivers/GopFramebuffer 为仓库原版（NativeLegacyDisplay.cpp 62,033 B），
     且不存在 OriginalLegacyFramebufferABI.h；
  4. 先跑一次纯绿灯构建确认基线，再做 R2 对照（清 native-legacy-display + xnu-prelink 后切换）。

R2 复现所需的三件套（已验证可编译、审计通过）：
  - support/gop-r20/NativeLegacyDisplay.cpp（sha 09d7811d…）
  - build/display-audit/OriginalLegacyFramebufferABI.h（由 audit_display.py 生成，21,362 B）
  - 我们的 build-arm-driver.py + -Wno-missing-designated-field-initializers

## 显示归档失败点的精确定位（2026-11-15 00:50）

失败信息来自 build-gop-legacy-display.py:141（该检查一直存在，非 R2 实验引入）：
    require(i not in pure, 'No unresolved original abstract backend target')
含义：编译出的显示驱动 vtable 里，若某个 IOMobileFramebufferLegacy 方法落在 pure（原版类 9 个 abstract leaf）
索引上，说明该 abstract leaf 未被驱动覆盖 → 判定为“未解析的原版抽象后端目标”。

pure 由 scripts/audit-mobile-graphics-legacy.py 审计 tree/build/xnu-prelink/native-owner-mobile-graphics-candidate.macho
得到；而该 candidate 是上一轮构建的产物（循环依赖）。R2 实验后 warm 树里的 candidate 与当前驱动/配方不匹配，
pure 判定随之改变，于是原版 R1 驱动（62,033 B）也过不了这条检查。

结论：
  - 该检查要求的正是 R2 驱动的 LEAF(n) 方案（显式覆盖全部 abstract leaf），
    因此 R2 驱动（sha 09d7811d… + 生成的 OriginalLegacyFramebufferABI.h）是配方的目标驱动；
  - 原版驱动能否通过取决于 candidate/pure 是否与其一致，属于构建树状态问题，不应作为基线继续。

下一步：
  1) 直接采用 R2 驱动 + 生成式 ABI，按“清 native-legacy-display + 清 xnu-prelink → 构建 → 再构建”的顺序
     消除 candidate 循环依赖（第一次让 candidate 重新生成，第二次用新 candidate 验证）；
  2) 若内核链接仍缺 IOBlockStorageDevice，再按 A1-a（对比 -force_load 清单）或 A1-b（归档移到 candidate）处理；
  3) 备选 A2：给 IOBlockStorageDevice 提供 weak stub（手法同 boot_log_export_put）。

## A1/A2 真机结果（2026-11-15 01:00）

1. A2（IOStorageFamily 弱桩）：内核链接通过（kernel 660da49d…），但真机 panic：
   Couldn't attach class "NativeRamBlock"; panic Kernel data abort at pc 0xfffffff00bf0b338。
   原因：NativeRamBlock/NativeUfsBlock 是 IOBlockStorageDevice 的 C++ 子类，零值弱桩
   （gMetaClass=0、init=ret、vtable 全零）无法承担运行期语义。**A2 结论：不可行**。
2. R2 显示驱动本身在真机上注册成功：
   DISPLAY: registered family name=IOMobileFramebufferLegacy size=0x1a0
   DISPLAY: meta[0] NativeLegacyDisplay size=0x1940 super=…IOMobileFramebufferLegacy
   DISPLAY: probe entered (registered=1) / late probe (offline-mobile-graphics-load)
   → verify_swap 相关的 crash 不再出现，swap 契约问题已解决。
3. A1 尝试：从 board.toml archives 移除 component:KonaUfs / ufs-archive-alias.py /
   component:RamBlock 后，框架拒绝："[kernel] archives needs component entries for: KonaUfs, RamBlock"。
   说明归档归属由框架的 kernel 计划强制要求，不能简单从列表删除。

下一步（真正可执行的 A1 路径）：
  - 查框架 kernel 计划中 archives 的展开逻辑（grep "needs component entries" 定位来源），
    寻找把某个 component 归档只放进 payload/prelinked candidate 而不 force-load 进内核的开关；
    若有 per-archive 归属字段，则把 KonaUfs/RamBlock 标记为 payload-only；
  - 或者：给 kernel 侧提供**真实的** IOStorageFamily 符号（把原始 kext 的绑定以 original-bindings.o
    形式加入内核链接，类似显示驱动的 original-bindings 做法），而不是零值桩；
  - 期间保持可启动基线：R1 显示驱动 + 无弱桩（kernel 476a9cdf…）需要在 R1-era candidate 下重建，
    否则 abstract-leaf 检查会失败（该检查依赖 tree/build/xnu-prelink 的 candidate，属于循环依赖）。

工作区：board.toml 已恢复；R2 驱动与生成式 ABI 保留在 _ref 与 tree/build/display-audit。

## R2 + GPU 同内核首启结果（2026-11-15 01:05，log console-r2gpu.log）

成功的部分：
  DISPLAY: meta[0] NativeLegacyDisplay size=0x1940 super=IOMobileFramebufferLegacy   ← R2 驱动注册
  NativeRamBlock: attachStore gate: mode=2 … open=1 read=1 write=1 sync=1 gpt=1     ← 存储绑定生效（不再 panic）
  GPU: power … cx=1 clocks=1 / gmu clocks step=5 failed=0
  GPU54 status=0 fw=20010008 ready=1/1 ack=c6 / GPU55 on=1 rbbm=00000001
  GPU56 status=0 marker=a6500c0d rptr=1b wptr=1b fence=0
  GPU57 mode10 status=0 zap=1 secure=1 readback=a002

新 panic（GPU57 之后、metal register 之前）：
  Kernel data abort at pc 0xfffffff00bea69dc, lr 0xfffffff00bea6808
  符号化：pc → OSMetaClass::postModLoad(void*)+0x448，lr → 同函数 +0x274

解读：panic 发生在 OSMetaClass::postModLoad —— kext 模块加载时执行其 __mod_init_func /
静态构造器的路径。当前内核同时含 R2 显示驱动（其 __GLOBAL__sub_I_NativeLegacyDisplay.cpp 被配方
用 globalize-static-init.py 提出）与新的 IOAcceleratorES 注册；在该路径上某次静态初始化访问了
无效地址。下一步：反汇编该 PC 附近确认是哪个模块的静态初始化；重点检查 R2 显示驱动的 late 对象
里 __mod_init_func 的处理（我们的配方对 late 对象做了 --remove-section=__DATA,__mod_init_func +
globalize 处理，R2 的 late.o 可能不适配该处理方式）。

## IOStorage 绑定的边界（2026-11-15 01:15）

实验：绑定集去掉数据符号（gMetaClass、__ZTV20IOBlockStorageDevice），只保留 9 个函数地址。
结果：链接失败 —— 仅这两项未定义：
  __ZN20IOBlockStorageDevice10gMetaClassE
  __ZTV20IOBlockStorageDevice
说明：NativeRamBlock 的 MetaClass 构造与 vtable 必须引用数据符号，无法只绑函数绕开。

框架机制确认（scripts/unixnu/kernel.py:105-138）：
  required = {name for spec in specs if spec.get('build') and spec.get('build_stage','kernel')=='kernel'}
  missing = required - visited → 报 '[kernel] archives needs component entries for: …'
build_stage 只有这一处使用（没有 payload 消费者），把存储组件标成 payload 会导致它们完全不被构建，
因此 A1-a 的 build_stage 方案在这版框架里不存在。

结论：
  - 数据符号必须绑定（否则链接不过），但绑定后在 kext 加载前读到 0 → OSMetaClass::postModLoad 空指针，
    以及 fsck/存储路径卡住；当前内核 (0f45096f) 可启动到 launchd 但停在 boot task。
  - 真正干净的解法是让 UFS/RamBlock 驱动只存在于 prelinked kext（运行时由 IOStorageFamily kext 提供父类），
    这需要框架支持“归档阶段”或设备侧提供 payload 阶段的构建入口（当前版本没有）。

下一步建议（按可行性）：
  1. 在框架 kernel.py 增加归档阶段支持（spec build_stage='payload' 时跳过内核链接但仍在 payload 构建），
     board.toml 的 archives 条目保持完整以通过校验；
  2. 或者给 kernel.py 加一个排除列表（如 board.toml [kernel] link_archives_exclude），把存储归档排除出链接；
  3. 或者在设备侧把 NativeUfs/NativeRamBlock 改为不继承 IOBlockStorageDevice 的形态（工作量大，不推荐）。

## 配置层结论：框架不允许把存储驱动移出内核（2026-11-15 01:35）

为把 NativeUfs/NativeRamBlock 移到 kext 侧（A1），把 defconfig 里三个键翻成 n 后依次被三层守卫拦下：
  1) CONFIG_XNU_NATIVE_UFS=n, CONFIG_XNU_NATIVE_RAM_BLOCK=n 生效后：
     'Boot progress requires the audited UFS/owner-root and source-corecrypto build'
  2) 再关 CONFIG_XNU_BOOT_PROGRESS=n 后：
     'Owner root acceptance requires actual mounted-media binding and verified block storage'
结论：owner-root 信任链与 boot progress 都要求内核内建存储驱动，框架没有“存储只走 kext”的形态。
A1（配置层移除）不可行；A2（零值弱桩）已被真机 panic（NativeRamBlock attach / data abort）否定。

两个必须记住的框架坑（本次排查耗时最多的地方）：
  a) 构建读的是 <out>/instantnoodlep/<os>/<ipsw>/.config（config.py:167 config_path = work/'.config'），
     不是设备仓库的 configs/*_defconfig；改配置后必须：
       python3 ./unixnu defconfig instantnoodlep_defconfig -D ../UniXNU-instantnoodlep
  b) <tree>/build/xnu/ios-obj/*/.personality-cflags 按目录缓存 -DXNU_* 宏，改配置后必须删除，
     否则重新生成的仍是旧值（曾导致内核哈希不变、误以为配置生效）。

当前配置已回滚到绿灯值（UFS=y / RAM_BLOCK=y / BOOT_PROGRESS=y），
内核 0f45096f…（R2 显示 + A650 全栈 + 11 符号 IOStorage 绑定）可稳定重建。

下一步只剩一条路：对比“绿灯（R1 显示）”与“R2 显示”两次内核链接的 -force_load 归档清单，
找出被 R2 显示归档挤掉的提供存储父类符号的归档并补回链接（而不是自造绑定）。

## gMetaClass 与 postModLoad 的最后一轮定位（2026-11-15 01:55）

1. 内核自身已含 IOBlockStorageDevice 的绝对符号：kernel.macho 有
   fffffff006add1d0 A __ZN20IOBlockStorageDevice10gMetaClassE；两个 prelinked candidate 里该地址相同。
2. 去掉 libIOStorageBindings.a 后内核链接立刻报未定义 —— 说明链接期必须提供这些符号，
   prelink 的绝对符号在内核链接阶段不可见。
3. 从 candidate 镜像里读取该 VA 的内容：**值为 0**。即 kext 静态镜像里 gMetaClass 本就是 0，
   由 kext loader 在 OSMetaClass::postModLoad 阶段填入。
   → 绑定为 .set 指向该 VA 是正确做法，运行期的 0 是“kext 尚未加载”的正常状态；
     因此 postModLoad 的空指针 panic 未必来自 gMetaClass。
4. 检查显示归档 libNativeLegacyDisplay.late.a 的所有对象：**没有任何 __mod_init_func 段**，
   所以 panic 也不是来自 late 对象里的空 init 函数指针。

剩余两个假设（下一轮验证）：
  a) 我们的 globalize-static-init.py 对 R2 late 对象的处理把某个静态构造器移到了错误位置，
     导致 postModLoad 遍历到无效的模块描述符（可对比他们 build-arm-driver.py 对 late 对象的处理）；
  b) kext 加载顺序：IOStorageFamily 尚未加载时，内核里内建的存储驱动（UFS/RamBlock）的
     类注册（MetaClass 构造）会读到 0，需要把它们挪到 kext 加载之后（或禁用内核内建存储的类注册）。

当前工作区已恢复为可构建状态（ram-watchdog + adreno650 + io-storage-bindings），
对应内核 0f45096f…（R2 显示 + A650 全栈，真机启动到 launchd boot task）。
