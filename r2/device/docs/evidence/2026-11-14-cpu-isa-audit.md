# CPU 与 ARM 指令集描述审计（2026-11-14）

审计范围：`docs/`、`patches/`、`board.toml`、`configs/`、框架 `docs/devices/`。

## 结论一览

| # | 主题 | 说法 A | 说法 B | 判定 |
| --- | --- | --- | --- | --- |
| 1 | 核心构成 | `zzz-max-cpus-8.patch`："四小 + 三大 + 一超大" | `2026-11-09-device-inventory.md`：cpu0-3 Kryo Silver(0x51/0x805)、**cpu4-7 全是 A77(0x41/0xd0d)** | **冲突** |
| 2 | 4/3/1 的归属 | 补丁注释暗示它属于 MPIDR 分组 | 框架 `docs/devices/mondrian.md`：4/3/1 是**缓存拓扑**，MPIDR 是**扁平 0x000..0x700** | **冲突（解释层面）** |
| 3 | 逻辑核映射 | 我们的 `sheng-smp-max` 按逻辑序号，隐含 logical = phys/0x100 | OP8 移植：`cpumask=0x13` + `ml_get_cpu_number(0x400)==2` | **不一致（待实测）** |
| 4 | ISA / PAC | 清单：Armv8.2-A、**无 FEAT_PAuth**、用 arm64 profile | 部分文档提到 arm64e | 内核 arm64，**一致**；arm64e 假设属其他设备 |
| 5 | MAX_CPUS / 簇 | 我们 `MAX_CPUS 8` / `MAX_CPU_CLUSTERS 1` | 扁平 MPIDR + 全 'P' 拓扑 | **一致** |

## 详细

### 1. 核心构成（真实冲突）

- `patches/xnu/zzz-max-cpus-8.patch:8`：`SM8250 has eight cores: four little, three big, one prime`
- `docs/evidence/2026-11-09-device-inventory.md:20-21`：
  `cpu0-3: implementer 0x51, part 0x805 (Kryo 585 Silver, A55 class)`
  `cpu4-7: implementer 0x41, part 0xd0d (Cortex-A77)`

`0xd0d` 是 Cortex-A77，Cortex-X1 是 `0xd44`。清单是按真机读数写的，因此 **cpu7 实际报的是 A77**，
与补丁注释的"一超大核"不符。两种可能：

1. 这台 IN2020 的 prime 核在 MIDR 上就报 A77（运营商/批次差异）；
2. 清单读数时把 cpu7 与 cpu4-6 合并记录了。

无论哪种，**补丁注释与实测清单冲突**，应以实测为准并注明 4/3/1 只是缓存/频率分组。

### 2. 4/3/1 的正确归属（解释冲突）

框架 `docs/devices/mondrian.md:123` 写得很清楚：

> SM8475 的硬件 MPIDR 是扁平的 0x000..0x700，Aff1 = 核序号。Android sysfs 里的 4/3/1（A510/A710/X2）是
> **L2/L3 缓存拓扑**，不是 MPIDR 亲和性；内核的 cluster-type 属性（全 'P'）把八个核塌缩成一个拓扑簇。

SM8250 同理。所以补丁注释把 4/3/1 挂在 `MAX_CPUS` 上描述**容易误导**——它不影响 MPIDR 或簇数。

### 3. 逻辑核映射（需要一次实测）

我们的门控 `zzz-smp-core-gate.patch` 用 `logical <= sheng-smp-max` 选择核心，隐含假设
`logical == phys / 0x100`。参考端口用 `cpumask=0x13` 并**显式校验** `ml_get_cpu_number(0x100)==1`、
`ml_get_cpu_number(0x400)==2`——说明在它那儿 **0x400 对应逻辑 2，而不是 4**。

若我们的板子也是这种映射，则 `sheng-smp-max=7` 覆盖的**不是全部 8 个物理核**。
验证方法：启动时打印 `ml_get_cpu_number(0x100*i)`（i=0..7）的实际返回值。

### 4. ISA / PAC（一致）

- 清单：`No FEAT_PAuth: the SoC is Armv8.2-A, so this board uses the arm64 profile, not arm64e`
- 我们构建的 XNU：arm64（非 arm64e）✓
- 操作系统选的是 **iPad7,1**（A10X，同样是 arm64 无 PAC），因此用户态也不需要 arm64e

其余文档里的 arm64e 说法属于其他设备或 source-storage 包，不构成本板冲突。

### 5. 簇数与拓扑（一致）

`MAX_CPU_CLUSTERS 1` 与「全 'P'、扁平 MPIDR」自洽；`cpu_regs = 0,0x100,...,0x700` 与扁平表一致。

## 建议动作

1. 改 `zzz-max-cpus-8.patch` 注释为：`8 cores, flat MPIDR 0x000..0x700; the 4/3/1 grouping is cache topology`；
2. 增加一次 `ml_get_cpu_number` 映射打印，确认 8 核选择覆盖全部物理核；
3. 若确认真机 cpu7 是 A77，则在设备清单里注明"prime 核 MIDR 报 A77"而不是 X1。

## 追加：ARM 架构特性寄存器是否被篡改（QEMU 结论的核对）

说法：若内核向用户态暴露的 ID_AA64ISAR0_EL1（LSE 原子）、CTR_EL0（缓存行）与实际硬件不符，
libdispatch 可能走到不同的原子操作路径，从而出现「消息入队却不唤醒」这类症状。

逐层核对，本端口不存在这个机制。

### 固件（Silicium SEC, uefi-instantnoodlep）

SetupExceptionLevel2 只做三件事：
- sctlr_el2 = 0
- hcr_el2 |= FIQ|IRQ|SERROR（把中断路由到 EL2）
- cptr_el2 = AARCH64_CPTR_DEFAULT（放行 FP/SIMD）
没有设置 HCR_EL2.TID0/TID2/TID3，因此 ID 寄存器不被 trap。
ArmLib.c 用 CTR_EL0.DminLine 做缓存维护，这是读硬件，不是覆盖。

### 内核（XNU）

- ID_AA64ISAR0_EL1 只在 proc_reg.h 定义位域，没有任何写入
- CTR_EL0 同样没有写入
- SCTLR_UCT_ENABLED（bit 15，trap 用户态读 CTR_EL0）定义了但从未使用
- arm64 代码里完全没有 HCR_EL2 操作（EL2 不归内核管）

结论：用户态读到的就是硬件值，内核没有中间层。

### 板级常量

board_config.h（BCM2837 段）MAX_L2_CLINE = 6，即 64 字节，与 SM8250 的 A77/A55 L1D 一致，
不存在 Apple 128 字节假设。

### 结论

这条假设在本端口不成立：从 SEC 固件到 XNU 再到用户态，两个寄存器都是直接读硬件。
若 QEMU 夹具观察到差异，更可能是夹具自身的 CPU 模型不同，而不是真机路径。

### 仍值得做的一次确认

真机早期打印 ID_AA64ISAR0_EL1 与 CTR_EL0 原始值，确认：
- ID_AA64ISAR0_EL1.Atomic（bits 23:20）>= 2，即支持 LSE
- CTR_EL0.DminLine（bits 19:16）= 4，即 64 字节

## 01:10 (Nov 14): the CPU/ISA audit items are closed

Three changes, all measured:

1. **`zzz-max-cpus-8.patch` comment** now says:
   `8 cores, flat MPIDR 0x000..0x700; the 4/3/1 grouping is cache topology`
2. **A CPU mapping print** runs once from the board probe (`print_cpu_map_once` in
   `package/LegacyDisplayProbe.cpp`); the first boot with it shows an identity mapping:

       CPUMAP phys=0x000 logical=0
       CPUMAP phys=0x100 logical=1
       CPUMAP phys=0x200 logical=2
       CPUMAP phys=0x300 logical=3
       CPUMAP phys=0x400 logical=4
       CPUMAP phys=0x500 logical=5
       CPUMAP phys=0x600 logical=6
       CPUMAP phys=0x700 logical=7

   so `sheng-smp-max=7` **does** cover every physical core on this board, and the sibling port's
   `ml_get_cpu_number(0x400) == 2` does not hold here.
3. **The device inventory** now notes that the prime core's MIDR reports **A77**, not X1, and that the 4/3/1 grouping
   belongs to cache topology.

Image: `72cba24899ac3fcaf27d260bc8a1b37ef9caa6316f366320de7e2bd6bfb9ff86`.

A useful negative result: `ml_cpu_init_completed()` is **never called** in this XNU tree (only declared and defined),
so the first attempt at the mapping print, placed there, produced nothing at all - the probe is the reliable place for
board-time instrumentation.