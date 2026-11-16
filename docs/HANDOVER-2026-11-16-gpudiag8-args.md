# gpudiag8 — 跳设置 + 静默屏幕控制台（args-only 变体）

基于 gpudiag7 同一内核（kernel.macho `dd51f339…` → AMFI 嵌入 `6216d72b…`，含公共路径 fence wait + 无锁快照），
**仅改引导参数**，不重编内核：

- `setup-skip=0` → `setup-skip=1`：写 SetupDone=true，跳过 Hello/设置直达桌面（probe native_owner_setup_skip）。
- 移除 `offline-legacy-console=1`：显示驱动第 4 帧后 disableConsoleOutput=1，屏幕滚动文字消失（串口 serial=3 抓取不受影响）。

## 镜像
- `D:\新建文件夹\instantnoodlep-artifacts\boot-gpudiag8.img`
- sha256 `e4ea9fa35813ed8b94ed6e15af4ad370562e8e928790de36ae3222c0271e125b`（两侧一致）
- 199782400 B；参数 **966 B**（≤1000）；AMFI 已打（不带 --no-amfi）。

## 目的
gpudiag7 卡在 Setup（step）页，无法测桌面触控。本轮进桌面 + 去文字后测触控与撕裂/卡顿。
fence 核对点同 gpudiag7（detail2=1 应消失）。

## 拼法
```bash
python3 tools/r2/build_boot_image.py --payload /tmp/dpu5-payload.img \
  --kernel ~/unixnu/UniXNU/out/instantnoodlep/ipados-17.7.11-21H461/instantnoodlep/kernel/kernel.macho \
  --skeleton "/mnt/d/新建文件夹/instantnoodlep-artifacts/bootEU.img" \
  --gpu-addon /home/dev/unixnu/_ref/artifacts/14012210733/gpuaddon.bin \
  --args-remove "setup-skip=0 offline-legacy-console=1" --args-add "setup-skip=1" \
  --out /tmp/boot-gpudiag8.img
```
