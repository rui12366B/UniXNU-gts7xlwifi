// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi board entry of the QcomDwc3 SM8250 port (drivers/QcomDwc3/QcomDwc3Sm8250.cpp): Apple's own USB device stack on the DWC3,
// opt-in with t970-usb=<bits> (bit 0 run, 1 SuperSpeed, 2 register sampling, 3 no private SMMU context, 4 invert the lane,
// 5 J120's chip-id/board-id/security-domain for userland). The order
// is upstream UniXNU-sheng UsbProbe.cpp's: the hardware, then the AppleUSBXDCI kext load, then this module's metaclasses (deferred,
// as HidInputProbe.cpp and T970Battery.cpp register theirs), then the provider nub and the controller.
//
// The CDC-ACM debug console (drivers/QcomXnuUsbDebug) drives the same controller, so it stands down when t970-usb bit 0 is set
// (qcom_usb_debug_init returns DISABLED). The D+/D- switch in the MAX77705 MUIC is closed by T970WifiProbe.cpp's routeUsb, which
// runs from its t970-qud-retry thread at about 25 s: keep t970-qud-retry=1 on the line, this thread waits for that route.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/OSKextLib.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
kern_return_t OSKextLoadKextWithIdentifier(const char *);
bool t970_dwc3_bring_up(uint32_t options);
void t970_dwc3_publish(void);
void t970_dwc3_diag(void);
void t970_dwc3_stat(unsigned index, uint32_t value);
void t970_dwc3_static_init(void) __asm__("__GLOBAL__sub_I_QcomDwc3Sm8250.cpp");
int t970_rpmh_write(const char *name, uint32_t word, uint32_t value) __attribute__((weak_import));
extern volatile int32_t t970_muic[8] __attribute__((weak_import));
extern volatile uint32_t t970_dwc3_stats[48];
}

namespace {
uint32_t gOptions;
// usb2_phy0 vdd 0.88 V, vdda18 1.8 V, vdda33 3.072 V; usb_qmp_dp_phy core 1.2 V and vdd 0.912 V (the board's FDT supplies).
// Each: voltage, high power mode, on. Returns a bit per rail whose vote failed (bit 0 ldoa5 ... bit 4 ldoa18).
uint32_t rails(bool superSpeed) {
    static const struct {const char *name; uint32_t mv;} list[5] = {
        {"ldoa5", 880}, {"ldoa12", 1800}, {"ldoa2", 3072}, {"ldoa9", 1200}, {"ldoa18", 912}};
    if (!&t970_rpmh_write) return 0x1fu;
    uint32_t failed = 0;
    for (unsigned i = 0; i < (superSpeed ? 5u : 3u); ++i) {
        int rc = t970_rpmh_write(list[i].name, 0, list[i].mv);
        if (!rc) rc = t970_rpmh_write(list[i].name, 8, 7);
        if (!rc) rc = t970_rpmh_write(list[i].name, 4, 1);
        if (rc) failed |= 1u << i;
    }
    return failed;
}
void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "t970-usb");
    // The MUIC route (about 25 s after T970WifiProbe starts its retry thread); 60 s at most, then go on without it.
    unsigned waited = 0;
    while (waited < 600 && !(&t970_muic && t970_muic[0])) {IOSleep(100); ++waited;}
    t970_dwc3_stat(39, waited | (uint32_t)((&t970_muic ? t970_muic[0] : 0) & 0xffff) << 16);
    const uint32_t failed = rails(gOptions & 2);
    t970_dwc3_stat(45, failed);
    if (failed & 7u) {t970_dwc3_stats[0] = (uint32_t)-2; thread_terminate(current_thread()); return;}
    if (failed) gOptions &= ~2u;   // no QMP supplies: high speed only
    IOSleep(2);
    if (!t970_dwc3_bring_up(gOptions)) {thread_terminate(current_thread()); return;}
    // AppleUSBXDCI has no personality of its own, so nothing has loaded it: its constructors and metaclasses, and those of
    // IOUSBDeviceFamily below it, run on this load.
    const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.driver.AppleUSBXDCI");
    t970_dwc3_stat(21, (uint32_t)loaded);
    if (loaded != kOSReturnSuccess && loaded != kOSKextReturnStartStopError) {t970_dwc3_stats[0] = (uint32_t)-9; thread_terminate(current_thread()); return;}
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {t970_dwc3_stats[0] = (uint32_t)-10; thread_terminate(current_thread()); return;}
    t970_dwc3_static_init();
    const OSReturn registered = OSMetaClass::postModLoad(handle);
    t970_dwc3_stat(35, 1u | (uint32_t)registered << 8);
    if (registered) {t970_dwc3_stats[0] = (uint32_t)-11; thread_terminate(current_thread()); return;}
    t970_dwc3_publish();
    if (gOptions & 4) t970_dwc3_diag();
    thread_terminate(current_thread());
}

// t970-usb bit 5: the identity a Mac looks up in MobileDevice's device map. lockdownd answers ChipID from MobileGestalt, which
// reads IODeviceTree:/chosen; main7 answered 356 (the payload's Qualcomm 0x164) for J120AP, so CoreDevice listed the tablet with
// no model. UniXNU sets J120's own values in the payload ADT (profile 24A446: board-id 12, security-domain 1, chip-id the board's);
// here they change only after the kernel's platform drivers have read /chosen (AppleARMPlatform keeps 0x164), from the late hook
// that runs before launchd. [46] of t970_dwc3_stats: chip-id written | board-id << 16, 0xdead on failure.
void presentAppleIdentity() {
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (!chosen) {t970_dwc3_stat(46, 0xdead); return;}
    const uint32_t chip = 0x8011, board = 12, domain = 1;
    OSData *c = OSData::withBytes(&chip, 4), *b = OSData::withBytes(&board, 4), *d = OSData::withBytes(&domain, 4);
    if (c && b && d && chosen->setProperty("chip-id", c) && chosen->setProperty("board-id", b) && chosen->setProperty("security-domain", d))
        t970_dwc3_stat(46, chip | board << 16);
    else t970_dwc3_stat(46, 0xdead);
    OSSafeReleaseNULL(c); OSSafeReleaseNULL(b); OSSafeReleaseNULL(d);
    chosen->release();
}
}  // namespace

// Called from t970_battery_start (package/T970Battery.cpp), which the display probe's late hook runs (bsd_init, before launchd).
extern "C" void t970_usb_start(void) {
    unsigned options = 0;
    if (!PE_parse_boot_argn("t970-usb", &options, sizeof options) || !(options & 1)) return;
    gOptions = options;
    if (options & 32) presentAppleIdentity();
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
