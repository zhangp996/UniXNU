# session-2026-11-16 脚本存档

本目录是 2026-11-16 会话中用于构建测试镜像的脚本存档（原路径 /mnt/d/新建文件夹/_agent/）。
其中路径写死为当时的 WSL /mnt/d 布局；新工作请优先使用通用的 tools/r2/build_boot_image.py。

| 脚本 | 用途 |
| --- | --- |
| hs_build_8c_gpu.py | 8 核内核 + GPU addon + persist=2 镜像 |
| hs_build_quiet_img.py | 屏幕静音镜像（去掉 offline-legacy-console） |
| hs_build_touch_img.py | 触控 release 修复内核的镜像 |
| hs_build_present6_img.py | 显示 present 优化内核的镜像 |
| hs_build_cik_gpu.py | CI 内核（已打 AMFI）测试镜像 |
| hs_build_dpu_img2.py | DPU 直接扫描输出测试镜像（dpu-scanout=1 + sheng-dpu-scanout=2） |
| hs_make_console_patch.py | 生成 zzz-console-output-off.patch |
| hs_swap_dbg2.py | 给组件 NativeLegacyDisplay 加 SWAPDBG（未编译验证，供参考） |
| hs_make_dpu_patch.py | 生成 patches/display/zzz-gop-dpu-diagnostics.patch |

注意：这些脚本会读写 /tmp 与 /mnt/d；复制到 Windows 侧后无法直接运行，仅作追溯。
