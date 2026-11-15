// SPDX-License-Identifier: BSD-2-Clause-Patent
// IN2020 PAS 13 wiring for UniXNU-alioth mode 10 (aad6aeb).
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include "NativeAdrenoMemory.h"

extern "C" {
extern uint64_t vm_first_phys, vm_last_phys;
boolean_t ml_set_interrupts_enabled(boolean_t enabled);
extern const uint8_t native_gpu_zap_firmware[];
}

namespace {
constexpr uint64_t kZapPhys = 0x8e21a000, kZapBytes = 0x2000; // Stock IN2020 DTB reg.
constexpr uint64_t kGpuPasId = 13;
// ELF program headers are checked by the recipe against the pinned OnePlus image.
constexpr size_t kHeaderBytes = 0x94, kHashOffset = 0x1000, kHashBytes = 0x1a38;
constexpr size_t kSegmentOffset = 0x3000, kSegmentBytes = 0x68c, kImageSpan = 0x1000;
NativeAdrenoMemory *gMetadata;

struct ScmResult { int64_t transport; uint64_t result; };

// ARM_SMCCC_CALL_VAL(STD, SMC_64, OWNER_SIP, PIL << 8 | cmd), as in alioth.
ScmResult scmPas(uint32_t cmd, uint64_t args, uint64_t p1, uint64_t p2)
{
    const uint64_t function = 0x42000200u | cmd;
    const boolean_t interrupts = ml_set_interrupts_enabled(FALSE);
    ScmResult result{-1000, 0};
    uint64_t first = function, resumeArg = 0;
    for (unsigned resumes = 0, busy = 0; resumes < 10000;) {
        register uint64_t x0 asm("x0") = first, x1 asm("x1") = args, x2 asm("x2") = kGpuPasId;
        register uint64_t x3 asm("x3") = p1, x4 asm("x4") = p2, x5 asm("x5") = 0;
        register uint64_t x6 asm("x6") = resumeArg, x7 asm("x7") = 0;
        asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 == 1) {
            first = 1;
            resumeArg = x6;
            ++resumes;
            continue;
        }
        if (int64_t(x0) == -12 && busy++ < 20) {
            first = function;
            resumeArg = 0;
            IODelay(30000);
            continue;
        }
        result = {int64_t(x0), x1};
        break;
    }
    ml_set_interrupts_enabled(interrupts);
    return result;
}

bool pasStep(const char *name, uint32_t command, uint64_t args, uint64_t p1, uint64_t p2)
{
    printf("GPU57 PAS %s\n", name);
    const auto result = scmPas(command, args, p1, p2);
    if (result.transport || result.result) {
        printf("GPU57 mode10 result PAS %s transport=%lld result=%lld\n", name,
               static_cast<long long>(result.transport), static_cast<long long>(result.result));
        return false;
    }
    return true;
}
}

extern "C" bool sm8250_alioth_zap_load()
{
    if (kZapPhys + kZapBytes > vm_first_phys && kZapPhys < vm_last_phys) {
        printf("GPU57 mode10 result ZAP reservation overlaps managed RAM\n");
        return false;
    }

    constexpr size_t metaBytes = (kHeaderBytes + kHashBytes + 0xfff) & ~size_t(0xfff);
    gMetadata = new NativeAdrenoMemory;
    if (!gMetadata || gMetadata->allocate(metaBytes, 40, true, metaBytes) || gMetadata->spanCount() != 1) {
        printf("GPU57 mode10 result ZAP metadata allocation failed\n");
        return false;
    }
    auto *meta = static_cast<uint8_t *>(gMetadata->mapping());
    memcpy(meta, native_gpu_zap_firmware, kHeaderBytes);
    memcpy(meta + kHeaderBytes, native_gpu_zap_firmware + kHashOffset, kHashBytes);
    if (gMetadata->publishForDevice(0, metaBytes, true)) {
        printf("GPU57 mode10 result ZAP metadata sync failed\n");
        return false;
    }
    if (!pasStep("init_image", 0x01, 2 | (2u << 6), gMetadata->spans()[0].physical, 0) ||
        !pasStep("mem_setup", 0x02, 3, kZapPhys, kImageSpan))
        return false;

    auto *memory = IODeviceMemory::withRange(kZapPhys, kZapBytes);
    auto *map = memory ? memory->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
    if (memory)
        memory->release();
    if (!map) {
        printf("GPU57 mode10 result ZAP reservation map failed\n");
        return false;
    }
    printf("GPU57 PAS copy\n");
    // The loadable segment is relocatable and starts at offset zero in the reservation.
    auto *region = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
    for (size_t i = 0; i < kSegmentBytes; ++i)
        region[i] = native_gpu_zap_firmware[kSegmentOffset + i];
    asm volatile("dsb sy" ::: "memory");
    map->release(); // Drop the CPU view before TrustZone authenticates the image.
    return pasStep("auth_and_reset", 0x05, 1, 0, 0);
}
