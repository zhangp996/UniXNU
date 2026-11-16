# HANDOVER 2026-11-16 — fence 查询无锁快照（gpudiag7）

状态：**gpudiag6 验证完成 → fence wait 已生效但锁忙抖动 → 无锁快照修复落地 → boot-gpudiag7.img 已拼好，等真机验证**。
前序：`HANDOVER-2026-11-16-fence-common.md`（gpudiag6）；硬约束见 `SESSION-BOOTSTRAP.md`。

---

## 1. gpudiag6 真机结果（23:08 boot，376s 抓取，用户手动停止）

- **fence wait 生效**：`FENCEWAIT pass detail2=2`，target 随提交递增；768 帧 copy-ok，376 次真实等待，
  累计 fence_us=3149778（3.15s），单次最长 103901us（104ms），fence_slow=0（无 ≥250ms）。
- **但锁忙空转严重**：defer 10752 次，绝大多数 `detail2=1`（trylock busy）→ 每帧平均 ~14 次 200µs 重试，叠加抖动。
- 主观（用户）：**卡顿/跳帧未改善，触控也没好**；撕裂是否减轻未明确确认（待问）。
- 早期"只有文字滚动"后 UI 正常出现；触控链路日志正常（s6sy791 ready、SYNTOUCH HID ready、report 事件流动、8ms 轮询）。
- 日志：`D:\新建文件夹\instantnoodlep-artifacts\gpudiag6-cap.log`（1018018 B）。

## 2. 根因（锁忙）

`native_adreno_fences_detail` 用 `IOLockTryLock(g.lock)`，而 **g.lock 是 GPU 连接全局锁**：
每次 submit/map/alloc/WaitFence poll 的 dispatch 都持它（NativeAdrenoPublish.cpp runCore）。
渲染密集时显示侧 200µs 查询几乎永远抢不到 → detail2=1 → 帧被无谓推迟。

## 3. 修复（设备仓 `650d29b`，bringup-2026-11-12）

补丁镜像：`docs/evidence/2026-11-16-fence-snapshot.patch`。

- 新增 **seqlock fence 快照**（gFenceSnap：seq/submitted/retired/known，dmb sy 屏障）。
- **每次 dispatch 后在锁内刷新**（WaitFence 每次 poll + 普通方法），mount/publish 时引导刷新。
- 查询重写：trylock 成功就刷新；**失败则直接 seqlock 读最近快照，不再返回 busy**。
  快照最多落后一次 dispatch——安全：帧的 submit dispatch 先于 compositor 的 swap 刷新快照。
- detail2=1 从此不应出现；defer 只应是真实 GPU 未完成（detail2=2）。
- GopFramebuffer 侧无改动（仍 cc329f1，父仓 gitlink 暂存保持）。

## 4. boot-gpudiag7.img（可直接核对）

- `D:\新建文件夹\instantnoodlep-artifacts\boot-gpudiag7.img`
- sha256 `49bc49a7df7a1de37689c025dc6f4d6e7de3021a30fd7171d1365e0e8875d2a7`（两侧一致）
- 199782400 B；参数 **991 B**；**不带 --no-amfi**（kernel.macho `dd51f339…` → 嵌入 `6216d72b…`）。
- payload 哈希与 gpudiag6 不同是**因为内嵌了新内核**（脚本 mcopy kernel.macho 进 FAT），其余输入完全相同。
- 内核 build EXIT 0（/tmp/kbuildEU.log）。

拼镜像命令同 gpudiag6（--out /tmp/boot-gpudiag7.img，不带 --no-amfi）。

## 5. 验证点（gpudiag7）

1. `FENCEWAIT defer … detail2=1` **应消失**；defer 只剩 detail2=2（真实等 GPU）。
2. defer 总数应大幅下降（gpudiag6：10752/376s）；pass 的 wait_us 分布不变（GPU 帧耗时是物理事实）。
3. 主观：卡顿/跳帧是否减轻；**撕裂是否消失（必须明确回答）**。
4. 触控：日志链路正常，需用户描述具体症状（无响应/延迟/漂移？以前哪个版本好过？）。
   注意工作区有历史未提交的 `package/S6sy791Touch.c` 改动（55 行，前几轮遗留，构建一直带着它）。

## 6. 若仍卡顿（后续，先测量再改）

- GPU 帧耗时本身：平均等待 8.4ms、峰值 104ms vs 60Hz 16.7ms 预算 → 掉帧是 GPU 慢的显现，fence 只是让它可见。
  下一步量化 GPU submit→retire 帧耗时（ring 侧诊断），再谈 GPU 性能。
- vsync 60 vs 参考板 120：dpu-scanout=2/3 测实际刷新率后再决定（未批准）。
- **不改 CPU 核心数**。

## 7. 提交

- 设备仓 UniXNU-instantnoodlep：**`650d29b`**（本地；远端推送按惯例以 full-project 镜像为准）。
- GopFramebuffer：`cc329f1`（父仓 gitlink 已暂存，重建前务必保持）。
- full-project：本文 + 补丁镜像，提交号见回复。

## 8. 环境坑（累积）

- pwsh 外层双引号展开 `$VAR`、破坏内嵌双引号/管道 → wsl 检查命令用字面路径、避免 grep -E 带管道；复杂逻辑写脚本文件中转。
- UNC 写 WSL EPERM → cp 出来改完 cp 回去；commit -F 文件中转。
- PAT 在 WSL /tmp/.ghtok（600，新 token）；收尾删除；旧 token 需吊销。
