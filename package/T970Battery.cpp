// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi battery (periph14, boot argument t970-battery=1): an IOPMPowerSource whose values come from the MAX77705's fuel gauge
// (ModelGauge m5, I2C 0x36) and charger (0x69), both on QUP SE0 beside the MUIC (T970WifiProbe.cpp t970_se0_*). powerd reads every
// IOPMPowerSource (CurrentCapacity/MaxCapacity, IsCharging, ExternalConnected, Voltage, Amperage) and the status bar, the lock
// screen's charging state and Settings > Battery follow from updateStatus(). Structure after SM-T733 package/T733Battery.cpp; the
// conversions are Samsung's drivers/battery_v2/max77705_fuelgauge.c with this board's fuelgauge,fg_resistor = 5 (live FDT):
//   RepSOC 0x06   (hi * 100 + lo * 100 / 256) / 10 = permille
//   VCell  0x09   (w & 0xfff) * 78125 / 1e6 + (((w & 0xf000) >> 4) * 78125 / 1e6 << 4) = mV
//   Current 0x0a  signed, raw * 15625 * fg_resistor / 100000 = mA (positive charging)
//   Temp   0x08   signed 1/256 C
//   FullCapRep 0x35, DesignCap 0x18   raw * fg_resistor / 2 = mAh;   Cycles 0x17  raw / 100 (Samsung keeps 1 % steps)
// Charger: CHG_INT_OK 0xb2 (bit 6 CHGIN_OK), CHG_DETAILS_01 0xb4 bits 3:0 (0-2 charging, 3-4 done, 5-7 not charging, 8/a/b off).
// Polled every 10 s, and at once when CHGIN_OK changes between polls of 1 s while plugged. One write: Samsung's charger driver turns on
// the charger watchdog (CHG_CNFG_00 0xb7 bit 4 WDTEN) and kicks it (max77705_chg_set_wdtmr_kick: CHG_CNFG_06 0xbd bits 1:0 = 01);
// under iPadOS nobody did, and periph14 read CHG_DETAILS_01 = 0x3b (state 0xb, charger off) with CHGIN_OK set. This kicks it every
// 10 s the same way (read-modify-write of bits 1:0 only).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <kern/thread.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern "C" {
void t970_battery_static_init(void) __asm__("__GLOBAL__sub_I_T970Battery.cpp");
extern kmod_info_t g_kernel_kmod_info;
bool t970_se0_read(uint8_t address, uint8_t reg, uint8_t *data, unsigned count);
bool t970_se0_write(uint8_t address, uint8_t reg, uint8_t value);
// [0] stage (1 class registered, 2 published, negative: failed step), [1] permille, [2] mV, [3] mA, [4] gauge temperature deci-C,
// [5] flags (1 external, 2 charging, 4 full, 8 present), [6] full mAh, [7] design mAh, [8] cycles, [9] samples, [10] read failures,
// [11] CHG_DETAILS_01 | CHG_INT_OK << 8, [12] watchdog kicks
volatile int32_t t970_battery[13];
}

namespace {
constexpr uint8_t kGauge = 0x36, kCharger = 0x69;
constexpr int kResistor = 5;

struct Sample {
    bool ok;
    int permille, mv, ma, decic, fullMah, designMah, cycles;
    bool external, charging, full;
    uint8_t details, intOk;
};

bool word(uint8_t reg, uint16_t *value) {
    uint8_t b[2] = {};
    if (!t970_se0_read(kGauge, reg, b, 2)) return false;
    *value = uint16_t(b[0] | b[1] << 8);
    return true;
}

bool readSample(Sample &s) {
    s = {};
    uint16_t soc = 0, vcell = 0, cur = 0, temp = 0, full = 0, design = 0, cycles = 0;
    if (!word(0x06, &soc) || !word(0x09, &vcell) || !word(0x0a, &cur)) return false;
    (void)word(0x08, &temp); (void)word(0x35, &full); (void)word(0x18, &design); (void)word(0x17, &cycles);
    s.permille = ((soc >> 8) * 100 + (soc & 0xff) * 100 / 256) / 10;
    if (s.permille > 1000) s.permille = 1000;
    s.mv = int((vcell & 0xfff) * 78125u / 1000000u + ((((vcell & 0xf000u) >> 4) * 78125u / 1000000u) << 4));
    s.ma = int(int16_t(cur)) * 15625 * kResistor / 100000;
    s.decic = int(int16_t(temp)) * 10 / 256;
    s.fullMah = full * kResistor / 2;
    s.designMah = design * kResistor / 2;
    s.cycles = cycles / 100;
    uint8_t intOk = 0, details = 0;
    if (t970_se0_read(kCharger, 0xb2, &intOk, 1) && t970_se0_read(kCharger, 0xb4, &details, 1)) {
        s.intOk = intOk; s.details = details;
        s.external = intOk & 0x40;
        const unsigned dtls = details & 0x0f;
        // 0-2 pre-charge / fast constant current / constant voltage, 3 top-off (still charging, slowly), 4 done.
        s.charging = s.external && dtls <= 3;
        s.full = s.external && dtls == 4;
    }
    s.ok = true;
    return true;
}
}

class T970Battery : public IOPMPowerSource {
    OSDeclareDefaultStructors(T970Battery)
    bool published_ = false;

    void setNumber(const char *name, int value) {
        const OSSymbol *key = OSSymbol::withCString(name);
        OSNumber *number = OSNumber::withNumber((unsigned long long)(long long)value, 32);
        if (key && number) setPSProperty(key, number);
        OSSafeReleaseNULL(number);
        OSSafeReleaseNULL(key);
    }
    void setBoolean(const char *name, bool value) {
        const OSSymbol *key = OSSymbol::withCString(name);
        if (key) setPSProperty(key, value ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(key);
    }

public:
    bool init() override {
        if (!IOPMPowerSource::init()) return false;
        setName("T970Battery");
        setProperty("Built-In", kOSBooleanTrue);
        setProperty("DeviceName", "MAX77705 Battery");
        setTimeRemaining(-1);
        return true;
    }
    void accept(const Sample &s) {
        setBatteryInstalled(true);
        setCurrentCapacity(unsigned(s.permille) / 10);
        setMaxCapacity(100);
        setAtWarnLevel(s.permille <= 150);
        setAtCriticalLevel(s.permille <= 50);
        setVoltage(unsigned(s.mv));
        setAmperage(s.ma);
        setNumber("InstantAmperage", s.ma);
        setExternalConnected(s.external);
        setBoolean("AppleRawExternalConnected", s.external);
        setIsCharging(s.charging);
        setBoolean("FullyCharged", s.full);
        if (s.designMah > 0) setNumber("DesignCapacity", s.designMah);
        if (s.fullMah > 0) setNumber("AppleRawMaxCapacity", s.fullMah);
        setCycleCount(unsigned(s.cycles));
        setNumber("Temperature", s.decic * 10);   // IOPMPowerSource reports hundredths of a degree
        updateStatus();
        if (!published_) {registerService(); published_ = true; t970_battery[0] = 2;}
    }
};

OSDefineMetaClassAndStructors(T970Battery, IOPMPowerSource)

namespace {
void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "t970-battery");
    volatile int32_t *b = t970_battery;
    OSDictionary *matching = IOService::resourceMatching("IOBSD");
    IOService *provider = matching ? IOService::waitForMatchingService(matching, 60000000000ULL) : nullptr;
    OSSafeReleaseNULL(matching);
    if (!provider) {b[0] = -1; return;}
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {provider->release(); b[0] = -2; return;}
    t970_battery_static_init();
    if (OSMetaClass::postModLoad(handle) != kOSReturnSuccess) {provider->release(); b[0] = -3; return;}
    b[0] = 1;
    T970Battery *battery = new T970Battery;
    const bool attached = battery && battery->init() && battery->attach(provider);
    if (!attached || !battery->start(provider)) {
        if (attached) battery->detach(provider);
        OSSafeReleaseNULL(battery);
        provider->release();
        b[0] = -4;
        return;
    }
    provider->release();
    bool lastExternal = false;
    unsigned sinceFull = 10;
    for (;;) {
        Sample s;
        if (readSample(s)) {
            ++b[9];
            b[1] = s.permille; b[2] = s.mv; b[3] = s.ma; b[4] = s.decic;
            b[5] = (s.external ? 1 : 0) | (s.charging ? 2 : 0) | (s.full ? 4 : 0) | 8;
            b[6] = s.fullMah; b[7] = s.designMah; b[8] = s.cycles; b[11] = s.details | s.intOk << 8;
            // A plug or unplug is published at once; otherwise every 10 s.
            if (sinceFull >= 10 || s.external != lastExternal) {battery->accept(s); sinceFull = 0;}
            lastExternal = s.external;
        } else {
            ++b[10];
        }
        if (sinceFull % 10 == 0) {   // charger watchdog kick, as max77705_chg_set_wdtmr_kick
            uint8_t cnfg06 = 0;
            if (t970_se0_read(kCharger, 0xbd, &cnfg06, 1) && t970_se0_write(kCharger, 0xbd, uint8_t((cnfg06 & ~3u) | 1u))) ++b[12];
        }
        IOSleep(1000);
        ++sinceFull;
    }
}
}

extern "C" void t970_battery_start(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("t970-battery", &enabled, sizeof enabled) || enabled != 1) return;
    static unsigned started;
    if (__atomic_exchange_n(&started, 1u, __ATOMIC_ACQ_REL)) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else t970_battery[0] = -7;
}
