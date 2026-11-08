// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi vibration motor (boot argument t970-haptics=<bits>, off by default).
//
// Hardware (live GTS7XL FDT, read from TWRP): the board has Samsung's "msm_vibrator" node, compatible "vibrator",
// samsung,vib_type "COINDC", motor-vdd_type 1 (the motor hangs straight off a regulator) and vibr_vdd-supply = PM8150L LDO7
// ("ldoc7", min = init = 2856 mV). Nothing else in the tree uses that LDO, so it is the motor's own rail: an ERM coin motor that
// spins while the LDO is on. (The PMIC's qcom,haptics@c000 LRA block in the same FDT is status = "disabled": not fitted.)
// Samsung's driver does exactly that: regulator_enable() for the on time, regulator_disable() after.
//
// So a pulse is three RPMh VRM votes through t970_rpmh_write (package/T970FtsTouch.cpp, the one lock for every APPS RSC DRV2
// writer): ldoc7 voltage 2856 mV, mode HPM (7, the motor draws tens of mA), enable 1; then after the on time enable 0. The on time
// is clamped to 10..1000 ms and the rail is always switched off again, whatever the vote result.
//
// Bits (independent): 1 publish the service (IOService "T970Haptics" under IOResources), 2 one 150 ms test pulse at boot.
// t970-haptics=2 is the motor test alone, =3 test + service. Userspace pulses the motor with IORegistryEntrySetCFProperty(service, "Pulse", <ms>); the property "PulseCount" and
// the BootLog row [t970 haptics] say what happened. iPadOS itself has no vibration path (no iPad has a motor, MobileGestalt says so),
// so nothing in the stock system calls this yet: the hookup is this service (routes from iPadOS to it: docs/haptics.md).
//
// Status t970_haptics[]: [0] stage (1 class registered, 2 published, 3 test pulse done, negative = failed step), [1] last vote rc
// (0 ok, -200 RSC not mapped yet, -100 no t970_rpmh_write), [2] pulses, [3] last on time ms, [4] failed votes, [5] boot test rc,
// [6] requests refused (busy / out of range), [7] rail-off rc of the last pulse.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
void t970_haptics_static_init(void) __asm__("__GLOBAL__sub_I_T970Haptics.cpp");
extern kmod_info_t g_kernel_kmod_info;
int t970_rpmh_write(const char *name, uint32_t word, uint32_t value) __attribute__((weak_import));
volatile int32_t t970_haptics[8];
}

namespace {
constexpr const char *kRail = "ldoc7";
constexpr uint32_t kMillivolts = 2856, kModeHpm = 7;
constexpr uint32_t kMinMs = 10, kMaxMs = 1000, kBootTestMs = 150;

IOLock *gLock;
uint32_t gPendingMs;     // under gLock: next pulse, 0 = none
bool gBusy;              // under gLock: a pulse is running

int vote(uint32_t word, uint32_t value) {
    if (!&t970_rpmh_write) return -100;
    // The touch worker maps the command DB and the RSC at start-up; until then every call answers -200. Wait up to 30 s.
    int rc = -200;
    for (unsigned i = 0; i < 300 && rc == -200; ++i) {
        rc = t970_rpmh_write(kRail, word, value);
        if (rc == -200) IOSleep(100);
    }
    return rc;
}

// One pulse on the calling thread. Only the worker calls it.
int pulse(uint32_t ms) {
    volatile int32_t *h = t970_haptics;
    if (ms < kMinMs) ms = kMinMs;
    if (ms > kMaxMs) ms = kMaxMs;
    int rc = vote(0, kMillivolts);
    if (!rc) rc = vote(8, kModeHpm);
    if (!rc) rc = vote(4, 1);
    if (!rc) IOSleep(ms);
    const int off = vote(4, 0);   // always, even after a failed step
    h[1] = rc; h[3] = int32_t(ms); h[7] = off;
    if (rc || off) ++h[4]; else ++h[2];
    return rc ? rc : off;
}
}

class T970Haptics : public IOService {
    OSDeclareDefaultStructors(T970Haptics)
public:
    bool init(OSDictionary *dictionary = nullptr) override {
        if (!IOService::init(dictionary)) return false;
        setName("T970Haptics");
        setProperty("DeviceName", "COINDC vibration motor (PM8150L LDO7)");
        setProperty("MinPulseMs", kMinMs, 32);
        setProperty("MaxPulseMs", kMaxMs, 32);
        return true;
    }
    // {"Pulse": <ms>}: queue one pulse. A request while one runs or is queued is refused (kIOReturnBusy).
    IOReturn setProperties(OSObject *properties) override {
        OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
        OSNumber *ms = dict ? OSDynamicCast(OSNumber, dict->getObject("Pulse")) : nullptr;
        if (!ms) return kIOReturnUnsupported;
        const uint64_t value = ms->unsigned64BitValue();
        if (value < kMinMs || value > kMaxMs) {++t970_haptics[6]; return kIOReturnBadArgument;}
        IOLockLock(gLock);
        const bool busy = gBusy || gPendingMs;
        if (!busy) {gPendingMs = uint32_t(value); IOLockWakeup(gLock, &gPendingMs, true);}
        IOLockUnlock(gLock);
        if (busy) {++t970_haptics[6]; return kIOReturnBusy;}
        return kIOReturnSuccess;
    }
    void count() {setProperty("PulseCount", (unsigned long long)t970_haptics[2], 32);}
};

OSDefineMetaClassAndStructors(T970Haptics, IOService)

namespace {
unsigned gBits;

void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "t970-haptics");
    volatile int32_t *h = t970_haptics;
    if (gBits & 2) {   // the boot test pulse runs first and does not wait for the service
        h[5] = pulse(kBootTestMs);
        h[0] = 3;
    }
    if (!(gBits & 1)) return;
    OSDictionary *matching = IOService::resourceMatching("IOBSD");
    IOService *provider = matching ? IOService::waitForMatchingService(matching, 60000000000ULL) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!provider) {h[0] = -1; return;}
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {provider->release(); h[0] = -2; return;}
    t970_haptics_static_init();
    if (OSMetaClass::postModLoad(handle) != kOSReturnSuccess) {provider->release(); h[0] = -3; return;}
    h[0] = 1;
    T970Haptics *service = new T970Haptics;
    const bool attached = service && service->init() && service->attach(provider);
    if (!attached || !service->start(provider)) {
        if (attached) service->detach(provider);
        OSSafeReleaseNULL(service);
        provider->release();
        h[0] = -4;
        return;
    }
    provider->release();
    service->count();
    service->registerService();
    h[0] = 2;
    for (;;) {
        IOLockLock(gLock);
        while (!gPendingMs) IOLockSleep(gLock, &gPendingMs, THREAD_UNINT);
        const uint32_t ms = gPendingMs;
        gPendingMs = 0;
        gBusy = true;
        IOLockUnlock(gLock);
        (void)pulse(ms);
        service->count();
        IOLockLock(gLock);
        gBusy = false;
        IOLockUnlock(gLock);
    }
}
}

extern "C" void t970_haptics_start(void) {
    unsigned bits = 0;
    if (!PE_parse_boot_argn("t970-haptics", &bits, sizeof bits) || !(bits & 3)) return;
    static unsigned started;
    if (__atomic_exchange_n(&started, 1u, __ATOMIC_ACQ_REL)) return;
    gBits = bits;
    gLock = IOLockAlloc();
    if (!gLock) {t970_haptics[0] = -6; return;}
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else t970_haptics[0] = -7;
}
