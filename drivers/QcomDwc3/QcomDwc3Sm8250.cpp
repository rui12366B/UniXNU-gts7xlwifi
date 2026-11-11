// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8250 (SM-T970, gts7xlwifi) device-mode port of UniXNU/QcomDwc3 e4efe95 QcomDwc3.cpp for iPadOS 17.7.11 (21H461).
//
// The design is upstream's: QcomDwc3Xdci derives from the system's AppleUSBXDCI and supplies only what an Apple SoC subclass
// supplies (the register window, the interrupt, power and the PHYs). IOUSBDeviceFamily, AppleUSBDeviceMux, configd's
// USBDeviceConfiguration and lockdownd stay the unchanged system parts. What differs from SM8550:
//   - the 21H461 class layout (scripts/build-qcom-dwc3.py generates OriginalUsbXdciABI.h from scripts/usb-abi-21H461.json): the
//     hooks are slots 179, 255-258, 263-265, the register window is at this + 0x238 (its map at + 0x230) and the controller
//     parameters at this + 0x2410, all read from Apple's own AppleT8011USBXDCI in the same kernelcache;
//   - the SNPS femto HS PHY at 0x088e3000 (no eUSB2, no repeater), with this board's qcom,param-override-seq;
//   - the QMP USB3/DP PHY with the generated SM8250 table (QcomXnuUsbDebug soc/Sm8250QmpUsb3.h) and the lane from the MAX77705's
//     CC_STATUS0 (t970_muic[6]), no retimer;
//   - SMMU stream 0x0, rails through the board's t970_rpmh_write, GCC offsets from gcc-sm8250.c;
//   - device role only: the cable role, UCSI and host mode are later steps (docs/usb-plan.md).
// Hardware facts: mainline Linux sm8250.dtsi, gcc-sm8250.c, dwc3-qcom.c, phy-qcom-snps-femto-v2.c, phy-qcom-qmp-combo.c, and the
// board's live FDT.
#include "OriginalUsbXdciABI.h"
#include "Sm8250QmpUsb3.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern "C" {
IOReturn armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
// [0] != 0 once the MUIC route ran (T970WifiProbe.cpp), [6] CC_STATUS0 << 8 | CC_STATUS1.
extern volatile int32_t t970_muic[8] __attribute__((weak_import));
// The board's [dwc3] rows (package/BootLog.c):
// [0] stage (negative: the step that failed), [1] GSNPSID, [2] GHWPARAMS0, [3] GHWPARAMS3, [4] GHWPARAMS7, [5] GDSC USB30,
// [6] options (t970-usb), [7] SMMU slot | bank << 8 | 0xfc << 16, or 0xffffffff when the stream is left as found,
// [8] femto PHY COMMON0 after init, [9] femto UTMI_CTRL5, [10] QSCRATCH HS_PHY_CTRL after the VBUS override, [11] interrupts,
// [12] powerOn calls, [13] powerOff calls, [14] controllerReset result, [15] DSTS at power on, [16] DCFG after reset,
// [17] GCTL after reset, [18] GUSB2PHYCFG after reset, [19] start result, [20] power state request result, [21] kext load result,
// with t970-usb bit 2, every 2 s: [22] GSBUSCFG0, [23] GSBUSCFG1, [24] GUCTL1, [25] SMMU global fault, [26] DSTS, [27] DALEPENA,
// [28] GEVNTCOUNT, [29] DEPCMD of EP0 OUT, [30] context bank FSR, [31] GEVNTSIZ, [32] QMP step (negative: failed), [33] PCS status,
// [34] CC_STATUS0 | reverse << 8, [35] metaclass registration (preModLoad 1, postModLoad result << 8), [36] SuperSpeed link states
// seen (bit n: GDBGLTSSM bits 25:22 read n), [37] GDBGLTSSM, [38] GUSB3PIPECTL, [39] MUIC wait in 100 ms steps | route state << 16,
// [40] context bank FAR low, [41] DCTL, [42] IOUSBDeviceInterface services, [43] AppleUSBDeviceMux services, [44] the cable
// property set, [45] rail votes that failed (bit 0 ldoa5, 1 ldoa12, 2 ldoa2, 3 ldoa9, 4 ldoa18), [46] identity written
// (t970-usb bit 5: chip-id | board-id << 16; 0xdead failed).
volatile uint32_t t970_dwc3_stats[48];
}

namespace {
constexpr uint64_t kCore = 0x0a600000, kAppleWindow = kCore + 0xc000, kQscratch = kCore + 0xf8800, kGcc = 0x100000, kPhy = 0x088e3000,
    kQmp = 0x088e8000, kSmmu = 0x15000000, kGicd = 0x17a00000;
constexpr uint32_t kIrq = 133 + 32, kSid = 0x0, kVmid = 0xfc;
// t970-usb bits: 0 run, 1 try SuperSpeed (QMP), 2 register sampling, 3 leave the SMMU stream as found (no private context),
// 4 invert the SuperSpeed lane select.
uint32_t gOptions;
struct Region {IOMemoryMap *map; volatile uint32_t *va;};
Region gGcc, gScratch, gPhy;
bool gSuperSpeed;
void stage(int value) {t970_dwc3_stats[0] = (uint32_t)value;}
bool mapRegion(Region &r, uint64_t pa, uint64_t bytes) {
    IODeviceMemory *d = IODeviceMemory::withRange(pa, bytes);
    r.map = d ? d->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(d);
    r.va = r.map ? reinterpret_cast<volatile uint32_t *>(r.map->getVirtualAddress()) : nullptr;
    return r.va != nullptr;
}
void release(Region &r) {if (r.map) r.map->release(); r = {};}
void barrier() {__asm__ volatile("dsb sy" ::: "memory");}
bool waitBits(volatile uint32_t *reg, uint32_t mask, uint32_t value, unsigned ms) {
    for (unsigned i = 0; i < ms * 10; ++i) {
        if ((*reg & mask) == value) return true;
        IODelay(100);
    }
    return (*reg & mask) == value;
}
void rmw(volatile uint32_t *reg, uint32_t clear, uint32_t set) {*reg = (*reg & ~clear) | set; barrier();}
// gcc-sm8250.c usb30_prim_gdsc: SW_COLLAPSE off, then PWR_ON.
bool gdsc(uint32_t offset) {
    volatile uint32_t *reg = gGcc.va + offset / 4;
    rmw(reg, 1u, 0);
    const bool ok = waitBits(reg, 0x80000000u, 0x80000000u, 100);
    t970_dwc3_stats[5] = *reg;
    return ok;
}
bool branch(uint32_t offset) {
    volatile uint32_t *reg = gGcc.va + offset / 4;
    rmw(reg, 0, 1u);
    return waitBits(reg, 0x80000000u, 0, 100);
}
bool rcg(uint32_t cmd, uint32_t cfg) {
    volatile uint32_t *c = gGcc.va + cmd / 4;
    if ((c[1] & 0x71fu) == cfg) return true;
    c[1] = (c[1] & ~0x71fu) | cfg;
    barrier();
    c[0] = c[0] | 1u;
    barrier();
    return waitBits(c, 1u, 0, 10);
}
bool clocksAndResets() {
    // gcc-sm8250.c: USB30_PRIM_BCR 0xf000 (dwc3_qcom_probe's core reset).
    volatile uint32_t *core = gGcc.va + 0xf000 / 4, *phy = gGcc.va + 0x12000 / 4;
    rmw(core, 0, 1u); IODelay(20);
    rmw(core, 1u, 0);
    // Master 200 MHz from GPLL0 main (source 1, half divider 5), mock UTMI 19.2 MHz from XO through the /1 post-divider, as
    // sm8250.dtsi assigns them.
    if (!rcg(0xf020, 1u << 8 | 5u) || !rcg(0xf038, 0)) return false;
    gGcc.va[0xf050 / 4] = 0; barrier();
    // cfg_noc / aggre AXI, master, sleep, mock UTMI.
    if (!branch(0xf07c) || !branch(0xf080) || !branch(0xf010) || !branch(0xf018) || !branch(0xf01c)) return false;
    rmw(phy, 0, 1u); IODelay(150);
    rmw(phy, 1u, 0);
    return true;
}
// usb_1_hsphy, "qcom,usb-hsphy-snps-femto": phy-qcom-snps-femto-v2.c qcom_snps_hsphy_init with this board's param-override-seq
// (e7@6c 0b@70 2f@74 03@78, device mode). The same sequence QcomXnuUsbDebug's t970_hsphy_init runs.
void phyInit() {
    volatile uint32_t *p = gPhy.va;
    rmw(p + 0x94 / 4, 0, 1u << 1);                 // CFG0: UTMI_PHY_CMN_CTRL_OVERRIDE_EN
    rmw(p + 0x50 / 4, 0, 1u << 1);                 // UTMI_CTRL5: POR
    rmw(p + 0x54 / 4, 7u << 4, 3u << 4);           // HS_PHY_CTRL_COMMON0: FSEL 19.2 MHz
    rmw(p + 0x58 / 4, 0, 1u << 5 | 1u << 4);       // COMMON1: PLLBTUNE, VBUSVLDEXTSEL0
    rmw(p + 0xa0 / 4, 3u, 2u);                     // REFCLK_CTRL
    rmw(p + 0x5c / 4, 0, 1u);                      // COMMON2: VREGBYPASS
    rmw(p + 0x60 / 4, 0, 1u);                      // HS_PHY_CTRL1: VBUSVLDEXT0
    rmw(p + 0x64 / 4, 0, 1u << 3 | 1u << 2);       // HS_PHY_CTRL2: USB2_SUSPEND_N_SEL, USB2_SUSPEND_N
    p[0x6c / 4] = 0xe7; p[0x70 / 4] = 0x0b; p[0x74 / 4] = 0x2f; p[0x78 / 4] = 0x03; barrier();
    rmw(p + 0x3c / 4, 0, 1u);                      // UTMI_CTRL0: SLEEPM
    rmw(p + 0x54 / 4, 1u << 2, 0);                 // COMMON0: SIDDQ off
    IODelay(20);
    rmw(p + 0x50 / 4, 1u << 1, 0);                 // POR off
    IODelay(20);
    rmw(p + 0x64 / 4, 1u << 3, 0);                 // SUSPEND_N_SEL off
    rmw(p + 0x94 / 4, 1u << 1, 0);                 // override off
    IODelay(200);
    t970_dwc3_stats[8] = p[0x54 / 4];
    t970_dwc3_stats[9] = p[0x50 / 4];
}
// dwc3_qcom_select_utmi_clk: no PIPE clock until the QMP PHY runs.
void selectPipeClock(bool utmi) {
    volatile uint32_t *cfg = gScratch.va + 0x8 / 4;
    rmw(cfg, 0, 1u << 8); IOSleep(1);
    if (utmi) rmw(cfg, 0, 1u | 1u << 3); else rmw(cfg, 1u | 1u << 3, 0);
    IOSleep(1);
    rmw(cfg, 1u << 8, 0);
}
void vbusOverride(bool on) {
    volatile uint32_t *ss = gScratch.va + 0x30 / 4, *hs = gScratch.va + 0x10 / 4;
    const uint32_t ssBits = 1u << 24, hsBits = 1u << 20 | 1u << 28;
    if (on) {*ss = *ss | ssBits; *hs = *hs | hsBits;} else {*ss = *ss & ~ssBits; *hs = *hs & ~hsBits;}
    barrier();
    t970_dwc3_stats[10] = *hs;
}
// QMP USB3/DP combo PHY at 0x88e8000 in USB 3 only mode: qmp_combo_com_init and qmp_combo_usb_power_on. The lane select is
// software (COM TYPEC_CTRL) from the MAX77705's CCPinStat (1 CC1, 2 CC2).
bool superSpeedInit() {
    if (!&t970_muic || t970_muic[0] <= 0) {t970_dwc3_stats[32] = (uint32_t)-1; return false;}
    const uint32_t cc0 = ((uint32_t)t970_muic[6] >> 8) & 0xffu, pin = cc0 >> 6;
    if (pin != 1 && pin != 2) {t970_dwc3_stats[34] = cc0; t970_dwc3_stats[32] = (uint32_t)-2; return false;}
    const bool reverse = (pin == 2) != ((gOptions >> 4) & 1u);
    t970_dwc3_stats[34] = cc0 | (uint32_t)reverse << 8;
    Region phy = {};
    if (!mapRegion(phy, kQmp, 0x3000)) {t970_dwc3_stats[32] = (uint32_t)-3; return false;}
    volatile uint8_t *base = reinterpret_cast<volatile uint8_t *>(phy.va);
    auto rd = [&](uint32_t o) {return *reinterpret_cast<volatile uint32_t *>(base + o);};
    auto wr = [&](uint32_t o, uint32_t v) {*reinterpret_cast<volatile uint32_t *>(base + o) = v; barrier();};
    volatile uint32_t *reset = gGcc.va + 0x50000 / 4, *common = gGcc.va + 0x50008 / 4;
    rmw(reset, 0, 1u); rmw(common, 0, 1u); IODelay(20);
    rmw(reset, 1u, 0); rmw(common, 1u, 0);
    // usb3_prim_phy_aux RCG from XO, then aux, com_aux and pipe (BRANCH_HALT_DELAY: no halt check, the PHY drives it).
    if (!rcg(0xf064, 0) || !branch(0xf054) || !branch(0xf058)) {release(phy); t970_dwc3_stats[32] = (uint32_t)-4; return false;}
    rmw(gGcc.va + 0xf05c / 4, 0, 1u);
    t970_dwc3_stats[32] = 1;
    wr(kQmpComPower, rd(kQmpComPower) | 1u);
    wr(kQmpComResetOvrd, rd(kQmpComResetOvrd) | 0xfu);
    wr(kQmpComTypec, 1u << 1 | (reverse ? 1u : 0));
    wr(kQmpComMode, 1u);                           // USB3_MODE
    wr(kQmpComResetOvrd, rd(kQmpComResetOvrd) & ~0xcu);
    wr(kQmpComSwi, rd(kQmpComSwi) & ~3u);
    wr(kQmpComReset, rd(kQmpComReset) & ~1u);
    wr(kQmpPowerDown, rd(kQmpPowerDown) | 1u);
    for (const auto &w : kQmpUsb3Init) wr(w.offset, w.value);
    IODelay(20);
    wr(kQmpReset, rd(kQmpReset) & ~1u);
    wr(kQmpStart, rd(kQmpStart) | 3u);
    bool ok = false;
    for (unsigned i = 0; i < 1000 && !ok; ++i) {ok = !(rd(kQmpStatus) & 1u << 6); if (!ok) IODelay(200);}
    t970_dwc3_stats[33] = rd(kQmpStatus);
    t970_dwc3_stats[32] = ok ? 2 : (uint32_t)-5;
    release(phy);
    return ok;
}
void maskLine(bool masked) {
    // GICD ICENABLER / ISENABLER for this one line (this board's ArmCpu exports no mask call; a weak import of a symbol nothing
    // defines does not link in this kernel).
    static Region gicd;
    if (!gicd.va && !mapRegion(gicd, kGicd, 0x1000)) return;
    gicd.va[((masked ? 0x180u : 0x100u) + (kIrq / 32) * 4) / 4] = 1u << (kIrq % 32);
    barrier();
}
Region gSmmu;
// Apple's controller driver does no cache maintenance. Coherence comes from the SMMU's output attributes, as in Linux (dma-coherent,
// IOMMU_CACHE page tables): a stage 1 context with an identity map of DRAM in 1 GiB blocks, Normal write-back inner shareable.
// SM8250 differences from upstream: stream 0x0, banks from NUMS2CB up only (the lower ones are stage 2 only), a dead SMR for the
// stream (its bank has translation off, as QcomXnuUsbDebug's lease leaves it or UEFI does) is reused rather than refused, and the
// bank gets its own VMID 0xfc so no other VMID 0 stage 1 bank's TLB entries can match (T970WifiStack.cpp does the same).
IOBufferMemoryDescriptor *gSmmuTable;
unsigned gSmmuBank, gSmmuPages;
constexpr uint32_t kCbSctlr = 0x0, kCbTcr2 = 0x10, kCbTtbr0 = 0x20, kCbTtbr1 = 0x28, kCbTcr = 0x30, kCbMair0 = 0x38, kCbMair1 = 0x3c, kCbFsr = 0x58,
    kCbFar = 0x60, kCbTlbiAll = 0x618, kCbTlbSync = 0x7f0, kCbTlbStatus = 0x7f4;
bool smmuIdentity() {
    if (!mapRegion(gSmmu, kSmmu, 0x100000)) return false;
    const uint32_t idr0 = gSmmu.va[0x20 / 4], idr1 = gSmmu.va[0x24 / 4];
    unsigned groups = idr0 & 255;
    if (groups > 128) groups = 128;
    const unsigned banks = idr1 & 255, ns2 = (idr1 >> 16) & 255, pages = 1u << (((idr1 >> 28) & 7) + 1);
    if ((idr1 >> 31) || !groups || banks < 2 || banks > 128 || ns2 >= banks || (pages + banks) * 4096u > 0x100000u) return false;
    uint64_t used[2] = {};
    unsigned slot = 0xffffffffu;
    for (unsigned i = 0; i < groups; ++i) {
        const uint32_t smr = gSmmu.va[(0x800 + 4 * i) / 4];
        if (!(smr & 0x80000000u)) {if (slot == 0xffffffffu) slot = i; continue;}
        const uint32_t s2cr = gSmmu.va[(0xc00 + 4 * i) / 4];
        if (((smr ^ kSid) & 0x7fff & ~((smr >> 16) & 0x7fff)) == 0) {
            // Covers this stream. A live context (translation on) belongs to someone else; a dead one is taken over.
            const unsigned cb = s2cr & 255;
            if (((s2cr >> 16) & 3) == 0 && cb < banks && (gSmmu.va[(pages + cb) * 1024] & 1u)) {
                IOLog("t970-dwc3: stream 0x%x already translated by bank %u (smr %x s2cr %x)\n", kSid, cb, smr, s2cr);
                return false;
            }
            if ((smr & 0x7fff0000u) || slot != 0xffffffffu) {
                // A masked group, or a second match: disable it, so exactly one entry of ours matches.
                gSmmu.va[(0x800 + 4 * i) / 4] = smr & 0x7fffffffu; barrier();
                if (slot == 0xffffffffu) slot = i;
            } else slot = i;
            continue;
        }
        if (((s2cr >> 16) & 3) == 0) used[(s2cr & 127) / 64] |= 1ull << ((s2cr & 127) % 64);
    }
    if (slot == 0xffffffffu) return false;
    // Highest free bank from NUMS2CB up, below the last one; bottom-up allocators (Wi-Fi, audio) start at NUMS2CB.
    unsigned bank = 0xffffffffu;
    for (unsigned b = banks - 1; b-- > ns2;) {
        if ((used[b / 64] >> (b % 64)) & 1) continue;
        if (gSmmu.va[(pages + b) * 1024] & 1u) continue;
        bank = b;
        break;
    }
    if (bank == 0xffffffffu) return false;
    gSmmuTable = IOBufferMemoryDescriptor::withOptions((IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryPhysicallyContiguous, 4096, 4096);
    if (!gSmmuTable || gSmmuTable->prepare() != kIOReturnSuccess) return false;
    uint64_t *table = static_cast<uint64_t *>(gSmmuTable->getBytesNoCopy());
    bzero(table, 4096);
    // T0SZ 28: 64 GiB, the walk starts at level 1. Blocks: AF, inner shareable, AP read/write, attribute 0. DRAM starts at 2 GiB;
    // below it a stray access faults.
    for (uint64_t i = 2; i < 64; ++i) table[i] = i << 30 | 1ull << 10 | 3ull << 8 | 1ull << 6 | 1ull;
    for (uintptr_t at = reinterpret_cast<uintptr_t>(table); at < reinterpret_cast<uintptr_t>(table) + 4096; at += 64) __asm__ volatile("dc civac, %0" :: "r"(at) : "memory");
    barrier();
    const uint64_t tablePa = gSmmuTable->getPhysicalSegment(0, nullptr, 0);
    volatile uint32_t *cb = gSmmu.va + (pages + bank) * 1024;
    gSmmu.va[(0x1800 + 4 * bank) / 4] = 1;                                      // CBA2R: AArch64
    gSmmu.va[(0x1000 + 4 * bank) / 4] = 1u << 16 | 0xfu << 12 | 3u << 8 | kVmid; // CBAR: stage 1, stage 2 bypass, VMID
    cb[kCbTcr2 / 4] = 2u | 7u << 15;                                            // 40-bit output
    cb[kCbTcr / 4] = 28u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23;            // walks cacheable inner shareable, 4 KiB, no TTBR1
    cb[kCbTtbr0 / 4] = (uint32_t)tablePa; cb[kCbTtbr0 / 4 + 1] = (uint32_t)(tablePa >> 32);
    cb[kCbTtbr1 / 4] = 0; cb[kCbTtbr1 / 4 + 1] = 0;
    cb[kCbMair0 / 4] = 0xff; cb[kCbMair1 / 4] = 0;                              // attribute 0: Normal write-back
    barrier();
    cb[kCbTlbiAll / 4] = 0;
    cb[kCbTlbSync / 4] = 0;
    barrier();
    for (unsigned i = 0; i < 1000 && (cb[kCbTlbStatus / 4] & 1u); ++i) IODelay(10);
    cb[kCbFsr / 4] = cb[kCbFsr / 4];
    cb[kCbSctlr / 4] = 1u | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12;             // M, TRE, AFE, CFRE, ASIDPNE
    barrier();
    gSmmu.va[(0xc00 + 4 * slot) / 4] = bank;                                    // S2CR: translate to the bank, then the match
    barrier();
    gSmmu.va[(0x800 + 4 * slot) / 4] = 0x80000000u | kSid;
    barrier();
    t970_dwc3_stats[7] = slot | bank << 8 | kVmid << 16;
    gSmmuBank = bank; gSmmuPages = pages;
    return (cb[kCbSctlr / 4] & 1u) && (gSmmu.va[(0xc00 + 4 * slot) / 4] & 255) == bank &&
           gSmmu.va[(0x800 + 4 * slot) / 4] == (0x80000000u | kSid);
}
}  // namespace

// The controller's provider: the register window and the core interrupt, in the IOService terms IOFilterInterruptEventSource uses.
class QcomDwc3Nub : public IOService {
    OSDeclareDefaultStructors(QcomDwc3Nub)
    IOInterruptAction action = nullptr;
    OSObject *target = nullptr;
    void *refcon = nullptr;
    bool enabled = false;
    static QcomDwc3Nub *volatile active;
    static bool attached;
    static void primary(void *) {
        QcomDwc3Nub *self = active;
        t970_dwc3_stats[11] = t970_dwc3_stats[11] + 1;
        if (self && self->action && self->enabled) self->action(self->target, self->refcon, self, 0);
    }
public:
    void activate() {active = this; __asm__ volatile("dmb ish" ::: "memory");}
    IOReturn getInterruptType(int source, int *type) override {
        if (source != 0 || !type) return kIOReturnNoInterrupt;
        *type = kIOInterruptTypeLevel;
        return kIOReturnSuccess;
    }
    IOReturn registerInterrupt(int source, OSObject *owner, IOInterruptAction handler, void *context) override {
        if (source != 0 || !handler) return kIOReturnNoInterrupt;
        if (action) return kIOReturnNoResources;
        target = owner; refcon = context; action = handler; enabled = false;
        __asm__ volatile("dmb ish" ::: "memory");
        if (!attached) {
            if (!&armcpu_spi_attach) {action = nullptr; return kIOReturnUnsupported;}
            if (armcpu_spi_attach(kIrq, false, primary, nullptr, nullptr, nullptr) != kIOReturnSuccess) {action = nullptr; return kIOReturnNoResources;}
            attached = true;
        }
        maskLine(true);
        return kIOReturnSuccess;
    }
    IOReturn unregisterInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = false;
        maskLine(true);
        action = nullptr; target = nullptr; refcon = nullptr;
        return kIOReturnSuccess;
    }
    IOReturn enableInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = true;
        maskLine(false);
        return kIOReturnSuccess;
    }
    IOReturn disableInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = false;
        maskLine(true);
        return kIOReturnSuccess;
    }
};
QcomDwc3Nub *volatile QcomDwc3Nub::active;
bool QcomDwc3Nub::attached;
OSDefineMetaClassAndStructors(QcomDwc3Nub, IOService)

class QcomDwc3Xdci : public AppleUSBXDCI {
    OSDeclareDefaultStructors(QcomDwc3Xdci)
    uint8_t *self() {return reinterpret_cast<uint8_t *>(this);}
    volatile uint32_t *regs() {return *reinterpret_cast<volatile uint32_t **>(self() + kXdciRegisters);}
    uint32_t read(uint32_t offset) {return regs()[offset / 4];}
    void write(uint32_t offset, uint32_t value) {regs()[offset / 4] = value; barrier();}
    uint32_t *parameters() {return reinterpret_cast<uint32_t *>(self() + kXdciParameters);}
public:
    // What AppleT8011USBXDCI::start does before its super call: map the provider's first range, keep the map and its address.
    bool start(IOService *provider) override {
        IOMemoryMap *map = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
        if (!map) return false;
        *reinterpret_cast<IOMemoryMap **>(self() + kXdciRegisterMap) = map;
        *reinterpret_cast<uint64_t *>(self() + kXdciRegisters) = map->getVirtualAddress();
        return AppleUSBXDCI::start(provider);
    }
    // False keeps the family's own configuration list: Apple's SoC class answers from its IOPort transport object, and this
    // class has none (upstream: bNumConfigurations 0 and host resets otherwise).
    bool XDCI_KEEP_CONFIGURATIONS() override {return false;}
    bool XDCI_TRANSPORT_ACTIVE() override {return true;}
    IOReturn XDCI_POWER_ON() override {
        t970_dwc3_stats[12] = t970_dwc3_stats[12] + 1;
        vbusOverride(true);
        t970_dwc3_stats[15] = read(0x70c);
        return kIOReturnSuccess;
    }
    IOReturn XDCI_POWER_OFF() override {
        t970_dwc3_stats[13] = t970_dwc3_stats[13] + 1;
        vbusOverride(false);
        return kIOReturnSuccess;
    }
    // [0] interrupters (one line, one event buffer), [1] GCTL PWRDNSCALE, [2] DCFG NUMP, [3] the count AppleT8011USBXDCI sets to 3
    // (21H461 caps it by another field). [1] and [2] are replaced from the hardware in the reset below, before the kext applies them.
    void XDCI_PARAMETERS() override {
        uint32_t *f = parameters();
        f[0] = 1; f[1] = 0x2ee; f[2] = 4; f[3] = 3;
    }
    IOReturn XDCI_CONTROLLER_RESET() override {
        const IOReturn result = AppleUSBXDCI::XDCI_CONTROLLER_RESET();
        t970_dwc3_stats[14] = (uint32_t)result;
        if (result != kIOReturnSuccess) return result;
        uint32_t *f = parameters();
        // dwc3_core_setup_global_control keeps the power down scale the hardware resets to.
        const uint32_t gctl = read(0x110);
        f[1] = gctl >> 19;
        write(0x110, (gctl & ~(3u << 12)) | 2u << 12);
        // dwc3_gadget_setup_nump: RAM2 bytes less 24 for the event overhead and 16 per setup, in 1 KiB packets, at most 16.
        const uint32_t hw0 = read(0x140), hw6 = read(0x158), hw7 = read(0x15c);
        const uint32_t mdwidth = ((hw0 >> 8) & 0xffu) | ((hw6 >> 8) & 3u) << 8;
        const uint32_t ram2 = hw7 >> 16;
        const uint32_t nump = ram2 * mdwidth / 8 > 40 ? (ram2 * mdwidth / 8 - 40) / 1024 : 1;
        f[2] = nump > 16 ? 16 : nump ? nump : 1;
        t970_dwc3_stats[2] = hw0; t970_dwc3_stats[3] = read(0x14c); t970_dwc3_stats[4] = hw7;
        // sm8250.dtsi usb_1_dwc3: snps,dis_u2_susphy_quirk, snps,dis_enblslpm_quirk.
        write(0x200, read(0x200) & ~(1u << 6 | 1u << 8));
        write(0x2c0, read(0x2c0) & ~(1u << 17));
        if (!gSuperSpeed) write(0x700, read(0x700) & ~7u);   // high speed
        t970_dwc3_stats[16] = read(0x700); t970_dwc3_stats[17] = read(0x110); t970_dwc3_stats[18] = read(0x200);
        return result;
    }
    // AppleUSBXDCI calls these when the first periodic endpoint opens and the last closes (Apple's fabric bandwidth): nothing here.
    void XDCI_PERIODIC_ON() override {}
    void XDCI_PERIODIC_OFF() override {}
    // The family's power states: 4 starts the bus (the cable-connected path of Apple's SoC classes), 0 takes it off.
    IOReturn cable(bool connected) {return changePowerStateTo(connected ? 4 : 0);}
};
OSDefineMetaClassAndStructors(QcomDwc3Xdci, AppleUSBXDCI)

namespace {
QcomDwc3Xdci *gController;
bool bringUp() {
    stage(1);
    if (!mapRegion(gGcc, kGcc, 0x1f0000) || !mapRegion(gPhy, kPhy, 0x1000) || !mapRegion(gScratch, kQscratch & ~0xfffull, 0x1000)) {
        stage(-1); return false;
    }
    gScratch.va += (kQscratch & 0xfff) / 4;
    stage(3);
    if (!gdsc(0xf004)) {stage(-3); return false;}
    stage(4);
    if (!clocksAndResets()) {stage(-4); return false;}
    stage(5);
    if (gOptions & 8) t970_dwc3_stats[7] = 0xffffffffu;
    else if (!smmuIdentity()) {stage(-5); return false;}
    stage(7);
    phyInit();
    IOSleep(2);
    gSuperSpeed = (gOptions & 2) && superSpeedInit();
    selectPipeClock(!gSuperSpeed);
    stage(8);
    Region window = {};
    if (!mapRegion(window, kAppleWindow, 0x1000)) {stage(-8); return false;}
    const uint32_t id = window.va[0x120 / 4];
    t970_dwc3_stats[1] = id;
    release(window);
    if ((id & 0xffff0000u) != 0x55330000u && (id & 0xffff0000u) != 0x33310000u && (id & 0xffff0000u) != 0x33320000u) {stage(-9); return false;}
    stage(9);
    return true;
}
}  // namespace

extern "C" bool t970_dwc3_bring_up(uint32_t options) {
    gOptions = options;
    t970_dwc3_stats[6] = options;
    return bringUp();
}
extern "C" void t970_dwc3_stat(unsigned index, uint32_t value) {if (index < 48) t970_dwc3_stats[index] = value;}

// After the module's metaclasses are registered: publish the provider and start the controller in the device role.
extern "C" void t970_dwc3_publish(void) {
    do {
        OSData *zero = OSData::withBytes("\0\0\0\0", 4);
        QcomDwc3Nub *nub = OSTypeAlloc(QcomDwc3Nub);
        IOService *platform = IOService::getPlatform();
        IODeviceMemory *range = IODeviceMemory::withRange(kAppleWindow, 0x1000);
        OSArray *ranges = range ? OSArray::withObjects((const OSObject **)&range, 1) : nullptr;
        if (!nub || !nub->init() || !ranges || !zero || !platform) {stage(-12); break;}
        nub->setDeviceMemory(ranges);
        nub->setName("usb-drd0");
        nub->setProperty("bus-number", zero);
        nub->setProperty("configuration-string", (void *)"standardMuxOnly", 16);
        // snps,dis-u1-entry-quirk, snps,dis-u2-entry-quirk.
        nub->setProperty("u1-init-disable", true);
        nub->setProperty("u2-init-disable", true);
        if (!nub->attach(platform)) {stage(-13); break;}
        nub->registerService();
        nub->activate();
        OSSafeReleaseNULL(ranges); OSSafeReleaseNULL(range); OSSafeReleaseNULL(zero);
        stage(10);
        QcomDwc3Xdci *controller = OSTypeAlloc(QcomDwc3Xdci);
        if (!controller || !controller->init()) {stage(-14); break;}
        if (!controller->attach(nub)) {stage(-15); break;}
        const bool started = controller->start(nub);
        t970_dwc3_stats[19] = started;
        if (!started) {stage(-16); break;}
        stage(11);
        gController = controller;
        // A USB host on the cable: lockdownd's launch event, then the family goes on the bus once configd's description arrives.
        IOService::getResourceService()->setProperty("AppleUSBCableType", "USBHost");
        t970_dwc3_stats[44] = 1;
        t970_dwc3_stats[20] = (uint32_t)controller->cable(true);
        stage(12);
    } while (false);
}

static uint32_t countServices(const char *name) {
    OSDictionary *match = IOService::serviceMatching(name);
    OSIterator *it = match ? IOService::getMatchingServices(match) : nullptr;
    uint32_t n = 0;
    while (it && it->getNextObject()) ++n;
    OSSafeReleaseNULL(it); OSSafeReleaseNULL(match);
    return n;
}

// t970-usb bit 2: what the core and the SMMU see while the system stack runs, sampled about every 5 ms for the link state.
extern "C" void t970_dwc3_diag(void) {
    Region window = {};
    if (!mapRegion(window, kAppleWindow, 0x1000)) return;
    const uint32_t pages = gSmmuPages, bank = gSmmuBank;
    for (uint32_t tick = 0;; ++tick) {
        const uint32_t ltssm = window.va[0x164 / 4];
        t970_dwc3_stats[36] = t970_dwc3_stats[36] | 1u << ((ltssm >> 22) & 15u);
        t970_dwc3_stats[37] = ltssm;
        if (!(tick % 400)) {
            t970_dwc3_stats[22] = window.va[0x100 / 4]; t970_dwc3_stats[23] = window.va[0x104 / 4]; t970_dwc3_stats[24] = window.va[0x11c / 4];
            t970_dwc3_stats[26] = window.va[0x70c / 4]; t970_dwc3_stats[27] = window.va[0x720 / 4]; t970_dwc3_stats[28] = window.va[0x40c / 4];
            t970_dwc3_stats[29] = window.va[0x80c / 4]; t970_dwc3_stats[31] = window.va[0x408 / 4];
            t970_dwc3_stats[38] = window.va[0x2c0 / 4]; t970_dwc3_stats[41] = window.va[0x704 / 4];
            if (gSmmu.va) {
                t970_dwc3_stats[25] = gSmmu.va[0x48 / 4];
                if (t970_dwc3_stats[7] != 0xffffffffu) {
                    t970_dwc3_stats[30] = gSmmu.va[(pages + bank) * 1024 + kCbFsr / 4];
                    t970_dwc3_stats[40] = gSmmu.va[(pages + bank) * 1024 + kCbFar / 4];
                }
            }
            t970_dwc3_stats[42] = countServices("IOUSBDeviceInterface");
            t970_dwc3_stats[43] = countServices("AppleUSBDeviceMux");
        }
        IOSleep(5);
    }
}
