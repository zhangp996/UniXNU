# 2026-11-16 会话产物与日志摘录

## 本地产物哈希（D:\新建文件夹\instantnoodlep-artifacts\）

| 文件 | 字节 | SHA256 |
| --- | --- | --- |
| bootEU.img | 199782400 | e6386a23c3f1815f0fbc98029d1f99f60f9dd0e2352d699e52ea9dde801f6ae1 |
| boot-8C-GPU.img | 199782400 | 6ba334a0fc52f046b6480191a34c5d620b8f92b0e6a3805098cf7898bf62057a |
| boot-8C-GPU-cpus.img | 199782400 | 24624be413698ee28fa709772df76aeabb1dad3124868ff814cd08bc2b8f7a85 |
| boot-8C-GPU-touch.img | 199782400 | 4a86a340df74ee312253761dfd92c953af4272f4108240deb2536070a7a9749c |
| boot-8C-GPU-quiet.img | 199782400 | 72964876e55af41617434c8e7c99bbae18df395e86f924dc5a84e47d9e038617 |
| boot-present6.img | 199782400 | d76137bcd47c871eef1fee83a81b224d5c886a0a0efe2530e6927d18451ae3d4 |
| boot-dpu.img | 199782400 | 2136944d5e615a3a90a917871761090ce636f23e480d77b26d3fef1f7a1e9147 |
| boot-8core.img | 199782400 | 1e6fb001cfef68dd8c1a967d3db124803bbfee1a5b35876eb4af79acb205f5bd |
| boot-skipsetup.img | 199782400 | 24667936a307b7b46b1e749b89b6e40377130a8d599ab086a82d2efac3c63e20 |
| boot-GPU.img | 199782400 | 0ed266e562dd6ba52549168ef8057bc8ea979fe6db333af04ca5aa8554ba4e61 |
| boot-displaydiag.img | 199782400 | ea31cc5a5dfb02949aaf80e10c3c5e59343a35805e1bb15b3165d5e00cbc6821 |

内核（AMFI 前 / 后）：8 核 4e85ffb4 / b3b0073d；触控 000027c8 / 711faf74；console-off fb1cea52；
present fa20c990 / 97ad027d；DPU af5bc697 / 1ee39fb9。

## 关键日志摘录

### 8 核 + GPU addon + Data（boot-8C-GPU-cpus 运行）
- SMP: cpus=8 boot=0 maxcpu=7
- GPU: addon files=6 written=0 failed=0; extensions stage=5 status=0
- GPU: A650 cold start mode 12
- disk1s1 mount-complete volume OwnerData
- 0 panic / 0 invalid argument / 0 I/O timeout（600 秒窗口）

### 第一次全稳定 GPU 运行（boot-GPU / v6 addon）
- GPU: addon files=6
- GPUDIAG opener=[mediaserverd|mediaserverd|backboardd]
- GPUDIAG tick=540 opens=4 closed=2 calls=3357 last_status=0
- DISPLAY: swap accepted=6 / present surface=6

### 显示优化内核（boot-present6）
- DISPLAY: present stats frames=1 interval_ticks=14217137632 avg_copy_ticks=827946 fps_x100=0
- 整套日志仅 3 次 swap/present，之后无新帧；PROC[1] backboardd ... w16
- 结论：合成器/交换完成链路停摆，不是渲染慢。

### 静音内核（boot-8C-GPU-quiet）
- 串口仅 85 字节（QCOM XNU USB CDC ACM v1 后无输出）；屏幕无 kernel 文字；
- 说明 disableConsoleOutput 生效且同时切断了串口日志（调试需 offline-legacy-console=1）。

## 事件时间线（2026-11-16）

- 04:00-04:30 恢复 R2 基线；CI kernel/payload 成功（43756212296）
- 04:25-04:40 旧内核 + v6 addon 复现 GPU 工作（opens=4 calls=3357），随后旧存储 bug 触发 panic
- 04:44 8 核内核（sheng-smp-max=7）+ GPU + persist=2 首次 600s 零错误
- 04:52 SMP: cpus=8 实测；随后触控 release 修复、屏幕静音、显示 present 优化逐版验证
- 05:35 present 优化内核测得合成器只出 3 帧后停摆
- 05:45 构建 boot-dpu.img（dpu-scanout=1 + sheng-dpu-scanout=2 + DPUDBG），待刷机验证
