# A650 GPU 与 Metal 适配的驱动接口定义

本文按 drivers 页 §1「定义驱动接口」的要求，记录本设备 GPU/Metal 适配的接口、硬件、传输、内存、OS ABI 与构建接入。设备：OnePlus 8 Pro（instantnoodlep），Adreno 650，iPadOS 17.7.11 / 21H461。

## 1. 接口（Interface）

- 内核设备类：AGXAccelerator : IOAcceleratorES，属性 MetalPluginName=NativeAdrenoMetal、MetalPluginClassName。
- 用户客户端：AGXDeviceUserClient（sandbox 以 open type 0x4147 区分插件）；另有 handoff 类型 0x4148（只处理 GOP 呈现，不挂 GPU 核）。
- 方法 ABI：NativeAdrenoABI.h 的 16 个方法（1 Query、2 BoAlloc、3 BoMap、4 BoFree、5 Submit、6 WaitFence、7 GetTimestamp、8 GetStatus、9 SetMetadata、10 GetMetadata、11 CreateQueue、12 CloseQueue、13 BoCpuMap、14 BoCpuUnmap、15 GetDeviceInfo、16 BoImport），input/output 均为固定 wire 记录。
- 额外通道：tunnel（magic NTNL，首 8 字节为真实方法号）、mailbox（一页共享内存 + 内核轮询线程）、GOP handoff（0x7000–0x7003 + NativeAdrenoScanoutType）。

## 2. 必需定义（Required definitions）

- NativeAdrenoAbiParams、NativeAdrenoAbiBoAlloc/BoMap/BoFree/BoImport、NativeAdrenoAbiSubmit（buffers 数组）、NativeAdrenoAbiWaitFence、NativeAdrenoAbiTimestamp、NativeAdrenoAbiStatus、NativeAdrenoAbiMetadata、NativeAdrenoAbiQueue*、NativeAdrenoAbiDeviceInfo。
- NativeAdrenoMetalDiag：openers/readers 记录、calls、last_selector/status、ext_entries/ext_selector/ext_status、tunnel_calls、probes、sel_bits[8]、marks、插件文本行。

## 3. 硬件（Hardware）

- Adreno 650；kgsl 0x3d00000，GMU 0x3d6a000；七个寄存器窗口；SMMU（ArmSmmu4K context）与固件 stream。
- 固件：a650_gmu.bin（GMU 镜像）、a650_sqe.fw（SQE）、a650_zap.elf（ZAP），来源与 SHA-256 见 proprietary-files.txt。

## 4. 传输（Transport）

- NativeAdrenoTransport.call 连接内核分发端，保留每个请求的输入/输出大小；
- 分发实现 AGXDeviceUserClient::externalMethod → runCore/run：查询 ABI → 分配 BO → 映射 IOVA → 填命令 → 提交并等待 fence；
- 提交路径由 NativeAdrenoSubmit + NativeAdrenoRing 完成（PM4 编码、WPTR/RPTR、CP_EVENT_WRITE fence）。

## 5. 内存（Memory）

- BO 来自系统内存：可分页 IOBufferMemoryDescriptor prepare() 锁定；VM 批量设置页属性；客户端视图映射该描述符；
- 预留内存与 GPU 虚拟地址窗口：firmware/ring/scanout/client 分配各自区域；GPU 映射非 cacheable，调用方负责 cache 同步；
- 失败路径保留请求类型、句柄与 fence。

## 6. OS ABI

- iPadOS 17.7.11（21H461），arm64，内核 KASLR slide 与 prelink 由 XnuPrelink 处理；
- Metal 插件为 NativeAdrenoMetal（metal2vulkan → Turnip → Darwin KMD），插件 ABI 与本内核的 16 方法 ABI 已核对一致；
- 8-bit BGRA 帧缓冲通过 backboardd 偏好 CAEnableDeepFramebuffer=false 与 CA_DISABLE_FRAMEBUFFER_COMPRESSION=1 固定下来。

## 7. 构建接入（Build integration）

- 内核归档：build-adreno650.py 生成本驱动 libAdreno650.a（驱动对象 + A650 GMU kit + ArmSmmu4K + AdrenoFirmware/PM4 + 固件字节数组），再加入内核预链接；
- 板级：package/Sm8250AliothCold.cpp（冷启动 mode 12）、package/Sm8250AliothMetal.cpp（加速器发布）；
- payload：--gpu-addon gpuaddon-r3.bin --owner-trust-cache ...，板级 loader 由 gpu-addon=1/sheng-gpu-addon=1 开启；
- 校验：llvm-ar t libAdreno650.a、llvm-nm --defined-only 核对驱动符号与固件符号（native_gpu_gmu_firmware/native_gpu_sqe_firmware/native_gpu_zap_firmware）。

## 8. 当前验证状态

- ✅ GPU 冷启动、加速器发布、Metal 客户端连接与调用（opens=8、calls=8300、tunnel=8301）；
- ✅ 8-bit BGRA 呈现（402–487 帧 fmt=42475241，无 10-bit）；
- ⚠️ 有界提交自检、[vm map]/[bo memory] 诊断行：待实现；
- ⚠️ gpu-preempt=2（ring0/ring1）调度：未实现（单 ring），需在记录中标注偏差；
- ⚠️ 插件自报结果行（starter/report/report2）仍空：等待 tunnel 方法直方图与报告诊断上机确认；
- ⚠️ addon 为预编译产物，无 primelist/pipeline pack（需 Mac 主机与该 OS 镜像按文档脚本构建）。
