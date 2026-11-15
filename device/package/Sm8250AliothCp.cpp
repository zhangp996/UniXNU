// SPDX-License-Identifier: BSD-2-Clause-Patent
// IN2020 wiring for mode 9 in UniXNU-alioth package/GpuProbe.cpp (aad6aeb).
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <mach/vm_prot.h>
#include "GpuPowerBoardAlioth.h"
#include "A650Gmu.h"
#include "AdrenoPM4.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoTimer.h"

extern "C" vm_offset_t ml_io_map_with_prot(vm_offset_t, vm_size_t, vm_prot_t);
extern "C" const uint8_t native_gpu_sqe_firmware[];
extern "C" const uint32_t native_gpu_sqe_firmware_size;
extern "C" bool sm8250_alioth_zap_load();
extern "C" void sm8250_alioth_publish(unsigned, NativeAdrenoRing *, const NativeAdrenoRingClock *, const NativeAdrenoTimeIo *, uint32_t);
extern "C" IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context,
                                     uint32_t *readback, uint64_t *router);

namespace {
constexpr unsigned kGpuSlots[] = {0, 1, 4, 5};
constexpr unsigned kGpuStreams[] = {0, 1, 0x400, 0x401};
constexpr uint32_t kMarker = 0xa6500c0d;
constexpr uint32_t kFenceTimeoutUs = 200000;
constexpr uint32_t kFenceInterrupt = 1u << 20;
constexpr uint32_t kGpuIntid = 32 + 300; // Stock DT SPI 300, level high.
vm_offset_t gGpu;
IOMemoryMap *gBufferMaps[GpuBufferCount];
NativeAdrenoRing *gRing;
uint32_t gFenceInterrupts;

#include "Sm8250AliothCpTables.h"

int gpuWrite(void *, uint32_t reg, uint32_t value)
{
    if (reg >= GPB_GPU_WINDOW / sizeof(uint32_t))
        return 1;
    reinterpret_cast<volatile uint32_t *>(gGpu)[reg] = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

int gpuRead(void *, uint32_t reg, uint32_t *value)
{
    if (reg >= GPB_GPU_WINDOW / sizeof(uint32_t))
        return 1;
    *value = reinterpret_cast<volatile const uint32_t *>(gGpu)[reg];
    return 0;
}

uint32_t gpuRead(uint32_t reg)
{
    uint32_t value = UINT32_MAX;
    gpuRead(nullptr, reg, &value);
    return value;
}

void gw(uint32_t reg, uint32_t value) { gpuWrite(nullptr, reg, value); }
void gw64(uint32_t reg, uint64_t value) { gw(reg, uint32_t(value)); gw(reg + 1, uint32_t(value >> 32)); }
int barrier(void *) { __asm__ volatile("dsb sy" ::: "memory"); return 0; }
int publish(void *cookie, const volatile uint32_t *, size_t) { return barrier(cookie); }
int acquire(void *cookie, volatile uint32_t *, size_t) { return barrier(cookie); }
int idle(void *) { IODelay(20); return 0; }

uint64_t now(void *)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000;
}

void gpuIrq(void *)
{
    const uint32_t status = gpuRead(0x201); // RBBM_INT_0_STATUS
    if (status)
        gw(0x37, status); // RBBM_INT_CLEAR_CMD before ArmCpu EOIs the level interrupt.
    if (status & kFenceInterrupt)
        __atomic_fetch_add(&gFenceInterrupts, 1u, __ATOMIC_RELEASE);
}

bool secureExitAndInterrupt(const NativeAdrenoRingClock &clock)
{
    if (!sm8250_alioth_zap_load())
        return false;

    uint32_t secure[] = {0, 0}, fence = 0, seen = 0;
    if (AdrenoPM4Type7(0x66, 1, secure) != AdrenoPM4Success || gRing->submit(secure, 2, &fence) ||
        gRing->wait(fence, &clock, kFenceTimeoutUs, &seen) != NativeAdrenoRingFenceComplete) {
        printf("GPU57 mode10 result ZAP accepted; CP_SET_SECURE_MODE fence failed\n");
        return false;
    }

    gw(0x038, kFenceInterrupt); // Mode 10 needs only the CP fence interrupt.
    gw(0x037, gpuRead(0x201)); // Discard the earlier polled fences before the IRQ test.
    uint32_t readback = 0;
    const auto attached = armcpu_spi_attach(kGpuIntid, false, gpuIrq, nullptr, &readback, nullptr);
    if (attached) {
        printf("GPU57 mode10 result IRQ attach=%x\n", attached);
        return false;
    }

    uint32_t nop = 0;
    const uint32_t before = __atomic_load_n(&gFenceInterrupts, __ATOMIC_ACQUIRE);
    const bool fenced = AdrenoPM4Type7(0x10, 0, &nop) == AdrenoPM4Success &&
        !gRing->submit(&nop, 1, &fence) && gRing->wait(fence, &clock, kFenceTimeoutUs, &seen) == NativeAdrenoRingFenceComplete;
    IOSleep(20);
    const uint32_t after = __atomic_load_n(&gFenceInterrupts, __ATOMIC_ACQUIRE);
    const bool ready = fenced && before != after;
    printf("GPU57 mode10 result status=%u zap=1 secure=1 intid=%u irq=%u>%u readback=%x nop=%u fence=%u\n",
           !ready, kGpuIntid, before, after, readback, fenced, seen);
    return ready;
}

void initHardware(const A650GmuIo &gmu)
{
    gw(0x3c45, 0); (void)gpuRead(0x3c45); // GBIF_HALT
    gw(0x0016, 0); (void)gpuRead(0x0016); // RBBM_GBIF_HALT
    gw(0xf803, 0); // RBBM_SECVID_TSB_CNTL
    gw64(0xf800, 0);
    gw(0xf802, 0);
    constexpr uint32_t addressMode[] = {0x842, 0xc01, 0x8601, 0x8e05, 0x9e01, 0xbe05,
                                       0xa601, 0x9601, 0xe00, 0xae01, 0xb601, 0xf810};
    for (uint32_t reg : addressMode)
        gw(reg, 1);

    uint32_t shaderClock = 0;
    gmu.read32(gmu.cookie, A650GmuCore, 0x80 * 4, &shaderClock);
    gmu.write32(gmu.cookie, A650GmuCore, 0x80 * 4, shaderClock & ~1u);
    for (const auto &entry : kHwcg)
        gw(entry[0], entry[1]);
    gmu.read32(gmu.cookie, A650GmuCore, 0x80 * 4, &shaderClock);
    gmu.write32(gmu.cookie, A650GmuCore, 0x80 * 4, shaderClock | 1u);
    gw(0x00ae, 0x8aa8aa82); // RBBM_CLOCK_CNTL

    for (uint32_t reg = 0x3c03; reg <= 0x3c06; ++reg)
        gw(reg, 0x00071620); // GBIF_QSB_SIDE0..3
    gw(0x0011, 3);
    gw(0x050b, UINT32_MAX);
    gw64(0xe05, 0x0001ffffffffffc0ull); // UCHE_WRITE_RANGE_MAX
    gw64(0xe09, 0x0001fffffffff000ull); // UCHE_TRAP_BASE
    gw64(0xe07, 0x0001fffffffff000ull); // UCHE_WRITE_THRU_BASE
    gw(0xe18, 0x804); gw(0xe17, 4);
    gw(0x8c2, 0x02000140); gw(0x8c1, 0x8040362c);
    gw(0x8c3, 128);
    gw(0x9e00, 0x00300200);
    gw(0x98d, 1);
    gw(0x500, 1); gw(0x8d0, 0);
    // A650 UBWC. The stock IN2020 DTB agrees with alioth on highest-bank-bit 16.
    gw(0x8e08, 0x816); gw(0xb604, 6); gw(0xae02, 0x26); gw(0xe01, 3u << 21);
    gw(0x001f, (1u << 30) | 0x1fffff);
    gw(0xe19, 1);
    gw(0xb608, 0); gw(0xb609, 0x3fe05ff4); gw(0xb60a, 0x3fa0ebee);
    gw(0xb60b, 0x3f5193ed); gw(0xb60c, 0x3f0243f0);
    gw(0x84f, (1u << 0) | (1u << 1) | (1u << 3)); // CP_PROTECT_CNTL
    for (unsigned i = 0; i + 1 < kProtectCount; ++i)
        gw(0x850 + i, kProtect[i]);
    gw(0x850 + 47, kProtect[kProtectCount - 1]);
    gw(0x844, (1u << 6) | (1u << 5) | (1u << 3) | (1u << 2) | (1u << 1)); // CP_APRIV_CNTL
    gw(0x038, NativeAdrenoIrq::maskA6xx());
}

IOReturn attachContext(volatile uint32_t *smmu, size_t bytes, NativeAdrenoGmuContextReport &report)
{
    const unsigned slots = gpb_smmu_num_smr(smmu[GPB_SMMU_ID0 / 4]);
    for (unsigned slot : kGpuSlots) {
        if (slot >= slots || (smmu[GPB_SMMU_SMR(slot) / 4] & (1u << 31)))
            return kIOReturnBusy;
    }
    for (unsigned slot : kGpuSlots) {
        auto &s2cr = smmu[GPB_SMMU_S2CR(slot) / 4];
        if ((s2cr & (3u << 16)) == (2u << 16))
            s2cr = 0x00020000u;
    }
    barrier(nullptr);
    const IOReturn status = NativeAdrenoGpuContextBringupRoutes(reinterpret_cast<volatile uint8_t *>(smmu),
        bytes, 0, 2, kGpuSlots, kGpuStreams, 4, nullptr, &report);
    if (status)
        return status;

    // alioth's MMU-500 bank 0 CPRE workaround and context TLB sync.
    const uint32_t id1 = report.id1;
    const uint32_t bank0 = ((id1 >> 31) ? 0x10000u : 0x1000u) << (((id1 >> 28) & 7) + 1);
    if (bank0 + 0x7f8 > bytes)
        return kIOReturnBadArgument;
    smmu[(bank0 + 4) / 4] &= ~2u;
    barrier(nullptr);
    smmu[(bank0 + 0x618) / 4] = 0;
    barrier(nullptr);
    smmu[(bank0 + 0x7f0) / 4] = 0;
    barrier(nullptr);
    for (unsigned i = 0; i < 1000; ++i) {
        if (!(smmu[(bank0 + 0x7f4) / 4] & 1))
            return kIOReturnSuccess;
        IODelay(1);
    }
    return kIOReturnTimeout;
}

bool mapBuffers(const NativeAdrenoGpuBuffer (&buffers)[GpuBufferCount], void *(&views)[GpuBufferCount])
{
    for (unsigned i = 0; i < GpuBufferCount; ++i) {
        auto *memory = IODeviceMemory::withRange(buffers[i].physical, buffers[i].bytes);
        if (!memory)
            return false;
        gBufferMaps[i] = memory->map(kIOMapAnywhere | kIOMapWriteCombineCache);
        memory->release();
        if (!gBufferMaps[i])
            return false;
        views[i] = reinterpret_cast<void *>(gBufferMaps[i]->getVirtualAddress());
    }
    return true;
}
}

extern "C" bool sm8250_alioth_cp_start(unsigned mode, vm_offset_t smmu, size_t bytes, const A650GmuIo *gmu)
{
    NativeAdrenoGmuContextReport context{};
    const auto attached = attachContext(reinterpret_cast<volatile uint32_t *>(smmu), bytes, context);
    if (attached) {
        printf("GPU56 mode9 result context=%x step=%u routes=%u program=%u route=%u\n",
               attached, context.step, context.routes_verified, context.program_status, context.route_status);
        return false;
    }

    NativeAdrenoGpuBuffer buffers[GpuBufferCount];
    void *views[GpuBufferCount];
    gGpu = ml_io_map_with_prot(GPB_GPU_BASE, GPB_GPU_WINDOW, VM_PROT_READ | VM_PROT_WRITE);
    if (!gGpu || NativeAdrenoGpuContextBuffers(buffers) || !mapBuffers(buffers, views)) {
        printf("GPU56 mode9 result buffer mapping failed\n");
        return false;
    }
    if (native_gpu_sqe_firmware_size < 8 || native_gpu_sqe_firmware_size - 4 > buffers[0].bytes) {
        printf("GPU56 mode9 result SQE size invalid\n");
        return false;
    }
    // adreno_fw_create_bo and alioth skip the first dword of the vendor image.
    auto *sqe = static_cast<volatile uint8_t *>(views[0]);
    for (uint32_t i = 4; i < native_gpu_sqe_firmware_size; ++i)
        sqe[i - 4] = native_gpu_sqe_firmware[i];
    barrier(nullptr);

    initHardware(*gmu);
    gw64(0x830, buffers[0].iova); // CP_SQE_INSTR_BASE
    const NativeAdrenoRingIo io{nullptr, gpuWrite, gpuRead, barrier, nullptr, nullptr};
    const NativeAdrenoRingCache cache{nullptr, publish, acquire};
    const NativeAdrenoRingClock clock{nullptr, now, idle};
    gRing = new NativeAdrenoRing;
    if (!gRing || gRing->setGeneration(6) ||
        gRing->attach(&io, &cache, views[1], buffers[1].bytes, buffers[1].iova,
                      views[2], buffers[2].bytes, buffers[2].iova)) {
        printf("GPU56 mode9 result ring attach failed\n");
        return false;
    }
    gw(0x808, 1); // CP_SQE_CNTL

    NativeAdrenoCpInit init;
    NativeAdrenoCpInitConfig config{buffers[0].iova, 0, 1};
    config.generation = 6;
    uint32_t fence = 0, observed = 0;
    const IOReturn initialized = init.run(&io, gRing, &config, &fence);
    const auto initWait = initialized ? NativeAdrenoRingFenceArgument :
        gRing->wait(fence, &clock, kFenceTimeoutUs, &observed);
    if (initWait != NativeAdrenoRingFenceComplete) {
        printf("GPU56 mode9 result CP init=%x fence=%u seen=%u rptr=%x wptr=%x fault=%x\n",
               initialized, unsigned(initWait), observed, gpuRead(0x806), gpuRead(0x807), gpuRead(0x821));
        return false;
    }

    auto *scratch = static_cast<volatile uint32_t *>(views[4]);
    scratch[0] = 0;
    barrier(nullptr);
    uint32_t words[] = {0, uint32_t(buffers[4].iova), uint32_t(buffers[4].iova >> 32), kMarker};
    if (AdrenoPM4Type7(0x3d, 3, words) != AdrenoPM4Success || gRing->submit(words, 4, &fence)) {
        printf("GPU56 mode9 result CP_MEM_WRITE submission failed\n");
        return false;
    }
    const auto completed = gRing->wait(fence, &clock, kFenceTimeoutUs, &observed);
    const uint32_t marker = scratch[0];
    const bool ready = completed == NativeAdrenoRingFenceComplete && marker == kMarker;
    printf("GPU56 mode9 result status=%u routes=%u fence=%u seen=%u marker=%08x rptr=%x wptr=%x fault=%x\n",
           !ready, context.routes_verified,
           unsigned(completed), observed, marker, gpuRead(0x806), gpuRead(0x807), gpuRead(0x821));
    if (ready) {
        if (context.program_readback_valid)
            printf("PLATFORM-DMA gpu-context bank=0 ttbr0=%llx spans=%u oas=%u\n",
                   context.image.ttbr0, context.spans, context.output_bits);
        for (unsigned i = 0; i < GpuBufferCount; ++i)
            printf("PLATFORM-DMA gpu index=%u pa=%llx bytes=%llx iova=%llx\n",
                   i, buffers[i].physical, buffers[i].bytes, buffers[i].iova);
    }
    if (ready && mode >= 10) {
        const bool irqReady = secureExitAndInterrupt(clock);
        if (mode >= 11) {
            if (!irqReady)
                return false;
            const NativeAdrenoTimeIo timer{nullptr, gpuRead};
            sm8250_alioth_publish(mode, gRing, &clock, &timer, context.output_bits);
        }
    }
    return ready;
}
