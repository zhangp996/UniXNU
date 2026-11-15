# 交接：2026-11-11 晚（instantnoodlep / iPadOS 17.7.11 21H461）

## 一句话状态

IPadOS 在这个端口上**完整启动到服务图**：内核、根卷、SystemOS cryptex、3 GiB Data、launchd、
蓝牙/公平播放/文件系统服务全部正常。**唯一的失败**是三个进程在用户态执行 `brk` 陷阱：
`watchdogd`、`liveactivitiesd`、`SpringBoard`。

## 已验证可用

| 项 | 值 |
| --- | --- |
| 镜像 | `D:\新建文件夹\instantnoodlep-artifacts\bootET.img`（`15512e3c3d81d0a60cca60b640487144`） |
| 内存窗口 | `owned_memory_size = "0x100000000"`（4 GiB，文档上限 8 GiB） |
| AMFI 平台策略 | `[adt.chosen.u32] "amfi-only-platform-code" = 0` |
| 种子 | `owner-data-3g.dbr`（真实 CI 扩容 3 GiB） |
| 串口 | COM14 @115200，**必须 `DtrEnable=$true`**，且**启动后 50 秒再打开**（早开会导致卡死） |

## 启动/抓日志的固定流程

```powershell
# 1) 烧写（只 fastboot boot，绝不 flash 固件）
fastboot boot bootET.img
# 2) 等 50-60 秒（让系统跑起来、串口重新枚举）
# 3) 打开串口，开 DTR，持续读 40 秒
$s = New-Object System.IO.Ports.SerialPort 'COM14',115200,'None',8,'One'
$s.DtrEnable = $true; $s.Open()
```

日志落在 `D:\新建文件夹\instantnoodlep-artifacts\console-*.log`。

## 今天解决的七件事（都写进了 docs/evidence/2026-11-11-watchdog-crossed.md）

1. `watchdogd` 的临界退出 panic → 社区 RamWatchdog 后端（`ram-system-watchdog=1`）
2. 屏幕无输出 → `ml_io_map`（2 参数）+ `vcattach` 里切 `kPETextScreen`；另加清屏与 2 倍字体
3. 串口读不到 → 模块 `usb/Dwc3.c::pump()` 只在主机 **DTR** 置位时提交 IN 传输
4. Data 卷不可写 → 3 GiB 种子 + 只选分区媒体（`selectVolumeSeed` 要求 UUID 唯一）+ 阈值 2→4 GiB
5. 服务成片 SIGKILL → 解码 exit reason：`ns=1` 是 **JETSAM**
6. jetsam 风暴 → **4 GiB 窗口**后完全消失（`vm_page_bootstrap: 992422 free pages`）
7. 剩余三个 SIGTRAP → `handle_breakpoint` 打印 PC/ESR：`watchdogd +0x50b0c brk #1`、
   `liveactivitiesd 0x1d2dea4f8 brk #1`、`SpringBoard 0x1b08d32ec brk #0`

## 下一步：把 PC 解析成镜像

事实：dyld 共享区域是 `0x1f7800000 -> 0x1fcc00000`，而 `0x1b08d32ec`、`0x1d2dea4f8` **低于**它，
所以它们属于**进程私有映射**，不在共享缓存里。liveactivitiesd 两个实例的地址完全相同，说明该映射
没有随机化。

三条候选路线：

1. **内核钩子解析 vnode 路径**：在 `handle_breakpoint` 里对 `pc` 做 `vm_map` 查找，取当前进程的
   映像列表（`p->p_vmspace` → `vm_map` → entry → `vnode_getpath`），打印命中镜像的路径。最直接。
2. **dyld 打印**：启动参数里打开 dyld 的镜像打印（例如 `DYLD_PRINT_SEGMENTS` 等），让每个进程把
   自己的镜像基址报到控制台。
3. **brk 注释**：`brk #1` 的 ISS 只带注释号 1；若 Swift 运行时用 `brk #0x100` 明确标识，可以绕过
   库解析直接知道是 Swift 的 `fatalError`。

## 工作树与构建

```sh
cd /home/dev/unixnu/UniXNU
export XNU_EXTRA_LD_ARCHIVES="-Wl,-force_load,$PWD/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/tree/build/ram-watchdog/libRamWatchdog.a"
./unixnu build kernel -D ../UniXNU-instantnoodlep      # 注意：路径必须绝对，否则链接时报 file not found
./unixnu build payload -D ../UniXNU-instantnoodlep --kernel <patched kernel> \
  --unique-chip-id 0123456789abcdef --trust-cache out/ipsw/21H461/trust.bundle \
  --data-ramdisk out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/data/owner-data-3g.dbr
```

**树的坑**：`tree/.cache/xnu-work` 是物化缓存；对它 `git reset --hard` 会让工具以为补丁已应用而跳过，
从而编译失败（`mockfs_fsnode.c` 的 `kalloc_type` 报错）。修复：`rm -rf` 该目录后重跑 `build kernel`。

## 设备仓库补丁（13 个）

```
zzz-boot-progress（arch 系列带入）      zzz-defer-framebuffer-mapping
zzz-arm-panic-region-guard / -late      zzz-qcom-usb-debug-console / -worker
zzz-root-unverified                      zzz-qcom-usb-report
zzz-owner-report                         zzz-owner-watchdog-override
zzz-exit-service-release-path            zzz-exit-observe
zzz-exit-reason-text                     zzz-trap-pc
zzz-shared-region-probe
```

外加两个 AMFI 二进制的**校验式**补丁脚本（在 payload 构建前对内核副本执行）：

* `scripts/patch-amfi-nopmgr.py` — 跳过无 PMGR 的 panic
* `scripts/patch-amfi-trustcache-gate.py` — `0xfffffff004ab73b8` 的 `tbz` 改 `nop`，允许任何进程查询
  trust cache（bring-up 用）

## 纪律

* 只 `fastboot boot`；需要写 userdata 时才 `fastboot flash userdata`。**绝不**刷 xbl/abl/tz。
* 进过 Android 就必须重刷 userdata（会清掉根 manifest，报 `UFS ROOT FAILURE`）。
* Windows 读不到 WSL 路径，产物要复制到 `D:\新建文件夹\instantnoodlep-artifacts\`。


## 18:00 补充：PC 已经解析成镜像（路线 1 完成）

路线 1 已实现、构建并在设备上跑过。证据与命令在 `docs/evidence/2026-11-11-trap-image.md`。

- 新补丁 `zzz-trap-image.patch`（`patches/xnu/series` 里排在 `zzz-trap-pc.patch` 之后）：
  - `bsd/kern/kern_mman.c`：`native_trap_image()` 打印 `XNU-TRAP-IMAGE`（`find_region_details()` +
    `vnode_getwithvid()`/`vn_getpath()`，只对文件映射有效）和 `XNU-DYLD-IMAGE`（读任务的 dyld 映像表
    `task_get_all_image_info_addr()` + `copyin()`，共享缓存里的映像也能命名）；
  - `osfmk/arm64/sleh.c`：`handle_breakpoint()` 在 `exception_triage()` 之前调用一次。
- 镜像：内核 `d5a69b98…`，AMFI 后 `a498a1ea…`，payload `95c82aa4…`，
  `bootEU.img` `56ec8037…`（183,005,184 字节）。三次抓日志结论一致。
- 三个 trap 的归属：

| 进程 | 命中 | 指令 |
| --- | --- | --- |
| watchdogd | `/usr/libexec/watchdogd+0xab0c` | `brk #1`，字符串 `connect_to_kext() unexpectedly failed - unable to lookup watchdog KEXT service` |
| liveactivitiesd | `SessionSyncEngine+0x1d4f8` | `cbz x20, <brk #1>` 的空分支：Swift 强制解包 nil |
| SpringBoard | `FrontBoardServices+0x1e2ec` | `brk #0`，紧跟一个编译器当作 noreturn 的调用之后：致命错误 helper 返回了 |

- 结论修正：这两个库不是进程私有映射，而是 iOS 17 **拆分共享缓存**的子缓存（`.09`、`.21`）。
  根卷上 `FrontBoardServices.framework` 只有 `Info.plist` 和 `_CodeSignature`；缓存本体在
  SystemOS cryptex 的 `System/Library/Caches/com.apple.dyld/`（1 个 491 KB 的映像表 + 52 个子缓存 +
  685 MB 的 `.symbols`）。

**下一步**：

1. `watchdogd`：发布 `connect_to_kext()` 期望的 watchdog user client（`AppleARMWatchdogTimer` 那类），
   或让 `RamWatchDog` 回答它；这条不需要缓存工具。
2. 两个缓存 PC 的符号：映像表 → 子缓存映射 → 文件偏移这条链已经打通（本文档里的两个文件偏移就是
   这样算出来的），缺的是 `.symbols` 的读取器（7-Zip 26.04 报 "Unsupported Method"）。

## 18:30 补充：官方记录给了符号化方法，三个 PC 全部定位

- 官方 `docs/devices/sirius-huajifruit.md`（同代 21H461 的参考移植）记着：`ipsw dyld a2s` 反查，
  且 `.symbols`、`.34/.52.dyldlinkedit` 这三个文件是 **LZBITMAP 压缩**，要用 **apfs-fuse** 解。
  照做成功（7-Zip 26.04 对这三个报 "Unsupported Method"）。两个注意点：缓存要放在**本地盘**上跑
  ipsw（9p 挂载一律 invalid argument）；`ipsw dsc disass` 会把整份缓存读进内存而 OOM，用 `a2s`/`a2f`。
- 结果（unslid 地址 = 镜像表地址 + 镜像内偏移）：

| 进程 | 符号 | 结论 |
| --- | --- | --- |
| SpringBoard | `-[FBSDisplayMonitor _initWithBookendObserver:transformer:] + 1148` | 走 `__bs_set_crash_log_message("failed to find the main CADisplay")` 后 trap：FBS 主动崩溃，不是错绑 |
| liveactivitiesd | `SessionSyncEngine func_1c18f6274 + 644` | 局部 Swift thunk 的 nil 分支；参考移植把同一处记作 `SessionCore.Daemon.workloop` 的 Swift 运行时陷阱 |
| watchdogd | `connect_to_kext() unexpectedly failed - unable to lookup watchdog KEXT service` | 找不到看门狗 user client |

  SpringBoard 分支读出来的常量串：`failed to find the main CADisplay`、`failed to find the symbol for CADisplay`、
  `two CADisplays had the same displayID(%u)`——整个 `FBSDisplayMonitor` 初始化都在找主 CADisplay。

- 新补丁 `zzz-os-crash-observe.patch`：在 `abort_with_payload_internal()` 打印进程/原因/线程信号状态，
  用来验证“os_crash 返回了”的假设。内核 `4f0dea31`、镜像 `d0f26181`（`console-bootEV.log`）整次启动
  有同样的 11 条 `XNU-DYLD-IMAGE` 和同样的 trap，但**没有任何 XNU-OSCRASH 行**：这些进程根本没进
  `abort_with_payload` 系统调用，假设不成立。补丁本身无输出，留作以后验证真实 abort 也可撤。

**下一步**：

1. SpringBoard：给 CoreAnimation 一个主 `CADisplay`（IOMobileFramebuffer 那条线），否则 FBSDisplayMonitor 会一直崩。
2. liveactivitiesd：先对照参考移植的 `SessionCore.Daemon.workloop` 记录，再判断是不是板级问题。
3. watchdogd：发布 `connect_to_kext()` 期望的 user client，或让 `RamWatchdog` 回答。
