// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi Wi-Fi bring-up, stages 1-4 (boot argument t970-wifi=<stage>), adapted from UniXNU-sheng package/WifiProbe.cpp (516a780,
// WCN7850 on SM8550 PCIe0) for the T970: QCA6490 (17cb:1103, ath11k WCN6855 family) on SM8250 PCIe0 (qcom,pcie@1c00000).
//
// Stage 1 reads what the boot loaders left (GDSC, branch halt bits, pins). Stage 2 powers the chip and trains the link, then reads the endpoint's ID.
// Stage 4 also gives BAR0 an address and reads the MHI/BHI registers the BAR starts with. Nothing here starts MHI or loads firmware.
//
// Facts from the live FDT (evidence/recovery-live.dts):
//   cnss-qca6490: wlan-en GPIO 90 (south tile), bt-en 21, sw-ctrl 124; rails vdd-wlan-aon/dig smpa6 0.95 V, io smpa4 1.8 V, rfa1 smpa5 2.04 V,
//   rfa2 smpc8 1.35 V, antenna switch ldoc5 1.8 V. PCIe0: parf 0x1c00000, phy 0x1c06000 (v4 gen3x1, qcom,phy-sequence below), dbi 0x60000000,
//   iatu 0x60001000, conf 0x60100000; perst GPIO 79 (gpio, pull-down), clkreq GPIO 80 (pci_e0), wake 81; PHY rails ldoa5 0.88 V / ldoa9 1.2 V
//   (shared with UFS, already on); iommu-map rid 0 -> 0x1c00, rid 0x100 -> 0x1c01.
// gcc-kona.c: GDSC 0x6b004; votes 0x52008 bits 0 slv_axi, 1 mstr_axi, 2 cfg_ahb, 3 aux, 4 pipe, 5 slv_q2a; halt registers 0x6b014 0x6b01c 0x6b024
//   0x6b028 0x6b02c 0x6b010; aux RCG 0x6b038 (TCXO); refgen RCG 0x6f014 (GPLL0 main / 6 = 100 MHz) and branch 0x6f02c; aggre_noc_pcie_tbu 0x9000c,
//   ddrss_pcie_sf_tbu 0x8d058, pcie_wifi_clkref_en 0x8c004; BCRs 0x6b000 (PCIe0) and 0x6c01c (PHY).
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/cpu_number.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <string.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include "MhiBoot.h"
#include "MhiBoot.inc"

extern "C" int t970_rpmh_write(const char *name, uint32_t word, uint32_t value) __attribute__((weak_import));

extern "C" {
// [0] stage (1 mapped, 2 survey read; 10.. bring-up steps, see bringUp; negative: the step that failed), [1] vote 0x52008, [2] GDSC,
// [3] halt bits (bit i: kBranches[i] off) before | after << 8, [4] pins (bit 0 in, bit 1 out: 79 | 80 << 8 | 90 << 16), [5] rails voted (bit i ok)
// | failing rail result << 16, [6] PCS_STATUS1 | polls << 16, [7] root port id, [8] port debug 0, [9] port debug 1, [10] PARF LTSSM,
// [11] link control/status, [12] link ms, [13] endpoint vendor/device, [14] endpoint class/revision, [15] BAR0 mask, [16] BAR0, [17] command,
// [18..23] MHI MHIREGLEN MHIVER MHICFG BHIOFF MHICTRL MHISTATUS, [24] BHI EE, [25] BHI status, [26] BHI error, [27] SID hash | entry << 31
volatile uint32_t t970_wifi_stats[28];
// The VRM resources the command DB names (0x80860000, the layout T970Rpmh.inc reads), space-separated, and each rail's vote result.
char t970_wifi_cmddb[640];
volatile int32_t t970_wifi_rail_rc[8];
// Stage 5a, read only: the apps SMMU (0x15000000) as UEFI left it, for the PCIe streams 0x1c00 (root port) and 0x1c01 (endpoint).
// [0] sCR0 (bit 0 CLIENTPD: all bypass; bit 10 USFCFG: unmatched streams fault), [1] IDR0 (NUMSMRG bits 7:0), [2] SMRs valid,
// [3] index of the SMR matching 0x1c01 (0xffff none), [4] that SMR, [5] its S2CR (type bits 17:16: 0 translate, 1 bypass, 2 fault;
// CBNDX bits 7:0), [6..8] the same for 0x1c00, [9] stage reached
volatile uint32_t t970_wifi_smmu[10];
// The late USB retry (t970-qud-retry=1): [0] step (1 sleeping, 2 rails, 3 bound, 4 retried, 5 polling; negative: failed), [1] rails rc
// ldoa5 | ldoa12 << 8 | ldoa2 << 16 (low byte of each), [2] retry result, [3] polls, [4] cpu the retry ran on
volatile int32_t t970_qud_retry[8];
// Stage 5: [0] step (1 SMMU bypass set, 2 firmware read, 3 DMA buffer, 4 MHI opened, 5 SBL started, 6 SBL ran / READY; negative: failed),
// [1] SMR index used | 0x10000 when an existing entry already matched, [2] amss20.bin size, [3] seconds waited for the file, [4] DMA PA low,
// [5] MhiBoot phase | error << 8 | failed phase << 16, [6] EE, [7] MHI state, [8] BHI status, [9] BHI error code, [10] ms in the MHI loop,
// [11] MHI status register, [12] firmware open/read error
volatile uint32_t t970_wifi_mhi[13];
// periph16 stage 5 DMA domain (wdom): [0] result (1 installed, negative: failed step), [1] SMMU CR0, [2] IDR1, [3] SMR already covering
// 0x1c01 | 0x10000, [4] bank | SMR index << 8 | ASID << 16, [5..6] page-table PA, [7] bank FSR after install, [8] FSR after BHI,
// [9..10] FAR, [11] FSYNR0. periph17: while install() runs [0] is 0x1000 + step and [12] the bank, [13] the register offset being
// touched, so the boot log's compose() (which reads these live) shows where a stuck access stopped.
volatile uint32_t t970_wifi_dom[14];
// MAX77705 MUIC USB-2 switch (before the late USB retry; t970-muic=0 skips it): [0] step (1 bus answered, 2 switched to USB, 3 already USB,
// negative: failed step), [1] UIC_HW_REV << 8 | UIC_FW_REV, [2] USBC_STATUS1 << 16 | USBC_STATUS2 << 8 | BC_STATUS, [3] CONTROL1 read
// before (DATAIN0 << 8 | DATAIN1), [4] CONTROL1 read after, [5] NACKs, [6] CC_STATUS0 << 8 | CC_STATUS1, [7] UIC_INT before the reads
volatile int32_t t970_muic[8];
// qcom_usb_debug_t970_faults() snapshot, refreshed with the [qud retry] DSTS sample (layout in QcomXnuUsbDebug.c)
volatile uint32_t t970_qud_faults[16];
}
extern "C" int sheng_fw_open(const char *path, void **handle, uint64_t *size) __attribute__((weak_import));
extern "C" int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length) __attribute__((weak_import));
extern "C" void sheng_fw_close(void *handle) __attribute__((weak_import));
extern "C" void flush_dcache64(addr64_t addr, unsigned count, int phys);
extern "C" uint64_t kvtophys(vm_offset_t va);
extern "C" int qcom_usb_debug_t970_retry(void) __attribute__((weak_import));
extern "C" void qcom_usb_debug_poll(void) __attribute__((weak_import));
extern "C" void qcom_usb_debug_peek_status(void *status) __attribute__((weak_import));
extern "C" void qcom_usb_debug_t970_faults(uint32_t out[16]) __attribute__((weak_import));
extern "C" processor_t cpu_to_processor(int cpu);
extern "C" processor_t thread_bind(processor_t processor);


namespace {
constexpr uint64_t kGcc = 0x100000, kTlmm = 0x0f000000, kParf = 0x1c00000, kDbi = 0x60000000, kPhy = 0x1c06000;
constexpr uint64_t kAtu = 0x60001000, kConfig = 0x60100000, kMemBase = 0x60300000, kMemSize = 0x3d00000;
constexpr uint32_t kNorth = 0x900000, kSouth = 0x500000;
constexpr uint32_t kVote = 0x52008, kVoteBits = 0x3f, kGdsc = 0x6b004, kPcieBcr = 0x6b000, kPhyBcr = 0x6c01c;
constexpr uint32_t kAuxRcg = 0x6b038, kRefgenRcg = 0x6f014;
constexpr uint32_t kDirect[] = {0x6f02c, 0x9000c, 0x8d058, 0x8c004};
// slv_axi, mstr_axi, cfg_ahb, aux, slv_q2a, pipe, refgen, aggre_noc_tbu, ddrss_sf_tbu
constexpr uint32_t kBranches[] = {0x6b014, 0x6b01c, 0x6b024, 0x6b028, 0x6b010, 0x6b02c, 0x6f02c, 0x9000c, 0x8d058};
constexpr uint32_t kNeeded = 0xf;   // slv_axi, mstr_axi, cfg_ahb, aux (the pipe branch, bit 5, runs once the PHY does)
constexpr unsigned kPerst = 79, kClkReq = 80, kWlanEn = 90;
constexpr uint32_t kParfSysCtrl = 0x0, kParfPhyCtrl = 0x40, kParfPmCtrl = 0x20, kParfMhiClockReset = 0x174, kParfAxiHalt = 0x1a8;
constexpr uint32_t kParfLtssm = 0x1b0, kParfDbiBase = 0x350, kParfDbiBaseHi = 0x354, kParfSlvSpace = 0x358, kParfSlvSpaceHi = 0x35c;
constexpr uint32_t kParfAtuBase = 0x634, kParfAtuBaseHi = 0x638, kParfDeviceType = 0x1000;
constexpr uint32_t kPortDebug0 = 0x728, kPortDebug1 = 0x72c, kLinkControl = 0x80, kBusNumbers = 0x18, kCommand = 0x4;
constexpr uint32_t kPcsStatus1 = 0x814;

// qcom,phy-sequence of qcom,pcie@1c00000: offset, value (every delay is 0). Starts with POWER_DOWN_CONTROL = 3, ends with SW_RESET = 0, START = 3.
constexpr uint16_t kPhySeq[][2] = {
    {0x840, 0x03}, {0x094, 0x08}, {0x154, 0x34}, {0x16c, 0x08}, {0x058, 0x0f}, {0x0a4, 0x42}, {0x110, 0x24}, {0x11c, 0x03}, {0x118, 0xb4},
    {0x10c, 0x02}, {0x1bc, 0x11}, {0x0bc, 0x82}, {0x0d4, 0x03}, {0x0d0, 0x55}, {0x0cc, 0x55}, {0x0b0, 0x1a}, {0x0ac, 0x0a}, {0x0c4, 0x68},
    {0x0e0, 0x02}, {0x0dc, 0xaa}, {0x0d8, 0xab}, {0x0b8, 0x34}, {0x0b4, 0x14}, {0x158, 0x01}, {0x074, 0x06}, {0x07c, 0x16}, {0x084, 0x36},
    {0x078, 0x06}, {0x080, 0x16}, {0x088, 0x36}, {0x1b0, 0x1e}, {0x1ac, 0xca}, {0x1b8, 0x18}, {0x1b4, 0xa2}, {0x050, 0x07}, {0x010, 0x01},
    {0x01c, 0x31}, {0x020, 0x01}, {0x024, 0xde}, {0x028, 0x07}, {0x030, 0x4c}, {0x034, 0x06}, {0x29c, 0x12}, {0x284, 0x35}, {0x23c, 0x11},
    {0x51c, 0x03}, {0x518, 0x1c}, {0x524, 0x1e}, {0x4e8, 0x00}, {0x4ec, 0x0e}, {0x4f0, 0x4a}, {0x4f4, 0x0f}, {0x5b4, 0x04}, {0x434, 0x7f},
    {0x444, 0x70}, {0x510, 0x17}, {0x4d4, 0x04}, {0x4d8, 0x07}, {0x598, 0xd4}, {0x59c, 0x54}, {0x5a0, 0xdb}, {0x5a4, 0x3b}, {0x5a8, 0x31},
    {0x584, 0x24}, {0x588, 0xe4}, {0x58c, 0xec}, {0x590, 0x3b}, {0x594, 0x36}, {0x570, 0x3f}, {0x574, 0x3f}, {0x578, 0xff}, {0x57c, 0x7f},
    {0x580, 0x14}, {0x4fc, 0x00}, {0x4f8, 0xc0}, {0x460, 0x30}, {0x464, 0x00}, {0x5bc, 0x0c}, {0x4dc, 0x1b}, {0x408, 0x0c}, {0x414, 0x03},
    {0x5b8, 0x30}, {0x9a4, 0x01}, {0xc90, 0x00}, {0xc40, 0x01}, {0xc48, 0x01}, {0xc50, 0x00}, {0xcb4, 0x33}, {0xcbc, 0x00}, {0xce0, 0x58},
    {0xca4, 0x0f}, {0x048, 0x90}, {0xc1c, 0xc1}, {0x988, 0x77}, {0x998, 0x0b}, {0x8dc, 0x0d}, {0x9ec, 0x12}, {0x800, 0x00}, {0x844, 0x03},
};

struct Rail {const char *name; uint32_t millivolts;};
// io first (it also feeds the chip's I/O ring), then the PMU inputs, then the antenna switch
constexpr Rail kRails[] = {{"smpa4", 1800}, {"smpa6", 950}, {"smpa5", 2040}, {"smpc8", 1350}, {"ldoc5", 1800}};

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void release(Window &w) {if (w.map) w.map->release(); w = {};}

// One window per pin (config at +0, io at +4). TLMM config: pull bits 0-1 (1 down, 3 up), function 2-5, drive (mA / 2 - 1) 6-8, OE bit 9.
struct Pin {Window w;};
Pin gPerst, gClkReq, gWlanEn;
bool mapPin(Pin &p, uint32_t tile, unsigned pin) {return mapWindow(p.w, kTlmm + tile + pin * 0x1000ull, 0x1000);}
void setPin(Pin &p, uint32_t config, int level) {
    if (level >= 0) p.w.va[1] = level ? 2u : 0u;
    p.w.va[0] = config;
    if (level >= 0) p.w.va[1] = level ? 2u : 0u;
}
uint32_t pinWord() {return (gPerst.w.va[1] & 3u) | (gClkReq.w.va[1] & 3u) << 8 | (gWlanEn.w.va[1] & 3u) << 16;}

bool waitClear(volatile uint32_t *reg, uint32_t mask, unsigned tries, unsigned *used = nullptr) {
    unsigned i = 0;
    for (; i < tries && (*reg & mask); ++i) IODelay(100);
    if (used) *used = i;
    return !(*reg & mask);
}
void pulseReset(volatile uint32_t *gcc, uint32_t bcr) {
    gcc[bcr / 4] |= 1u;
    IODelay(1000);
    gcc[bcr / 4] &= ~1u;
    IODelay(1000);
}
uint32_t haltBits(volatile uint32_t *g) {
    uint32_t off = 0;
    for (unsigned i = 0; i < sizeof kBranches / sizeof kBranches[0]; ++i)
        if (g[kBranches[i] / 4] & (1u << 31)) off |= 1u << i;
    return off;
}

// List the command DB's VRM (slave 4) resource names into t970_wifi_cmddb. Header: version 1, magic 0x0c0330db, then 8 groups of
// 16 bytes (slave u16, offset u16 at +2, count u16 at +6); entries of 24 bytes at 0x90 + offset (8-byte name, address at +16).
void listCmdDb() {
    Window db = {};
    if (!mapWindow(db, 0x80860000, 0x20000)) {strlcpy(t970_wifi_cmddb, "map failed", sizeof t970_wifi_cmddb); return;}
    const volatile uint8_t *b = reinterpret_cast<const volatile uint8_t *>(db.va);
    auto u16 = [&](uint32_t o) {return uint32_t(b[o]) | uint32_t(b[o + 1]) << 8;};
    size_t at = 0;
    at += snprintf(t970_wifi_cmddb + at, sizeof t970_wifi_cmddb - at, "v%x m%x:", db.va[0], db.va[1]);
    for (unsigned g = 0; g < 8 && at < sizeof t970_wifi_cmddb - 16; ++g) {
        const uint32_t h = 8 + g * 16, slave = u16(h), offset = u16(h + 2), count = u16(h + 6);
        if (!slave) break;
        at += snprintf(t970_wifi_cmddb + at, sizeof t970_wifi_cmddb - at, " [s%u n%u]", slave, count);
        if (slave != 4 || offset > 0x20000 - 0x90 || count > (0x20000 - 0x90 - offset) / 24) continue;
        for (uint32_t e = 0; e < count && at < sizeof t970_wifi_cmddb - 12; ++e) {
            const uint32_t o = 0x90 + offset + e * 24;
            char name[9] = {};
            for (unsigned k = 0; k < 8; ++k) name[k] = char(b[o + k]);
            at += snprintf(t970_wifi_cmddb + at, sizeof t970_wifi_cmddb - at, " %s", name);
        }
    }
    release(db);
}

int powerRails() {
    if (!&t970_rpmh_write) return -100;
    uint32_t ok = 0;
    int first = 0;
    // every rail is tried and its own result kept, so one missing name does not hide the others
    for (unsigned i = 0; i < sizeof kRails / sizeof kRails[0]; ++i) {
        int rc = t970_rpmh_write(kRails[i].name, 0, kRails[i].millivolts);
        if (!rc) rc = t970_rpmh_write(kRails[i].name, 8, 7);                 // HPM / PWM
        if (!rc) rc = t970_rpmh_write(kRails[i].name, 4, 1);
        t970_wifi_rail_rc[i] = rc;
        if (rc) {if (!first) first = -1 - int(i); continue;}
        ok |= 1u << i;
        t970_wifi_stats[5] = ok;
        IOSleep(2);
    }
    t970_wifi_stats[5] = ok;
    return first;
}

// Stage 4: memory window (outbound iATU region 1, root port base/limit), BAR0 at the window's first aligned address, MHI/BHI registers.
void endpointProbe(volatile uint32_t *a, volatile uint32_t *c, volatile uint32_t *d) {
    volatile uint32_t *st = t970_wifi_stats;
    volatile uint32_t *r1 = a + 0x200 / 4;
    r1[0x08 / 4] = uint32_t(kMemBase);
    r1[0x0c / 4] = uint32_t(kMemBase >> 32);
    r1[0x10 / 4] = uint32_t(kMemBase + kMemSize - 1);
    r1[0x20 / 4] = uint32_t((kMemBase + kMemSize - 1) >> 32);
    r1[0x14 / 4] = uint32_t(kMemBase);
    r1[0x18 / 4] = uint32_t(kMemBase >> 32);
    r1[0x00 / 4] = 0x0;
    r1[0x04 / 4] = 1u << 31;
    for (unsigned i = 0; i < 5 && !(r1[0x04 / 4] & (1u << 31)); ++i) IODelay(10);
    d[0x20 / 4] = uint32_t(((kMemBase + kMemSize - 1) & 0xfff00000u) | ((kMemBase >> 16) & 0xfff0u));
    st[0] = 18;
    const uint32_t saved = c[0x10 / 4];
    c[0x10 / 4] = 0xffffffffu;
    const uint32_t mask = c[0x10 / 4];
    c[0x10 / 4] = saved;
    st[15] = mask;
    const uint32_t size = ~(mask & ~0xfu) + 1;
    if (!(mask & ~0xfu) || !size || size > kMemSize) {st[0] = -18; return;}
    const uint32_t address = uint32_t((kMemBase + size - 1) & ~uint64_t(size - 1));
    c[0x10 / 4] = address | (mask & 0xfu);
    if ((mask & 6u) == 4u) c[0x14 / 4] = 0;
    c[0x04 / 4] = (c[0x04 / 4] & 0xffffu) | 0x6u;
    st[16] = c[0x10 / 4];
    st[17] = c[0x04 / 4];
    st[0] = 19;
    Window bar = {};
    if (!mapWindow(bar, address, size < 0x20000 ? size : 0x20000)) {st[0] = -19; return;}
    volatile uint32_t *m = bar.va;
    static const uint32_t regs[6] = {0x00, 0x08, 0x10, 0x28, 0x38, 0x48};
    for (unsigned i = 0; i < 6; ++i) st[18 + i] = m[regs[i] / 4];
    const uint32_t bhi = m[0x28 / 4];
    if (bhi && bhi < 0x20000 - 0x40) {
        st[24] = m[(bhi + 0x28) / 4];
        st[25] = m[(bhi + 0x2c) / 4];
        st[26] = m[(bhi + 0x30) / 4];
    }
    st[0] = 20;
    release(bar);
}

void bringUp(volatile uint32_t *g, unsigned level) {
    volatile uint32_t *st = t970_wifi_stats;
    // 10: rails (the command DB's names are listed first, for the log)
    listCmdDb();
    const int rails = powerRails();
    if (rails) {st[0] = -10; return;}
    st[0] = 10;
    IOSleep(5);
    // 11: PERST# low, CLKREQ# to pci_e0, WLAN_EN low then high (cnss: >= 100 ms off, then the chip's power-up delay)
    setPin(gPerst, 1u | 1u << 9, 0);
    setPin(gClkReq, 3u | 1u << 2, -1);
    setPin(gWlanEn, 3u | 7u << 6 | 1u << 9, 0);
    IOSleep(150);
    setPin(gWlanEn, 3u | 7u << 6 | 1u << 9, 1);
    IOSleep(60);
    st[4] = pinWord();
    st[0] = 11;
    // 12: GDSC on (SW_COLLAPSE bit 0 clear, PWR_ON bit 31)
    g[kGdsc / 4] &= ~1u;
    unsigned tries = 0;
    while (tries++ < 100 && !(g[kGdsc / 4] & (1u << 31))) IODelay(10);
    st[2] = g[kGdsc / 4];
    if (!(st[2] & (1u << 31))) {st[0] = -12; return;}
    st[0] = 12;
    // 13: clock sources (an RCG takes its CFG on CMD bit 0), branch votes and direct branches
    g[(kAuxRcg + 4) / 4] = 0;                 // TCXO, divider bypass: 19.2 MHz
    g[kAuxRcg / 4] |= 1u;
    g[(kRefgenRcg + 4) / 4] = 1u << 8 | 11u;  // GPLL0 main, divide by 6: 100 MHz
    g[kRefgenRcg / 4] |= 1u;
    waitClear(&g[kAuxRcg / 4], 1u, 100);
    waitClear(&g[kRefgenRcg / 4], 1u, 100);
    g[kVote / 4] |= kVoteBits;
    for (uint32_t r : kDirect) g[r / 4] |= 1u;
    IODelay(200);
    uint32_t off = haltBits(g);
    st[3] = (st[3] & 0xffu) | off << 8;
    st[1] = g[kVote / 4];
    if (off & kNeeded) {st[0] = -13; return;}   // the pipe branch runs only once the PHY drives it
    pulseReset(g, kPcieBcr);
    pulseReset(g, kPhyBcr);
    st[0] = 13;
    // 14: PARF (qcom_pcie_init_2_7_0, as used by SM8250's ops_1_9_0), PHY tables, PHY ready
    Window parf = {}, dbi = {}, phy = {};
    if (!mapWindow(parf, kParf, 0x3000) || !mapWindow(dbi, kDbi, 0x1000) || !mapWindow(phy, kPhy, 0x1000)) {
        release(parf); release(dbi); release(phy); st[0] = -14; return;
    }
    volatile uint32_t *f = parf.va, *d = dbi.va, *p = phy.va;
    f[kParfDeviceType / 4] = 4u;
    f[kParfPhyCtrl / 4] &= ~1u;
    f[kParfDbiBase / 4] = uint32_t(kDbi);
    f[kParfDbiBaseHi / 4] = uint32_t(kDbi >> 32);
    f[kParfAtuBase / 4] = uint32_t(kAtu);
    f[kParfAtuBaseHi / 4] = uint32_t(kAtu >> 32);
    f[kParfSlvSpace / 4] = 0;
    f[kParfSlvSpaceHi / 4] = 0x80000000u;
    f[kParfSysCtrl / 4] &= ~(1u << 29);
    f[kParfMhiClockReset / 4] |= 1u << 4;
    f[kParfPmCtrl / 4] &= ~(1u << 5);
    f[kParfAxiHalt / 4] |= 1u << 31;
    for (const auto &e : kPhySeq) {p[e[0] / 4] = e[1]; __asm__ volatile("dsb sy" ::: "memory");}
    IODelay(1000);
    unsigned polls = 0;
    const bool ready = waitClear(&p[kPcsStatus1 / 4], 1u << 6, 100, &polls);
    st[6] = (p[kPcsStatus1 / 4] & 0xffffu) | polls << 16;
    st[3] = (st[3] & 0xffu) | haltBits(g) << 8;
    if (!ready) {release(phy); release(dbi); release(parf); st[0] = -14; return;}
    st[0] = 14;
    // 15: SID table (requester 0x100 -> stream 0x1c00 + 1), root port bus numbers and command
    {
        for (unsigned i = 0; i < 256; ++i) f[(0x2000 + 4 * i) / 4] = 0;
        f[0x2c00 / 4] &= ~1u;
        uint8_t crc = 0;
        const uint8_t bytes[2] = {0x01, 0x00};
        for (uint8_t byte : bytes) {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 7) : (uint8_t)(crc << 1);
        }
        f[(0x2000 + 4 * crc) / 4] = 0x100u << 16 | 1u << 8;
        __asm__ volatile("dsb sy" ::: "memory");
        st[27] = crc | (f[(0x2000 + 4 * crc) / 4] != 0) << 31;
    }
    d[kBusNumbers / 4] = 0x00010100;
    d[kCommand / 4] |= 0x6u;
    st[7] = d[0];
    st[0] = 15;
    // 16: PERST# held low >= 100 ms, released, LTSSM enable, wait for L0 (port debug 0 state 0x11, debug 1 bit 4 link up)
    IOSleep(100);
    setPin(gPerst, 1u | 1u << 9, 1);
    IODelay(1000);
    f[kParfLtssm / 4] |= 1u << 8;
    const uint64_t start = mach_absolute_time();
    unsigned ms = 0;
    for (; ms < 1000; ++ms) {
        if ((d[kPortDebug1 / 4] & (1u << 4)) && (d[kPortDebug0 / 4] & 0x3f) == 0x11) break;
        IOSleep(1);
    }
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    st[12] = uint32_t(ns / 1000000);
    st[8] = d[kPortDebug0 / 4];
    st[9] = d[kPortDebug1 / 4];
    st[10] = f[kParfLtssm / 4];
    st[11] = d[kLinkControl / 4];
    st[4] = pinWord();
    if (ms == 1000) {release(phy); release(dbi); release(parf); st[0] = -16; return;}
    st[0] = 16;
    // 17: outbound iATU region 0 = type 0 config accesses to bus 1, read the endpoint's ID
    Window atu = {}, config = {};
    if (mapWindow(atu, kAtu, 0x1000) && mapWindow(config, kConfig, 0x1000)) {
        volatile uint32_t *a = atu.va;
        a[0x08 / 4] = uint32_t(kConfig);
        a[0x0c / 4] = uint32_t(kConfig >> 32);
        a[0x10 / 4] = uint32_t(kConfig + 0xfffff);
        a[0x20 / 4] = uint32_t((kConfig + 0xfffff) >> 32);
        a[0x14 / 4] = 1u << 24;
        a[0x18 / 4] = 0;
        a[0x00 / 4] = 0x4;
        a[0x04 / 4] = 1u << 31;
        for (unsigned i = 0; i < 5 && !(a[0x04 / 4] & (1u << 31)); ++i) IODelay(10);
        st[13] = config.va[0];
        st[14] = config.va[2];
        st[0] = 17;
        if (level >= 4 && (config.va[0] & 0xffff) == 0x17cb) endpointProbe(a, config.va, d);
    } else st[0] = -17;
    release(config); release(atu); release(phy); release(dbi); release(parf);
}

// Stage 5a (t970-wifi=5): read where the PCIe streams land in the apps SMMU. SMMUv2 GR0: sCR0 0x0, IDR0 0x20, SMR(n) 0x800 + 4n
// (bit 31 valid, mask 30:16, id 14:0), S2CR(n) 0xc00 + 4n. Nothing is written.
void smmuSurvey() {
    volatile uint32_t *sm = t970_wifi_smmu;
    Window w = {};
    if (!mapWindow(w, 0x15000000, 0x1000)) {sm[9] = uint32_t(-1); return;}
    volatile uint32_t *r = w.va;
    sm[0] = r[0];
    sm[1] = r[0x20 / 4];
    const unsigned n = sm[1] & 0xff;
    sm[3] = sm[6] = 0xffff;
    unsigned valid = 0;
    for (unsigned i = 0; i < n && i < 128; ++i) {
        const uint32_t smr = r[(0x800 + 4 * i) / 4];
        if (!(smr & (1u << 31))) continue;
        ++valid;
        const uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
        for (unsigned k = 0; k < 2; ++k) {
            const uint32_t sid = k ? 0x1c00 : 0x1c01;
            volatile uint32_t *slot = sm + (k ? 6 : 3);
            if (slot[0] == 0xffff && ((sid ^ id) & ~mask & 0x7fff) == 0) {slot[0] = i; slot[1] = smr; slot[2] = r[(0xc00 + 4 * i) / 4];}
        }
    }
    sm[2] = valid;
    sm[9] = 1;
    release(w);
}

// Stage 5 (t970-wifi=5): the QCA6390's secondary boot loader over BHI. The apps SMMU faults unmatched streams (periph4: sCR0 USFCFG
// set, no SMR for 0x1c00/0x1c01), so a free SMR (bit 31 clear) gets stream 0x1c00 mask 1 with a bypass S2CR first: the endpoint's DMA
// then reaches physical memory as is. The boot loader is the first 512 KiB of amss20.bin (ath11k: sbl_size 0x80000), copied into a
// physically contiguous buffer below 4 GiB and cleaned to memory. MhiBoot (UniXNU-sheng) starts it and is stepped until the device
// reports READY (MhiNeedContexts), which proves the stream, the DMA and the image; the MHI contexts and the full image are the next stage.
constexpr uint32_t kSblBytes = 0x80000;
// ---- Wi-Fi DMA domain (periph16) -------------------------------------------------------------------------------------
// periph9/10: an SMMU bypass for 0x1c00/0x1c01 and a buffer above 4 GiB reset the tablet ~20 s in at the first DMA. Now the
// endpoint stream 0x1c01 gets a private AArch64 stage-1 context of the apps SMMU (0x15000000) with 32-bit IOVAs from 0xa0000000
// (Chengdong-Yu/pong-wifi's choice; Android cnss uses 0xa0000000 + 256 MiB), as QcomXnuUsbDebug's QcomUsbDmaDomain does for the USB
// stream, but over a 4 MiB arena of ordinary (not contiguous) pages. A DMA outside the arena faults in this bank (terminated, no
// stall: SCTLR CFCFG 0) instead of reaching memory; t970_wifi_dom records it. Register values as QcomUsbDmaDomain.c: CBA2R VA64,
// CBAR S1 translation / S2 bypass 0x1f300, TCR 0x803520 (T0SZ 32, WBWA inner-shareable walks, EPD1), SCTLR 0x27; MAIR attr 0 is
// Normal non-cacheable (0x44) so the CPU side keeps explicit cache maintenance. The bank and ASID are taken from the top down,
// QcomUsbDmaDomain takes them from the bottom up.
namespace wdom {
constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0x100000, kIova = 0xa0000000;
constexpr uint32_t kStream = 0x1c01, kArenaBytes = 4u << 20, kL3Tables = kArenaBytes >> 21, kTablePages = 2 + kL3Tables;
constexpr uint64_t kLeaf = 0x0060000000000f43ull;   // AF, inner shareable, nG, AP EL0 RW, UXN/PXN, page; AttrIndx 0
Window gSmmu;
IOBufferMemoryDescriptor *gTables, *gArena;
uint8_t *gArenaVa;
uint32_t gCb = 0xffff, gIndex = 0xffff, gAsid, gShift, gPages;
uint64_t gCbBase;
void at(uint64_t off) {t970_wifi_dom[13] = uint32_t(off); __asm__ volatile("dsb sy" ::: "memory");}
void step(uint32_t n) {t970_wifi_dom[0] = 0x1000 + n; __asm__ volatile("dsb sy" ::: "memory");}
uint32_t r32(uint64_t off) {at(off); return gSmmu.va[off / 4];}
void w32(uint64_t off, uint32_t v) {at(off | 0x80000000u); gSmmu.va[off / 4] = v; __asm__ volatile("dsb sy" ::: "memory");}
// 64-bit registers as two 32-bit accesses, low word first (as the USB lease's platform does for its table registers)
void w64(uint64_t off, uint64_t v) {w32(off, uint32_t(v)); w32(off + 4, uint32_t(v >> 32));}
uint64_t r64(uint64_t off) {const uint32_t lo = r32(off); return lo | uint64_t(r32(off + 4)) << 32;}
uint64_t bank(uint32_t i) {return (uint64_t(gPages) + i) << gShift;}
// Returns 0 or the failing step (negative).
int install() {
    volatile uint32_t *d = t970_wifi_dom;
    step(1);
    if (!mapWindow(gSmmu, kSmmu, kSmmuBytes)) return -1;
    step(2);
    const uint32_t cr0 = r32(0), id0 = r32(0x20), id1 = r32(0x24), id2 = r32(0x28);
    d[1] = cr0; d[2] = id1;
    if (cr0 & (1u << 3)) return -2;                       // extended stream IDs: not this SMMU's mode
    const uint32_t streams = id0 & 0xff, banks = id1 & 0xff, ns2 = (id1 >> 16) & 0xff;
    gShift = (id1 >> 31) ? 16 : 12; gPages = 2u << ((id1 >> 28) & 7);
    if (!streams || streams > 128 || !banks || banks > 128 || bank(banks) > kSmmuBytes) return -3;
    // the SMR slot: none may already cover the stream; take the first invalid one
    step(3);
    bool used[128] = {};
    uint32_t freeSmr = 0xffff;
    for (uint32_t i = 0; i < streams; ++i) {
        const uint32_t smr = r32(0x800 + 4 * i), s2cr = r32(0xc00 + 4 * i);
        if (!(smr & (1u << 31))) {if (freeSmr == 0xffff) freeSmr = i; continue;}
        const uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
        if (((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) < banks) used[s2cr & 0xff] = true;
        if (((kStream ^ id) & ~mask & 0x7fff) == 0) {d[3] = i | 0x10000; return -4;}
    }
    if (freeSmr == 0xffff) return -5;
    // the bank: from the top, stage-1 capable (>= NUMS2CB), not routed to, translation off, no recorded fault
    uint16_t asids[256] = {};
    unsigned nAsid = 0;
    step(4);
    for (uint32_t i = 0; i < banks; ++i) {
        d[12] = i;
        const uint32_t cbar = r32((1ull << gShift) + 4 * i);
        if (((cbar >> 16) & 3) == 0) continue;            // stage-2 bank: VMID, no ASID
        if (nAsid + 2 <= 256) {asids[nAsid++] = uint16_t(r64(bank(i) + 0x20) >> 48); asids[nAsid++] = uint16_t(r64(bank(i) + 0x28) >> 48);}
    }
    // bottom up from NUMS2CB, as QcomUsbDmaDomain (proven on this SMMU); the USB lease at 25 s then skips this enabled bank
    step(5);
    for (uint32_t i = ns2; i < banks; ++i) {
        d[12] = i;
        if (used[i] || (r32(bank(i)) & 1) || (r32(bank(i) + 0x58) & 0xc00001feu)) continue;
        gCb = i; break;
    }
    if (gCb == 0xffff) return -6;
    for (uint32_t a = 0xfc00; a > 0xfa00 && !gAsid; --a) {
        bool taken = false;
        for (unsigned j = 0; j < nAsid; ++j) if (asids[j] == a) taken = true;
        if (!taken) gAsid = a;
    }
    if (!gAsid) return -7;
    gCbBase = bank(gCb);
    d[12] = gCb;
    d[4] = gCb | freeSmr << 8 | gAsid << 16;
    step(6);
    // memory: the tables contiguous (a few pages), the arena page by page
    gTables = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                               kTablePages * 4096, 0x000000fffffff000ull);
    gArena = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, kArenaBytes, 4096);
    if (!gTables || !gArena || gTables->prepare() != kIOReturnSuccess || gArena->prepare() != kIOReturnSuccess) return -8;
    uint64_t *t = static_cast<uint64_t *>(gTables->getBytesNoCopy());
    gArenaVa = static_cast<uint8_t *>(gArena->getBytesNoCopy());
    const uint64_t tpa = t ? kvtophys(vm_offset_t(t)) : 0;
    if (!t || !gArenaVa || !tpa) return -9;
    for (uint32_t i = 1; i < kTablePages; ++i) if (kvtophys(vm_offset_t(t) + 4096 * i) != tpa + 4096 * i) return -10;
    d[5] = uint32_t(tpa); d[6] = uint32_t(tpa >> 32);
    memset(t, 0, kTablePages * 4096);
    t[(kIova >> 30) & 3] = (tpa + 4096) | 3;                                   // L1 (T0SZ 32: four 1 GiB entries) -> L2
    for (uint32_t k = 0; k < kL3Tables; ++k) t[512 + (((kIova >> 21) + k) & 511)] = (tpa + 8192 + 4096ull * k) | 3;   // L2 -> L3
    for (uint32_t off = 0; off < kArenaBytes; off += 4096) {
        const uint64_t pa = kvtophys(vm_offset_t(gArenaVa + off));
        if (!pa || (pa & 4095)) return -11;
        t[1024 + off / 4096] = (pa & 0x0000fffffffff000ull) | kLeaf;
    }
    flush_dcache64(tpa, kTablePages * 4096, TRUE);
    step(7);
    // the context bank, disabled while written
    const uint32_t ps = id2 & 15;
    w32((1ull << gShift) + 0x800 + 4 * gCb, 1);                               // CBA2R: VA64
    w32((1ull << gShift) + 4 * gCb, 0x1f300);                                 // CBAR
    w32(gCbBase + 0x10, (r32(gCbBase + 0x10) & ~0x38017u) | 0x38010u | ps);    // TCR2: SEP, 16-bit ASID, PASize
    w32(gCbBase + 0x30, 0x803520);                                            // TCR
    w64(gCbBase + 0x20, tpa | uint64_t(gAsid) << 48);                         // TTBR0
    w64(gCbBase + 0x28, uint64_t(gAsid) << 48);                               // TTBR1
    w32(gCbBase + 0x38, 0x44); w32(gCbBase + 0x3c, 0);                         // MAIR0/1: attr 0 Normal-NC
    w32(gCbBase + 0x34, 0);                                                   // CONTEXTIDR
    step(8);
    w32(gCbBase + 0x610, gAsid);                                              // TLBIASID
    w32(gCbBase + 0x7f0, 0xffffffffu);                                        // TLBSYNC
    unsigned n = 0;
    for (; n < 1000 && (r32(gCbBase + 0x7f4) & 1); ++n) IODelay(1);
    if (n == 1000) return -12;
    step(9);
    w32(gCbBase, 0x27);                                                       // SCTLR: CFRE AFE TRE M
    step(10);
    w32(0xc00 + 4 * freeSmr, gCb);                                            // S2CR: translate via the bank
    w32(0x800 + 4 * freeSmr, (1u << 31) | kStream);                           // SMR: valid, exact 0x1c01
    gIndex = freeSmr;
    d[7] = r32(gCbBase + 0x58);
    return 0;
}
// Fault picture of the bank: FSR, FAR, FSYNR0 into t970_wifi_dom[8..11].
void sample() {
    if (!gSmmu.va || gCb == 0xffff) return;
    volatile uint32_t *d = t970_wifi_dom;
    d[8] = r32(gCbBase + 0x58);
    const uint64_t far = r64(gCbBase + 0x60);
    d[9] = uint32_t(far); d[10] = uint32_t(far >> 32); d[11] = r32(gCbBase + 0x68);
}
}  // namespace wdom

void wifiStage5() {
    volatile uint32_t *mh = t970_wifi_mhi;
    // 1: the stream's own SMMU context and its 4 MiB arena at IOVA 0xa0000000
    {
        const int rc = wdom::install();
        t970_wifi_dom[0] = uint32_t(rc ? rc : 1);
        if (rc) {mh[0] = uint32_t(-1); return;}
        mh[1] = wdom::gIndex | wdom::gCb << 8;
    }
    mh[0] = 1;
    // 2: the boot loader from Data (the GPU addon writes /private/var/ngpu/firmware/qca6390 before its mount; wait up to 90 s)
    if (!&sheng_fw_open || !&sheng_fw_read || !&sheng_fw_close) {mh[0] = uint32_t(-2); return;}
    void *fw = nullptr;
    uint64_t fwSize = 0;
    int err = 0;
    for (unsigned s = 0; s < 90; ++s) {
        err = sheng_fw_open("/private/var/ngpu/firmware/qca6390/amss20.bin", &fw, &fwSize);
        if (!err) break;
        mh[3] = s;
        IOSleep(1000);
    }
    if (err || fwSize < kSblBytes) {mh[12] = uint32_t(err); mh[0] = uint32_t(-2); if (!err) sheng_fw_close(fw); return;}
    mh[2] = uint32_t(fwSize);
    // 3: the boot loader at the start of the arena; the device sees it at IOVA 0xa0000000. The CPU copy is cleaned to memory
    // (the device side is non-cacheable).
    uint8_t *va = wdom::gArenaVa;
    const uint64_t iova = wdom::kIova;
    mh[4] = uint32_t(iova);
    err = sheng_fw_read(fw, 0, va, kSblBytes);
    sheng_fw_close(fw);
    if (err) {mh[12] = uint32_t(err) | 0x10000; mh[0] = uint32_t(-2); return;}
    for (uint32_t off = 0; off < kSblBytes; off += 4096) flush_dcache64(kvtophys(vm_offset_t(va + off)), 4096, TRUE);
    const uint64_t pa = iova;   // what BHI is given below

    // 4: the MHI register space at BAR0 (stage 4 assigned it)
    const uint64_t bar = t970_wifi_stats[16] & ~0xfull;
    Window bw = {};
    if (!bar || !mapWindow(bw, bar, 0x100000)) {mh[0] = uint32_t(-4); return;}
    struct Io {static uint32_t rd(void *c, uint32_t reg) {return static_cast<volatile uint32_t *>(c)[reg / 4];}
               static void wr(void *c, uint32_t reg, uint32_t v) {static_cast<volatile uint32_t *>(c)[reg / 4] = v; __asm__ volatile("dsb sy" ::: "memory");}};
    MhiIo io = {Io::rd, Io::wr, const_cast<uint32_t *>(bw.va), 0x100000};
    static MhiBoot boot;
    if (mhi_boot_open(&boot, &io)) {mh[5] = boot.phase | boot.error << 8; release(bw); mh[0] = uint32_t(-4); return;}
    mh[0] = 4;
    // 5: BHI with the boot loader, then step until READY or 3 s
    mhi_boot_begin(&boot, 0, pa, kSblBytes, 0, 0, uint32_t(mach_absolute_time()));
    mh[0] = 5;
    unsigned ms = 0;
    for (; ms < 3000; ++ms) {
        const uint32_t phase = mhi_boot_step(&boot);
        if (phase == MhiNeedContexts || phase == MhiFailed || phase == MhiDone) break;
        IOSleep(1);
    }
    if (ms == 3000) mhi_boot_timeout(&boot);
    mh[5] = boot.phase | boot.error << 8 | boot.failed_phase << 16;
    mh[6] = mhi_boot_ee(&boot);
    mh[7] = mhi_boot_state(&boot);
    mh[8] = bw.va[(boot.bhi + MHI_BHI_STATUS) / 4];
    mh[9] = bw.va[(boot.bhi + MHI_BHI_ERRCODE) / 4];
    mh[10] = ms;
    mh[11] = bw.va[MHI_STATUS / 4];
    if (boot.phase == MhiNeedContexts) mh[0] = 6;
    wdom::sample();
    release(bw);
    // the buffer stays: the device may still read it until the next stage resets it
}

// Late USB debug start: the early attempt (PE_init_kprintf) cannot vote the USB2 PHY's supplies and finds the PHY off. At 25 s,
// vote ldoa5 0.88 V, ldoa12 1.8 V, ldoa2 3.072 V (usb2_phy0 vdd, vdda18, vdda33), bind to master_cpu (the driver serves only from
// there) and initialise again with the PHY bring-up; on success keep polling from here, as the boot-time worker would have.
// ---- MAX77705 MUIC ---------------------------------------------------------------------------------------------------
// The USB-C D+/D- reach the SoC's HS PHY through the MAX77705's internal switch (live FDT /soc/i2c@980000/max77705@66; the
// MUIC/USBC block answers at 0x25, Samsung max77705.c I2C_ADDR_MUIC 0x4a >> 1). Its firmware takes AP commands as opcodes:
// write opcode + data from AP_DATAOUT0 (0x21), terminate with 0 at AP_DATAOUT32 (0x41), read the reply from AP_DATAIN0 (0x51).
// CONTROL1_WRITE (0x06) with COMN1SW = COMP2SW = 1 (0x09, COM_USB) routes D+/D- to the AP (max77705-muic.c com_to_usb_ap); TWRP
// with adb working reads CONTROL1 = 0x09. QUP SE0's pins (GPIO 28 SDA, 29 SCL, TLMM NORTH) are driven here as a GPIO I2C master
// at about 100 kHz, as T970FtsTouch.cpp does on SE5; nothing else in this kernel uses SE0.
namespace muic {
constexpr unsigned kSda = 28, kScl = 29, kAddr = 0x25, kHalf = 5;
Window gSdaW, gSclW;
unsigned gNacks;
// periph14: the bus is shared with the battery service (T970Battery.cpp, fuel gauge 0x36 and charger 0x69): one transaction at a
// time under this lock, the pins mapped and configured once by whichever user comes first.
volatile uint32_t gBusy;
bool gBusReady;
void lock() {for (uint32_t idle = 0; !__atomic_compare_exchange_n(&gBusy, &idle, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED); idle = 0) IOSleep(1);}
void unlock() {__atomic_store_n(&gBusy, 0u, __ATOMIC_RELEASE);}
unsigned gAddr = kAddr;
volatile uint32_t *pinWin(unsigned pin) {return pin == kSda ? gSdaW.va : gSclW.va;}
void release(unsigned pin) {pinWin(pin)[0] &= ~(1u << 9);}
void drive(unsigned pin) {pinWin(pin)[1] &= ~2u; pinWin(pin)[0] |= 1u << 9;}
bool high(unsigned pin) {return pinWin(pin)[1] & 1u;}
bool sclUp() {release(kScl); for (unsigned i = 0; i < 2000; ++i) {if (high(kScl)) return true; IODelay(1);} return false;}
void recover() {for (unsigned i = 0; i < 9 && !high(kSda); ++i) {drive(kScl); IODelay(kHalf); sclUp(); IODelay(kHalf);}}
bool start() {
    release(kSda); if (!sclUp()) return false; IODelay(kHalf);
    if (!high(kSda)) {recover(); if (!high(kSda)) return false;}
    drive(kSda); IODelay(kHalf); drive(kScl); IODelay(kHalf); return true;
}
void stop() {drive(kSda); IODelay(kHalf); sclUp(); IODelay(kHalf); release(kSda); IODelay(kHalf);}
bool writeByte(uint8_t b) {
    for (int bit = 7; bit >= 0; --bit) {
        if (b & (1u << bit)) release(kSda); else drive(kSda);
        IODelay(kHalf); if (!sclUp()) return false; IODelay(kHalf); drive(kScl);
    }
    release(kSda); IODelay(kHalf); if (!sclUp()) return false;
    const bool ack = !high(kSda); IODelay(kHalf); drive(kScl);
    if (!ack) ++gNacks;
    return ack;
}
uint8_t readByte(bool ack) {
    uint8_t b = 0; release(kSda);
    for (int bit = 7; bit >= 0; --bit) {IODelay(kHalf); sclUp(); if (high(kSda)) b |= uint8_t(1u << bit); IODelay(kHalf); drive(kScl);}
    if (ack) drive(kSda); else release(kSda);
    IODelay(kHalf); sclUp(); IODelay(kHalf); drive(kScl); release(kSda);
    return b;
}
bool write(const uint8_t *w, unsigned n) {
    bool ok = start() && writeByte(uint8_t(gAddr << 1));
    for (unsigned i = 0; ok && i < n; ++i) ok = writeByte(w[i]);
    stop(); return ok;
}
bool read(uint8_t reg, uint8_t *r, unsigned n) {
    bool ok = start() && writeByte(uint8_t(gAddr << 1)) && writeByte(reg);
    if (ok) {release(kSda); IODelay(kHalf); ok = sclUp(); IODelay(kHalf);}
    ok = ok && start() && writeByte(uint8_t((gAddr << 1) | 1));
    for (unsigned i = 0; ok && i < n; ++i) r[i] = readByte(i + 1 < n);
    stop(); return ok;
}
// One opcode: opcode + data at 0x21.., 0 at 0x41, then the reply at 0x51 (echoed opcode, data). The firmware answers within a few ms.
bool opcode(uint8_t op, const uint8_t *data, unsigned n, uint8_t reply[2]) {
    uint8_t w[4] = {0x21, op, 0, 0};
    for (unsigned i = 0; i < n && i < 2; ++i) w[2 + i] = data[i];
    const uint8_t end[2] = {0x41, 0x00};
    if (!write(w, 2 + n) || !write(end, 2)) return false;
    for (unsigned t = 0; t < 10; ++t) {
        IOSleep(10);
        if (!read(0x51, reply, 2)) return false;
        if (reply[0] == op) return true;
    }
    return false;
}
void configure(unsigned pin) {
    volatile uint32_t *c = pinWin(pin);
    c[1] &= ~2u;
    c[0] = (c[0] & ~(0x3cu | 3u | (1u << 9))) | 3u;   // function 0 (GPIO), pull-up, input (released)
}
bool ensureBus() {   // under lock()
    if (gBusReady) return true;
    if (!gSdaW.va && !mapWindow(gSdaW, kTlmm + kNorth + kSda * 0x1000ull, 0x1000)) return false;
    if (!gSclW.va && !mapWindow(gSclW, kTlmm + kNorth + kScl * 0x1000ull, 0x1000)) return false;
    configure(kSda); configure(kScl);
    IODelay(50);
    gBusReady = true;
    return true;
}
void routeUsbLocked();
void routeUsb() {
    volatile int32_t *m = t970_muic;
    unsigned enabled = 1;
    if (PE_parse_boot_argn("t970-muic", &enabled, sizeof enabled) && !enabled) {m[0] = -100; return;}
    lock();
    gAddr = kAddr;
    if (!ensureBus()) m[0] = -1;
    else routeUsbLocked();
    unlock();
}
void routeUsbLocked() {
    volatile int32_t *m = t970_muic;
    uint8_t id[2] = {}, st[3] = {}, cc[2] = {}, irq = 0;
    if (!read(0x02, &irq, 1) || !read(0x00, id, 2)) {m[5] = int32_t(gNacks); m[0] = -2; return;}
    m[7] = irq;
    m[1] = id[0] << 8 | id[1];
    m[0] = 1;
    if (read(0x06, st, 3)) m[2] = st[0] << 16 | st[1] << 8 | st[2];
    if (read(0x0a, cc, 2)) m[6] = cc[0] << 8 | cc[1];
    uint8_t reply[2] = {};
    // The chip keeps its last reply across an AP reboot; CONTROL2_READ first makes the CONTROL1 echo below a fresh one.
    (void)opcode(0x07, nullptr, 0, reply);
    if (!opcode(0x05, nullptr, 0, reply)) {m[5] = int32_t(gNacks); m[0] = -3; return;}   // CONTROL1_READ
    m[3] = reply[0] << 8 | reply[1];
    if (reply[1] == 0x09) {m[4] = m[3]; m[5] = int32_t(gNacks); m[0] = 3; return;}
    const uint8_t usb = 0x09;   // COM_USB
    if (!opcode(0x06, &usb, 1, reply)) {m[5] = int32_t(gNacks); m[0] = -4; return;}   // CONTROL1_WRITE
    if (!opcode(0x05, nullptr, 0, reply)) {m[5] = int32_t(gNacks); m[0] = -5; return;}
    m[4] = reply[0] << 8 | reply[1];
    m[5] = int32_t(gNacks);
    m[0] = reply[1] == 0x09 ? 2 : -6;
}
}  // namespace muic
}  // namespace

// The SE0 bus for other board services (battery): one register write at `address`, under the bus lock.
extern "C" bool t970_se0_write(uint8_t address, uint8_t reg, uint8_t value) {
    muic::lock();
    bool ok = muic::ensureBus();
    const uint8_t w[2] = {reg, value};
    if (ok) {muic::gAddr = address; ok = muic::write(w, 2); muic::gAddr = muic::kAddr;}
    muic::unlock();
    return ok;
}
// The SE0 bus for other board services (battery): one register read at `address`, under the bus lock.
extern "C" bool t970_se0_read(uint8_t address, uint8_t reg, uint8_t *data, unsigned count) {
    muic::lock();
    bool ok = muic::ensureBus();
    if (ok) {muic::gAddr = address; ok = muic::read(reg, data, count); muic::gAddr = muic::kAddr;}
    muic::unlock();
    return ok;
}

namespace {

void usbRetryThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "t970-qud-retry");
    volatile int32_t *q = t970_qud_retry;
    q[0] = 1;
    IOSleep(25000);
    muic::routeUsb();   // before the controller starts, so the host sees the pull-up through a closed switch
    if (!&qcom_usb_debug_t970_retry || !&qcom_usb_debug_poll || !&t970_rpmh_write) {q[0] = -1; return;}
    static const struct {const char *name; uint32_t mv;} rails[3] = {{"ldoa5", 880}, {"ldoa12", 1800}, {"ldoa2", 3072}};
    uint32_t rcs = 0;
    for (unsigned i = 0; i < 3; ++i) {
        int rc = t970_rpmh_write(rails[i].name, 0, rails[i].mv);
        if (!rc) rc = t970_rpmh_write(rails[i].name, 8, 7);
        if (!rc) rc = t970_rpmh_write(rails[i].name, 4, 1);
        rcs |= uint32_t(rc & 0xff) << (8 * i);
    }
    q[1] = int32_t(rcs);
    q[0] = 2;
    IOSleep(5);
    processor_t master = cpu_to_processor(master_cpu);
    if (!master) {q[0] = -3; return;}
    thread_bind(master);
    thread_block(THREAD_CONTINUE_NULL);
    q[4] = cpu_number();
    if (cpu_number() != master_cpu) {q[0] = -3; thread_bind(PROCESSOR_NULL); return;}
    q[0] = 3;
    q[2] = qcom_usb_debug_t970_retry();
    q[0] = 4;
    if (!q[2]) {thread_bind(PROCESSOR_NULL); return;}
    q[0] = 5;
    static uint32_t status[128];
    Window dwc = {};
    (void)mapWindow(dwc, 0x0a600000 + 0xc000, 0x1000);   // DWC3 global/device registers, read only here: DCFG 0x700, DCTL 0x704, DEVTEN 0x708, DSTS 0x70c
    for (;;) {
        qcom_usb_debug_poll();
        ++q[3];
        if (dwc.va && (q[3] & 1023) == 1) {
            q[5] = int32_t(dwc.va[0x70c / 4]); q[6] = int32_t(dwc.va[0x704 / 4]); q[7] = int32_t(dwc.va[0x708 / 4]);
            if (&qcom_usb_debug_t970_faults) {
                uint32_t f[16];
                qcom_usb_debug_t970_faults(f);
                for (unsigned i = 0; i < 16; ++i) t970_qud_faults[i] = f[i];
            }
        }
        if ((q[3] & 255) == 0 && &qcom_usb_debug_peek_status) {
            qcom_usb_debug_peek_status(status);
            if (status[0] != 3) break;   // QCOM_USB_DEBUG_ACTIVE
        }
        IOSleep(5);
    }
    q[0] = -5;
    thread_bind(PROCESSOR_NULL);
}

void wifiThread(void *arg, wait_result_t) {
    thread_set_thread_name(current_thread(), "t970-wifi");
    const unsigned level = unsigned(reinterpret_cast<uintptr_t>(arg));
    // the touch driver votes its rails over the same RSC slot at start-up; t970_rpmh_write serialises later callers, this keeps clear of the first ones
    IOSleep(20000);
    Window gcc = {};
    if (!mapWindow(gcc, kGcc, 0xa0000) || !mapPin(gPerst, kNorth, kPerst) || !mapPin(gClkReq, kNorth, kClkReq) || !mapPin(gWlanEn, kSouth, kWlanEn)) {
        t970_wifi_stats[0] = -1;
        release(gcc);
        return;
    }
    volatile uint32_t *g = gcc.va;
    t970_wifi_stats[0] = 1;
    t970_wifi_stats[1] = g[kVote / 4];
    t970_wifi_stats[2] = g[kGdsc / 4];
    t970_wifi_stats[3] = haltBits(g);
    t970_wifi_stats[4] = pinWord();
    t970_wifi_stats[0] = 2;
    if (level >= 2) bringUp(g, level);
    if (level >= 5) {smmuSurvey(); if (t970_wifi_stats[0] == 20) wifiStage5();}
    release(gcc);   // pins and PCIe stay mapped and powered for a later stage
}
}  // namespace

extern "C" void alioth_pcie_init(void) {
    unsigned retry = 0;
    if (PE_parse_boot_argn("t970-qud-retry", &retry, sizeof retry) && retry == 1 && !t970_qud_retry[0]) {
        thread_t usb = nullptr;
        if (kernel_thread_start(usbRetryThread, nullptr, &usb) == KERN_SUCCESS) thread_deallocate(usb);
        else t970_qud_retry[0] = -7;
    }
    unsigned stage = 0;
    if (!PE_parse_boot_argn("t970-wifi", &stage, sizeof stage) || stage < 1 || t970_wifi_stats[0]) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(wifiThread, reinterpret_cast<void *>(uintptr_t(stage)), &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else t970_wifi_stats[0] = -7;
}
