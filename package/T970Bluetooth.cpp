// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi Bluetooth bring-up: the QCA6390's Bluetooth side over QUP0 SE6 (GENI UART), boot argument t970-bt=<bits>, off by default.
// Board glue for UniXNU/QcaBluetooth (the Linux btqca.c start-up, H4 framing, IBS and GENI DMA engine, sources/periph/QcaBluetooth),
// after UniXNU-sheng package/BluetoothProbe.cpp (WCN7850 on SM8550 QUP2 SE6), adapted to SM8250 and to this board's live FDT:
//   bt_qca6390 (Samsung btpower): bt-reset-gpio 76 (south tile; the bt_en_sleep pinctrl drives it), wl-reset 20, sw-ctrl 124,
//     vdd-aon/dig pm8150_s6 0.95 V, rfa1 pm8150_s5 2.04 V, rfa2 pm8150a_s8 1.35 V (the same rails as the cnss Wi-Fi node, which also
//     names qcom,bt-en-gpio 21, but 20-23 are the secure SE7 fingerprint SPI pins: 21 is never touched; t970-bt-en overrides)
//   qupv3_se6_4uart @0x998000 (msm-geni-serial-hs, wakeup byte 0xfd), SPI 607 (INTID 639), wake = RX GPIO 19; pins (north tile)
//     16 CTS, 17 RTS, 18 TX, 19 RX, function 1 (qup6); QUP0 wrapper @0x9c0000, apps-SMMU stream 0x5a3
//   GCC (gcc-kona.c): SE6 RCG 0x17730 (CMD, CFG +4, M +8, N +c, D +10), branch halt 0x1772c, vote 0x52008 bits 16 (s6), 6/7 (m/s
//     AHB), 8/9 (core/core_2x)
// SM8250's SE is older than QUP 2.5, so the UART oversamples 32 times (Linux qcom_geni_serial halves it only from 2.5): the RCG runs
// at 96 MHz (GPLL0 even 300 MHz, M/N 8/25), which gives 3 Mbaud with divider 1 and 115200 with divider 26 (0.16 % off).
// The engine is polled (the worker services GENI every millisecond while powered) and the serial clock is never gated, so no IRQ or
// wake GPIO is needed for this first round. DMA buffers: cached pages below 4 GiB behind a bypass-emulation context bank (as
// T970Audio.cpp routes stream 0x1801), cache maintenance as sheng does.
// Firmware: htbtfw<rom>.tlv / htnv<rom>.bin (QCA6390 rom 2.0 => 20) from the device's vendor firmware, carried by the GPU addon to
// /private/var/ngpu/firmware/qca6390/ (the Wi-Fi firmware directory; the same chip). Nothing proprietary is in this repository.
//
// t970-bt bits: 1 run (HCI_Reset + Read Local Version at 115200 from ROM, then the btqca rampatch/NVM download at 3 Mbaud, then
// HCI_Reset + Read Local Version again); 2 stop after the ROM readback (no rampatch/NVM, no baud change); 4 GENI internal loopback
// self-test before BT_EN; 8 (GPIO 21) is now a no-op: secure pin; 16 stay at 115200 for the download; 32 leave the
// controller powered after the test (default: power it off again, then the worker exits); 64 step mode (the first 48 accesses each
// wait 2.6 s after their checkpoint, so the UFS boot log names the access about to run); 128 mirror each checkpoint to the USB console.
// t970-bt-stop=<n>: stop before 1 anything (after the wake), 2 the first read (mapped), 3 GCC (TLMM read), 4 rails (one GCC read),
// 5 DMA/SMMU, 6 the first SMMU access, 7 pin writes/clocks, 8 the first SE6 access, 9 BT_EN; err = 1000 + n.
// t970-bt-en=<gpio> (default 76), t970-bt-delay=<s> (default 60: after Wi-Fi stage 6 claims its SMMU bank at ~43 s).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <stdarg.h>
#include "qbt.h"
#include "qbt_geni.h"
extern "C" {
#include "qbt_internal.h"   // qbt_command: the core's own command/reply wait, for HCI_Reset and Read Local Version
}

extern "C" {
int t970_rpmh_write(const char *name, uint32_t word, uint32_t value) __attribute__((weak_import));
size_t qcom_usb_debug_write(const void *, size_t) __attribute__((weak_import));
int sheng_fw_open(const char *path, void **handle, uint64_t *size) __attribute__((weak_import));
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length) __attribute__((weak_import));
void sheng_fw_close(void *handle) __attribute__((weak_import));
extern volatile uint32_t t970_wifi_stats[28] __attribute__((weak_import));
vm_offset_t kvtophys(vm_offset_t va);

// [0] stage (see kStage*; negative = failed there), [1] error, [2] FW_REV (SE 0x68), [3] QUP wrapper version, [4] SE6 branch CBCR
// before | after << 16 (bit 31 off), [5] pins (bit i: 16..19 level, bit 4: BT_EN out, bit 5: sw-ctrl 124), [6] SMMU
// route kind | slot << 8 | bank << 16, [7] DMA PA low, [8] loopback (length | sent << 16 | match << 17), [9] baud,
// [10] product id, [11] soc id, [12] rom << 16 | patch, [13] tx bytes, [14] rx bytes, [15] commands, [16] timeouts,
// [17] tlv sent << 16 | acks, [18] hw errors | last << 8 | tlv final << 16, [19] GENI rx dma | tx dma << 16,
// [20] GENI line | dma errors << 16, [21] GENI fault M IRQ, [22] fault S IRQ, [23] fault RX DMA IRQ, [24] BT_EN gpio,
// [25] HCI version | revision << 8, [26] LMP version | subversion << 8, [27] manufacturer, [28] rails voted (bits) | rc << 16,
// [29] reset status (HCI_Reset reply status, 0x100 = no reply), [30] RCG CFG readback, [31] uptime ms at the last update
volatile int32_t t970_bt[32];
// Checkpoint before every MMIO access (combo1 request): [0] block << 28 | byte offset (1 SE6, 2 apps SMMU, 3 GCC, 4 TLMM gpio << 12 |
// reg, 5 QUP0 wrapper, 6 RPMh rail index, 7 phase: 1 start delay, 2 Wi-Fi wait, 3 mapping), [1] accesses so far, [2] uptime ms.
// A CPU stuck on an access leaves [0] naming it.
volatile uint32_t t970_bt_cp[3];
// The start-up log lines (btqca's own and this file's), '|' separated, for BootLog.
char t970_bt_text[1024];
volatile uint32_t t970_bt_text_length;
}

namespace {
enum {
    kStageArgs = 1, kStageMapped, kStageRails, kStageAddress, kStageDma, kStageClocks, kStageEngine, kStageLoopback, kStageEnabled,
    kStageReset, kStageVersion, kStageBoot, kStageReady, kStageOff
};
constexpr uint64_t kGcc = 0x100000, kTlmm = 0x0f000000, kWest = 0x100000, kSouth = 0x500000, kNorth = 0x900000;
constexpr uint64_t kSe6 = 0x998000, kQup0 = 0x9c0000, kSmmu = 0x15000000, kSmmuBytes = 0x100000;
constexpr uint16_t kStream = 0x5a3;
constexpr unsigned kCts = 16, kTx = 18, kRx = 19, kSwCtrl = 124, kCnssBtEn = 21;   // RTS is 17
constexpr uint32_t kRcg = 0x17730, kBranch = 0x1772c, kVote = 0x52008;
constexpr uint32_t kVoteBits = (1u << 16) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9);
constexpr uint32_t kSourceHz = 96000000;
constexpr uint32_t kOut = 1u << 9, kDrive16 = 7u << 6;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    if (w.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void barrier() {__asm__ volatile("dsb sy" ::: "memory");}
unsigned gMode;   // t970-bt bits, read by cp() before the worker's state exists
unsigned gStepMs = 2600;   // t970-bt-step=<ms>: step-mode pause (2600 > one BootLog period names the exact access; 300 ~ within 7)
void cp(uint32_t block, uint32_t offset) {
    t970_bt_cp[0] = block << 28 | (offset & 0x0fffffff); t970_bt_cp[1] = t970_bt_cp[1] + 1;
    uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); t970_bt_cp[2] = uint32_t(ns / 1000000);
    barrier();
    // bit 128: mirror every checkpoint to the USB CDC console at once (survives a reset if the Mac captures serial).
    if ((gMode & 128) && &qcom_usb_debug_write) {
        char line[40];
        const int n = snprintf(line, sizeof line, "BTCP %08x %u %u\n", t970_bt_cp[0], t970_bt_cp[1], t970_bt_cp[2]);
        if (n > 0) qcom_usb_debug_write(line, size_t(n));
    }
    // bit 64 (step mode): the first 48 unproven accesses each wait longer than one BootLog period (2 s) AFTER the checkpoint is recorded
    // and BEFORE the access, so the slot on UFS always names the access about to run; a reset leaves it as the culprit.
    // combo4/5 proved the TLMM pins (block 4: 16-19, 76, 124), the rails (6) and the GCC branch read safe, and the SMMU SMR/S2CR
    // tables (block 2, 0x800-0xfff) are scanned by Wi-Fi/USB too: those no longer pause, so the 48 pauses reach the context bank,
    // GCC writes and the first SE6/QUP0 accesses instead.
    static uint32_t pauses;
    const bool proven = block == 4 || block == 6 || (block == 2 && offset >= 0x800 && offset < 0x1000);
    if ((gMode & 64) && gStepMs && (block != 7 || offset >= 2) && !proven && pauses < 48) {++pauses; IOSleep(gStepMs);}
}
// t970-bt-stop=<n>: stop the worker before block n, touching nothing more (no power-off sequence either).
unsigned gStop;
bool gate(unsigned n) {
    if (gStop != n) return false;
    t970_bt[1] = int32_t(1000 + n);
    printf("[t970 bt] stopped at gate %u (t970-bt-stop)\n", n);
    return true;
}

struct Bt {
    Window gcc, se, qup, west, south, north, smmu;
    IOBufferMemoryDescriptor *dma;
    uint8_t *bytes;
    uint64_t pa[3];   // TX page, RX pages 0/1 (two 2048-byte buffers each)
    struct qbt_geni geni;
    struct qbt *core;
    unsigned mode, btEn, delay;
    uint8_t address[6];
    bool powered, loopback;
    uint8_t loop_rx[16]; unsigned loop_length;
} bt;

void note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void note(const char *fmt, ...) {
    char line[160];
    va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    printf("[t970 bt] %s\n", line);
    uint32_t at = t970_bt_text_length;
    size_t n = strlen(line);
    if (at + n + 2 < sizeof t970_bt_text) {
        memcpy(t970_bt_text + at, line, n); t970_bt_text[at + n] = '|'; t970_bt_text[at + n + 1] = 0;
        __atomic_store_n(&t970_bt_text_length, at + uint32_t(n) + 1, __ATOMIC_RELEASE);
    }
}
uint64_t nowNs(void *) {uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns;}
void stamp() {t970_bt[31] = int32_t(nowNs(nullptr) / 1000000);}
void stage(int s) {t970_bt[0] = s; stamp();}

// ---- pins ----
// The tile of every SM8250 GPIO 0-179 (pinctrl-sm8250.c PINGROUP: w west 0x100000, s south 0x500000, n north 0x900000).
constexpr char kTiles[] = "ssssnnnnnnnnnnnnnnnnnnnnssssnnnnssssssssssssssssssssssssssssssssssnnnssssssssnnnnnnnnsssssssnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnsssssssswwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwww";
// TLMM pins whose registers are secure (XPU) on this board: reading one resets the SoC. 20-23 are QUP SE7, the Goodix in-display
// fingerprint SPI that Samsung runs from TrustZone (combo4 died on the first read of GPIO 21); 28-31 and 40-43 are SM8250's usual
// gpio-reserved-ranges. pinReg refuses them, so no t970-bt-en value or mode bit can reach them.
constexpr bool securePin(unsigned gpio) {return (gpio >= 20 && gpio <= 23) || (gpio >= 28 && gpio <= 31) || (gpio >= 40 && gpio <= 43);}
volatile uint32_t *pinReg(unsigned gpio) {
    if (gpio >= sizeof kTiles - 1 || securePin(gpio)) return nullptr;
    Window &w = kTiles[gpio] == 'w' ? bt.west : kTiles[gpio] == 's' ? bt.south : bt.north;
    return w.va ? w.va + gpio * 0x1000 / 4 : nullptr;
}
void pin(unsigned gpio, uint32_t cfg, int out = -1) {
    volatile uint32_t *r = pinReg(gpio);
    if (!r) return;
    if (out >= 0) {cp(4, gpio << 12 | 4); r[1] = out ? 2 : 0;}
    cp(4, gpio << 12); r[0] = cfg; barrier();
}
uint32_t pinIn(unsigned gpio) {volatile uint32_t *r = pinReg(gpio); if (!r) return 0; cp(4, gpio << 12 | 4); return r[1] & 1;}
uint32_t pinWord() {
    uint32_t w = 0;
    for (unsigned i = 0; i < 4; ++i) w |= pinIn(kCts + i) << i;
    volatile uint32_t *en = pinReg(bt.btEn);
    if (en) {cp(4, bt.btEn << 12 | 4); w |= ((en[1] >> 1) & 1) << 4;}
    w |= pinIn(kSwCtrl) << 5;   // bit 6 (GPIO 21) is gone: that pin is secure, see securePin
    return w;
}

// ---- GENI callbacks ----
uint32_t urd(void *, uint32_t reg) {cp(1, reg); return bt.se.va[reg / 4];}
void uwr(void *, uint32_t reg, uint32_t value) {cp(1, reg); bt.se.va[reg / 4] = value; barrier();}
void dmaBarrier(void *) {barrier();}
void dmaSync(void *, const struct qbt_dma_buffer *buffer, size_t bytes, bool device, bool tx) {
    if (bytes && (device || !tx)) {
        uint64_t ctr; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
        const size_t line = 4u << ((ctr >> 16) & 15);
        uintptr_t at = reinterpret_cast<uintptr_t>(buffer->bytes) & ~(line - 1);
        const uintptr_t end = reinterpret_cast<uintptr_t>(buffer->bytes) + bytes;
        for (; at < end; at += line) {
            if (tx) __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
            else if (device) __asm__ volatile("dc civac, %0" :: "r"(at) : "memory");
            else __asm__ volatile("dc ivac, %0" :: "r"(at) : "memory");
        }
    }
    barrier();
}
void irqMask(void *, bool) {}            // polled: no interrupt line attached
void wakeWorker(void *) {}
size_t rxBytes(void *, const uint8_t *bytes, size_t n) {
    if (bt.loopback) {
        size_t count = sizeof bt.loop_rx - bt.loop_length; if (count > n) count = n;
        memcpy(bt.loop_rx + bt.loop_length, bytes, count); bt.loop_length += unsigned(count);
        return n;
    }
    return qbt_receive(bt.core, bytes, n);
}

// ---- qbt platform ----
void *allocate(void *, size_t n) {return IOMallocData(n);}
void release(void *, void *p, size_t n) {IOFreeData(p, n);}
size_t writeUart(void *, const uint8_t *p, size_t n) {return qbt_geni_write(&bt.geni, p, n);}
bool drained(void *) {return qbt_geni_drained(&bt.geni);}
int flow(void *, bool enabled) {qbt_geni_set_flow(&bt.geni, enabled); return 0;}
void pump(void *) {
    if (qbt_geni_service(&bt.geni)) (void)qbt_geni_service(&bt.geni);
    if (bt.geni.state == QBT_GENI_FAILED) qbt_fail(bt.core, bt.geni.error);
}
// Polled executor: never sleep longer than 1 ms, so GENI completions are seen.
void waitUntil(void *, uint64_t deadline) {
    const uint64_t t = nowNs(nullptr);
    if (deadline <= t) return;
    uint64_t ns = deadline - t; if (ns > 1000000) ns = 1000000;
    if (ns >= 1000000) IOSleep(1); else IODelay(uint32_t(ns / 1000) + 1);
}
int clockVote(void *, bool enabled) {return enabled ? 0 : -ENOTSUP;}   // never gated in this round (no RX wake source)
int quiesce() {
    const uint64_t end = nowNs(nullptr) + 40000000ULL;
    for (;;) {
        int e = qbt_geni_quiesce(&bt.geni); if (!e) return 0;
        if (e != -EINPROGRESS && e != -EBUSY) return e;
        pump(nullptr); if (bt.geni.state == QBT_GENI_STOPPED) return 0;
        if (nowNs(nullptr) >= end) return -ETIMEDOUT;
        IODelay(50);
    }
}
int setRate(void *, uint32_t baud) {
    int e = quiesce(); if (e) {note("rate %u: quiesce %d", baud, e); return e;}
    e = qbt_geni_set_rate(&bt.geni, baud, kSourceHz, 32);
    if (!e) e = qbt_geni_resume(&bt.geni);
    note("rate %u -> %d", baud, e);
    if (!e) t970_bt[9] = int32_t(baud);
    return e;
}
int firmware(void *, const char *name, const uint8_t **data, size_t *size) {
    *data = nullptr; *size = 0;
    if (!&sheng_fw_open || !name || strncmp(name, "qca/", 4)) return -ENOENT;
    char path[128];
    snprintf(path, sizeof path, "/private/var/ngpu/firmware/qca6390/%s", name + 4);
    void *f = nullptr; uint64_t length = 0;
    int rc = sheng_fw_open(path, &f, &length);
    if (rc || !f) {note("fw %s: open %d", path, rc); return -ENOENT;}
    if (!length || length > 4u * 1024 * 1024) {sheng_fw_close(f); note("fw %s: size %llu", path, length); return -EINVAL;}
    uint8_t *buffer = static_cast<uint8_t *>(IOMallocData(size_t(length)));
    if (!buffer) {sheng_fw_close(f); return -ENOMEM;}
    rc = sheng_fw_read(f, 0, buffer, size_t(length));
    sheng_fw_close(f);
    if (rc) {IOFreeData(buffer, size_t(length)); note("fw %s: read %d", path, rc); return -EIO;}
    note("fw %s: %llu bytes", name, length);
    *data = buffer; *size = size_t(length);
    return 0;
}
void firmwarePut(void *, const uint8_t *data, size_t size) {if (data) IOFreeData(const_cast<uint8_t *>(data), size);}
size_t hostRx(void *, const uint8_t *, size_t n) {return n;}   // no host stack yet: forwarded packets are consumed
void log(void *, int level, const char *line) {if (level <= 6) note("%s", line);}

// ---- SMMU: route stream 0x5a3 to a bypass-emulation bank (S2CR then SMR; the bank SCTLR off, CBAR S1_TRANS_S2_BYPASS) ----
uint32_t gShift, gPages;
uint32_t sr(uint64_t off) {cp(2, uint32_t(off)); return bt.smmu.va[off / 4];}
void sw(uint64_t off, uint32_t v) {cp(2, uint32_t(off)); bt.smmu.va[off / 4] = v; barrier();}
uint64_t cb(uint32_t i) {return (uint64_t(gPages) + i) << gShift;}
int routeStream() {
    const uint32_t cr0 = sr(0), id0 = sr(0x20), id1 = sr(0x24);
    if (cr0 & (1u << 3)) return -2;                       // stage 1 off for the whole SMMU (CLIENTPD?) - unexpected
    const uint32_t streams = id0 & 0xff, banks = id1 & 0xff, ns2 = (id1 >> 16) & 0xff;
    gShift = (id1 >> 31) ? 16 : 12; gPages = 2u << ((id1 >> 28) & 7);
    if (!streams || streams > 128 || !banks || banks > 128 || cb(banks) > kSmmuBytes) return -3;
    bool used[128] = {};
    uint32_t freeSmr = 0xffff, bank = 0xffff;
    for (uint32_t i = 0; i < streams; ++i) {
        const uint32_t smr = sr(0x800 + 4 * i), s2cr = sr(0xc00 + 4 * i);
        if (!(smr & (1u << 31))) {if (freeSmr == 0xffff) freeSmr = i; continue;}
        const uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
        const bool translate = ((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) < banks;
        if (translate) used[s2cr & 0xff] = true;
        if (((kStream ^ id) & ~mask & 0x7fff) == 0) {      // a mapping exists already: keep it only if it is a bypass bank
            const uint32_t b = s2cr & 0xff;
            if (translate && sr((1ull << gShift) + 4 * b) == 0x10000 && !(sr(cb(b)) & 1)) return 1 | int(i << 8) | int(b << 16);
            return -4;
        }
    }
    if (freeSmr == 0xffff) return -5;
    // Our own bank (never shared, never restored, so no other teardown can undo it), searched BOTTOM-UP from NUMS2CB, reading only
    // each candidate's SCTLR and FSR, exactly as Wi-Fi stage 6, the USB console and audio do. combo1 (bt1) searched top-down: those
    // high banks are ones no driver here has ever read, and Wi-Fi froze on reads of other banks' registers (int2) -> the board
    // reset right when this worker woke. Banks nobody uses come first bottom-up, so this reads only proven-safe registers.
    for (uint32_t i = ns2; i < banks && bank == 0xffff; ++i)
        if (!used[i] && !(sr(cb(i)) & 1) && !(sr(cb(i) + 0x58) & 0xc00001feu)) bank = i;
    if (bank == 0xffff) return -6;
    // Claim check just before programming (Wi-Fi, USB and audio claim banks at run time too): no valid SMR may route to the bank yet
    // and the free SMR must still be free; otherwise give up cleanly rather than share by accident.
    for (uint32_t i = 0; i < streams; ++i)
        if ((sr(0x800 + 4 * i) & (1u << 31)) && ((sr(0xc00 + 4 * i) >> 16) & 3) == 0 && (sr(0xc00 + 4 * i) & 0xff) == bank) return -10;
    if ((sr(0x800 + 4 * freeSmr) & (1u << 31)) || (sr(cb(bank)) & 1)) return -11;
    sw(cb(bank), 0);                                       // SCTLR: translation off
    sw((1ull << gShift) + 0x800 + 4 * bank, 1);            // CBA2R: VA64
    sw((1ull << gShift) + 4 * bank, 0x10000);              // CBAR: S1 translate (off), S2 bypass
    if (sr(cb(bank)) != 0 || sr((1ull << gShift) + 4 * bank) != 0x10000) return -7;
    sw(0xc00 + 4 * freeSmr, bank);                         // S2CR first
    if ((sr(0xc00 + 4 * freeSmr) & 0x300ff) != bank) return -8;
    sw(0x800 + 4 * freeSmr, 0x80000000u | kStream);        // then the exact SMR
    if (sr(0x800 + 4 * freeSmr) != (0x80000000u | kStream)) return -9;
    return 3 | int(freeSmr << 8) | int(bank << 16);
}

// ---- power, clocks, engine ----
int rails() {
    // The Wi-Fi probe votes the same supplies (T970WifiProbe.cpp powerRails, never released); repeating the vote is idempotent and
    // keeps Bluetooth working with t970-wifi off. Nothing here ever turns a shared rail off.
    if (!&t970_rpmh_write) return -100;
    static const struct {const char *name; uint32_t mv;} r[] = {{"smpa6", 950}, {"smpa5", 2040}, {"smpc8", 1350}, {"smpa4", 1800}};
    int first = 0; uint32_t ok = 0;
    for (unsigned i = 0; i < 4; ++i) {
        cp(6, i);
        int rc = t970_rpmh_write(r[i].name, 0, r[i].mv);
        if (!rc) rc = t970_rpmh_write(r[i].name, 8, 7);
        if (!rc) rc = t970_rpmh_write(r[i].name, 4, 1);
        if (rc) {if (!first) first = rc; continue;}
        ok |= 1u << i; IOSleep(2);
    }
    t970_bt[28] = int32_t(ok | uint32_t(first & 0xffff) << 16);
    return ok ? 0 : first;
}
uint32_t grd(uint32_t off) {cp(3, off); return bt.gcc.va[off / 4];}
void gwr(uint32_t off, uint32_t v) {cp(3, off); bt.gcc.va[off / 4] = v; barrier();}
uint32_t qrd(uint32_t off) {cp(5, off); return bt.qup.va[off / 4];}
bool branchOn() {
    // SE6 plus the QUP0 wrapper's m/s AHB and core/core_2x branches (halt registers 0x17004, 0x17008, 0x23000, 0x23008): every one
    // must read on before the first SE6 or wrapper register access, or that access stalls the CPU on an unclocked slave.
    static const uint32_t halts[] = {kBranch, 0x17004, 0x17008, 0x23000, 0x23008};
    gwr(kVote, grd(kVote) | kVoteBits);
    uint32_t off = 0;
    for (unsigned i = 0; i < 1000; ++i) {
        off = 0;
        // A branch counts as on when CLK_OFF (bit 31) is clear, or when it runs under hardware clock gating (HW_CTL, bit 1):
        // then the halt bit reads off while the bus idles, and Linux's clk_branch_wait skips the check (gcc-sm8250: the wrap-0 m/s
        // AHB branches have hwcg_bit 1). combo7 stopped here on s_ahb (0x17008) while SE6 itself read 0x4220 = on.
        for (unsigned h = 0; h < 5; ++h) {
            const uint32_t v = grd(halts[h]);
            if ((v & (1u << 31)) && !(v & 2)) off |= 1u << h;
        }
        if (!off) {
            note("branches on: se6 %x m_ahb %x s_ahb %x core %x core_2x %x", grd(halts[0]), grd(halts[1]), grd(halts[2]),
                 grd(halts[3]), grd(halts[4]));
            return true;
        }
        IODelay(1);
    }
    note("branches still off: mask %x (bit 0 se6, 1 m_ahb, 2 s_ahb, 3 core, 4 core_2x): %x %x %x %x %x", off, grd(halts[0]),
         grd(halts[1]), grd(halts[2]), grd(halts[3]), grd(halts[4]));
    return false;
}
int sourceClock() {
    // RCG2: CFG = SRC_SEL 6 (GPLL0 out even) << 8 | dual-edge MND mode 2 << 12 | HID 1 (div 1), M = 8, N = ~(25 - 8), D = ~25.
    gwr(kRcg + 8, (grd(kRcg + 8) & ~0xffffu) | 8);                                  // 16-bit MND fields (mnd_width 16)
    gwr(kRcg + 0xc, (grd(kRcg + 0xc) & ~0xffffu) | (~uint32_t(25 - 8) & 0xffff));
    gwr(kRcg + 0x10, (grd(kRcg + 0x10) & ~0xffffu) | (~uint32_t(25) & 0xffff));
    gwr(kRcg + 4, (grd(kRcg + 4) & ~0x371fu) | (6u << 8) | (2u << 12) | 1u);
    gwr(kRcg, grd(kRcg) | 1);                              // CMD_RCGR UPDATE
    unsigned i = 0;
    for (; i < 1000 && (grd(kRcg) & 1); ++i) IODelay(1);
    t970_bt[30] = int32_t(grd(kRcg + 4));
    return i == 1000 ? -ETIMEDOUT : 0;
}
int stopEngine() {
    uwr(nullptr, 0xe1c, 0);
    if ((urd(nullptr, 0x40) & 0x1001) || (urd(nullptr, 0xe40) & 3)) {
        uwr(nullptr, 0x604, 2); uwr(nullptr, 0x634, 2);
        unsigned i = 0; for (; i < 1000 && (urd(nullptr, 0x40) & 0x1001); ++i) IODelay(1);
        if (i == 1000) return -ETIMEDOUT;
        uwr(nullptr, 0xc58, 1); uwr(nullptr, 0xd58, 1);
        for (i = 0; i < 1000 && (urd(nullptr, 0xe40) & 3); ++i) IODelay(1);
        if (i == 1000) return -ETIMEDOUT;
    }
    uwr(nullptr, 0x618, UINT32_MAX); uwr(nullptr, 0x648, UINT32_MAX);
    uwr(nullptr, 0xc44, UINT32_MAX); uwr(nullptr, 0xd44, UINT32_MAX);
    bt.geni.state = QBT_GENI_OFF; bt.geni.rx_busy = bt.geni.tx_busy = false;
    return 0;
}
int prepareEngine() {
    struct qbt_dma_buffer tx = {bt.bytes, bt.pa[0], QBT_GENI_TX_SIZE}, rx[QBT_GENI_RX_COUNT];
    for (unsigned i = 0; i < QBT_GENI_RX_COUNT; ++i)
        rx[i] = {bt.bytes + 4096 + i * QBT_GENI_RX_SIZE, bt.pa[1 + i / 2] + (i & 1) * QBT_GENI_RX_SIZE, QBT_GENI_RX_SIZE};
    struct qbt_geni_ops ops = {nullptr, urd, uwr, dmaBarrier, dmaSync, nowNs, irqMask, wakeWorker, rxBytes};
    int e = qbt_geni_prepare(&bt.geni, &ops, &tx, rx); if (e) return e;
    e = qbt_geni_set_rate(&bt.geni, 115200, kSourceHz, 32); if (e) return e;
    t970_bt[9] = 115200;
    return 0;
}
int loopbackCheck() {
    static const uint8_t pattern[16] = {0x01, 0x55, 0xaa, 0x00, 0xff, 0x04, 0x0e, 0x7e, 0x81, 0x42, 0x24, 0x18, 0x10, 0x20, 0x40, 0x80};
    uwr(nullptr, 0x22c, 3); bt.loopback = true; bt.loop_length = 0;
    int e = qbt_geni_start(&bt.geni);
    const size_t sent = e ? 0 : qbt_geni_write(&bt.geni, pattern, sizeof pattern);
    const uint64_t end = nowNs(nullptr) + 50000000ULL;
    while (sent && bt.loop_length < sizeof pattern && bt.geni.state == QBT_GENI_RUN && nowNs(nullptr) < end)
        if (!qbt_geni_service(&bt.geni)) IOSleep(1);
    const bool match = bt.loop_length == sizeof pattern && !memcmp(bt.loop_rx, pattern, sizeof pattern);
    t970_bt[8] = int32_t(bt.loop_length | (sent ? 1u << 16 : 0) | (match ? 1u << 17 : 0));
    note("loopback start %d sent %zu got %u match %d", e, sent, bt.loop_length, match);
    e = bt.geni.state == QBT_GENI_RUN ? quiesce() : -EIO;
    bt.loopback = false; uwr(nullptr, 0x22c, 0);
    if (!e && bt.geni.state == QBT_GENI_STOPPED) return 0;
    e = stopEngine(); if (e) return e;
    return prepareEngine();
}
bool deriveAddress() {
    // A stable locally administered address from the chip id (as sheng): no identifier in the source, nothing written to the device.
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *id = chosen ? OSDynamicCast(OSData, chosen->getProperty("unique-chip-id")) : nullptr;
    uint64_t hash = 0xcbf29ce484222325ULL;
    static const char domain[] = "unixnu-bluetooth";
    for (unsigned i = 0; i < sizeof domain - 1; ++i) hash = (hash ^ uint8_t(domain[i])) * 0x100000001b3ULL;
    bool nonzero = false;
    if (id) {
        const uint8_t *seed = static_cast<const uint8_t *>(id->getBytesNoCopy());
        for (unsigned i = 0; i < id->getLength(); ++i) {nonzero |= seed[i] != 0; hash = (hash ^ seed[i]) * 0x100000001b3ULL;}
    }
    if (chosen) chosen->release();
    if (!nonzero) hash ^= mach_absolute_time();   // still valid for a bring-up test, just not stable
    for (unsigned i = 0; i < 6; ++i) bt.address[i] = uint8_t(hash >> (i * 8));
    bt.address[0] = (bt.address[0] & 0xfe) | 2;
    return nonzero;
}
void hexReply(const char *what, int e, const uint8_t *r, size_t n) {
    char hex[3 * 20 + 1] = {}; size_t at = 0;
    for (size_t i = 0; i < n && i < 20; ++i) at += snprintf(hex + at, sizeof hex - at, "%02x ", r[i]);
    note("%s: %d len %zu [%s]", what, e, n, hex);
}
// HCI_Reset and Read Local Version Information, logged byte for byte (Command Complete return parameters, status first).
int hciIdentify(const char *when) {
    uint8_t r[258]; size_t n = sizeof r;
    int e = qbt_command(bt.core, 0x0c03, nullptr, 0, 0, r, &n);
    char what[48]; snprintf(what, sizeof what, "%s HCI_Reset", when);
    hexReply(what, e, r, e ? 0 : n);
    t970_bt[29] = e ? 0x100 : (n ? r[0] : 0x101);
    stage(kStageReset);
    n = sizeof r;
    e = qbt_command(bt.core, 0x1001, nullptr, 0, 0, r, &n);
    snprintf(what, sizeof what, "%s Read_Local_Version", when);
    hexReply(what, e, r, e ? 0 : n);
    if (!e && n >= 9 && !r[0]) {
        // status, HCI version, HCI revision (2), LMP version, manufacturer (2), LMP subversion (2)
        t970_bt[25] = int32_t(r[1] | uint32_t(r[2] | r[3] << 8) << 8);
        t970_bt[26] = int32_t(r[4] | uint32_t(r[7] | r[8] << 8) << 8);
        t970_bt[27] = int32_t(r[5] | r[6] << 8);
        note("%s: HCI %u rev %04x LMP %u sub %04x manufacturer %04x", when, r[1], r[2] | r[3] << 8, r[4], r[7] | r[8] << 8,
             r[5] | r[6] << 8);
        stage(kStageVersion);
        return 0;
    }
    return e ? e : -EIO;
}
void stats() {
    struct qbt_stats s; qbt_get_stats(bt.core, &s);
    struct qbt_info info; qbt_get_info(bt.core, &info);
    t970_bt[10] = int32_t(info.product); t970_bt[11] = int32_t(info.soc); t970_bt[12] = int32_t(uint32_t(info.rom) << 16 | info.patch);
    t970_bt[13] = int32_t(s.tx_bytes); t970_bt[14] = int32_t(s.rx_bytes); t970_bt[15] = int32_t(s.commands); t970_bt[16] = int32_t(s.timeouts);
    t970_bt[17] = int32_t(uint32_t(s.tlv_sent) << 16 | uint32_t(s.tlv_acks & 0xffff));
    t970_bt[18] = int32_t(uint32_t(s.hardware_errors & 0xff) | uint32_t(s.last_hardware_error & 0xff) << 8 | uint32_t(s.tlv_final_status) << 16);
    t970_bt[19] = int32_t(uint32_t(bt.geni.stats.rx_dma & 0xffff) | uint32_t(bt.geni.stats.tx_dma) << 16);
    t970_bt[20] = int32_t(uint32_t(bt.geni.stats.line_errors & 0xffff) | uint32_t(bt.geni.stats.dma_errors) << 16);
    t970_bt[21] = int32_t(bt.geni.fault_irq[0]); t970_bt[22] = int32_t(bt.geni.fault_irq[1]); t970_bt[23] = int32_t(bt.geni.fault_irq[3]);
    t970_bt[5] = int32_t(pinWord());
    stamp();
}
int dmaBuffers() {
    // 3 pages: TX (4096), RX 0/1, RX 2/3. Each buffer stays inside one page. combo5: a below-4 GiB mask got -ENOMEM (no such
    // contiguous pages on this map), so use the 40-bit mask the Wi-Fi/GPU drivers use; GENI DMA takes PTR_L/PTR_H (64-bit) and the
    // SID 0x5a3 bank is bypass, so the PA goes straight to the bus.
    bt.dma = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                              3 * 4096, 0x000000fffffff000ULL);
    if (!bt.dma) return -ENOMEM;
    if (bt.dma->prepare() != kIOReturnSuccess) {bt.dma->release(); bt.dma = nullptr; return -ENOMEM;}
    bt.bytes = static_cast<uint8_t *>(bt.dma->getBytesNoCopy());
    if (!bt.bytes) return -ENOMEM;
    memset(bt.bytes, 0, 3 * 4096);
    for (unsigned i = 0; i < 3; ++i) {
        bt.pa[i] = kvtophys(reinterpret_cast<vm_offset_t>(bt.bytes + i * 4096));
        if (!bt.pa[i] || (bt.pa[i] & 0xfff)) return -EIO;
    }
    t970_bt[7] = int32_t(uint32_t(bt.pa[0]));
    note("dma pa %llx %llx %llx", bt.pa[0], bt.pa[1], bt.pa[2]);
    return 0;
}

int powerOn() {
    int e;
    stage(kStageMapped);
    cp(7, 3);
    if (!mapWindow(bt.gcc, kGcc, 0xa0000) || !mapWindow(bt.se, kSe6, 0x4000) || !mapWindow(bt.qup, kQup0, 0x2000) ||
        !mapWindow(bt.west, kTlmm + kWest, 0xb4000) || !mapWindow(bt.south, kTlmm + kSouth, 0xb4000) ||
        !mapWindow(bt.north, kTlmm + kNorth, 0xb4000) ||
        !mapWindow(bt.smmu, kSmmu, kSmmuBytes)) return -ENOMEM;
    if (gate(2)) return 1;                                  // mapped, nothing read yet
    t970_bt[24] = int32_t(bt.btEn);
    t970_bt[5] = int32_t(pinWord());
    if (gate(3)) return 1;                                  // TLMM reads done
    t970_bt[4] = int32_t(grd(kBranch) >> 16);
    if (gate(4)) return 1;                                  // one GCC read done
    note("pins before %x (cts rts tx rx | en<<4 | swctrl<<5), bt-en gpio %u", pinWord(), bt.btEn);

    stage(kStageRails);
    e = rails(); note("rails %x rc %d", unsigned(t970_bt[28]) & 0xffff, e); if (e) return e;
    if (gate(5)) return 1;                                  // rails voted

    stage(kStageAddress);
    const bool stable = deriveAddress();
    note("address %02x:%02x:%02x:%02x:%02x:%02x%s", bt.address[0], bt.address[1], bt.address[2], bt.address[3], bt.address[4],
         bt.address[5], stable ? "" : " (not stable: no unique-chip-id)");

    stage(kStageDma);
    e = dmaBuffers(); if (e) {note("dma %d", e); return e;}
    if (gate(6)) return 1;                                  // DMA pages, before any SMMU access
    e = routeStream(); t970_bt[6] = e;
    note("smmu route %x (1 existing bypass, 3 new bank; slot<<8 bank<<16)", unsigned(e));
    if (e < 0) return -EIO;

    // Controller held in reset while the UART is set up (BT_EN low, pins as gpio with the Android sleep biases).
    pin(bt.btEn, kDrive16 | kOut, 0);
    if (bt.mode & 8) pin(kCnssBtEn, kDrive16 | kOut, 0);
    for (unsigned g = kCts; g <= kRx; ++g) pin(g, g >= kTx ? 3u : 1u);
    IOSleep(100);

    if (gate(7)) return 1;                                  // SMMU routed, before pin writes and clocks
    stage(kStageClocks);
    e = sourceClock(); if (e) {note("rcg %d", e); return e;}
    if (!branchOn()) {note("se6 branch stays off %x", grd(kBranch)); return -EIO;}
    t970_bt[4] |= int32_t((grd(kBranch) >> 16) << 16);
    t970_bt[2] = int32_t(urd(nullptr, 0x68)); t970_bt[3] = int32_t(qrd(4));
    note("rcg cfg %x, se6 cbcr %x, FW_REV %x (proto %u), QUP %x", unsigned(t970_bt[30]), grd(kBranch), urd(nullptr, 0x68),
         (urd(nullptr, 0x68) >> 8) & 255, qrd(4));
    if (((urd(nullptr, 0x68) >> 8) & 255) != 2) return -ENXIO;   // the boot firmware did not load UART microcode into SE6

    if (gate(8)) return 1;                                  // clocks on, before the first SE6 access
    stage(kStageEngine);
    e = stopEngine(); if (e) {note("stop %d", e); return e;}
    e = prepareEngine(); if (e) {note("prepare %d", e); return e;}
    if (bt.mode & 4) {stage(kStageLoopback); e = loopbackCheck(); if (e) return e;}

    if (gate(9)) return 1;                                  // engine prepared, before BT_EN
    stage(kStageEnabled);
    pin(bt.btEn, kDrive16 | kOut, 1);
    if (bt.mode & 8) pin(kCnssBtEn, kDrive16 | kOut, 1);
    bt.powered = true;
    IOSleep(110);   // Android HAL: PowerUpChip then ~105 ms before it opens ttyHS0 at 115200 with flow control
    for (unsigned g = kCts; g <= kRx; ++g) pin(g, (1u << 2) | (g >= kTx ? 3u : 1u));   // function 1 = qup6
    e = qbt_geni_start(&bt.geni); if (e) {note("start %d", e); return e;}
    for (unsigned i = 0; i < 50; ++i) {pump(nullptr); IOSleep(1);}
    note("BT_EN high; pins %x, rx bytes before the first command %llu", pinWord(), bt.geni.stats.rx_bytes);
    return 0;
}
void powerOff() {
    if (bt.core && bt.geni.state == QBT_GENI_RUN) {(void)qbt_shutdown(bt.core); (void)quiesce();}
    if (bt.se.va) (void)stopEngine();
    pin(bt.btEn, kDrive16 | kOut, 0);
    if (bt.mode & 8) pin(kCnssBtEn, kDrive16 | kOut, 0);
    for (unsigned g = kCts; g <= kRx; ++g) pin(g, g >= kTx ? 3u : 1u);
    bt.powered = false;
    // the SMMU route, the DMA pages and the RCG stay: the engine is stopped, so nothing reaches the pages again
}

void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "t970-bt");
    stage(kStageArgs);
    // Wi-Fi starts at 20 s and owns WLAN_EN and the PCIe side; wait for it to settle (or fail) before touching the shared chip.
    cp(7, 1);
    IOSleep(bt.delay * 1000);
    cp(7, 2);
    note("start delay over (%u s)", bt.delay);
    if (&t970_wifi_stats) {
        for (unsigned i = 0; i < 300; ++i) {
            const int32_t s = int32_t(t970_wifi_stats[0]);
            if (s == 0 || s < 0 || s >= 20) break;
            IOSleep(100);
        }
        note("wifi stage %d", int(int32_t(t970_wifi_stats[0])));
    }
    if (gate(1)) return;                                    // woke, before any allocation or mapping
    struct qbt_platform ops = {nullptr, allocate, release, nowNs, writeUart, drained, setRate, flow, clockVote, pump, waitUntil,
                               hostRx, firmware, firmwarePut, log};
    bt.core = qbt_create(&ops);
    if (!bt.core) {t970_bt[1] = -ENOMEM; stage(-kStageArgs); return;}
    int e = powerOn();
    if (e == 1) return;                                     // stopped at a gate: no stats (they read TLMM)
    if (e) {
        const int at = t970_bt[0];
        t970_bt[1] = e; stats(); note("power on failed at stage %d: %d", at, e);
        powerOff(); stage(-at); return;
    }
    // ROM first, at 115200, on every run: a version readback that needs no firmware files.
    e = hciIdentify("rom");
    if (e) note("rom identify %d (continuing with btqca)", e);
    if (bt.mode & 2) {
        // stop here
    } else {
        stage(kStageBoot);
        const uint32_t baud = (bt.mode & 16) ? 115200 : 3000000;
        note("btqca start-up (QCA6390, %u baud)", baud);
        e = qbt_boot(bt.core, baud, bt.address);
        struct qbt_info info; qbt_get_info(bt.core, &info);
        stats();
        note("qbt_boot %d: state %u product %x soc %x rom %x patch %x fw '%s' baud %u", e, info.state, info.product, info.soc, info.rom,
             info.patch, info.firmware, info.baud);
        if (!e) e = hciIdentify("patched");
    }
    stats();
    t970_bt[1] = e;
    if (e) {
        const int at = t970_bt[0];
        note("failed after stage %d: %d (geni state %d err %d irq %x/%x/%x/%x)", at, e, int(bt.geni.state), bt.geni.error,
             bt.geni.fault_irq[0], bt.geni.fault_irq[1], bt.geni.fault_irq[2], bt.geni.fault_irq[3]);
        powerOff(); stage(-at); return;
    }
    stage(kStageReady);
    note("controller ready");
    if (bt.mode & 32) {
        // keep it powered and serviced (IBS sleep indications etc.) until a host stack exists
        for (;;) {pump(nullptr); qbt_step(bt.core); IOSleep(5); static unsigned n; if (!(++n % 200)) stats();}
    }
    powerOff(); stats(); stage(kStageOff);
    note("powered off again (t970-bt bit 32 keeps it on)");
}
}

extern "C" void t970_bluetooth_start(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("t970-bt", &mode, sizeof mode) || !(mode & 1)) return;
    static uint32_t started;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&started, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    bt.mode = mode; gMode = mode; bt.btEn = 76; bt.delay = 60;
    PE_parse_boot_argn("t970-bt-stop", &gStop, sizeof gStop);
    PE_parse_boot_argn("t970-bt-step", &gStepMs, sizeof gStepMs);
    if (gStepMs > 5000) gStepMs = 5000;
    PE_parse_boot_argn("t970-bt-en", &bt.btEn, sizeof bt.btEn);
    PE_parse_boot_argn("t970-bt-delay", &bt.delay, sizeof bt.delay);
    if (bt.btEn > 179 || securePin(bt.btEn)) bt.btEn = 76;   // 21 (the cnss bt-en) is a secure pin: never touched
    if (bt.delay > 120) bt.delay = 120;
    t970_bt[0] = kStageArgs;
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else t970_bt[0] = -kStageArgs;
}
