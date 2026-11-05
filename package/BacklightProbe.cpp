// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi (Galaxy Tab S7+, SM8250) panel brightness through the DSI command FIFO.
// Boot argument t970-bl=<n> (default 3, 0 off):
//   1  read-only: map DSI0 and DSI1 and record their registers. No write of any kind.
//   2  the visible test: maximum, minimum, then t970-bl-level=<10..2047> (default 1029), two seconds apart
//   3  t970-bl-level, then the iOS brightness service (presented as AppleARMBacklight): iOS sets the level
// t970-bl-dsi=<mask>: which controllers carry the commands, 1 DSI0, 2 DSI1, 3 both (default). The panel is
// split over both links and Samsung's driver broadcasts every command (samsung,not-support-single-tx); this
// driver sends the same packets on each selected link in turn instead of a hardware broadcast.
//
// The panel and its commands are documented in BacklightCore.h (S6TUUM3_AMSA24VU01, DCS 0x68/0x51/0x53, DBV
// 10..2047). The live FDT gives the two controllers: dsi-ctrl-0 reg 0xae94000, dsi-ctrl-1 reg 0xae96000, both
// qcom,dsi-ctrl-hw-v2.4, so the TPG FIFO path of dsi_ctrl_hw_cmn_kickoff_fifo_command applies unchanged from the
// alioth version of this file (same SoC, same controller): no DMA buffer and no display SMMU mapping.
// The panel is dsi_cmd_mode, so the transfer is not scheduled for a front-porch line; the software trigger
// starts it once the command engine is idle between frames.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include "BacklightCore.h"

// BootLog.c already composes two rows from these, in sheng's wording. On this board the same
// slots carry the DSI controllers, documented here so the row can be read (the start values are DSI0's):
//   [0] stage (1 mapped, 11..13 test steps and the starting level, 2 level set, 15..17 service start/register, 3 published,
//       negative: the step that failed; -7 no link took a packet)
//   [1] brightness requests taken          [2] last level written
//   [3] DCS 0x52 read-back at start (raw)  [4] DCS 0x52 read-back after the visible test (raw)
//   [5] transfers that failed              [6] last INT_CTRL of a failed transfer
//   [7] DSI_CTRL at start (bit 0 DSI_EN, bit 1 video engine, bit 2 command engine)
//   [8] DSI_STATUS at start (bit 2 command-mode MDP busy)
//   [9] last error                         [10] commands sent
//   [11] commands timed out                [12] last command's time in microseconds
//   [13] HW_VERSION                        [14] TPG_CTRL at start
//   [15] COMMAND_MODE_DMA_CTRL at start
extern "C" {volatile int32_t native_backlight_stats[16];}
// [0] goal level, [1] fade ms, [2] request actions taken, [3] fades begun,
// [4] reads that returned nothing, [5] last read-back count (RDBK_DATA_CTRL bits 31:16).
extern "C" {volatile int32_t native_backlight_request[6];}

namespace {
// dsi-ctrl-0 reg = <0xae94000 0x400 ...>, dsi-ctrl-1 reg = <0xae96000 0x400 ...> (live FDT). Every register below
// is inside those.
constexpr uint64_t kDsiBase[2] = {0xae94000, 0xae96000}, kDsiBytes = 0x400;
constexpr uint32_t kHwVersion = 0x000, kCtrl = 0x004, kStatus = 0x008, kRdbkData0 = 0x06c,
                   kDmaCtrl = 0x03c, kDmaLength = 0x04c, kIntCtrl = 0x110, kTpgCtrl = 0x15c,
                   kTpgInit = 0x17c, kRdbkDataCtrl = 0x1d4, kTpgFifoReset = 0x1ec, kSwTrigger = 0x090;
constexpr uint32_t kStatusMdpBusy = 1u << 2, kIntDmaDone = 1u << 0;
// The level range, the DCS commands and the packet words live in BacklightCore.h, where a host
// test checks them. These aliases keep the driver's own names.
constexpr uint32_t kMaxLevel = BL_MAX_LEVEL, kMinLevel = BL_MIN_LEVEL, kDefaultLevel = BL_DEFAULT_LEVEL;
constexpr uint32_t kTimeoutUs = 50000, kIdleTimeoutUs = 20000;
constexpr uint8_t kDcsBrightnessGet = BL_DCS_BRIGHTNESS_GET,
                  kDcsDisplayOn = BL_DCS_DISPLAY_ON, kDcsDisplayOff = BL_DCS_DISPLAY_OFF,
                  kDcsSleepOut = BL_DCS_SLEEP_OUT, kDcsSleepIn = BL_DCS_SLEEP_IN;

IOMemoryMap *gMap[2];
volatile uint32_t *gCtrl[2];
volatile uint32_t *gDsi;   // the controller the next packet goes to
unsigned gLinks = 3;       // t970-bl-dsi
unsigned gUsable = 0;      // links whose first packet completed (only those carry the level)
uint32_t gLevel = kDefaultLevel;   // t970-bl-level, and where the service starts
IOLock *gLock;

uint32_t rd(uint32_t reg) {return gDsi[reg / 4];}
void wr(uint32_t reg, uint32_t value) {gDsi[reg / 4] = value; __asm__ volatile("dsb sy" ::: "memory");}
void fail(int step, int error) {native_backlight_stats[9] = error; native_backlight_stats[0] = -step;}
void bump(int index) {native_backlight_stats[index] = native_backlight_stats[index] + 1;}

// One packet through the TPG FIFO. words holds the header and payload words; count is how many.
// Returns 0, or -1 when the DMA-done interrupt never came or the command path stayed busy.
int sendPacket(const uint32_t *words, unsigned count, bool lp) {
    const uint64_t began = mach_absolute_time();
    // Take the FIFO only when the command path is idle, as the vendor driver does before every
    // transfer: a busy engine means an earlier packet is still in flight.
    bool idle = false;
    for (uint32_t waited = 0; waited < kIdleTimeoutUs; waited += 2) {
        if (!(rd(kStatus) & kStatusMdpBusy)) {idle = true; break;}
        IODelay(2);
    }
    if (!idle) {bump(5); native_backlight_stats[6] = (int32_t)rd(kStatus); return -1;}
    wr(kIntCtrl, (rd(kIntCtrl) & ~((1u << 24) | (1u << 28))) | kIntDmaDone);   // clear a stale done
    wr(kTpgCtrl, (1u << 1) | (1u << 2) | (3u << 16));                          // TPG enable, FIFO mode, custom pattern
    for (unsigned i = 0; i < count; ++i) wr(kTpgInit, words[i]);
    if (count & 1) wr(kTpgInit, 0);                                            // the extra DWORD the hardware wants
    // gts7xlwifi: DMA_CTRL goes back to what the boot loader left after the packet. v84 left embedded mode set, and the
    // boot stopped a few seconds after the visible test.
    const uint32_t dmaBefore = rd(kDmaCtrl);
    uint32_t dma = dmaBefore & ~((1u << 31) | (1u << 30));
    dma = lp ? (dma | (1u << 26)) : (dma & ~(1u << 26));
    wr(kDmaCtrl, dma | (1u << 28));                                            // embedded mode
    wr(kDmaLength, count * 4);
    wr(kSwTrigger, 1);
    bool done = false;
    for (uint32_t waited = 0; waited < kTimeoutUs; waited += 20) {
        if (rd(kIntCtrl) & kIntDmaDone) {done = true; break;}
        IODelay(20);
    }
    if (!done) {bump(5); bump(11); native_backlight_stats[6] = (int32_t)rd(kIntCtrl);}
    wr(kIntCtrl, (rd(kIntCtrl) & ~((1u << 24) | (1u << 28))) | kIntDmaDone);
    wr(kTpgCtrl, 0);
    wr(kDmaCtrl, dmaBefore);
    wr(kTpgFifoReset, 1);
    IODelay(1);
    wr(kTpgFifoReset, 0);
    bump(10);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    native_backlight_stats[12] = (int32_t)(ns / 1000);
    return done ? 0 : -1;
}

// One DCS long write on every selected link. Returns 0 when every link took it.
int longWrite(const uint8_t *bytes, unsigned count) {
    uint32_t words[BL_MAX_PACKET_WORDS];
    const unsigned n = bl_pack_long_write(bytes, count, words, BL_MAX_PACKET_WORDS);
    if (!n) return -1;
    int rc = 0;
    for (unsigned link = 0; link < 2; ++link) {
        if (!(gLinks & gUsable & (1u << link)) || !gCtrl[link]) continue;
        gDsi = gCtrl[link];
        rc |= sendPacket(words, n, false);
    }
    gDsi = gCtrl[0];
    return rc;
}

// Sets the panel's brightness (a DBV) with Samsung's normal-level sequence (BacklightCore.h). Any level
// outside 10..2047 is clamped: 0 is the minimum, not display off.
int writeBrightness(uint32_t level) {
    level = bl_clamp_level(level);
    static const uint8_t gamma[] = {0x68, 0x19, 0x20, 0x00, 0x14};
    static const uint8_t control[] = {BL_DCS_CONTROL, 0x20};
    uint8_t dbv[3];
    bl_brightness_bytes(level, dbv);
    int rc = longWrite(gamma, sizeof gamma);
    rc |= longWrite(dbv, sizeof dbv);
    rc |= longWrite(control, sizeof control);
    if (!rc) native_backlight_stats[2] = (int32_t)level;
    return rc;
}

// The panel's own brightness register (DCS 0x52, two bytes), read on DSI0. Returns the raw value or -1.
// The controller must be told how large a return packet to expect before the read
// (SET_MAXIMUM_RETURN_PACKET_SIZE, dsi_set_max_return_size in dsi_ctrl.c), which is a short
// write with one parameter.
[[maybe_unused]] int32_t readBrightness() {
    wr(kRdbkDataCtrl, 0x1);
    __asm__ volatile("dsb sy" ::: "memory");
    wr(kRdbkDataCtrl, 0x0);
    __asm__ volatile("dsb sy" ::: "memory");
    const uint32_t size[2] = {bl_max_return_header(2), bl_max_return_payload(2)};
    if (sendPacket(size, 2, false)) return -1;
    const uint32_t read[1] = {bl_read_header(kDcsBrightnessGet)};
    if (sendPacket(read, 1, false)) return -1;
    IOSleep(2);
    const uint32_t count = rd(kRdbkDataCtrl) >> 16;
    native_backlight_request[5] = (int32_t)count;
    if (!count) {bump(4); return -1;}
    // RDBK_DATA0..3 carry the returned packet, read highest word first and byte-swapped
    // (get_cmd_read_data, dsi_ctrl_hw_cmn.c:828-860). The payload is the two brightness bytes.
    const uint32_t word = __builtin_bswap32(rd(kRdbkData0));
    (void)kDcsDisplayOn; (void)kDcsDisplayOff; (void)kDcsSleepOut; (void)kDcsSleepIn;
    return (int32_t)bl_decode_brightness(word);
}

// Reads only: what the boot loader left the controller in. Called from the read-only mode.
void snapshot() {
    native_backlight_stats[13] = (int32_t)rd(kHwVersion);
    native_backlight_stats[7] = (int32_t)rd(kCtrl);
    native_backlight_stats[8] = (int32_t)rd(kStatus);
    native_backlight_stats[14] = (int32_t)rd(kTpgCtrl);
    native_backlight_stats[15] = (int32_t)rd(kDmaCtrl);
}
}  // namespace

// The iOS-facing service, shaped after the two ports that already answer CoreBrightness
// (asteroids' AsteroidsBacklight.cpp and sheng's AppleARMBacklight.cpp): publish
// IODisplayParameters with min/max, accept brightness and FadePeriod in setProperties, step the
// fade on a 16 ms timer, and present as AppleARMBacklight so backboardd's sandbox profile lets
// it set those keys.
class T970Backlight : public IOService {
    OSDeclareDefaultStructors(T970Backlight)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *fade = nullptr;
    uint32_t level = 0, from = 0, to = 0;
    uint64_t start = 0, period = 0;

    static uint64_t now() {return mach_absolute_time();}

    void publish(uint32_t value) {
        OSDictionary *brightness = OSDictionary::withCapacity(3), *parameters = OSDictionary::withCapacity(1);
        OSNumber *min = OSNumber::withNumber((uint64_t)kMinLevel, 32), *max = OSNumber::withNumber((uint64_t)kMaxLevel, 32),
                 *current = OSNumber::withNumber((uint64_t)value, 32);
        if (brightness && parameters && min && max && current) {
            brightness->setObject("min", min); brightness->setObject("max", max); brightness->setObject("value", current);
            parameters->setObject("brightness", brightness);
            setProperty("IODisplayParameters", parameters);
        }
        OSSafeReleaseNULL(min); OSSafeReleaseNULL(max); OSSafeReleaseNULL(current);
        OSSafeReleaseNULL(brightness); OSSafeReleaseNULL(parameters);
    }
    // Presents as AppleARMBacklight: the vtable is copied and getMetaClass (slot 7) answers that
    // metaclass, so IOObjectConformsTo and the sandbox walk both see the name CoreBrightness looks
    // for. The same technique and the same reason as sheng (int-275/276) and asteroids.
    static inline const OSMetaClass *gAppleClass = nullptr;
    static const OSMetaClass *appleMetaClass(const OSMetaClassBase *) {return gAppleClass;}
    bool impersonate() {
        enum {kSlots = 320, kGetMetaClass = 7};
        const OSSymbol *name = OSSymbol::withCString("AppleARMBacklight");
        gAppleClass = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
        OSSafeReleaseNULL(name);
        if (!gAppleClass) return false;
        uintptr_t *copy = static_cast<uintptr_t *>(IOMalloc(kSlots * sizeof(uintptr_t)));
        if (!copy) return false;
        memcpy(copy, *reinterpret_cast<uintptr_t **>(this), kSlots * sizeof(uintptr_t));
        copy[kGetMetaClass] = reinterpret_cast<uintptr_t>(&appleMetaClass);
        __asm__ volatile("dmb ish" ::: "memory");
        *reinterpret_cast<uintptr_t **>(this) = copy;
        return true;
    }
    // Steps a fade from the level now shown to the goal, on the service's own timer.
    static void fadeStep(OSObject *owner, IOTimerEventSource *sender) {
        T970Backlight *self = static_cast<T970Backlight *>(owner);
        if (!self) return;
        const uint64_t elapsed = now() - self->start;
        uint32_t next = self->to;
        if (elapsed < self->period)
            next = (uint32_t)((int64_t)self->from + ((int64_t)self->to - (int64_t)self->from) * (int64_t)elapsed / (int64_t)self->period);
        if (next != self->level) {
            IOLockLock(gLock);
            writeBrightness(next);
            IOLockUnlock(gLock);
            self->level = next;
        }
        if (next != self->to) sender->setTimeoutMS(16);
    }
    // A request taken on the work loop: fade from the level now shown to the newest goal.
    static IOReturn takeRequest(OSObject *owner, void *, void *, void *, void *) {
        T970Backlight *self = static_cast<T970Backlight *>(owner);
        if (!self) return kIOReturnBadArgument;
        const uint32_t goal = (uint32_t)native_backlight_request[0];
        const uint64_t milliseconds = (uint64_t)native_backlight_request[1];
        self->fade->cancelTimeout();
        self->from = self->level;
        self->to = goal;
        self->start = now();
        nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &self->period);
        native_backlight_request[3] = native_backlight_request[3] + 1;
        if (!self->period) {
            IOLockLock(gLock);
            writeBrightness(goal);
            IOLockUnlock(gLock);
            self->level = goal;
            return kIOReturnSuccess;
        }
        fadeStep(self, self->fade);
        return kIOReturnSuccess;
    }
    static IOReturn begin(OSObject *owner, void *, void *, void *, void *) {
        T970Backlight *self = static_cast<T970Backlight *>(owner);
        if (!self) return kIOReturnBadArgument;
        // No DCS 0x52 read: v84 stopped right after the read-back (a bus turnaround to the panel). The service starts from the
        // level the visible test left.
        native_backlight_stats[0] = 16;
        self->level = gLevel;
        self->publish(self->level);
        // Only the built-in display gets backlight-control, and only with the conformance above:
        // sheng measured (int-236) that publishing it without the AppleARMBacklight conformance
        // stops the libEDR server from starting and freezes the screen.
        if (self->impersonate()) {
            self->setProperty("NativeSandboxGrantProcess", "backboardd");
            self->setProperty("backlight-control", true);
        }
        native_backlight_stats[0] = 17;
        self->registerService();
        native_backlight_stats[0] = 3;
        return kIOReturnSuccess;
    }
public:
    bool startService() {
        loop = IOWorkLoop::workLoop();
        fade = IOTimerEventSource::timerEventSource(this, &T970Backlight::fadeStep);
        if (!loop || !fade || loop->addEventSource(fade)) return false;
        loop->runAction(&T970Backlight::begin, this);
        return true;
    }
    IOReturn setProperties(OSObject *properties) override {
        OSDictionary *dictionary = OSDynamicCast(OSDictionary, properties);
        if (!dictionary) return kIOReturnBadArgument;
        OSNumber *brightness = OSDynamicCast(OSNumber, dictionary->getObject("brightness"));
        if (!brightness) return kIOReturnSuccess;
        OSNumber *fadePeriod = OSDynamicCast(OSNumber, dictionary->getObject("FadePeriod"));
        const int32_t requested = (int32_t)brightness->unsigned32BitValue();
        const uint32_t goal = requested < 0 ? 0 : (uint32_t)requested > kMaxLevel ? kMaxLevel : (uint32_t)requested;
        uint64_t milliseconds = fadePeriod ? ((uint64_t)fadePeriod->unsigned32BitValue() * 1000) >> 16 : 0;
        if (milliseconds > 5000) milliseconds = 5000;
        bump(1);
        native_backlight_request[0] = (int32_t)goal;
        native_backlight_request[1] = (int32_t)milliseconds;
        native_backlight_request[2] = native_backlight_request[2] + 1;
        // Handed to the work loop without waiting for it: a runAction here would block
        // backboardd's brightness thread on the work loop's gate (sheng int-277 measured 53
        // requests in the first 41 s and none after).
        loop->runAction(&T970Backlight::takeRequest, this, nullptr, nullptr, nullptr);
        publish(goal);
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(T970Backlight, IOService)

// gts7xlwifi: no preModLoad/postModLoad here. build-gop-legacy-display.py leaves this object's __mod_init_func in place (only
// NativeLegacyDisplay, the presenter and the HID publisher are staged late), so the kernel's own init already registered
// T970Backlight's metaclass. Running the static initializer a second time from this thread (v84-v86) rebuilt that registered
// metaclass and stopped the boot at stats[0] = 14.

namespace {
unsigned gMode;

void panelThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "t970-bl");
    // Mode 1 is read-only: snapshot is the whole job, and the thread returns without writing.
    if (gMode == 1) return;
    // Which links take a packet: the first of Samsung's three normal-level writes, sent alone on each link. v84 saw every
    // packet on one link time out (6 of 12). native_backlight_request[4] = usable mask, [5] = links tried.
    IOLockLock(gLock);
    static const uint8_t probe[] = {0x68, 0x19, 0x20, 0x00, 0x14};
    uint32_t words[BL_MAX_PACKET_WORDS];
    const unsigned n = bl_pack_long_write(probe, sizeof probe, words, BL_MAX_PACKET_WORDS);
    for (unsigned link = 0; link < 2; ++link) {
        if (!(gLinks & (1u << link))) continue;
        gDsi = gCtrl[link];
        native_backlight_request[5] = native_backlight_request[5] | (int32_t)(1u << link);
        if (!sendPacket(words, n, false)) gUsable |= 1u << link;
    }
    gDsi = gCtrl[0];
    native_backlight_request[4] = (int32_t)gUsable;
    IOLockUnlock(gLock);
    if (!gUsable) {fail(7, -5); return;}
    // The starting level (t970-bl-level, default 1029). Mode 2 adds the visible test before it: maximum, then minimum, 2 s each
    // (v84-v86 showed both on the panel). stats[0] 11..13 mark the steps.
    if (gMode == 2) {
        native_backlight_stats[0] = 11;
        IOLockLock(gLock);
        writeBrightness(kMaxLevel);
        IOLockUnlock(gLock);
        IOSleep(2000);
        native_backlight_stats[0] = 12;
        IOLockLock(gLock);
        writeBrightness(kMinLevel);
        IOLockUnlock(gLock);
        IOSleep(2000);
    }
    native_backlight_stats[0] = 13;
    IOLockLock(gLock);
    writeBrightness(gLevel);
    IOLockUnlock(gLock);
    native_backlight_stats[0] = 2;
    if (gMode == 2) return;
    // The service.
    native_backlight_stats[0] = 15;
    T970Backlight *backlight = OSTypeAlloc(T970Backlight);
    if (!backlight || !backlight->init() || !backlight->attach(IOService::getPlatform()) || !backlight->startService()) {
        if (backlight) backlight->release();
        fail(6, -19);
        return;
    }
    backlight->release();
}
}  // namespace

extern "C" void t970_dpu_set_refresh(void);
// The entry point keeps alioth's name: LegacyDisplayProbe.cpp calls it.
extern "C" void alioth_backlight_probe(void) {
    t970_dpu_set_refresh();
    // On by default (mode 3): the kernel command line is at its 1000-byte limit. t970-bl=0 turns it off.
    unsigned mode = 3;
    PE_parse_boot_argn("t970-bl", &mode, sizeof mode);
    if (!mode || mode > 3 || native_backlight_stats[0]) return;
    PE_parse_boot_argn("t970-bl-level", &gLevel, sizeof gLevel);
    gLevel = bl_clamp_level(gLevel);
    PE_parse_boot_argn("t970-bl-dsi", &gLinks, sizeof gLinks);
    if (!(gLinks & 3)) gLinks = 3;
    gLock = IOLockAlloc();
    for (unsigned link = 0; link < 2; ++link) {
        IODeviceMemory *memory = IODeviceMemory::withRange(kDsiBase[link], kDsiBytes);
        gMap[link] = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (memory) memory->release();
        gCtrl[link] = gMap[link] ? reinterpret_cast<volatile uint32_t *>(gMap[link]->getVirtualAddress()) : nullptr;
    }
    gDsi = gCtrl[0];
    if (!gLock || !gCtrl[0] || !gCtrl[1]) {gDsi = nullptr; fail(1, -12); return;}
    snapshot();
    native_backlight_stats[0] = 1;
    gMode = mode;
    thread_t thread = nullptr;
    if (kernel_thread_start(panelThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else fail(4, -12);
}

// gts7xlwifi: display timing (porting guide, display step 3, DisplayTimingProbe): read-only samples of the DPU the boot loader left
// running. Live FDT mdss_mdp@ae00000: qcom,sde-pp-off 0x71000 + 0x800 * n (n 0..5), qcom,sde-intf-off 0x6b000/0x6b800/0x6c000/0x6c800
// (dp, dsi, dsi, dp). Register offsets from Samsung's techpack/display/msm/sde: PP_TEAR_CHECK_EN 0x000, PP_SYNC_CONFIG_VSYNC 0x004,
// PP_SYNC_CONFIG_HEIGHT 0x008, PP_INT_COUNT_VAL 0x014 (read pointer frame count in bits 31:16, line count in 15:0), PP_LINE_COUNT
// 0x02C, PP_AUTOREFRESH_CONFIG 0x030; INTF_TIMING_ENGINE_EN 0x000, INTF_FRAME_COUNT 0x0AC, INTF_STATUS 0x26C. Each call gives the
// pingpongs with tear check on: their frame counts' rate since the last call (frames per second x10) and configuration.
extern "C" volatile uint32_t native_t970_dpu[5];
namespace {
volatile uint32_t *gMdp;
uint32_t gPpFrames[6];
uint64_t gPpAt;
}
extern "C" int t970_dpu_timing_row(char *row, size_t cap)
{
    if (!gMdp) {
        IODeviceMemory *memory = IODeviceMemory::withRange(0xae00000, 0x84208);
        IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (memory) memory->release();
        if (!map) return snprintf(row, cap, "[dpu] no map");
        gMdp = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
    }
    auto rd = [](uint32_t off) {return gMdp[off / 4];};
    const uint64_t now = mach_absolute_time();
    uint64_t ns = 0;
    if (gPpAt) absolutetime_to_nanoseconds(now - gPpAt, &ns);
    int at = snprintf(row, cap, "[dpu] ar %x>%x waits %u late %u us %u |", native_t970_dpu[0], native_t970_dpu[1], native_t970_dpu[2],
                      native_t970_dpu[3], native_t970_dpu[4]);
    for (unsigned n = 0; n < 6 && at < (int)cap - 60; ++n) {
        const uint32_t base = 0x71000 + 0x800 * n;
        const uint32_t te = rd(base), vsync = rd(base + 0x4), height = rd(base + 0x8), count = rd(base + 0x14), line = rd(base + 0x2c),
                       autoref = rd(base + 0x30);
        const uint32_t frames = count >> 16;
        const uint32_t rate = ns && gPpAt ? (uint32_t)((uint64_t)((frames - gPpFrames[n]) & 0xffff) * 10000000000ull / ns) : 0;
        gPpFrames[n] = frames;
        if (!te && !autoref && !count) continue;
        at += snprintf(row + at, cap - at, " pp%u te%x v%x h%x f%u l%u/%u ar%x fps%u.%u", n, te, vsync, height, frames, count & 0xffff, line,
                       autoref, rate / 10, rate % 10);
    }
    gPpAt = now;
    for (unsigned i = 0; i < 4 && at < (int)cap - 40; ++i) {
        const uint32_t base = 0x6b000 + 0x800 * i;
        at += snprintf(row + at, cap - at, " if%u en%x fc%x st%x", i, rd(base), rd(base + 0xac), rd(base + 0x26c));
    }
    // v93 measured INTF1 (DSI0) frames at 30.1 per second with every pingpong register at 0: on this DPU the command-mode tear check
    // lives in the interface (sde_hw_intf.c INTF_TEAR_*, 0x280..0x2b8). For INTF1 and INTF2: MDP_VSYNC_SEL, TEAR_CHECK_EN, SYNC_CONFIG_VSYNC,
    // SYNC_CONFIG_HEIGHT, VSYNC_INIT_VAL, INT_COUNT_VAL (frame << 16 | line), START_POS, RD_PTR_IRQ, LINE_COUNT, AUTOREFRESH_CONFIG.
    for (unsigned i = 1; i <= 2 && at < (int)cap - 100; ++i) {
        const uint32_t base = 0x6b000 + 0x800 * i;
        at += snprintf(row + at, cap - at, " te%u sel%x en%x v%x h%x init%x cnt%x pos%x rd%x line%x ar%x", i, rd(base + 0x280), rd(base + 0x284),
                       rd(base + 0x288), rd(base + 0x28c), rd(base + 0x294), rd(base + 0x298), rd(base + 0x2a0), rd(base + 0x2a4),
                       rd(base + 0x2b0), rd(base + 0x2b4));
    }
    return at;
}

// gts7xlwifi: display step 3, the refresh rate and the scanout copy's timing. v94 read INTF1's (DSI0's) tear check:
// INTF_TEAR_AUTOREFRESH_CONFIG = 0x80000002, autorefresh on with a frame count of 2, i.e. one panel update every second TE pulse; the
// panel's TE runs at 60 Hz, so the boot loader left the panel at 30 updates a second (v93 measured 30.1). Writing 0x80000001 updates it on
// every TE (Samsung's sde_hw_intf.c setup_autorefresh writes BIT(31) | frame_count to the same register). Only that exact value is changed.
// native_t970_dpu[0] autorefresh before, [1] after, [2] frame waits, [3] waits that timed out, [4] total microseconds waited.
extern "C" {volatile uint32_t native_t970_dpu[5];}
static constexpr uint32_t kIntf1 = 0x6b800, kIntfFrameCount = 0xac, kIntfAutorefresh = 0x2b4;
extern "C" void t970_dpu_set_refresh(void)
{
    if (!gMdp) {
        IODeviceMemory *memory = IODeviceMemory::withRange(0xae00000, 0x84208);
        IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (memory) memory->release();
        if (!map) return;
        gMdp = reinterpret_cast<volatile uint32_t *>(map->getVirtualAddress());
    }
    volatile uint32_t *reg = &gMdp[(kIntf1 + kIntfAutorefresh) / 4];
    const uint32_t before = *reg;
    native_t970_dpu[0] = before;
    if (before == 0x80000002u) {*reg = 0x80000001u; __asm__ volatile("dsb sy" ::: "memory");}
    native_t970_dpu[1] = *reg;
}
// Called by the GPU presenter right before it hands a frame to the copy: returns just after INTF1's frame counter moves (a panel update
// began or ended), at most 20 ms later, so the 1.7 ms copy runs inside the gap between two updates instead of across one (tearing).
extern "C" void t970_dpu_wait_frame(void)
{
    if (!gMdp) return;
    const uint64_t began = mach_absolute_time();
    const uint32_t start = gMdp[(kIntf1 + kIntfFrameCount) / 4];
    bool moved = false;
    for (unsigned i = 0; i < 80; ++i) {
        IODelay(250);
        if (gMdp[(kIntf1 + kIntfFrameCount) / 4] != start) {moved = true; break;}
    }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    native_t970_dpu[2] = native_t970_dpu[2] + 1;
    if (!moved) native_t970_dpu[3] = native_t970_dpu[3] + 1;
    native_t970_dpu[4] = native_t970_dpu[4] + (uint32_t)(ns / 1000);
}
