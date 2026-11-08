// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi Wi-Fi stage 6 (t970-wifi=6): the full stack. Linux ath11k (QCA6390 hw2.0), MHI, QMI, QRTR, mac80211 and cfg80211 from
// libT970Wifi.a (scripts/build-t970-wifi.py) run on this file's lx_platform/nw_board callbacks, and T970WifiNet.cpp publishes en0 and
// the Apple80211 interface once the station exists. T970WifiProbe.cpp stages 1-4 power the chip, train PCIe0, assign BAR0 and leave
// ASPM off; then it calls t970_wifi_stack_run() on its thread, which never returns while the stack runs.
//
// Adapted from UniXNU-sheng package/WifiProbe.cpp stage 6 (wifiStack, the arena, iMSI, firmware cache, control endpoint; 5d51e87) for
// this board. What differs:
//  - DMA: sheng borrows a window NativeUfs maps. Here the endpoint's streams (PCIe0 0x1c00 mask 0x7f) get a private stage-1 context
//    of the apps SMMU, as stage 5's wdom (T970WifiProbe.cpp, periph16-27) but with its own bank and a 64 MiB arena (sheng uses
//    128 MiB; t970-wifi-dma-mib=<n> changes it): ath11k's rings, the QMI host memory segments, the BHIe image and every skb live in
//    it. The CPU uses an uncached alias of the arena, so lx_platform.dma_sync has nothing to do; the device side's MAIR attribute
//    is Normal non-cacheable unless t970-wifi-mair6=<attr> says otherwise (periph27 tests write-back on stage 5).
//  - Buffers are aligned to their size rounded up to a power of two (sheng's rule: the boot loader refuses an image that is
//    only page aligned, and an IOMMU allocator aligns that way).
//  - MSI: the DesignWare iMSI receiver of PCIe0 (DBI 0x60000000 + 0x820) on SPI 141 (sm8250.dtsi pcie0 "msi"), its address the
//    arena's first page, as sheng. Without the line the worker polls every millisecond.
//  - Firmware: Data files under /private/var/ngpu/firmware/qca6390 (the GPU addon installs them) read with sheng_fw_open/read
//    (AdspFirmware.c). ath11k asks for amss.bin; this tablet's file is amss20.bin. The board file is bdwlan.elf / bdwlan.eNN by
//    the QMI board ID (scripts/build-t970-wifi.py patches core.c). wlan_mac.bin is made from /chosen/unique-chip-id.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSData.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <kern/thread.h>
#include <kern/cpu_data.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include "nw_abi.h"
#include "nw_control_endpoint.h"

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" int sheng_fw_open(const char *path, void **handle, uint64_t *size) __attribute__((weak_import));
extern "C" int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length) __attribute__((weak_import));
extern "C" void sheng_fw_close(void *handle) __attribute__((weak_import));
extern "C" void flush_dcache64(addr64_t addr, unsigned count, int phys);
extern "C" uint64_t kvtophys(vm_offset_t va);
extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_wifi_net_static_init(void) __asm__("__GLOBAL__sub_I_T970WifiNet.cpp");
extern "C" void t970_wifi_net_start(const uint8_t mac[6]);
extern "C" volatile uint32_t t970_wifi_stats[28];
extern "C" volatile int nw_wifi_bhi_next;
extern "C" void (*volatile nw_wifi_pre_bhi)(uint32_t off, uint32_t v);   // T970Ath11k.c: before every BAR write below 0x1000
extern "C" volatile int nw_wifi_t970_quirks;
extern "C" volatile unsigned int nw_wifi_t970_dbg;   // T970Ath11k.c: ath11k debug mask (t970-wifi-dbg)
extern "C" void nw_wifi_wr_note(uint32_t off, uint32_t v);   // T970Ath11k.c write trace   // T970Ath11k.c; bits in t970_wifi_stack_run   // T970Ath11k.c, set by MHI boot.c before the BHI buffer is allocated   // T970WifiProbe.cpp: [15] BAR0 sizing mask

extern "C" {
// The module's messages, a ring of 512 lines (newest at native_wifi_text_n - 1), and those of level 6 and below in a ring of their own;
// BootLog.c prints the first (the names are UniXNU-sheng's, so the same log reader applies).
char native_wifi_text[512][112];
volatile uint32_t native_wifi_text_n;
char native_wifi_note[128][112];
volatile uint32_t native_wifi_note_n;
// Stage 6: [0] checkpoint: the step about to run (0x10 delay, 0x101.. SMMU context: 0x101 map, 0x102 ID registers, 0x103 SMR scan,
// 0x104 bank choice (0x1800|i before bank i's SCTLR/FSR reads, 0x1c00 chosen; [12] the
// bank, [13] the SMMU offset being accessed, bit 31 a write), 0x105 tables, 0x106 arena, 0x107 page tables, 0x108 uncached alias, 0x109 context bank, 0x10a TLB sync,
// 0x10b SCTLR, 0x10c S2CR/SMR; 0x111 arena, 0x112 BAR size, 0x1120 DBI link check ([12] LNKCTL/STA, [13] port debug 0),
// 0x1121 config map, 0x1122 first config read ([14] its ID word until the firmware wait), 0x1123 BAR map, 0x1124 caps, 0x113 Data firmware, 0x114 control endpoint, 0x115 iMSI program,
// 0x116 SPI attach, 0x117 nw_wifi_start (inside it: [1] stage 1 = ath11k PCI probe/MHI power-up, 2 probe returned, 4 netdev),
// then 5 running polled, 6 running on MSI, 7 en0 published; negative: the failing step, 0x80000000 | -error when nw_wifi_start failed), [1] nw_wifi_stats.stage | scans << 8
// | bss_seen << 16, [2] SMMU bank | SMR << 8 | ASID << 16, [3] arena MiB | MAIR << 16, [4] arena used KiB, [5] peak KiB, [6] alloc
// failures, [7] MSIs, [8] MSI vectors seen, [9] MSI attach result, [10] frames, [11] last error, [12] bank FSR, [13] FAR low,
// [14] seconds waited for Data, [15] worker rounds / 1024
volatile uint32_t t970_wifi_stack[16];
}

namespace {
constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0x100000, kIova = 0xa0000000, kDbi = 0x60000000, kConfig = 0x60100000;
constexpr uint32_t kStream = 0x1c00, kStreamMask = 0x7f, kMsiSpi = 141;
constexpr uint32_t kMsiAddrLo = 0x820, kMsiAddrHi = 0x824, kMsiEnable0 = 0x828, kMsiMask0 = 0x82c, kMsiStatus0 = 0x830;
constexpr uint64_t kLeaf = 0x0060000000000f43ull;   // AF, inner shareable, nG, AP EL0 RW, UXN/PXN, page; AttrIndx 0
constexpr char kFirmwareRoot[] = "/private/var/ngpu/firmware/qca6390/";

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *mem = IODeviceMemory::withRange(base, size);
    if (!mem) return false;
    w.map = mem->map(kIOMapAnywhere | kIOMapInhibitCache);
    mem->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void barrier() {__asm__ volatile("dsb sy" ::: "memory");}
// The step about to run, in t970_wifi_stack[0] (BootLog [t970 wifi stack] "cp"): a hang leaves the last one in the log.
// t970-wifi-step-ms=<ms>: pause that long after each checkpoint, so the boot log (a slot every 2 s) records every step before a stall.
unsigned gStepMs;
void cp(uint32_t step) {t970_wifi_stack[0] = step; barrier(); if (gStepMs) IOSleep(gStepMs);}

void pLog(int level, const char *line);
extern "C" void t970_wifi_logf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

// ---- the private SMMU context: as wdom::install (T970WifiProbe.cpp), sized at run time ----------------------------------------
namespace sdom {
Window gSmmu;
IOBufferMemoryDescriptor *gTables, *gArena;
IOMemoryMap *gAlias;
IOMemoryDescriptor *gAliasMd;
IOMemoryDescriptor *gSblDevMd; IOMemoryMap *gSblDev;
unsigned gPath, gRuns;   // CPU view: 1 write-combine alias of the physical runs, 2 cached (device MAIR write-back)
uint8_t *gVa, *gCached;
uint64_t *gTablesVa;
uint32_t gBytes, gCb = 0xffff, gSmr = 0xffff, gAsid, gShift, gPages, gStreams, gVmid;
uint64_t gCbBase;
// Every register access first records its offset (bit 31: a write) in t970_wifi_stack[13] behind a dsb, exactly as stage 5's
// wdom::at() does (int2 froze inside the bank scan of this copy; wdom's accesses never did): the access pattern is now wdom's and a
// stalled access names itself in the boot log.
void at(uint64_t off) {t970_wifi_stack[13] = uint32_t(off); barrier();}
uint32_t r32(uint64_t off) {at(off); return gSmmu.va[off / 4];}
void w32(uint64_t off, uint32_t v) {at(off | 0x80000000u); gSmmu.va[off / 4] = v; barrier();}
// A checkpoint without the step pause, for the per-bank sub-steps; [12] the bank.
void sub(uint32_t step, uint32_t i) {t970_wifi_stack[12] = i; t970_wifi_stack[0] = step; barrier();}
void w64(uint64_t off, uint64_t v) {w32(off, uint32_t(v)); w32(off + 4, uint32_t(v >> 32));}
uint64_t r64(uint64_t off) {const uint32_t lo = r32(off); return lo | uint64_t(r32(off + 4)) << 32;}
uint64_t bank(uint32_t i) {return (uint64_t(gPages) + i) << gShift;}
// 0 or the failing step (negative).
int install(uint32_t bytes, uint32_t mair) {
    gBytes = bytes;
    const uint32_t l3 = bytes >> 21, tablePages = 2 + l3;
    cp(0x101);
    if (!mapWindow(gSmmu, kSmmu, kSmmuBytes)) return -1;
    cp(0x102);
    const uint32_t cr0 = r32(0), id0 = r32(0x20), id1 = r32(0x24), id2 = r32(0x28);
    if (cr0 & (1u << 3)) return -2;
    const uint32_t streams = id0 & 0xff, banks = id1 & 0xff, ns2 = (id1 >> 16) & 0xff;
    gShift = (id1 >> 31) ? 16 : 12; gPages = 2u << ((id1 >> 28) & 7);
    if (!streams || streams > 128 || !banks || banks > 128 || bank(banks) > kSmmuBytes) return -3;
    gStreams = streams;
    cp(0x103);
    bool used[128] = {};
    for (uint32_t i = 0; i < streams; ++i) {
        const uint32_t smr = r32(0x800 + 4 * i), s2cr = r32(0xc00 + 4 * i);
        if (!(smr & (1u << 31))) {if (gSmr == 0xffff) gSmr = i; continue;}
        const uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
        if (((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) < banks) used[s2cr & 0xff] = true;
        if (((kStream ^ id) & ~(mask | kStreamMask) & 0x7fff) == 0) return -4;   // stage 5 or UEFI already routes the stream
    }
    if (gSmr == 0xffff) return -5;
    cp(0x104);
    // The bank: t970-wifi-bank=<n>, else the first one from NUMS2CB up that no SMR routes to, with translation off and no recorded
    // fault; only each candidate's SCTLR and FSR are read. The CBAR/TTBR reads of every bank (to
    // find a free ASID) are gone: int2 froze in them at 90 s (wdom's identical scan at 20 s never did). The ASID is t970-wifi-asid=<a>, default 0xfb00 (stage 5 takes 0xfc00).
    unsigned forced = 0, asid = 0xfb00;
    PE_parse_boot_argn("t970-wifi-asid", &asid, sizeof asid);
    gAsid = uint16_t(asid ? asid : 0xfb00);
    if (PE_parse_boot_argn("t970-wifi-bank", &forced, sizeof forced) && forced >= ns2 && forced < banks) {
        sub(0x1800 | forced, forced);
        if (used[forced] || (r32(bank(forced)) & 1) || (r32(bank(forced) + 0x58) & 0xc00001feu)) return -6;
        gCb = forced;
    }
    for (uint32_t i = ns2; i < banks && gCb == 0xffff; ++i) {
        if (used[i]) continue;
        sub(0x1800 | i, i);
        if ((r32(bank(i)) & 1) || (r32(bank(i) + 0x58) & 0xc00001feu)) continue;
        gCb = i;
    }
    if (gCb == 0xffff) return -6;
    sub(0x1c00, gCb);
    if (!gAsid) return -7;
    gCbBase = bank(gCb);
    cp(0x105);
    gTables = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                               tablePages * 4096, 0x000000fffffff000ull);
    cp(0x106);
    gArena = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, bytes, 4096);
    if (!gTables || !gArena || gTables->prepare() != kIOReturnSuccess || gArena->prepare() != kIOReturnSuccess) return -8;
    uint64_t *t = static_cast<uint64_t *>(gTables->getBytesNoCopy());
    uint8_t *cached = static_cast<uint8_t *>(gArena->getBytesNoCopy());
    const uint64_t tpa = t ? kvtophys(vm_offset_t(t)) : 0;
    if (!t || !cached || !tpa) return -9;
    for (uint32_t i = 1; i < tablePages; ++i) if (kvtophys(vm_offset_t(t) + 4096 * i) != tpa + 4096 * i) return -10;
    cp(0x107);
    memset(t, 0, tablePages * 4096);
    t[(kIova >> 30) & 3] = (tpa + 4096) | 3;
    for (uint32_t k = 0; k < l3; ++k) t[512 + (((kIova >> 21) + k) & 511)] = (tpa + 8192 + 4096ull * k) | 3;
    memset(cached, 0, bytes);
    for (uint32_t off = 0; off < bytes; off += 4096) {
        const uint64_t pa = kvtophys(vm_offset_t(cached + off));
        if (!pa || (pa & 4095)) return -11;
        t[1024 + off / 4096] = (pa & 0x0000fffffffff000ull) | kLeaf;
        flush_dcache64(pa, 4096, TRUE);
    }
    // t970-wifi-hole=N (as stage 5's periph22 test): page N-1 of the arena left unmapped, so a device read of it must fault in this
    // bank (FSR TF, FAR = the address). hole=1 (the boot loader's first page) shows whether the PBL's reads reach this bank at all.
    unsigned hole = 0;
    if (PE_parse_boot_argn("t970-wifi-hole", &hole, sizeof hole) && hole >= 1 && hole <= bytes / 4096) t[1024 + hole - 1] = 0;
    flush_dcache64(tpa, tablePages * 4096, TRUE);
    // The CPU's view, as UniXNU-sheng's NativeUfs makes its Wi-Fi window: clean and invalidate the cached lines by VA, then a
    // separate write-combine (Normal non-cacheable) kernel mapping. int4: the earlier map(kIOMapInhibitCache) is Device memory
    // on arm64 or may hand back the cached mapping; the PBL then failed the SBL segment load (ef0a0104).
    cp(0x108);
    for (uint32_t off = 0; off < bytes; off += 64) __asm__ volatile("dc civac, %0" :: "r"(cached + off) : "memory");
    barrier();
    // int5: createMappingInTask on the buffer gave no separate mapping. A descriptor of its own over the arena's physical page
    // runs is mapped write-combine instead (a physical descriptor has no existing kernel mapping to hand back).
    {
        unsigned runs = 0;
        IOAddressRange *r = static_cast<IOAddressRange *>(IOMallocData((bytes / 4096) * sizeof(IOAddressRange)));
        if (r) {
            for (uint32_t off = 0; off < bytes; off += 4096) {
                const uint64_t pa = t[1024 + off / 4096] & 0x0000fffffffff000ull;
                if (runs && r[runs - 1].address + r[runs - 1].length == pa) r[runs - 1].length += 4096;
                else r[runs++] = {pa, 4096};
            }
            gAliasMd = IOMemoryDescriptor::withAddressRanges(r, runs, kIODirectionInOut | kIOMemoryTypePhysical64, TASK_NULL);
            IOFreeData(r, (bytes / 4096) * sizeof(IOAddressRange));
        }
        gAlias = gAliasMd ? gAliasMd->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
        gVa = gAlias ? reinterpret_cast<uint8_t *>(gAlias->getVirtualAddress()) : nullptr;
        gRuns = runs;
    }
    // the alias must show the same pages: its first and last page translate to the leaf entries' addresses
    if (gVa && (gVa == cached || kvtophys(vm_offset_t(gVa)) != (t[1024] & 0x0000fffffffff000ull) ||
                kvtophys(vm_offset_t(gVa + bytes - 4096)) != (t[1024 + bytes / 4096 - 1] & 0x0000fffffffff000ull))) {
        OSSafeReleaseNULL(gAlias); gVa = nullptr;
    }
    // int16b: an uncached (Device) view of the boot loader's 512 KiB, for the check before the BHI doorbell (made here, where
    // mapping is allowed; the check runs inside the MMIO write hook)
    {
        IOAddressRange r[128]; unsigned runs = 0;
        for (uint32_t off = 0; off < 0x80000 && off < bytes; off += 4096) {
            const uint64_t pa = t[1024 + off / 4096] & 0x0000fffffffff000ull;
            if (runs && r[runs - 1].address + r[runs - 1].length == pa) r[runs - 1].length += 4096;
            else r[runs++] = {pa, 4096};
        }
        gSblDevMd = runs ? IOMemoryDescriptor::withAddressRanges(r, runs, kIODirectionInOut | kIOMemoryTypePhysical64, TASK_NULL) : nullptr;
        gSblDev = gSblDevMd ? gSblDevMd->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    }
    // quirk bit 7 (128): no alias. The CPU uses the cached mapping and the device MAIR is write-back, as Samsung's dma-coherent
    // PCIe0: no mismatched attributes at all, if the PCIe0 path really snoops.
    if (gVa && (nw_wifi_t970_quirks & 128)) {OSSafeReleaseNULL(gAlias); gVa = nullptr;}
    if (gVa) gPath = 1;
    else {
        // fallback: the cached mapping, with the device side write-back (PCIe0 is dma-coherent in Samsung's DT; periph27 passed
        // the PBL with MAIR 0xff): coherent accesses need no maintenance then
        gVa = cached; gPath = 2; mair = 0xff;
        t970_wifi_stack[3] = (t970_wifi_stack[3] & 0xffffu) | 0xffu << 16;
    }
    gCached = cached; gTablesVa = t;
    cp(0x109);
    // Claim check: the apps SMMU is shared with the USB console (~25 s) and audio (~4 s, borrows a bank and restores it), all
    // taking banks bottom up. Just before programming, the chosen bank must still have SCTLR.M clear and no valid SMR routed to
    // it; otherwise take the next one that passes the same check.
    for (uint32_t tries = 0; tries < banks; ++tries) {
        bool routed = false;
        for (uint32_t i = 0; i < streams && !routed; ++i)
            if ((r32(0x800 + 4 * i) & (1u << 31)) && ((r32(0xc00 + 4 * i) >> 16) & 3) == 0 && (r32(0xc00 + 4 * i) & 0xff) == gCb) routed = true;
        if (!routed && !(r32(bank(gCb)) & 1) && !(r32(0x800 + 4 * gSmr) & (1u << 31))) break;
        if (r32(0x800 + 4 * gSmr) & (1u << 31)) {   // the free SMR went too: find another
            gSmr = 0xffff;
            for (uint32_t i = 0; i < streams; ++i) if (!(r32(0x800 + 4 * i) & (1u << 31))) {gSmr = i; break;}
            if (gSmr == 0xffff) return -5;
            continue;
        }
        uint32_t next = 0xffff;
        for (uint32_t i = gCb + 1; i < banks; ++i) if (!(r32(bank(i)) & 1) && !(r32(bank(i) + 0x58) & 0xc00001feu)) {next = i; break;}
        if (next == 0xffff) return -6;
        gCb = next; gCbBase = bank(gCb);
    }
    t970_wifi_stack[2] = gCb | gSmr << 8 | gAsid << 16;
    const uint32_t ps = id2 & 15;
    w32((1ull << gShift) + 0x800 + 4 * gCb, 1);                               // CBA2R: VA64
    // int15: CBAR VMID. Stage 5 (and QcomUsbDmaDomain) leave it 0, shared with every other stage-1 bank on VMID 0; stage 6 runs
    // at ~43 s, after the USB console's lease (~25 s), where stage 5 ran before it. A VMID of its own (t970-wifi-vmid=<v>, default
    // 0xfb; 0 = as stage 5) keeps any other bank's global TLB entries for 0xa0000000 from matching this one.
    unsigned vmid = 0xfb;
    PE_parse_boot_argn("t970-wifi-vmid", &vmid, sizeof vmid);
    gVmid = vmid & 0xff;
    w32((1ull << gShift) + 4 * gCb, 0x1f300u | gVmid);                         // CBAR: S1 translation, S2 bypass, VMID
    w32(gCbBase + 0x10, (r32(gCbBase + 0x10) & ~0x38017u) | 0x38010u | ps);    // TCR2
    w32(gCbBase + 0x30, 0x803520);                                            // TCR: T0SZ 32, WBWA IS walks, EPD1
    w64(gCbBase + 0x20, tpa | uint64_t(gAsid) << 48);                         // TTBR0
    w64(gCbBase + 0x28, uint64_t(gAsid) << 48);                               // TTBR1
    w32(gCbBase + 0x38, mair & 0xff); w32(gCbBase + 0x3c, 0);                 // MAIR0 attr 0
    w32(gCbBase + 0x34, 0);
    cp(0x10a);
    w32(gCbBase + 0x610, gAsid);                                              // TLBIASID
    w32(gCbBase + 0x618, 0);                                                  // TLBIALL (the bank's VMID, global entries too)
    w32(gCbBase + 0x7f0, 0xffffffffu);                                        // TLBSYNC
    unsigned n = 0;
    for (; n < 1000 && (r32(gCbBase + 0x7f4) & 1); ++n) IODelay(1);
    if (n == 1000) return -13;
    cp(0x10b);
    w32(gCbBase, 0x27);                                                       // SCTLR: CFRE AFE TRE M
    cp(0x10c);
    w32(0xc00 + 4 * gSmr, gCb);                                               // S2CR: translate via the bank
    w32(0x800 + 4 * gSmr, (1u << 31) | kStreamMask << 16 | kStream);           // SMR: PCIe0's streams
    return 0;
}
void sample() {
    if (!gSmmu.va || gCb == 0xffff) return;
    t970_wifi_stack[12] = r32(gCbBase + 0x58);
    t970_wifi_stack[13] = uint32_t(r64(gCbBase + 0x60));
}
// Every valid SMR that matches PCIe0's stream 0x1c00 (this bank's own included), as "i:smr/s2cr", and how many there are.
unsigned smrMatches(char *out, size_t len) {
    unsigned n = 0; size_t at = 0; out[0] = 0;
    if (!gSmmu.va) return 0;
    for (uint32_t i = 0; i < gStreams; ++i) {
        const uint32_t smr = r32(0x800 + 4 * i);
        if (!(smr & (1u << 31))) continue;
        const uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
        if (((kStream ^ id) & ~mask & 0x7fff) != 0) continue;
        ++n;
        if (at + 32 < len) at += size_t(snprintf(out + at, len - at, " %u:%08x/%08x", i, smr, r32(0xc00 + 4 * i)));
    }
    return n;
}
}  // namespace sdom

// ---- the arena: first page the MSI sink, then first fit at size-rounded-to-a-power-of-two alignment (UniXNU-sheng arenaAlloc) ----
constexpr unsigned kArenaBlocks = 16384;
uint32_t arenaMs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return uint32_t(ns / 1000000);}
struct Arena {uint8_t *va; uint64_t iova; size_t bytes; struct Block {uint32_t off, size; bool used; uint32_t born;} *blocks; unsigned count; size_t used, peak; unsigned failures;} gArena;
// The shim may allocate DMA memory under its own preemption-disabled spinlocks, where a blocking lock is not allowed: a plain spin.
uint32_t gArenaLock;
void arenaLock() {while (__atomic_exchange_n(&gArenaLock, 1u, __ATOMIC_ACQUIRE)) __asm__ volatile("yield");}
void arenaUnlock() {__atomic_store_n(&gArenaLock, 0u, __ATOMIC_RELEASE);}
bool gSblReserved;   // the first 512 KiB of the arena are held for the BHI boot loader
bool arenaInit() {
    gArena.blocks = static_cast<Arena::Block *>(IOMallocData(kArenaBlocks * sizeof(Arena::Block)));
    if (!gArena.blocks) return false;
    gArena.va = sdom::gVa; gArena.iova = kIova; gArena.bytes = sdom::gBytes;
    // the last page is the MSI sink (stage 5's kMsiSink); the first 512 KiB wait for the first request of that size, the BHI
    // boot loader, so it sits at IOVA 0xa0000000 as in stage 5 (t970-wifi-sbl-base=0 turns that off)
    unsigned base = 1;
    PE_parse_boot_argn("t970-wifi-sbl-base", &base, sizeof base);
    gSblReserved = base != 0;
    gArena.blocks[0] = {gSblReserved ? 0x80000u : 0u, uint32_t(gArena.bytes - 4096 - (gSblReserved ? 0x80000u : 0u)), false, 0};
    gArena.count = 1;
    gArena.used = gArena.peak = 4096;
    // quirk 32: the endpoint keeps writing its MSIs to stage 4's sink (IOVA 0xa03ff000): that page is taken out of the free block
    if ((nw_wifi_t970_quirks & 32) && gArena.bytes > 0x400000) {
        const Arena::Block whole = gArena.blocks[0];
        const uint32_t page = 0x3ff000;
        if (page > whole.off && page + 4096 < whole.off + whole.size) {
            gArena.blocks[0] = {whole.off, page - whole.off, false, 0};
            gArena.blocks[1] = {page, 4096, true, 0};
            gArena.blocks[2] = {page + 4096, whole.off + whole.size - page - 4096, false, 0};
            gArena.count = 3; gArena.used += 4096;
        }
    }
    return true;
}
uint64_t sinkIova() {return gArena.iova + gArena.bytes - 4096;}
// combo2: a block handed out may still have cache lines from an earlier cached access to its pages (bhieCheck read all the AMSS
// segments through the cached mapping; a freed block is reused), and the alias's non-cacheable accesses can hit them. Every block
// is cleaned and invalidated before it is zeroed, so it starts with no line in the cache (quirk 256 skips it).
void allocClean(uint32_t off, uint32_t n) {
    if ((nw_wifi_t970_quirks & 256) || !sdom::gCached) return;
    for (uint32_t k = 0; k < n; k += 64) __asm__ volatile("dc civac, %0" :: "r"(sdom::gCached + off + k) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
void *arenaAllocLocked(size_t n, uint64_t *bus) {
    // the BHI boot loader, announced by MHI's boot.c (nw_wifi_bhi_next); anything else of 512 KiB (ath11k's RDDM segments come
    // first) goes elsewhere
    if (gSblReserved && nw_wifi_bhi_next && n == 0x80000) {
        gSblReserved = false; nw_wifi_bhi_next = 0;
        gArena.used += n; if (gArena.used > gArena.peak) gArena.peak = gArena.used;
        // a block of its own at offset 0, kept in the table so arenaFree finds it
        if (gArena.count + 1 <= kArenaBlocks) {
            memmove(&gArena.blocks[1], &gArena.blocks[0], gArena.count * sizeof(Arena::Block));
            gArena.blocks[0] = {0, 0x80000u, true, arenaMs()}; ++gArena.count;
            allocClean(0, uint32_t(n));
            bzero(gArena.va, n);
            if (bus) *bus = gArena.iova;
            return gArena.va;
        }
    }
    n = (n + 63) & ~size_t(63);
    if (n >= 4096) n = (n + 4095) & ~size_t(4095);
    uint32_t align = 64;
    if (n >= 4096) {align = 4096; while (align < n) align <<= 1;}
    for (unsigned i = 0; i < gArena.count; ++i) {
        Arena::Block &b = gArena.blocks[i];
        if (b.used) continue;
        const uint32_t start = (b.off + align - 1) & ~(align - 1), pad = start - b.off;
        if (b.size < pad + n) continue;
        const bool tail = b.size > pad + n;
        const unsigned extra = (pad ? 1u : 0u) + (tail ? 1u : 0u);
        if (gArena.count + extra > kArenaBlocks) continue;
        const Arena::Block whole = b;
        memmove(&gArena.blocks[i + extra], &gArena.blocks[i], (gArena.count - i) * sizeof(Arena::Block));
        gArena.count += extra;
        unsigned at = i;
        if (pad) {gArena.blocks[at] = {whole.off, pad, false, 0}; ++at;}
        gArena.blocks[at] = {start, uint32_t(n), true, arenaMs()};
        if (tail) gArena.blocks[at + 1] = {start + uint32_t(n), whole.size - pad - uint32_t(n), false, 0};
        uint8_t *p = gArena.va + start;
        gArena.used += n; if (gArena.used > gArena.peak) gArena.peak = gArena.used;
        allocClean(start, uint32_t(n));
        bzero(p, n);
        if (bus) *bus = gArena.iova + start;
        return p;
    }
    ++gArena.failures;
    return nullptr;
}
void *arenaAlloc(size_t n, uint64_t *bus) {
    arenaLock();
    void *p = arenaAllocLocked(n, bus);
    const size_t used = gArena.used, total = gArena.bytes;
    arenaUnlock();
    if (!p) t970_wifi_logf(3, "dma window full: want %zu used %zu of %zu", n, used, total);
    return p;
}
void arenaFree(void *p) {
    if (!p || static_cast<uint8_t *>(p) < gArena.va || static_cast<uint8_t *>(p) >= gArena.va + gArena.bytes) return;
    const uint32_t off = uint32_t(static_cast<uint8_t *>(p) - gArena.va);
    arenaLock();
    for (unsigned i = 0; i < gArena.count; ++i) {
        if (gArena.blocks[i].off != off || !gArena.blocks[i].used) continue;
        gArena.used -= gArena.blocks[i].size;
        gArena.blocks[i].used = false;
        if (i + 1 < gArena.count && !gArena.blocks[i + 1].used) {
            gArena.blocks[i].size += gArena.blocks[i + 1].size;
            memmove(&gArena.blocks[i + 1], &gArena.blocks[i + 2], (gArena.count - i - 2) * sizeof(Arena::Block)); --gArena.count;
        }
        if (i > 0 && !gArena.blocks[i - 1].used) {
            gArena.blocks[i - 1].size += gArena.blocks[i].size;
            memmove(&gArena.blocks[i], &gArena.blocks[i + 1], (gArena.count - i - 1) * sizeof(Arena::Block)); --gArena.count;
        }
        break;
    }
    arenaUnlock();
}

// ---- lx_platform ---------------------------------------------------------------------------------------------------------------
uint64_t pNowNs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns;}
uint64_t pBootNs() {uint64_t ns = 0; absolutetime_to_nanoseconds(mach_continuous_time(), &ns); return ns;}
void pBusyDelayUs(unsigned int us) {IODelay(us);}
uint64_t pNow() {return pNowNs() / 1000000;}
void pSleep(unsigned ms) {IOSleep(ms);}
volatile uint32_t gMsiPending, gMsiCount, gMsiBits, gMsiWake;
bool gMsiAttached;
void cleanBlocks(uint32_t maxSize);
void pPoll() {
    if (!(nw_wifi_t970_quirks & 512)) cleanBlocks(0x10000);   // device-written events and contexts visible to the alias (see preWrite); quirk 512: not here
    nw_wifi_irq_mode(gMsiAttached);
    const uint32_t pending = __atomic_exchange_n(&gMsiPending, 0u, __ATOMIC_ACQ_REL);
    if (pending) nw_wifi_irq_pending(pending);
    nw_wifi_poll();
}
bool has(const char *s, const char *w) {for (; *s; ++s) {size_t k = 0; while (w[k] && s[k] == w[k]) ++k; if (!w[k]) return true;} return false;}
// Each line carries the milliseconds since the first one; a line equal to one of the last sixteen is only counted (UniXNU-sheng pLog).
uint32_t gLogRepeats;
// At the first BHI failure (int4: "Image transfer failed", ef0a0104): the bank's fault registers and BHI's error registers as
// they are then; dmaCheck() compares the boot loader's pages after nw_wifi_start returned.
struct BhiFail {bool seen; uint32_t fsr, fsynr0, errcode, dbg1, dbg2, dbg3; uint64_t far;
               uint32_t gfsr, gfsynr0, gfar, smrs, status, ee, mhist, pbl[5], sram[20]; char smr[160];} gBhiFail;
volatile uint32_t *gBarVa;
void bhiFailed() {
    if (gBhiFail.seen) return;
    gBhiFail.seen = true;
    if (sdom::gSmmu.va && sdom::gCb != 0xffff) {
        gBhiFail.fsr = sdom::r32(sdom::gCbBase + 0x58); gBhiFail.far = sdom::r64(sdom::gCbBase + 0x60);
        gBhiFail.fsynr0 = sdom::r32(sdom::gCbBase + 0x68);
        // int15: the global fault registers (an unmatched or conflicting stream shows there, not in the bank) and every SMR that
        // matches stream 0x1c00 at the moment of the failure
        gBhiFail.gfsr = sdom::r32(0x48); gBhiFail.gfsynr0 = sdom::r32(0x50); gBhiFail.gfar = sdom::r32(0x40);
        gBhiFail.smrs = sdom::smrMatches(gBhiFail.smr, sizeof gBhiFail.smr);
    }
    if (volatile uint32_t *m = gBarVa) {
        uint32_t bhi = m[0x28 / 4]; if (bhi > 0x1000 || (bhi & 3)) bhi = 0x100;
        gBhiFail.errcode = m[(bhi + 0x30) / 4]; gBhiFail.dbg1 = m[(bhi + 0x34) / 4];
        gBhiFail.dbg2 = m[(bhi + 0x38) / 4]; gBhiFail.dbg3 = m[(bhi + 0x3c) / 4];
        gBhiFail.status = m[(bhi + 0x2c) / 4]; gBhiFail.ee = m[(bhi + 0x28) / 4]; gBhiFail.mhist = m[0x48 / 4];
        // what the PBL logged, as stage 5's [t970 wifi pbl] row (T970WifiProbe.cpp periph23), through the 0x310c register window;
        // the window ath11k had selected is put back
        const uint32_t saved = m[0x310c / 4];
        auto rd = [m](uint32_t off) -> uint32_t {
            m[0x310c / 4] = 0x40000000u | ((off >> 19) & 0x3f); barrier();
            (void)m[0x310c / 4];
            return m[(0x80000 + (off & 0x7ffff)) / 4];
        };
        static const uint32_t regs[5] = {0x1b000f8, 0x1e22b34, 0x1910008, 0x1e0e238, 0x1f8031c};
        for (unsigned k = 0; k < 5; ++k) gBhiFail.pbl[k] = rd(regs[k]);
        for (unsigned k = 0; k < 20; ++k) gBhiFail.sram[k] = rd(0x1403d58 + 4 * k);
        m[0x310c / 4] = saved; barrier(); (void)m[0x310c / 4];
    }
}
void pLog(int level, const char *line) {
    if (has(line, "Image transfer failed") || has(line, "did not load image")) bhiFailed();
    if (has(line, "chan scan list") || has(line, "mac channel [")) {++gLogRepeats; return;}
    if (level >= 7 && (has(line, "htc rx completion") || has(line, "rx ce pipe") || has(line, "ext irq:") || has(line, "event mgmt rx") ||
                       has(line, "mgmt rx event") || has(line, "chan info vdev_id") || has(line, "scan event foreign channel"))) {++gLogRepeats; return;}
    static uint64_t first;
    static uint32_t recent[16];
    uint32_t h = 2166136261u;
    for (const char *c = line; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    h ^= static_cast<uint32_t>(level);
    for (uint32_t r : recent) if (r == h) {++gLogRepeats; return;}
    memmove(&recent[1], &recent[0], sizeof recent - sizeof recent[0]);
    recent[0] = h;
    const uint64_t now = pNow();
    if (!first) first = now;
    const uint32_t i = __atomic_fetch_add(const_cast<uint32_t *>(&native_wifi_text_n), 1u, __ATOMIC_RELAXED) % 512;
    snprintf(native_wifi_text[i], sizeof native_wifi_text[i], "%u %d %.100s", static_cast<uint32_t>(now - first), level, line);
    if (level <= 6) {
        const uint32_t j = __atomic_fetch_add(const_cast<uint32_t *>(&native_wifi_note_n), 1u, __ATOMIC_RELAXED) % 128;
        memcpy(native_wifi_note[j], native_wifi_text[i], sizeof native_wifi_note[j]);
    }
    if (has(line, "MHI_CB_EE_RDDM") || has(line, "smmu") || has(line, "stuck:")) sdom::sample();
}
void pHalt() {for (;;) IOSleep(1000);}
void *hAlloc(size_t n) {
    uint8_t *p = static_cast<uint8_t *>(IOMallocData(n + 16));
    if (!p) return nullptr;
    *reinterpret_cast<size_t *>(p) = n + 16;
    return p + 16;
}
void hFree(void *q) {
    if (!q) return;
    uint8_t *p = static_cast<uint8_t *>(q) - 16;
    IOFreeData(p, *reinterpret_cast<size_t *>(p));
}
void *pDmaAlloc(size_t n, uint64_t *bus) {return arenaAlloc(n, bus);}
void pDmaFree(size_t, void *p, uint64_t) {arenaFree(p);}
void pDmaSync(uint64_t bus, size_t n, int);
uint64_t pDmaMap(void *p, size_t n, int) {
    if (static_cast<uint8_t *>(p) < gArena.va || static_cast<uint8_t *>(p) >= gArena.va + gArena.bytes) return 0;
    const uint64_t bus = gArena.iova + uint64_t(static_cast<uint8_t *>(p) - gArena.va);
    pDmaSync(bus, n, 0);   // the CPU filled it through the alias: to DRAM before the device is told
    return bus;
}
void civacRange(const uint8_t *c, uint32_t n);
// Streaming syncs (map, unmap, sync_for_cpu/device) clean and invalidate the range by VA: the alias's lines can sit in the cache
// (int16b), so neither direction is safe without it.
void pDmaSync(uint64_t bus, size_t n, int) {
    if ((nw_wifi_t970_quirks & 256) || !sdom::gCached || bus < gArena.iova || bus + n > gArena.iova + gArena.bytes || !n) return;
    const uint64_t off = (bus - gArena.iova) & ~63ull;
    civacRange(sdom::gCached + off, uint32_t(((bus - gArena.iova) + n - off + 63) & ~63ull));
    barrier();
}

// ---- firmware: Data files read once and kept for the boot ------------------------------------------------------------------
struct FwCache {char name[24]; uint8_t *data; size_t size; bool tried;} gFw[10];
// The radio's address: from /chosen/unique-chip-id (FNV-1a, locally administered, unicast), the same at every boot (UniXNU-sheng boardMac).
bool boardMac(uint8_t m[6]) {
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *id = chosen ? OSDynamicCast(OSData, chosen->getProperty("unique-chip-id")) : nullptr;
    uint64_t seed = 0;
    if (id && id->getLength() >= sizeof seed) memcpy(&seed, id->getBytesNoCopy(), sizeof seed);
    OSSafeReleaseNULL(chosen);
    if (!seed) return false;
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned i = 0; i < 8; ++i) h = (h ^ ((seed >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    for (unsigned i = 0; i < 6; ++i) m[i] = uint8_t(h >> (8 * i));
    m[0] = uint8_t((m[0] & 0xfe) | 0x02);
    return true;
}
int macFile(const uint8_t **data, size_t *size) {
    static char text[40]; static size_t len;
    if (!len) {
        uint8_t m[6];
        if (!boardMac(m)) return -1;
        len = size_t(snprintf(text, sizeof text, "address=%02x%02x%02x%02x%02x%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]));
    }
    *data = reinterpret_cast<const uint8_t *>(text); *size = len;
    return 0;
}
// The wireless daemon derives every network's private address from /chosen mac-address-wifi0 and aborts a join without it.
void publishMac() {
    uint8_t m[6];
    if (!boardMac(m)) return;
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        if (!chosen->getProperty("mac-address-wifi0")) chosen->setProperty("mac-address-wifi0", m, sizeof m);
        chosen->release();
    }
    if (IORegistryEntry *wlan = IORegistryEntry::fromPath("/arm-io/wlan", gIODTPlane)) {
        if (!wlan->getProperty("local-mac-address")) wlan->setProperty("local-mac-address", m, sizeof m);
        wlan->release();
    }
}
int readData(const char *name, uint8_t **data, size_t *size) {
    if (!&sheng_fw_open || !&sheng_fw_read || !&sheng_fw_close) return ENOTSUP;
    char path[96];
    snprintf(path, sizeof path, "%s%s", kFirmwareRoot, name);
    void *f = nullptr; uint64_t n = 0;
    int e = sheng_fw_open(path, &f, &n);
    if (e) return e;
    if (!n || n > 32u * 1024 * 1024) {sheng_fw_close(f); return EINVAL;}
    uint8_t *b = static_cast<uint8_t *>(IOMallocData(size_t(n)));
    if (!b) {sheng_fw_close(f); return ENOMEM;}
    e = sheng_fw_read(f, 0, b, size_t(n));
    sheng_fw_close(f);
    if (e) {IOFreeData(b, size_t(n)); return e;}
    *data = b; *size = size_t(n);
    return 0;
}
int pFwGet(const char *path, const uint8_t **data, size_t *size) {
    const char *base = path;
    for (const char *c = path; *c; ++c) if (*c == '/') base = c + 1;
    if (!strcmp(base, "wlan_mac.bin")) return macFile(data, size);
    char want[24];
    strlcpy(want, strcmp(base, "amss.bin") ? base : "amss20.bin", sizeof want);   // QCA6390 hw2.0 image as Samsung ships it
    FwCache *slot = nullptr;
    for (auto &f : gFw) if (f.tried && !strncmp(f.name, want, sizeof f.name)) {slot = &f; break;}
    if (!slot) for (auto &f : gFw) if (!f.tried) {slot = &f; strlcpy(f.name, want, sizeof f.name); f.tried = true; break;}
    if (!slot) return -1;
    if (!slot->data) {
        const int e = readData(want, &slot->data, &slot->size);
        if (e) {t970_wifi_logf(6, "wifi: Data firmware %s: error %d", want, e); slot->tried = false; return -1;}
        t970_wifi_logf(6, "wifi: Data firmware %s bytes %zu", want, slot->size);
    }
    *data = slot->data; *size = slot->size;
    return 0;
}

// ---- nw_board: config space through the iATU CFG0 window stage 4 left at 0x60100000 (bus 1) ---------------------------------------
Window gCfgW, gBarW, gDbiW;
volatile uint32_t *gCfg;
uint32_t gPcieCap, gMsiCap;
bool findCaps() {
    uint8_t cap = uint8_t(gCfg[0x34 / 4] & 0xff);
    for (unsigned guard = 0; cap >= 0x40 && guard < 48; ++guard) {
        const uint32_t w = gCfg[cap / 4];
        if (uint8_t(w) == 0x10) gPcieCap = cap;
        if (uint8_t(w) == 0x05) gMsiCap = cap;
        cap = uint8_t(w >> 8);
    }
    return gPcieCap && gMsiCap;
}
int pCfgRead(int off, int size, uint32_t *v) {
    uint32_t o = uint32_t(off);
    if (o & 0x80000000u) o = gPcieCap + (o & 0x7fffffffu);
    if (o >= 0x1000) return -1;
    const uint32_t w = gCfg[(o & ~3u) / 4];
    *v = size == 4 ? w : size == 2 ? (w >> ((o & 2) * 8)) & 0xffff : (w >> ((o & 3) * 8)) & 0xff;
    return 0;
}
int pCfgWrite(int off, int size, uint32_t v) {
    uint32_t o = uint32_t(off);
    if (o & 0x80000000u) o = gPcieCap + (o & 0x7fffffffu);
    nw_wifi_wr_note(0x80000000u | uint32_t(size & 15) << 24 | o, v);
    // quirk bit 2: leave the endpoint's device control (MRRS) as stage 4 left it; the module lowers it to 128 bytes
    if ((nw_wifi_t970_quirks & 4) && gPcieCap && (o & ~3u) == gPcieCap + 8) return 0;
    // quirk bit 5 (32): the MSI capability stays as stage 4 programmed it (address 0xa03ff000, 4 vectors, enabled)
    if ((nw_wifi_t970_quirks & 32) && gMsiCap && o >= gMsiCap && o < gMsiCap + 0x18) return 0;
    // quirk bit 3: at most 4 MSI vectors (multiple message enable 2), as stage 5 enabled them; the worker polls every vector anyway
    if ((nw_wifi_t970_quirks & 8) && gMsiCap && (o & ~3u) == gMsiCap) {
        if (size == 4 && ((v >> 20) & 7) > 2) v = (v & ~(7u << 20)) | 2u << 20;
        if (size == 2 && (o & 2) && ((v >> 4) & 7) > 2) v = (v & ~(7u << 4)) | 2u << 4;
    }
    if (o >= 0x1000) return -1;
    if (size == 4) {gCfg[o / 4] = v; barrier(); return 0;}
    const unsigned shift = (size == 2 ? (o & 2) : (o & 3)) * 8;
    const uint32_t mask = (size == 2 ? 0xffffu : 0xffu) << shift;
    gCfg[(o & ~3u) / 4] = (gCfg[(o & ~3u) / 4] & ~mask) | ((v << shift) & mask);
    barrier();
    return 0;
}
// The boot loader's pages as the device reads them: the leaf entry of the failing IOVA (BHI ERRDBG1), then for each page of
// ERRDBG3 bytes the DRAM content (an uncached map of its physical page) against the CPU's alias and against amss20.bin.
// Just before BHI_IMGADDR_LOW (T970Ath11k.c mmio hook): stage 5's way of handing over the boot loader. int16b: DRAM page 1 of
// the boot loader differed from both the write-combine alias and amss20.bin (int15: no page did). The image is written again
// through the arena's cached mapping, cleaned and invalidated to the point of coherency by VA, and then DRAM is compared with
// the file through an uncached view before the doorbell. Quirk bit 6 (64) skips the rewrite (check only).
struct PreBhi {uint32_t calls, lo, bytes, rewritten, pages, diff, first, word, dram, file;} gPreBhi = {0, 0, 0, 0, 0, 0, ~0u, 0, 0, 0};
void preBhiImage(uint32_t lo) {
    PreBhi &p = gPreBhi;
    ++p.calls; p.lo = lo;
    if (lo < kIova || lo + 0x80000ull > kIova + sdom::gBytes) return;
    const uint8_t *image = nullptr; size_t imageBytes = 0;
    if (pFwGet("amss.bin", &image, &imageBytes) || !image) return;
    const uint32_t off = lo - uint32_t(kIova), n = uint32_t(imageBytes < 0x80000 ? imageBytes : 0x80000);
    p.bytes = n;
    if (!(nw_wifi_t970_quirks & 64) && sdom::gCached) {
        uint8_t *c = sdom::gCached + off;
        memcpy(c, image, n);
        for (uint32_t k = 0; k < n; k += 64) __asm__ volatile("dc civac, %0" :: "r"(c + k) : "memory");
        barrier();
        p.rewritten = 1;
    }
    if (!sdom::gSblDev || off != 0) return;
    const volatile uint32_t *d = reinterpret_cast<const volatile uint32_t *>(sdom::gSblDev->getVirtualAddress());
    p.pages = 0; p.diff = 0;
    for (uint32_t pg = 0; pg < n / 4096; ++pg) {
        bool differs = false;
        for (unsigned k = 0; k < 1024; ++k) {
            const uint32_t w = d[pg * 1024 + k];
            uint32_t f; memcpy(&f, image + pg * 4096 + 4 * k, 4);
            if (w != f) {
                if (p.first == ~0u) {p.first = pg; p.word = pg * 4096 + 4 * k; p.dram = w; p.file = f;}
                differs = true; break;
            }
        }
        ++p.pages; if (differs) ++p.diff;
    }
}
// int17a: past the PBL, MHI stopped at "Wait for device to enter SBL or Mission mode": the device reads the host's contexts and
// rings and writes events back, both through the same pages. Write-combine alias writes can sit in the cache (int16b), and alias
// reads can hit stale lines. So every block the arena handed out is cleaned and invalidated to the point of coherency:
// blocks of at most 64 KiB (contexts, rings, small buffers) before every MHI register or doorbell write and every shim poll; all
// blocks before the BHIe vector doorbell. Quirk bit 8 (256) turns it off. Counters: [calls, small bytes, all, last bhie].
struct Maint {uint32_t small, all, bytes, bhieSegs, bhieDiff, bhieFirst, bhieWord;} gMaint = {0, 0, 0, 0, 0, ~0u, 0};
void civacRange(const uint8_t *c, uint32_t n) {for (uint32_t k = 0; k < n; k += 64) __asm__ volatile("dc civac, %0" :: "r"(c + k) : "memory");}
void cleanBlocks(uint32_t maxSize) {
    if ((nw_wifi_t970_quirks & 256) || !sdom::gCached || !gArena.blocks) return;
    uint32_t bytes = 0;
    const uint32_t now = arenaMs();
    arenaLock();
    for (unsigned i = 0; i < gArena.count; ++i) {
        const Arena::Block &b = gArena.blocks[i];
        // combo1b: past MISSION MODE, QMI stalled after the M3 image (266 KiB, one block) was handed over. A block of any size is
        // also cleaned for 2 s after it was handed out: the CPU fills it (m3.bin, QMI host memory) and the firmware reads it then.
        if (!b.used || (b.size > maxSize && now - b.born > 2000)) continue;
        civacRange(sdom::gCached + b.off, b.size); bytes += b.size;
    }
    arenaUnlock();
    barrier();
    gMaint.bytes = bytes;
    if (maxSize == ~0u) ++gMaint.all; else ++gMaint.small;
}
// The BHIe vector table (TXVECADDR/TXVECSIZE as just written): after the clean, each segment as DRAM holds it (read through the
// cached mapping, the lines having just been invalidated) against amss20.bin in order; the last entry is the table itself.
void bhieCheck(volatile uint32_t *m, uint32_t bhie) {
    const uint64_t tab = m[(bhie + 0x2c) / 4] | uint64_t(m[(bhie + 0x30) / 4]) << 32;
    const uint32_t tabBytes = m[(bhie + 0x34) / 4];
    const uint8_t *image = nullptr; size_t imageBytes = 0;
    if (tab < kIova || tab + tabBytes > kIova + sdom::gBytes || pFwGet("amss.bin", &image, &imageBytes) || !image) return;
    const uint64_t *e = reinterpret_cast<const uint64_t *>(sdom::gCached + (tab - kIova));
    size_t at = 0; gMaint.bhieSegs = 0; gMaint.bhieDiff = 0;
    for (uint32_t i = 0; i < tabBytes / 16 && at < imageBytes; ++i) {
        const uint64_t a = e[2 * i], n = e[2 * i + 1];
        if (a == tab || a < kIova || a + n > kIova + sdom::gBytes) continue;
        const uint8_t *d = sdom::gCached + (a - kIova);
        const size_t len = n < imageBytes - at ? size_t(n) : imageBytes - at;
        ++gMaint.bhieSegs;
        for (size_t k = 0; k < len; ++k) if (d[k] != image[at + k]) {
            ++gMaint.bhieDiff;
            if (gMaint.bhieFirst == ~0u) {gMaint.bhieFirst = i; gMaint.bhieWord = uint32_t(at + k);}
            break;
        }
        civacRange(d, uint32_t((n + 63) & ~63ull));   // read through the cache: leave no line behind (combo2)
        at += len;
    }
    civacRange(sdom::gCached + (tab - kIova), tabBytes);
    barrier();
}
// T970Ath11k.c's hook: every BAR write below 0x1000, before it is stored.
// int23: the MHI context array addresses (CCABAP 0x58/0x5c, ECABAP 0x60/0x64) as the host programs them, so the report can read
// what the device wrote into the contexts: combo6 sent our HELLO answer and then saw nothing for 700 s; whether the device's read
// pointers move (host not consuming) or stand (device silent) decides between a host and a firmware/protocol cause.
uint64_t gCcaBap, gEcaBap;
void ringLog() {
    auto at = [](uint64_t iova) -> const volatile uint8_t * {
        return iova >= gArena.iova && iova + 64 <= gArena.iova + gArena.bytes && gArena.va ? gArena.va + (iova - gArena.iova) : nullptr;
    };
    auto q = [](const volatile uint8_t *b, unsigned o) -> unsigned long long {
        return (unsigned long long)(*reinterpret_cast<const volatile uint32_t *>(b + o)) |
               (unsigned long long)(*reinterpret_cast<const volatile uint32_t *>(b + o + 4)) << 32;
    };
    const volatile uint8_t *ec = at(gEcaBap), *cc = at(gCcaBap + 20 * 44);
    if (!ec || !cc || !at(gEcaBap + 2 * 44)) return;
    const volatile uint8_t *e1 = ec + 44, *c20 = cc, *c21 = cc + 44;
    // two rows: a slot row is cut at about 120 characters (combo7 lost ch21)
    t970_wifi_logf(6, "wifi: rings er0 rp %llx wp %llx er1 rp %llx wp %llx", q(ec, 28), q(ec, 36), q(e1, 28), q(e1, 36));
    t970_wifi_logf(6, "wifi: rings ch20 rp %llx wp %llx ch21 rp %llx wp %llx", q(c20, 28), q(c20, 36), q(c21, 28), q(c21, 36));
    const volatile uint8_t *r = at(q(e1, 12));     // er1's first four elements: ptr, dword0 (len/code), dword1 (type/chid)
    if (r) t970_wifi_logf(6, "wifi: er1[0..3] %08x/%08x %08x/%08x %08x/%08x %08x/%08x",
                          *reinterpret_cast<const volatile uint32_t *>(r + 8), *reinterpret_cast<const volatile uint32_t *>(r + 12),
                          *reinterpret_cast<const volatile uint32_t *>(r + 24), *reinterpret_cast<const volatile uint32_t *>(r + 28),
                          *reinterpret_cast<const volatile uint32_t *>(r + 40), *reinterpret_cast<const volatile uint32_t *>(r + 44),
                          *reinterpret_cast<const volatile uint32_t *>(r + 56), *reinterpret_cast<const volatile uint32_t *>(r + 60));
}
void preWrite(uint32_t off, uint32_t v) {
    volatile uint32_t *m = gBarVa;
    if (!m) return;
    if (off == 0x58) gCcaBap = (gCcaBap & ~0xffffffffull) | v;
    if (off == 0x5c) gCcaBap = (gCcaBap & 0xffffffffull) | uint64_t(v) << 32;
    if (off == 0x60) gEcaBap = (gEcaBap & ~0xffffffffull) | v;
    if (off == 0x64) gEcaBap = (gEcaBap & 0xffffffffull) | uint64_t(v) << 32;
    const uint32_t bhi = m[0x28 / 4], bhie = m[0x2c / 4];
    if (bhi < 0x1000 && off == bhi + 0x08) {preBhiImage(v); return;}
    if (bhi < 0x1000 && off >= bhi && off < bhi + 0x40) return;               // the rest of BHI
    if (bhie < 0x1000 && off == bhie + 0x3c) {                                 // BHIe TXVECDB: AMSS download
        cleanBlocks(~0u);
        if (!(nw_wifi_t970_quirks & 256)) bhieCheck(m, bhie);
        return;
    }
    if (bhie < 0x1000 && off >= bhie && off < bhie + 0x80) return;             // the rest of BHIe (RDDM vector set-up too)
    cleanBlocks(0x10000);
}
void preBhiLog() {
    const PreBhi &p = gPreBhi;
    t970_wifi_logf(p.diff ? 3 : 6, "wifi: pre-BHI rw %u diff %u/%u", p.rewritten, p.diff, p.pages);
    if (p.diff) t970_wifi_logf(3, "wifi: pre-BHI first +%x %08x file %08x", p.word, p.dram, p.file);
    t970_wifi_logf(6, "wifi: maint small %u all %u last %uK", gMaint.small, gMaint.all, gMaint.bytes >> 10);
    if (gMaint.all) t970_wifi_logf(gMaint.bhieDiff ? 3 : 6, "wifi: BHIe segs %u diff %u first %d at %x", gMaint.bhieSegs, gMaint.bhieDiff,
                                   int(gMaint.bhieFirst), gMaint.bhieWord);
}
void dmaCheck() {
    if (!gBhiFail.seen) return;
    t970_wifi_logf(3, "bhi fail: errcode %08x dbg %08x %08x %08x; bank fsr %08x far %llx fsynr0 %08x", gBhiFail.errcode, gBhiFail.dbg1,
                   gBhiFail.dbg2, gBhiFail.dbg3, gBhiFail.fsr, (unsigned long long)gBhiFail.far, gBhiFail.fsynr0);
    t970_wifi_logf(3, "bhi fail: status %08x ee %x mhistatus %08x; gfsr %08x gfsynr0 %08x gfar %08x; vmid %x", gBhiFail.status,
                   gBhiFail.ee, gBhiFail.mhist, gBhiFail.gfsr, gBhiFail.gfsynr0, gBhiFail.gfar, sdom::gVmid);
    t970_wifi_logf(3, "bhi fail: %u SMRs match 0x1c00:%s", gBhiFail.smrs, gBhiFail.smr);
    t970_wifi_logf(3, "bhi fail: pbl log %08x cfg %08x boot %08x dbg2 %08x qfprom %08x", gBhiFail.pbl[0], gBhiFail.pbl[1],
                   gBhiFail.pbl[2], gBhiFail.pbl[3], gBhiFail.pbl[4]);
    const uint32_t *w = gBhiFail.sram;
    t970_wifi_logf(3, "bhi fail: sram %x %x %x %x %x %x %x %x %x %x", w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9]);
    t970_wifi_logf(3, "bhi fail: sram+ %x %x %x %x %x %x %x %x %x %x", w[10], w[11], w[12], w[13], w[14], w[15], w[16], w[17], w[18], w[19]);
    const uint64_t iova = gBhiFail.dbg1;
    if (iova < kIova || iova >= kIova + sdom::gBytes || !sdom::gTablesVa) {t970_wifi_logf(3, "bhi fail: %llx is outside the arena", (unsigned long long)iova); return;}
    const uint32_t first = uint32_t((iova - kIova) >> 12);
    const uint64_t pte = sdom::gTablesVa[1024 + first];
    uint32_t bytes = gBhiFail.dbg3 ? gBhiFail.dbg3 : 0x80000;
    if (bytes > 0x80000) bytes = 0x80000;
    const uint8_t *image = nullptr; size_t imageBytes = 0;
    pFwGet("amss.bin", &image, &imageBytes);
    unsigned pages = 0, unmapped = 0, aliasDiff = 0, fileDiff = 0, firstAlias = ~0u, firstFile = ~0u;
    for (uint32_t off = 0; off < bytes && first + off / 4096 < sdom::gBytes / 4096; off += 4096) {
        const uint64_t e = sdom::gTablesVa[1024 + first + off / 4096];
        if ((e & 3) != 3) {++unmapped; continue;}
        IODeviceMemory *pm = IODeviceMemory::withRange(e & 0x0000fffffffff000ull, 4096);
        IOMemoryMap *mp = pm ? pm->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (pm) pm->release();
        if (!mp) continue;
        const volatile uint32_t *d = reinterpret_cast<const volatile uint32_t *>(mp->getVirtualAddress());
        const uint32_t *a = reinterpret_cast<const uint32_t *>(gArena.va + (iova - kIova) + off);
        bool da = false, df = false;
        for (unsigned k = 0; k < 1024; ++k) {
            const uint32_t w = d[k];
            if (!da && w != a[k]) {
                da = true;
                if (firstAlias == ~0u) t970_wifi_logf(3, "bhi fail: first difference at +%x: DRAM %08x alias %08x", off + 4 * k, w, a[k]);
            }
            if (!df && image && off + 4 * k + 4 <= imageBytes) {
                uint32_t f; memcpy(&f, image + off + 4 * k, 4);
                if (w != f) {df = true; if (firstFile == ~0u) t970_wifi_logf(3, "bhi fail: first file difference at +%x: DRAM %08x file %08x", off + 4 * k, w, f);}
            }
        }
        mp->release();
        ++pages;
        if (da) {++aliasDiff; if (firstAlias == ~0u) firstAlias = off / 4096;}
        if (df) {++fileDiff; if (firstFile == ~0u) firstFile = off / 4096;}
    }
    t970_wifi_logf(3, "bhi fail: iova %llx pte %llx; %u pages, %u unmapped", (unsigned long long)iova, (unsigned long long)pte, pages, unmapped);
    t970_wifi_logf(3, "bhi fail: DRAM!=alias %u (first %d), DRAM!=amss20 %u (first %d)", aliasDiff, int(firstAlias), fileDiff, int(firstFile));
}
int pMsiTarget(uint32_t *lo, uint32_t *hi, uint32_t *data) {*lo = uint32_t(sinkIova()); *hi = uint32_t(sinkIova() >> 32); *data = 0; return 0;}

// ---- MSI: the DesignWare iMSI receiver takes an inbound write to MSI_ADDR, sets bit <data> of controller 0 and raises SPI 141 ----
void msiIrq(void *) {
    const uint32_t status = gDbiW.va[kMsiStatus0 / 4];
    if (status) gDbiW.va[kMsiStatus0 / 4] = status;   // write-one-to-clear: the level line drops before the end of interrupt
    gMsiCount = gMsiCount + 1;
    gMsiBits = gMsiBits | status;
    // a line that fires with no iMSI status set is not this receiver's (or stays asserted): stop listening to the receiver after
    // 100000 such calls; the worker then polls (the SPI itself cannot be masked from here)
    static uint32_t empty;
    if (!status && ++empty > 100000 && gMsiAttached) {gDbiW.va[kMsiEnable0 / 4] = 0; gMsiAttached = false;}
    __atomic_fetch_or(&gMsiPending, status, __ATOMIC_RELEASE);
    __atomic_store_n(&gMsiWake, 1u, __ATOMIC_RELEASE);
    thread_wakeup(event_t(&gMsiWake));
}
int msiAttach() {
    if (!&armcpu_spi_attach) return -11;
    if (!gDbiW.va && !mapWindow(gDbiW, kDbi, 0x1000)) return -12;
    gDbiW.va[kMsiAddrLo / 4] = uint32_t(sinkIova());
    gDbiW.va[kMsiAddrHi / 4] = uint32_t(sinkIova() >> 32);
    gDbiW.va[kMsiMask0 / 4] = 0;
    gDbiW.va[kMsiEnable0 / 4] = 0xffffffffu;
    barrier();
    cp(0x116);
    const int rc = armcpu_spi_attach(32 + kMsiSpi, false, msiIrq, nullptr, nullptr, nullptr);
    gMsiAttached = rc == 0;
    return rc;
}
// Sleeps until an MSI, a frame to send (t970_wifi_wake) or ms milliseconds; returns at once if one came since the last wait.
void msiWait(unsigned ms) {
    if (__atomic_exchange_n(&gMsiWake, 0u, __ATOMIC_ACQ_REL)) return;
    assert_wait_timeout(event_t(&gMsiWake), THREAD_UNINT, ms, NSEC_PER_MSEC);
    if (__atomic_exchange_n(&gMsiWake, 0u, __ATOMIC_ACQ_REL)) {clear_wait(current_thread(), THREAD_AWAKENED); return;}
    thread_block(THREAD_CONTINUE_NULL);
    __atomic_store_n(&gMsiWake, 0u, __ATOMIC_RELEASE);
}

// ---- the cfg80211 control endpoint for a userspace supplicant (org.unixnu.wifi.control), as UniXNU-sheng ------------------------
struct ControlClient {uint64_t session; uint32_t unit;};
kern_ctl_ref gControlRef;
IOLock *gControlLock;
ControlClient *gControlClient;
errno_t controlError(int status) {
    switch (status) {
        case NW_CONTROL_OK: return 0;
        case NW_CONTROL_INVALID: return EINVAL;
        case NW_CONTROL_BUSY: return EWOULDBLOCK;
        case NW_CONTROL_NOT_READY: return ENXIO;
        case NW_CONTROL_AUTH_FAILED: return EACCES;
        case NW_CONTROL_TIMED_OUT: return ETIMEDOUT;
        case NW_CONTROL_UNSUPPORTED: return EOPNOTSUPP;
        case NW_CONTROL_NO_MEMORY: return ENOBUFS;
        case NW_CONTROL_NOT_FOUND: return ENOENT;
        case NW_CONTROL_STALE: return ESTALE;
        default: return EIO;
    }
}
errno_t controlConnect(kern_ctl_ref, sockaddr_ctl *address, void **information) {
    auto *owner = static_cast<ControlClient *>(IOMalloc(sizeof(ControlClient)));
    if (!owner) return ENOMEM;
    const int status = nw_wifi_control_attach(&owner->session);
    if (status) {IOFree(owner, sizeof(*owner)); return controlError(status);}
    owner->unit = address->sc_unit;
    IOLockLock(gControlLock); gControlClient = owner; IOLockUnlock(gControlLock);
    *information = owner;
    return 0;
}
errno_t controlDisconnect(kern_ctl_ref, uint32_t, void *information) {
    if (!information) return 0;
    auto *owner = static_cast<ControlClient *>(information);
    nw_wifi_control_detach(owner->session);
    IOLockLock(gControlLock); if (gControlClient == owner) gControlClient = nullptr; IOLockUnlock(gControlLock);
    IOFree(owner, sizeof(*owner));
    return 0;
}
int controlReadMbuf(void *source, void *destination, size_t length) {return mbuf_copydata(static_cast<mbuf_t>(source), 0, length, destination);}
errno_t controlReceive(kern_ctl_ref, uint32_t, void *information, mbuf_t packet, int) {
    auto *owner = static_cast<ControlClient *>(information);
    const int status = nw_wifi_control_receive(owner->session, mbuf_pkthdr_len(packet), controlReadMbuf, packet);
    mbuf_freem(packet);
    return controlError(status);
}
void controlReceived(kern_ctl_ref, uint32_t, void *information, int) {nw_wifi_control_writable(static_cast<ControlClient *>(information)->session);}
errno_t controlGetOption(kern_ctl_ref, uint32_t, void *information, int option, void *bytes, size_t *length) {
    if (option != NW_CONTROL_GET_SESSION) return ENOPROTOOPT;
    if (!length) return EINVAL;
    if (!bytes || *length < sizeof(uint64_t)) {*length = sizeof(uint64_t); return EMSGSIZE;}
    const uint64_t session = static_cast<ControlClient *>(information)->session;
    memcpy(bytes, &session, sizeof(session)); *length = sizeof(session);
    return 0;
}
int controlSend(void *, uint64_t session, const void *bytes, size_t length) {
    IOLockLock(gControlLock);
    const bool active = gControlClient && gControlClient->session == session;
    const uint32_t unit = active ? gControlClient->unit : 0;
    IOLockUnlock(gControlLock);
    if (!active) return NW_CONTROL_SEND_CLOSED;
    const errno_t result = ctl_enqueuedata(gControlRef, unit, const_cast<void *>(bytes), length, CTL_DATA_EOR);
    if (!result) return NW_CONTROL_SEND_OK;
    if (result == ENOBUFS || result == EWOULDBLOCK) return NW_CONTROL_SEND_AGAIN;
    return NW_CONTROL_SEND_CLOSED;
}
void controlWake(void *);
int controlStart() {
    if (gControlRef) return 0;
    if (!gControlLock) gControlLock = IOLockAlloc();
    if (!gControlLock) return ENOMEM;
    kern_ctl_reg registration{};
    snprintf(registration.ctl_name, sizeof(registration.ctl_name), "%s", NW_CONTROL_NAME);
    registration.ctl_flags = CTL_FLAG_PRIVILEGED | CTL_FLAG_REG_EXTENDED;
    registration.ctl_sendsize = 256 * 1024; registration.ctl_recvsize = 512 * 1024;
    registration.ctl_connect = controlConnect; registration.ctl_disconnect = controlDisconnect;
    registration.ctl_send = controlReceive; registration.ctl_getopt = controlGetOption; registration.ctl_rcvd = controlReceived;
    const errno_t result = ctl_register(&registration, &gControlRef);
    if (result) return result;
    const nw_control_transport transport{nullptr, controlSend, controlWake};
    const int status = nw_wifi_control_transport_set(&transport);
    if (status) {ctl_deregister(gControlRef); gControlRef = nullptr; return controlError(status);}
    return 0;
}

// The network side (T970WifiNet.cpp): its classes are registered with the kernel's module late, then en0 is made.
void netStart() {
    struct nw_wifi_status status;
    nw_wifi_status_get(&status);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {t970_wifi_logf(3, "wifi: network classes: no module handle"); return;}
    native_wifi_net_static_init();
    const OSReturn r = OSMetaClass::postModLoad(handle);
    if (r) {t970_wifi_logf(3, "wifi: network classes not registered: %d", int(r)); return;}
    t970_wifi_net_start(status.mac);
    t970_wifi_stack[0] = 7;
}
}  // namespace

extern "C" void t970_wifi_logf(int level, const char *fmt, ...) {
    char line[160]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    pLog(level, line);
}
// WifiNet.cpp's names: its log, a frame queued to send, a cached Data file (join.txt for t970-wifi-aj=1).
extern "C" void native_wifi_logf(int level, const char *fmt, ...) {
    char line[160]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    pLog(level, line);
}
extern "C" void t970_wifi_wake(void) {__atomic_store_n(&gMsiWake, 1u, __ATOMIC_RELEASE); thread_wakeup(event_t(&gMsiWake));}
namespace { void controlWake(void *) {t970_wifi_wake();} }
extern "C" int t970_wifi_file(const char *name, const uint8_t **data, size_t *size) {return pFwGet(name, data, size);}

// Stage 6, on T970WifiProbe's thread after stage 4 assigned BAR0 at barPa (bit 0..3 of the BAR register cleared). Returns only on failure.
extern "C" void t970_wifi_stack_run(uint64_t barPa) {
    volatile uint32_t *st = t970_wifi_stack;
    publishMac();
    // t970-wifi-quirks=<bits> bisects the sequence against stage 5: 1 no ath11k sw_reset, 2 no MHI pre-load hook (context registers
    // before BHI), 4 keep MRRS, 8 MSI 4 vectors, 16 no RDDM (no BHIe RX vector doorbell before BHI), 32 keep stage 4's MSI
    unsigned quirks = 0;
    PE_parse_boot_argn("t970-wifi-quirks", &quirks, sizeof quirks);
    nw_wifi_t970_quirks = int(quirks);
    // t970-wifi-dbg=<mask>: ath11k debug lines printed (debug.h ATH11K_DBG_*); default BOOT|QMI (0x60), as combo1b stalled in QMI
    unsigned dbg = 0x60;
    PE_parse_boot_argn("t970-wifi-dbg", &dbg, sizeof dbg);
    nw_wifi_t970_dbg = dbg;
    unsigned mib = 64, mair = 0x44;
    PE_parse_boot_argn("t970-wifi-dma-mib", &mib, sizeof mib);
    PE_parse_boot_argn("t970-wifi-mair6", &mair, sizeof mair);
    if (mib < 16 || mib > 256 || (mib & 1)) mib = 64;
    st[3] = mib | (mair & 0xff) << 16;
    // t970-wifi-delay=<s>: start the stack that many seconds after stage 4 (the link stays up meanwhile), e.g. once the boot log
    // and the USB console run, so the last checkpoint reaches the log
    unsigned delay = 0;
    PE_parse_boot_argn("t970-wifi-delay", &delay, sizeof delay);
    PE_parse_boot_argn("t970-wifi-step-ms", &gStepMs, sizeof gStepMs);
    if (gStepMs > 10000) gStepMs = 10000;
    if (delay) {cp(0x10); st[14] = delay; IOSleep(delay * 1000);}
    const int dom = sdom::install(mib << 20, mair);
    st[2] = sdom::gCb | sdom::gSmr << 8 | sdom::gAsid << 16;
    st[3] |= sdom::gPath << 24;   // mib/mair row field: bit 24 write-combine alias, bit 25 cached fallback (MAIR forced to 0xff)
    if (sdom::gPath) t970_wifi_logf(6, "wifi: DMA arena %u MiB bank %u, CPU view %s (%u physical runs), device MAIR %02x", mib, sdom::gCb,
                                    sdom::gPath == 1 ? "write-combine alias" : "cached, coherent", sdom::gRuns, (st[3] >> 16) & 0xff);
    if (dom) {st[0] = uint32_t(-100 + dom); return;}
    cp(0x111);
    if (!arenaInit()) {st[0] = uint32_t(-2); return;}
    cp(0x112);
    // Before any TLP: the root port's own view of the link (DBI, local registers). [12] LNKCTL/LNKSTA (bit 29 DLLA), [13] port
    // debug 0 (LTSSM state in bits 5:0, 0x11 = L0). A link that is not up would leave a config or memory read unanswered, which
    // stalls the bus; the stack then stops at -0x13 instead.
    cp(0x1120);
    if (!gDbiW.va && !mapWindow(gDbiW, kDbi, 0x1000)) {st[0] = uint32_t(-0x12); return;}
    st[12] = gDbiW.va[0x80 / 4]; st[13] = gDbiW.va[0x728 / 4];
    if ((st[13] & 0x3f) != 0x11 || !(st[12] & (1u << 29))) {st[0] = uint32_t(-0x13); return;}
    // BAR0's own size (stage 4's sizing mask): a mapping past it would let an access end in an unsupported request
    const uint32_t mask = t970_wifi_stats[15] & ~0xfu;
    const uint64_t barBytes = mask ? uint64_t(~mask) + 1 : 0;
    if (barBytes < 0x100000 || barBytes > 0x400000) {st[0] = uint32_t(-0x12); return;}
    cp(0x1121);
    if (!mapWindow(gCfgW, kConfig, 0x1000)) {st[0] = uint32_t(-3); return;}
    gCfg = gCfgW.va;
    cp(0x1122);   // the first config read through the iATU CFG0 window stage 4 left
    st[14] = gCfg[0];
    cp(0x1123);
    if (!mapWindow(gBarW, barPa, barBytes)) {st[0] = uint32_t(-3); return;}
    gBarVa = gBarW.va;
    cp(0x1124);
    if ((gCfg[0] & 0xffff) != 0x17cb || !findCaps()) {st[0] = uint32_t(-3); return;}
    cp(0x113);
    // the firmware files are on Data once the GPU addon has installed them: wait for amss20.bin (up to 120 s)
    const uint8_t *image = nullptr; size_t imageBytes = 0;
    unsigned waited = 0;
    for (; waited < 120 && pFwGet("amss.bin", &image, &imageBytes); ++waited) {st[14] = waited; IOSleep(1000);}
    if (!image) {t970_wifi_logf(3, "wifi: Data firmware unavailable at %s", kFirmwareRoot); st[0] = uint32_t(-4); return;}
    cp(0x114);
    static const struct lx_platform plat = {pNow, pSleep, pPoll, pLog, pHalt, hAlloc, hFree, pDmaAlloc, pDmaFree, pDmaMap, pDmaSync, pFwGet,
                                            pNowNs, pBootNs, pBusyDelayUs, _disable_preemption, _enable_preemption};
    nw_wifi_set_platform(&plat);
    static struct nw_board board;
    board.bar = const_cast<uint32_t *>(gBarW.va); board.bar_len = size_t(barBytes); board.device_id = uint16_t(gCfg[0] >> 16);
    board.msi_cap = uint16_t(gMsiCap); board.irq_base = 1000;
    board.cfg_read = pCfgRead; board.cfg_write = pCfgWrite; board.msi_target = pMsiTarget; board.map_phys = nullptr;
    const int control = controlStart();
    if (control) t970_wifi_logf(3, "wifi: control endpoint registration failed: %d", control);
    // The endpoint's MSI capability is enabled either way (stage 4, then ath11k's probe; the PBL needs it, periph27b). Taking the
    // writes as an interrupt is opt-in (t970-wifi-msi6=1): SPI 141 is the DW iMSI "msi0" line in mainline sm8250.dtsi, but Samsung's
    // msm_pcie lists only SPI 140 and INTx 149-152 and never used it, so it is unmeasured here. Without it the worker polls each ms.
    unsigned msiLine = 0;
    PE_parse_boot_argn("t970-wifi-msi6", &msiLine, sizeof msiLine);
    cp(0x115);
    st[9] = uint32_t(msiLine ? msiAttach() : -1);
    nw_wifi_irq_mode(gMsiAttached);
    t970_wifi_logf(6, gMsiAttached ? "wifi: MSI line attached (SPI 141)" : "wifi: MSI line not attached (%d), polling", int(st[9]));
    if (quirks) t970_wifi_logf(6, "wifi: quirks %x", quirks);
    {
        char smr[160]; const unsigned n = sdom::smrMatches(smr, sizeof smr);
        t970_wifi_logf(6, "wifi: bank %u VMID %x gfsr %08x; %u SMRs match 0x1c00:%s", sdom::gCb, sdom::gVmid, sdom::r32(0x48), n, smr);
    }
    cp(0x117);
    nw_wifi_pre_bhi = preWrite;
    const int r = nw_wifi_start(&plat, &board);
    nw_wifi_pre_bhi = nullptr;
    preBhiLog();
    st[11] = uint32_t(nw_wifi_stats.last_err);
    if (r) {
        // int14 reset the tablet ~30 s after a failed start (quirk 32 kept the endpoint's MSI live): quiesce the endpoint whatever
        // the quirks, MSI off and no bus mastering, by direct config writes (not through pCfgWrite's quirk filter). The SMMU bank and
        // the arena stay as they are (never released), and T970WifiProbe.cpp then holds PERST# and WLAN_EN low.
        if (gMsiCap) {gCfg[gMsiCap / 4] &= ~(1u << 16); barrier();}
        gCfg[0x04 / 4] = (gCfg[0x04 / 4] & 0xffffu) & ~0x4u; barrier();
        st[1] = nw_wifi_stats.stage | nw_wifi_stats.scans << 8 | nw_wifi_stats.bss_seen << 16;
        dmaCheck();
        st[12] = gBhiFail.fsr; st[13] = gBhiFail.dbg1;
        st[0] = 0x80000000u | uint32_t(-r & 0xffff); return;   // [12]/[13]: the bank FSR and BHI ERRDBG1 at the failure
    }
    st[0] = gMsiAttached ? 6 : 5;
    bool netDone = false;
    uint64_t nextReport = 0, rounds = 0;
    for (;;) {
        const int busy = nw_wifi_step();
        ++rounds;
        if (!netDone && nw_wifi_stats.stage >= 4) {netDone = true; netStart();}
        if (!busy) {
            if (gMsiAttached) {const unsigned wait = nw_wifi_wait_ms(1000); if (wait) msiWait(wait);}
            else IOSleep(1);
        } else if ((rounds & 63) == 0) thread_yield_to_preemption();
        if ((rounds & 63) == 0) {
            st[1] = nw_wifi_stats.stage | nw_wifi_stats.scans << 8 | nw_wifi_stats.bss_seen << 16;
            st[4] = uint32_t(gArena.used >> 10); st[5] = uint32_t(gArena.peak >> 10); st[6] = gArena.failures;
            st[7] = gMsiCount; st[8] = gMsiBits; st[10] = nw_wifi_stats.frames; st[11] = uint32_t(nw_wifi_stats.last_err);
            st[15] = uint32_t(rounds >> 10);
        }
        const uint64_t now = pNow();
        if (now >= nextReport) {
            nextReport = now + 30000;
            sdom::sample();
            t970_wifi_logf(6, "wifi: %u MSIs, vectors %08x; DMA used %zu peak %zu of %zu, failures %u", gMsiCount, gMsiBits,
                           gArena.used, gArena.peak, gArena.bytes, gArena.failures);
            char link[128]; nw_wifi_link_state(link, sizeof(link));
            if (link[0]) t970_wifi_logf(6, "wifi: link %s", link);
            ringLog();
        }
    }
}
