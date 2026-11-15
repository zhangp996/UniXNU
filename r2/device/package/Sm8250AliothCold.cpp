// SPDX-License-Identifier: BSD-2-Clause-Patent
// IN2020 wiring for the UniXNU-alioth mode 7 GMU cold start.
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <mach/vm_prot.h>
#include "GpuPowerBoardAlioth.h"
#include "A650Gmu.h"
#include "NativeAdrenoGmuContext.h"

extern "C" vm_offset_t ml_io_map_with_prot(vm_offset_t, vm_size_t, vm_prot_t);
extern "C" const uint8_t native_gpu_gmu_firmware[];
extern "C" const uint32_t native_gpu_gmu_firmware_size;
extern "C" bool sm8250_alioth_cp_start(unsigned mode, vm_offset_t smmu, size_t bytes, const A650GmuIo *gmu);

namespace {
constexpr uint32_t kSmmuBytes = 0x10000;
constexpr unsigned kSlots[] = {2, 3};
constexpr unsigned kStreams[] = {5, 0x405};
constexpr vm_size_t kWindowBytes[] = {A650_GMU_CORE_BYTES, A650_GMU_RSCC_BYTES, A650_GMU_PDC_BYTES};
vm_offset_t gWindows[A650GmuWindows];
IOMemoryMap *gPayloadMaps[5];
A650GmuDevice gDevice;
alignas(8) uint32_t gCommandDb[GPB_CMD_DB_BYTES / sizeof(uint32_t)];

int readGmu(void *, unsigned window, size_t offset, uint32_t *value)
{
    if (window >= A650GmuWindows || (offset & 3) || offset >= kWindowBytes[window])
        return 1;
    *value = *reinterpret_cast<volatile uint32_t *>(gWindows[window] + offset);
    return 0;
}

int writeGmu(void *, unsigned window, size_t offset, uint32_t value)
{
    if (window >= A650GmuWindows || (offset & 3) || offset >= kWindowBytes[window])
        return 1;
    *reinterpret_cast<volatile uint32_t *>(gWindows[window] + offset) = value;
    return 0;
}

int gmuBarrier(void *) { __asm__ volatile("dsb sy" ::: "memory"); return 0; }
int gmuWait(void *, unsigned us) { IODelay(us); return 0; }

int gmuNow(void *, uint64_t *us)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    *us = ns / 1000;
    return 0;
}

bool startupTables(AdrenoHfiStartupConfig &startup)
{
    const auto mapping = ml_io_map_with_prot(GPB_CMD_DB_BASE, GPB_CMD_DB_BYTES, VM_PROT_READ);
    if (!mapping)
        return false;
    const auto *source = reinterpret_cast<const volatile uint32_t *>(mapping);
    for (unsigned i = 0; i < sizeof(gCommandDb) / sizeof(gCommandDb[0]); ++i)
        gCommandDb[i] = source[i];

    const auto *db = reinterpret_cast<const uint8_t *>(gCommandDb);
    const char *names[] = {"gfx.lvl", "mx.lvl", "cx.lvl", "MC0", "SH0", "ACV", "CN0"};
    gpb_cmd_db_entry entry[7]{};
    for (unsigned i = 0; i < sizeof(entry) / sizeof(entry[0]); ++i) {
        if (gpb_cmd_db_find(db, sizeof(gCommandDb), names[i], &entry[i]))
            return false;
    }
    return A650GmuBuildStartup(entry[0].aux, entry[0].aux_bytes, entry[1].aux, entry[1].aux_bytes,
        entry[2].aux, entry[2].aux_bytes, entry[3].address, entry[4].address,
        entry[5].address, entry[6].address, &startup) == AdrenoHfiStartupSuccess;
}

bool mapGmuPayloads(A650GmuMemory (&views)[5])
{
    NativeAdrenoGmuPayload payloads[5];
    if (NativeAdrenoGmuContextPayloads(payloads))
        return false;
    for (unsigned i = 0; i < 5; ++i) {
        IODeviceMemory *memory = IODeviceMemory::withRange(payloads[i].physical, payloads[i].bytes);
        if (!memory)
            return false;
        gPayloadMaps[i] = memory->map(kIOMapAnywhere | kIOMapWriteCombineCache);
        memory->release();
        if (!gPayloadMaps[i])
            return false;
        printf("PLATFORM-DMA gmu index=%u pa=%llx bytes=%llx iova=%llx\n",
               i, payloads[i].physical, payloads[i].bytes, payloads[i].iova);
        views[i] = {reinterpret_cast<volatile uint32_t *>(gPayloadMaps[i]->getVirtualAddress()),
                    size_t(payloads[i].bytes), payloads[i].iova};
    }
    return true;
}

bool startGx(unsigned perf, const gpb_io &io)
{
    A650GmuGxResult gx{};
    const auto status = A650GmuPowerGx(&gDevice, perf, &gx);
    uint32_t gdscr = 0;
    const bool powered = gx.gx_on && !io.read32(io.cookie, GPB_WIN_GPUCC, GPB_CC_GX_GDSCR, &gdscr) &&
                         gpb_gdsc_on(gdscr);
    uint32_t rbbm = UINT32_MAX;
    if (powered) {
        const auto gpu = ml_io_map_with_prot(GPB_GPU_BASE, 0x1000, VM_PROT_READ);
        if (gpu)
            rbbm = *reinterpret_cast<volatile const uint32_t *>(gpu + GPB_RBBM_STATUS_BYTES);
    }
    printf("GPU55 mode8 result status=%u on=%u perf=%u vote=%u error=%x oob=%u gx=%08x>%08x gdscr=%08x rbbm=%08x\n",
           status, powered, perf, gx.vote_status, gx.vote_error, gx.oob_polls, gx.gx_before, gx.gx_after, gdscr, rbbm);
    return powered;
}
}

extern "C" bool sm8250_alioth_gmu_cold_start(unsigned mode, const gpb_io *io)
{
    AdrenoHfiStartupConfig startup{};
    if (!startupTables(startup)) {
        printf("GPU54 mode7 result command-db unavailable\n");
        return false;
    }

    const auto smmu = ml_io_map_with_prot(GPB_SMMU_BASE, kSmmuBytes, VM_PROT_READ | VM_PROT_WRITE);
    if (!smmu) {
        printf("GPU54 mode7 result SMMU map failed\n");
        return false;
    }
    auto *registers = reinterpret_cast<volatile uint32_t *>(smmu);
    const unsigned slots = gpb_smmu_num_smr(registers[GPB_SMMU_ID0 / 4]);
    if (slots <= kSlots[1] || slots > GPB_SMMU_MAX_SMR) {
        printf("GPU54 mode7 result invalid SMMU slot count %u\n", slots);
        return false;
    }
    for (unsigned i = 0; i < slots; ++i) {
        if (registers[GPB_SMMU_SMR(i) / 4] & (1u << 31)) {
            printf("GPU54 mode7 result SMMU slot %u already owned\n", i);
            return false;
        }
    }
    // alioth accepts the firmware's idle FAULT encoding only on the two GMU slots.
    for (unsigned slot : kSlots) {
        auto &s2cr = registers[GPB_SMMU_S2CR(slot) / 4];
        if ((s2cr & (3u << 16)) == (2u << 16))
            s2cr = 0x00020000u;
    }
    gmuBarrier(nullptr);

    NativeAdrenoGmuContextReport context{};
    const IOReturn attached = NativeAdrenoGmuContextBringupRoutes(
        reinterpret_cast<volatile uint8_t *>(smmu), kSmmuBytes, 1, 1, kSlots, kStreams, 2, nullptr, &context);
    if (attached) {
        printf("GPU54 mode7 result attach=%x step=%u phase=%u routes=%u\n",
               attached, context.step, context.program_phase, context.routes_verified);
        return false;
    }

    printf("PLATFORM-DMA gmu-context bank=1 ttbr0=%llx spans=%u oas=%u\n",
           context.ttbr0, context.spans, context.output_bits);
    A650GmuMemory views[5];
    if (!mapGmuPayloads(views)) {
        printf("GPU54 mode7 result payload map failed\n");
        return false;
    }
    const vm_offset_t physical[] = {GPB_GMU_BASE, GPB_RSCC_BASE, 0x0b290000};
    for (unsigned i = 0; i < A650GmuWindows; ++i) {
        gWindows[i] = ml_io_map_with_prot(physical[i], kWindowBytes[i], VM_PROT_READ | VM_PROT_WRITE);
        if (!gWindows[i]) {
            printf("GPU54 mode7 result GMU window %u map failed\n", i);
            return false;
        }
    }

    gDevice.io = {nullptr, {kWindowBytes[0], kWindowBytes[1], kWindowBytes[2]}, readGmu, writeGmu, gmuBarrier};
    gDevice.services = {nullptr, gmuNow, gmuWait};
    const A650GmuConfig config = {A650_GMU_CHIP_ID, native_gpu_gmu_firmware, native_gpu_gmu_firmware_size,
                                  views[0], views[1], views[3], views[4], 100};
    const A650GmuStartupLimits limits = {1000, 100, 50000, 2000000};
    A650GmuStartupResult result{};
    const auto status = A650GmuColdStart(&gDevice, &config, &startup, &limits, &result);
    printf("GPU54 mode7 result status=%u phase=%u routes=%u blocks=%u fw=%08x ready=%u/%u hfi=%u ack=%x error=%x\n",
           status, result.load.phase, context.routes_verified, result.load.blocks, result.load.firmware_version,
           result.load.firmware_ready_observed, result.load.hfi_ready_observed, result.protocol_status,
           result.protocol.acknowledged_stages, result.protocol.firmware_error);
    if (status != A650GmuSuccess)
        return false;
    if (mode < 8)
        return true;
    const bool gxStarted = startGx(startup.gpu_count - 1, *io);
    if (mode >= 9)
        return gxStarted && sm8250_alioth_cp_start(mode, smmu, kSmmuBytes, &gDevice.io);
    return true;
}
