// SPDX-License-Identifier: BSD-2-Clause-Patent
// The audio service iOS's audio server plug-in opens (hal/T970AudioPlugIn.c, interface in T970AudioAbi.h). The service maps the
// control page and the playback ring that T970Audio.cpp owns and passes start and stop requests to its thread; the samples
// themselves never pass through here. T970Audio.cpp registers this file's classes (static initializer taken out of __mod_init_func
// by the build, as T970Battery.cpp) and starts the service once the amplifiers and the DSP answer (t970-audio bit 32).
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include "T970AudioAbi.h"

extern "C" IOBufferMemoryDescriptor *t970_audio_memory(uint32_t type);   // retained; null when absent
extern "C" int t970_audio_request(uint32_t selector);                     // 0 accepted
// [0] service state (1 published; negative: the step that failed), [1] opens, [2] closes, [3] requests, [4] refused requests,
// [5] memory maps
extern "C" {volatile int32_t t970_audio_service[8];}
extern "C" vm_offset_t phystokv(uint64_t pa);
extern "C" uint64_t pmap_find_pa(void *pmap, uint64_t va);
// [audio-clock]: the last host clock request: the step it stopped at (0 done, 1 address, 2 prepare, 3 read through the caller, 4 physical
// address, 5 value), the caller's address, the physical address, the value the caller reads (bits).
extern "C" {volatile uint64_t t970_audio_clock[4];}

// CoreAudio takes the host clock to be 24 MHz (the timer of Apple's chips; AudioGetHostClockFrequency and the audio server's IO clock
// read one constant), so on this board's 19.2 MHz timer the audio server slept 512000 ticks (26.7 ms) for each 1024-frame cycle of
// 21.3 ms and fell behind until a time stamp pulled it forward (i546: 858 of 1474 cycles 25 to 40 ms apart, skew down to -13052 frames,
// stutter). The constant lives in the dyld shared cache, on a page with code, so the caller cannot write it; this sets the shared page
// itself, once for every process. Only a 24 MHz value inside the shared region is changed.
static IOReturn setHostClock(task_t task, uint64_t address) {
    constexpr uint64_t kApple = 0x4176e36000000000ULL;   // 24000000.0
    t970_audio_clock[1] = address;
    t970_audio_clock[2] = 0;
    t970_audio_clock[3] = 0;
    t970_audio_clock[0] = 1;
    if ((address & 7) || address < 0x180000000ULL || address >= 0x400000000ULL) return kIOReturnBadArgument;
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(address, 8, kIODirectionOut, task);
    if (!memory) return kIOReturnNoMemory;
    t970_audio_clock[0] = 2;
    IOReturn r = memory->prepare();
    if (r != kIOReturnSuccess) {memory->release(); return r;}
    // The physical address from the caller's own page table: the one IOKit gave (getPhysicalSegment) held 0 (i549, i551).
    uint64_t seen = 0;
    t970_audio_clock[0] = 3;
    r = copyin(address, &seen, 8) ? kIOReturnNotReadable : kIOReturnNoResources;
    t970_audio_clock[3] = seen;
    const uint64_t pa = r == kIOReturnNoResources ? pmap_find_pa(get_task_pmap(task), address) : 0;
    t970_audio_clock[2] = pa;
    if (pa) {
        t970_audio_clock[0] = 4;
        volatile uint64_t *value = reinterpret_cast<volatile uint64_t *>(phystokv(pa));
        mach_timebase_info_data_t tb;
        clock_timebase_info(&tb);
        // The timer frequency as a double, built from the integer (no floating point here).
        const uint64_t hz = 1000000000ULL * tb.denom / tb.numer;
        const unsigned top = 63 - (unsigned)__builtin_clzll(hz);
        const uint64_t bits = (uint64_t)(1023 + top) << 52 | ((hz << (52 - top)) & ((1ULL << 52) - 1));
        r = kIOReturnUnsupported;
        if (*value == seen && (seen == kApple || seen == bits)) {
            t970_audio_clock[0] = 5;
            *value = bits;
            __asm__ volatile("dsb sy" ::: "memory");
            t970_audio_clock[0] = 0;
            r = kIOReturnSuccess;
        }
    }
    memory->complete();
    memory->release();
    return r;
}

class T970Audio : public IOService {
    OSDeclareDefaultStructors(T970Audio)
public:
    bool startAudio() {
        // iOS's audio server is sandboxed against unknown user clients; the kernel lets these processes open this service.
        setProperty("NativeSandboxGrantProcess", "audiomxd,mediaserverd");
        setProperty("IOUserClientClass", "T970AudioUserClient");
        registerService();
        return true;
    }
};
OSDefineMetaClassAndStructors(T970Audio, IOService)

class T970AudioUserClient : public IOUserClient {
    OSDeclareDefaultStructors(T970AudioUserClient)
    task_t owner = nullptr;
public:
    bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override {
        if (!IOUserClient::initWithTask(task, security, type, properties)) return false;
        owner = task;
        return true;
    }
    bool start(IOService *provider) override {
        if (!OSDynamicCast(T970Audio, provider) || !IOUserClient::start(provider)) return false;
        t970_audio_service[1] = t970_audio_service[1] + 1;
        return true;
    }
    IOReturn clientClose() override {
        // The plug-in's process went away or closed: nothing plays from its rings any more.
        t970_audio_request(T970AudioStopOutput);
        t970_audio_request(T970AudioStopInput);
        t970_audio_service[2] = t970_audio_service[2] + 1;
        terminate();
        return kIOReturnSuccess;
    }
    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override {
        IOBufferMemoryDescriptor *m = t970_audio_memory(type);
        if (!m) return kIOReturnBadArgument;
        *options = 0;
        *memory = m;   // the caller consumes the reference
        t970_audio_service[5] = t970_audio_service[5] + 1;
        return kIOReturnSuccess;
    }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch, OSObject *target,
                            void *reference) override {
        if (selector == T970AudioSetHostClock && args->scalarInputCount == 1 && !args->structureInputSize)
            return setHostClock(owner, args->scalarInput[0]);
        if (selector > T970AudioStopInput || args->scalarInputCount || args->structureInputSize) return kIOReturnBadArgument;
        t970_audio_service[3] = t970_audio_service[3] + 1;
        if (t970_audio_request(selector)) {t970_audio_service[4] = t970_audio_service[4] + 1; return kIOReturnNotReady;}
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(T970AudioUserClient, IOUserClient)

// Called by T970Audio.cpp once the speaker path is ready and this file's classes are registered.
extern "C" void t970_audio_service_start(void) {
    T970Audio *audio = OSTypeAlloc(T970Audio);
    if (!audio || !audio->init() || !audio->attach(IOService::getPlatform())) {
        if (audio) audio->release();
        t970_audio_service[0] = -1;
        return;
    }
    if (!audio->start(IOService::getPlatform()) || !audio->startAudio()) {
        t970_audio_service[0] = -2;
        audio->detach(IOService::getPlatform());
        audio->release();
        return;
    }
    t970_audio_service[0] = 1;
    audio->release();   // the platform's attachment keeps it
}
