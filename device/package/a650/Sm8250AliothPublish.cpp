// SPDX-License-Identifier: BSD-2-Clause-Patent
// IN2020 wiring for the alioth mode 11 service and one bounded client submission.
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include "AdrenoPM4.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoUserClient.h"

extern "C" IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *,
    const NativeAdrenoRingClock *, const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
extern "C" IOService *native_adreno_published_service();
extern "C" void sm8250_alioth_metal_backend_ready();

// The alioth shader-cache export endpoint expects its board's persistent log.
extern "C" int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) { return -1; }

namespace {
constexpr NativeAdrenoAbiDeviceInfo kDeviceInfo{
    0x06050002u, 0x120000u, 0, 0x1fffffffff000ull, 0x20000000ull, 0x60000000ull,
    16, 6, 1, 4, NativeAdrenoDeviceClientIova, 0
};
constexpr uint32_t kMarker = 0x0badc0de;
NativeAdrenoSubmit *gSubmit;
NativeAdrenoTimer *gTimer;

IOReturn clientCall(IOUserClient *client, uint32_t method, const void *input, void *output)
{
    IOExternalMethodArguments args{};
    args.version = kIOExternalMethodArgumentsCurrentVersion;
    args.selector = method;
    args.structureInput = input;
    args.structureInputSize = uint32_t(NativeAdrenoUserClient::inputSize(method));
    args.structureOutput = output;
    args.structureOutputSize = uint32_t(NativeAdrenoUserClient::outputSize(method));
    return client->externalMethod(method, &args, nullptr, nullptr, nullptr);
}

struct ClientBuffer {
    NativeAdrenoAbiBoAllocReply buffer{};
    IOMemoryDescriptor *descriptor = nullptr;
    IOMemoryMap *map = nullptr;

    ~ClientBuffer()
    {
        if (map)
            map->release();
        if (descriptor)
            descriptor->release();
    }

    IOReturn open(IOUserClient *client, uint64_t iova, uint32_t permissions)
    {
        const NativeAdrenoAbiBoAlloc allocate{4096, 12, 0};
        IOReturn status = clientCall(client, NativeAdrenoMethodBoAlloc, &allocate, &buffer);
        if (status)
            return status;
        const NativeAdrenoAbiBoMap mapRequest{buffer.handle, iova, permissions, 0, 2, 0};
        NativeAdrenoAbiBoMapReply mapped{};
        status = clientCall(client, NativeAdrenoMethodBoMap, &mapRequest, &mapped);
        if (status)
            return status;
        const NativeAdrenoAbiBoCpuMap cpuMap{buffer.handle};
        NativeAdrenoAbiBoCpuMapReply cpuView{};
        status = clientCall(client, NativeAdrenoMethodBoCpuMap, &cpuMap, &cpuView);
        if (status)
            return status;
        IOOptionBits options = 0;
        status = client->clientMemoryForType(cpuView.token, &options, &descriptor);
        if (status)
            return status;
        if (descriptor)
            map = descriptor->createMappingInTask(kernel_task, 0, options);
        return map ? kIOReturnSuccess : kIOReturnNoMemory;
    }
};

IOReturn submitOnce(IOUserClient *client, const char *&phase, uint64_t &fence, uint32_t &marker)
{
    constexpr uint64_t ibIova = 0x20004000, targetIova = 0x20005000;
    ClientBuffer ib, target;
    phase = "ib map";
    IOReturn status = ib.open(client, ibIova, 1);
    if (status)
        return status;
    phase = "target map";
    status = target.open(client, targetIova, 3);
    if (status)
        return status;

    auto *commands = reinterpret_cast<volatile uint32_t *>(ib.map->getVirtualAddress());
    auto *scratch = reinterpret_cast<volatile uint32_t *>(target.map->getVirtualAddress());
    uint32_t header = 0;
    if (AdrenoPM4Type7(0x3d, 3, &header) != AdrenoPM4Success)
        return kIOReturnInternalError;
    *scratch = 0;
    commands[0] = header;
    commands[1] = uint32_t(targetIova);
    commands[2] = uint32_t(targetIova >> 32);
    commands[3] = kMarker;
    asm volatile("dsb sy" ::: "memory");

    phase = "submit";
    NativeAdrenoAbiSubmit request{};
    request.buffer_count = 1;
    request.buffers[0] = {ib.buffer.handle, ibIova, 4 * sizeof(uint32_t)};
    NativeAdrenoAbiSubmitReply submitted{};
    status = clientCall(client, NativeAdrenoMethodSubmit, &request, &submitted);
    if (status)
        return status;
    phase = "fence";
    fence = submitted.fence;
    const NativeAdrenoAbiWaitFence wait{fence, 2000000000ull};
    status = clientCall(client, NativeAdrenoMethodWaitFence, &wait, nullptr);
    asm volatile("dsb sy" ::: "memory");
    marker = *scratch;
    if (status)
        return status;
    phase = "readback";
    return marker == kMarker ? kIOReturnSuccess : kIOReturnIOError;
}
}

extern "C" void sm8250_alioth_publish(unsigned mode, NativeAdrenoRing *ring, const NativeAdrenoRingClock *clock,
    const NativeAdrenoTimeIo *timerIo, uint32_t outputBits)
{
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    gSubmit = new NativeAdrenoSubmit;
    gTimer = new NativeAdrenoTimer;
    if (!vm || !gSubmit || !gTimer) {
        printf("GPU58 mode11 result service allocation failed\n");
        return;
    }
    const IOReturn submitted = gSubmit->attach(vm, ring);
    const IOReturn timed = gTimer->attach(timerIo, NativeAdrenoTimeCp);
    const IOReturn published = submitted ? submitted :
        native_adreno_publish(vm, gSubmit, ring, clock, timed ? nullptr : gTimer, &kDeviceInfo, 31, outputBits);
    if (published) {
        printf("GPU58 mode11 result publish=%x submit=%x timer=%x\n", published, submitted, timed);
        return;
    }
    if (mode == 12) {
        sm8250_alioth_metal_backend_ready();
        return;
    }

    IOUserClient *client = nullptr;
    IOService *service = native_adreno_published_service();
    const IOReturn opened = service ? service->newUserClient(kernel_task, nullptr, 0x4147, nullptr, &client) : kIOReturnNotReady;
    if (opened || !client) {
        printf("GPU58 mode11 result publish=0 open=%x client=%u\n", opened, client != nullptr);
        return;
    }
    const char *phase = "open";
    uint64_t fence = 0;
    uint32_t marker = 0;
    const IOReturn tested = submitOnce(client, phase, fence, marker);
    const IOReturn closed = client->clientClose();
    client->release();
    printf("GPU58 mode11 result status=%x publish=0 timer=%x phase=%s fence=%llu marker=%08x close=%x\n",
           tested ? tested : closed, timed, phase, static_cast<unsigned long long>(fence), marker, closed);
}
