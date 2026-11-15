# 8 核基线（R2 baseline with all eight cores）

2026-11-16 起，R2 基线的 SMP 配置改为 **8 核**：

- 参数：在原基线 args 中加入 `sheng-smp-max=7`（args 982 → 998 字节，仍 ≤ 1000 字节规则）。
- 语义：SMP 门控 `selected = logical == boot || (sheng-smp && logical <= sheng-smp-max)`，
  因此 logical 0..7 全部被选中并拉起。
- 验证：`boot-8core.img`（原基线 + `sheng-smp-max=7`）在真机 600 秒：
  0 panic / 0 invalid argument / 0 I/O timeout，SpringBoard 正常。
- 仓库配置：`r2/device/board.toml` 的 `[payload]` kernel_args 已加入 `"sheng-smp-max=7"`，
  CI 构建出的 payload 默认就是 8 核基线。

## 重建方法

```
python3 ./unixnu build payload -D device ...   # args 直接来自 board.toml（含 sheng-smp-max=7）
```

或对已有 payload 用 mtools 在 args 中插入 ` sheng-smp-max=7`（原 args 982 字节 → 998 字节）。

## 硬件证据

SMP 门控的计数器（`zzz-smp-core-gate.patch`）已在逻辑 CPU 选择时打印：

```
SMPGATE logical=0 selected=1 eligible=1 withheld=0
...
SMPGATE logical=7 selected=1 eligible=8 withheld=0
```

`eligible=8 / withheld=0` 出现在串口日志中即表示 8 个 CPU nub 全部被注册。
