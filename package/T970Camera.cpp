// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi camera stage C1: power one sensor, talk to it over the CCI and read its chip ID (boot argument t970-camera=<bits>,
// off by default). Nothing streams yet; this proves power, clocks, MCLK, reset and the CCI I2C master. Notes: docs/camera-notes.md.
//
// Hardware (live GTS7XL FDT, evidence/recovery-live.dtb, Samsung's cam_sensor nodes):
//   front  cam-sensor@1  CCI0 (0xac4f000) master 1, GPIO 103/104, 8-bit 0x5a (7-bit 0x2d), MCLK2 GPIO 96, reset GPIO 30,
//                        VANA enable GPIO 38, VIO enable GPIO 37 (shared by every module), CSIPHY 3
//   rear   cam-sensor0   CCI0 master 0, GPIO 101/102, 7-bit 0x2d, MCLK0 GPIO 94, reset 73, VANA 39, VIO 37, CSIPHY 0
//   wide   cam-sensor2   CCI1 (0xac50000) master 0, GPIO 105/106, 7-bit 0x10, MCLK1 GPIO 95, reset 78, VANA 42, VIO 37, CSIPHY 2
// All module rails are GPIO-switched load switches (regulator rgltr-* = 0); the only "regulator" is titan_top_gdsc.
//
// Sequence (each step's number is the stage in t970_camera[0]; negative = failed there):
//   1 camcc + gcc mapped; GCC_CAMERA_AHB (0xb008, normally left on by the bootloader), HF/SF AXI votes (0xb02c/0xb030)
//   2 TITAN_TOP_GDSC (camcc 0xc144) on, POLL_CFG_GDSCR power-up complete
//   3 roots on XO 19.2 MHz (no camcc PLL needed): slow AHB 0x7058, camnoc AXI 0xc0f8, CCI0/1 0xc0bc/0xc0d8, MCLKn 0x5000+0x1c*n
//   4 branches on, CLK_OFF clear: gdsc 0xc16c, core AHB 0xc150, CPAS AHB 0xc0f4, camnoc AXI 0xc114, CCI 0xc0d4/0xc0f0
//   5 CCI pins func 1 (cci_i2c), CCI reset, master timing (qcom-cci v1.5 standard mode at 19.2 MHz, ~100 kHz)
//   6 sensor power: VIO, VANA, 2 ms, MCLK pin func 1 + MCLK branch, 2 ms, reset high, 10 ms
//   7 write 16-bit register 0x0000, read 4 bytes (SMIA model ID 0x0000..0x0001, revision 0x0002..)
//   8 done (powered down again unless bit 8)
// Bits: 1 front, 2 rear, 4 ultra-wide (each probed in turn), 8 leave the last sensor powered and the clocks on,
// 16 stream test (stage C2a, sensor side only): after the ID, mode_select 0x0100 = 1 on the power-on defaults (no init list,
// no CSIPHY: the lanes run into an unpowered PHY for under a second), read frame_count 0x0005 at 100 ms and 400 ms, then
// 0x0100 = 0. A count that moves proves the sensor's PLL and timing generator run on the 19.2 MHz MCLK.
// 32 receive test (stage C3a, front sensor only): read the sensor's SMIA output setup, camcc PLL0 on (1200 MHz Lucid; even 600,
// odd 400), CSIPHY 3 (D-PHY, lanes from 0x0114, settle from 0x0820), CSID-lite0 CSI2 RX on that PHY with long and short packet
// capture, stream 300 ms on the defaults, then read the CSID's IRQ status, packet/ECC/CRC counters and captured headers. No IFE,
// no DMA, no SMMU. Overrides: t970-camera-lanes=<1..4>, t970-camera-settle=<count>, t970-camera-assign=<hex DL0..3 nibbles>.
// 64 frame capture (stage C4, front sensor only, includes the C3a receive setup): CSID-lite0 RDI0 (payload only, DT from 0x0112;
// t970-camera-lines=<n> crops to n lines around the centre, default the whole frame), IFE-lite0 bus write master 0 in MIPI raw
// mode into pil_cdsp_region (0x8e300000, 26 MiB, inside UEFI's "PIL Reserved", no CDSP is started here; 16 MiB mapped, so even
// an uncropped 3280x2464 RAW10 frame of 10.1 MB and any overrun stay inside the carve-out; t970-camera-pa moves it), routed through the apps
// SMMU like T970Audio.cpp routes 0x1801: four SMRs as the FDT's msm_cam_smmu_ife has them (0x800/0x801/0x840/0x841, mask 0x400;
// a pair whose masked SMR is refused is retried as the exact SID) to a bypass bank (a bypass-emulation bank already in use is
// shared, else the first free one from NUMS2CB up), restored afterwards. The buffer is filled with 0xa5 first; after the first write-master done the frame is measured (coverage,
// mean/min/max of the 8 MSBs) and a 64x48 single-Bayer-site thumbnail goes to t970_camera_thumb, which BootLog prints as 48 rows
// "[t970 camera thumb NN] <128 hex>" (tools/camera-thumb.py turns them into a PGM). The full frame stays in the carve-out.
// 128 auto exposure (stage C4b, with 64): after the first frame, up to 8 more frames with coarse integration 0x0202 and analogue
// gain 0x0204 scaled towards a mean of t970-camera-target=<MSB 24..230, default 72>; the thumbnail is then the last frame.
// t970-camera-exp=<lines> / t970-camera-gain=<code, 0x20 = 1x> set fixed values before streaming (with or without 128).
// Status in t970_camera_ae[] (see autoExposure), BootLog row "[t970 camera c4b]".
//
// Status t970_camera[]: [0] stage, [1] error detail (CCI IRQ status, or the register that did not come up), [2] GDSCR read back,
// [3] CCI HW version, [4..6] per sensor (front, rear, wide): ID word (bytes 0x0000..0x0003 big-endian), 0 = not tried,
// -1 NACK, -2 timeout, -3 other CCI error; [7] last CCI IRQ status, [8] ms spent; [9..11] per sensor stream test:
// first count | second count << 8 | mode_select read back << 16 | (CCI rc & 0xff) << 24, 0 = not run.
// C3a ([12..25], front): [12] csi_data_format 0x0112 | csi_lane_mode 0x0114 << 16, [13] x_output 0x034c | y_output 0x034e << 16,
// [14] line_length_pck 0x0342 | frame_length_lines 0x0340 << 16, [15] requested link rate 0x0820 (Mbps, 16.16 >> 16),
// [16] C3 stage (1 sensor setup read, 2 PLL0 locked, 3 roots/branches, 4 CSIPHY, 5 CSID, 6 streamed, 7 done; negative = failed),
// [17] PLL0 MODE read back, [18] CSID CSI2 RX IRQ status (OR of everything seen), [19] total packets, [20] ECC stats,
// [21] total CRC errors, [22] captured long packet header (WC [15:0], DT [21:16], VC [26:22]), [23] captured short packet,
// [24] CSID HW version, [25] settle | lanes << 8 | lane assign << 16; [26] refgen BIAS_EN | BG_CTRL << 8,
// [27] CSIPHY common status 0..3 bytes after the stream.
// C4 ([28..39], front): [28] C4 stage (1 buffer mapped, 2 SMMU routed, 3 IFE reset, 4 WM + RDI armed, 5 frame done, 6 measured,
// 7 done; negative = failed), [29] SMMU route: kind (2 shared bank, 3 new bank) | routed pairs << 4 (bit n kSids[n], bit n + 4
// only as the exact SID) | first slot << 12 | bank << 20 (combo9 and earlier: slot << 8, bank << 16), or negative (-4 an SMR overlaps, det = it; -9 none read back, det = the
// first refused SMR as read back; with any pair routed, det also holds the first refused readback),
// [30] IFE-lite HW version, [31] IFE top IRQ status seen | bus IRQ status 0 seen << 16 (bit 16+4 = WM0 comp done),
// [32] CSID RDI0 IRQ status seen, [33] CSID RDI0 format measure (height << 16 | width), [34] bytes of the expected window
// written (sampled every 64th byte, x64), [35] mean | min << 8 | max << 16 of the sampled MSBs, [36] ms from stream on to done,
// [37] buffer PA >> 12, [38] stride, [39] SMMU fault status of the bank after the frame.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

extern "C" {volatile int32_t t970_camera[40]; volatile uint8_t t970_camera_thumb[64 * 48];}
#include "T970CameraAbi.h"
// C5 status: [0] state (1 streaming, 2 done after t970-camera-seconds, 3 stopped by the client, -1 too many frame timeouts, -2 no room
// for 2 buffers) | sessions << 8, [1] frames, [2] frame timeouts, [3] ms streamed, [4] frames per 10 s, [5] coarse | gain << 16 at
// the end, [6] last AE mean, [7] buffers | buffer KiB << 8, [8] MSB means of the four Bayer sites of the last frame, (y0 x0) |
// (y0 x1) << 8 | (y1 x0) << 16 | (y1 x1) << 24, [9] service (T970CameraService.cpp; 1 published), [10] user clients opens |
// starts << 8 | maps << 16 | closes << 24, [11] AE changes while streaming, [12..15] user-space status words (T970CameraReport;
// tools/t970camerad: [12]/[13] the stage bits of its two identities, [14] frames sent to CoreMediaIO, [15] last error, [16] its
// AVFoundation probe's stage bits, [17] devices listed (total | built-in << 8 | external << 16 | ours << 24), [18] frames the
// probe's AVCaptureVideoDataOutput received, [19] reserved).
extern "C" {volatile int32_t t970_camera_c5[20];}
extern kmod_info_t g_kernel_kmod_info;
extern "C" void t970_camera_service_static_init(void) __asm__("__GLOBAL__sub_I_T970CameraService.cpp");
extern "C" void t970_camera_service_start(void);

namespace {
constexpr uint64_t kGcc = 0x00100000, kCamcc = 0x0ad00000, kTlmm = 0x0f000000, kSouth = 0x500000, kNorth = 0x900000;
constexpr uint64_t kCci[2] = {0x0ac4f000, 0x0ac50000};

struct Sensor {const char *name; unsigned cci, master, addr, mclk, mclkGpio, reset, vana, sda, scl;};
// MCLK index n: root cmd 0x5000 + 0x1c*n, branch 0x5018 + 0x1c*n (camcc-sm8250.c).
constexpr Sensor kSensors[3] = {
    {"front", 0, 1, 0x2d, 2, 96, 30, 38, 103, 104},
    {"rear",  0, 0, 0x2d, 0, 94, 73, 39, 101, 102},
    {"wide",  1, 0, 0x10, 1, 95, 78, 42, 105, 106},
};
constexpr unsigned kVio = 37;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void unmap(Window &w) {if (w.map) w.map->release(); w = {};}
void barrier() {__asm__ volatile("dsb sy" ::: "memory");}
uint64_t nowMs() {uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}

volatile int32_t *const st = t970_camera;
bool fail(int s, int32_t detail) {st[0] = -s; st[1] = detail; return false;}

Window gGccW, gCamccW, gCciW[2];
volatile uint32_t *gGcc, *gCamcc;
uint32_t rd(volatile uint32_t *b, uint32_t off) {return b[off / 4];}
void wr(volatile uint32_t *b, uint32_t off, uint32_t v) {b[off / 4] = v; barrier();}

// ---- TLMM pins ----
Window gPin[16];
unsigned gPinGpio[16], gPins;
// Tiles from pinctrl-sm8250.c: 30, 78, 94..108 north; 37, 38, 39, 42, 73 south.
volatile uint32_t *pin(unsigned gpio) {
    for (unsigned i = 0; i < gPins; ++i) if (gPinGpio[i] == gpio) return gPin[i].va;
    if (gPins == 16) return nullptr;
    const bool north = gpio == 30 || gpio == 78 || (gpio >= 94 && gpio <= 108);
    if (!mapWindow(gPin[gPins], kTlmm + (north ? kNorth : kSouth) + gpio * 0x1000ull, 0x1000)) return nullptr;
    gPinGpio[gPins] = gpio;
    return gPin[gPins++].va;
}
// TLMM CFG: [1:0] pull (0 none, 1 down, 3 up), [5:2] function, [8:6] drive (mA/2 - 1), [9] output enable. IN_OUT [1] output value.
bool output(unsigned gpio, bool level) {
    volatile uint32_t *p = pin(gpio);
    if (!p) return false;
    p[1] = level ? 2u : 0u; barrier();
    p[0] = (0u << 2) | (0u << 6) | (1u << 9); barrier();
    return true;
}
bool function(unsigned gpio, unsigned func, unsigned mA, unsigned pull) {
    volatile uint32_t *p = pin(gpio);
    if (!p) return false;
    p[0] = pull | (func << 2) | ((mA / 2 - 1) << 6); barrier();
    return true;
}
void parkLow(unsigned gpio) {output(gpio, false);}   // the FDT's suspend state for every camera pin: GPIO, output low

// ---- clocks ----
bool branchOn(volatile uint32_t *b, uint32_t cbcr, int stage) {
    wr(b, cbcr, rd(b, cbcr) | 1u);
    for (unsigned i = 0; i < 200; ++i) {if (!(rd(b, cbcr) & (1u << 31))) return true; IODelay(5);}
    return fail(stage, int32_t(cbcr));
}
void branchOff(volatile uint32_t *b, uint32_t cbcr) {wr(b, cbcr, rd(b, cbcr) & ~1u);}
// RCG2: CMD [0] update, CFG = cmd + 4: [4:0] divider (2 * div - 1; 0 = bypass), [10:8] source (0 = XO), [13:12] MND mode.
bool rootSet(uint32_t cmd, uint32_t source, uint32_t hid, int stage) {
    wr(gCamcc, cmd + 4, source << 8 | hid);
    wr(gCamcc, cmd, rd(gCamcc, cmd) | 1u);
    for (unsigned i = 0; i < 200; ++i) {if (!(rd(gCamcc, cmd) & 1u)) return true; IODelay(5);}
    return stage > 0 ? fail(stage, int32_t(cmd)) : false;   // stage 0: teardown, report nothing
}
bool rootXo(uint32_t cmd, int stage) {return rootSet(cmd, 0, 0, stage);}

bool clocksOn() {
    st[0] = 1;
    if (!mapWindow(gGccW, kGcc, 0x10000) || !mapWindow(gCamccW, kCamcc, 0x10000)) return fail(1, 0);
    gGcc = gGccW.va; gCamcc = gCamccW.va;
    // GCC_CAMERA_AHB is the camcc register clock: branch with CLK_OFF in bit 31. HF/SF AXI are voted branches (no reliable halt).
    if (!branchOn(gGcc, 0xb008, 1)) return false;
    wr(gGcc, 0xb02c, rd(gGcc, 0xb02c) | 1u);
    wr(gGcc, 0xb030, rd(gGcc, 0xb030) | 1u);

    st[0] = 2;
    // GDSCR [0] SW_COLLAPSE, [31] PWR_ON; CFG_GDSCR (+4) [16] power-up complete.
    uint32_t g = rd(gCamcc, 0xc144);
    if (g & 1u) wr(gCamcc, 0xc144, g & ~1u);
    bool on = false;
    for (unsigned i = 0; i < 500 && !on; ++i) {on = (rd(gCamcc, 0xc144) & (1u << 31)) || (rd(gCamcc, 0xc148) & (1u << 16)); if (!on) IODelay(10);}
    st[2] = int32_t(rd(gCamcc, 0xc144));
    if (!on) return fail(2, int32_t(rd(gCamcc, 0xc148)));

    st[0] = 3;
    if (!rootXo(0x7058, 3) || !rootXo(0xc0f8, 3) || !rootXo(0xc0bc, 3) || !rootXo(0xc0d8, 3)) return false;
    for (unsigned n = 0; n < 3; ++n) if (!rootXo(0x5000 + 0x1c * n, 3)) return false;

    st[0] = 4;
    return branchOn(gCamcc, 0xc16c, 4) && branchOn(gCamcc, 0xc150, 4) && branchOn(gCamcc, 0xc0f4, 4) &&
           branchOn(gCamcc, 0xc114, 4) && branchOn(gCamcc, 0xc0d4, 4) && branchOn(gCamcc, 0xc0f0, 4);
}
void clocksOff() {
    if (!gCamcc) return;
    for (unsigned n = 0; n < 3; ++n) branchOff(gCamcc, 0x5018 + 0x1c * n);
    branchOff(gCamcc, 0xc0d4); branchOff(gCamcc, 0xc0f0);
    // The GDSC, AHB and AXI clocks stay on: cheap, and nothing else in iPadOS uses the camera block.
}

// ---- CCI (i2c-qcom-cci.c register map) ----
constexpr uint32_t kReset = 0x004, kQueueStart = 0x008, kHalt = 0x034;
constexpr uint32_t kIrqGlobalClear = 0xc00, kIrqMask = 0xc04, kIrqClear = 0xc08, kIrqStatus = 0xc0c;
constexpr uint32_t sclCtl(unsigned m) {return 0x100 + 0x100 * m;}
constexpr uint32_t readData(unsigned m) {return 0x118 + 0x100 * m;}
constexpr uint32_t readLevel(unsigned m) {return 0x11c + 0x100 * m;}
constexpr uint32_t execCount(unsigned m, unsigned q) {return 0x300 + 0x200 * m + 0x100 * q;}
constexpr uint32_t curCount(unsigned m, unsigned q) {return 0x304 + 0x200 * m + 0x100 * q;}
constexpr uint32_t loadData(unsigned m, unsigned q) {return 0x310 + 0x200 * m + 0x100 * q;}
constexpr uint32_t kSetParam = 1, kReport = 8, kWrite = 9, kRead = 10, kReportIrq = 1u << 8;
constexpr uint32_t kRstDone = 1u << 24, kErrM0 = 0x18000ee6, kErrM1 = 0x60ee6000;
constexpr uint32_t doneBits(unsigned m) {return m ? (1u << 12 | 1u << 16 | 1u << 20) : (1u << 0 | 1u << 4 | 1u << 8);}
constexpr uint32_t nackBits(unsigned m) {return m ? (1u << 29 | 1u << 30) : (1u << 27 | 1u << 28);}

// Polled: the CCI interrupt (SPI 460 / 271) is not routed to us. Clears what it saw.
uint32_t waitIrq(volatile uint32_t *c, uint32_t want, unsigned us) {
    uint32_t seen = 0;
    for (unsigned i = 0; i < us / 10; ++i) {
        const uint32_t s = rd(c, kIrqStatus);
        if (s) {wr(c, kIrqClear, s); wr(c, kIrqGlobalClear, 1); seen |= s;}
        if (seen & want) break;
        IODelay(10);
    }
    st[7] = int32_t(seen);
    return seen;
}
bool cciInit(volatile uint32_t *c, unsigned master) {
    // cci_init's mask: done/report per master and queue, reset and halt acks, both masters' errors.
    wr(c, kIrqMask, 0x07111111u | kErrM0 | kErrM1);
    wr(c, kReset, 0x0f73f3f7);
    if (!(waitIrq(c, kRstDone, 100000) & kRstDone)) return fail(5, int32_t(st[7]));
    // qcom-cci v1.5 standard mode (19.2 MHz CCI clock): thigh 78, tlow 114, tsu_sto/sta 28, thd_dat 10, thd_sta 77, tbuf 118,
    // trdhld 6, tsp 1.
    wr(c, sclCtl(master) + 0x0, 78u << 16 | 114u);
    wr(c, sclCtl(master) + 0x4, 28u << 16 | 28u);
    wr(c, sclCtl(master) + 0x8, 10u << 16 | 77u);
    wr(c, sclCtl(master) + 0xc, 118u);
    wr(c, sclCtl(master) + 0x10, 0u << 8 | 6u << 4 | 1u);
    return true;
}
// Runs what is loaded on (master, queue). 0 ok, -1 NACK, -2 timeout, -3 other error.
int runQueue(volatile uint32_t *c, unsigned m, unsigned q) {
    wr(c, execCount(m, q), rd(c, curCount(m, q)));
    wr(c, kQueueStart, 1u << (m * 2 + q));
    const uint32_t err = m ? kErrM1 : kErrM0;
    const uint32_t s = waitIrq(c, doneBits(m) | err, 100000);
    if (s & err) {
        wr(c, kHalt, 1u << m);
        IODelay(1000);
        wr(c, kReset, m ? 0x0003f001 : 0x000003f1);
        (void)waitIrq(c, kRstDone, 20000);
        return (s & nackBits(m)) ? -1 : -3;
    }
    return (s & doneBits(m)) ? 0 : -2;
}
// 16-bit register address, then a read of n (<= 11) bytes.
int readRegs(volatile uint32_t *c, unsigned m, unsigned addr, uint16_t reg, uint8_t *out, unsigned n) {
    wr(c, loadData(m, 0), kSetParam | (addr & 0x7f) << 4);
    wr(c, loadData(m, 0), kWrite | 2u << 4 | uint32_t(reg >> 8) << 8 | uint32_t(reg & 0xff) << 16);
    wr(c, loadData(m, 0), kReport | kReportIrq);
    int rc = runQueue(c, m, 0);
    if (rc) return rc;
    wr(c, loadData(m, 1), kSetParam | (addr & 0x7f) << 4);
    wr(c, loadData(m, 1), kRead | n << 4);
    rc = runQueue(c, m, 1);
    if (rc) return rc;
    unsigned words = rd(c, readLevel(m)), got = 0;
    if (words != n / 4 + 1) {st[1] = int32_t(words); return -3;}
    bool first = true;   // the low byte of the first word echoes the slave address
    while (words--) {
        const uint32_t v = rd(c, readData(m));
        for (unsigned i = 0; i < 4 && got < n; ++i) {
            if (first) {first = false; continue;}
            out[got++] = uint8_t(v >> (8 * i));
        }
    }
    return 0;
}

// One 8-bit register at a 16-bit address.
int writeReg(volatile uint32_t *c, unsigned m, unsigned addr, uint16_t reg, uint8_t value) {
    wr(c, loadData(m, 0), kSetParam | (addr & 0x7f) << 4);
    wr(c, loadData(m, 0), kWrite | 3u << 4 | uint32_t(reg >> 8) << 8 | uint32_t(reg & 0xff) << 16 | uint32_t(value) << 24);
    wr(c, loadData(m, 0), kReport | kReportIrq);
    return runQueue(c, m, 0);
}

// Stage C2a: stream on the defaults and watch the frame counter (SMIA frame_count 0x0005, mode_select 0x0100).
int32_t streamTest(volatile uint32_t *c, const Sensor &s) {
    uint8_t a = 0, b = 0, mode = 0;
    int rc = writeReg(c, s.master, s.addr, 0x0100, 1);
    if (!rc) {IOSleep(100); rc = readRegs(c, s.master, s.addr, 0x0005, &a, 1);}
    if (!rc) {IOSleep(300); rc = readRegs(c, s.master, s.addr, 0x0005, &b, 1);}
    if (!rc) rc = readRegs(c, s.master, s.addr, 0x0100, &mode, 1);
    const int off = writeReg(c, s.master, s.addr, 0x0100, 0);   // always
    IOSleep(50);
    if (!rc) rc = off;
    IOLog("t970-camera: %s stream count %u -> %u mode %u rc %d\n", s.name, a, b, mode, rc);
    return int32_t(uint32_t(a) | uint32_t(b) << 8 | uint32_t(mode) << 16 | (uint32_t(rc) & 0xffu) << 24);
}

void powerDown(const Sensor &s) {
    parkLow(s.reset);
    branchOff(gCamcc, 0x5018 + 0x1c * s.mclk);
    parkLow(s.mclkGpio);
    parkLow(s.vana);
    parkLow(kVio);
}


// ---- stage C3a: CSIPHY 3 + CSID-lite0 receive ----
constexpr uint64_t kCsiphy3 = 0x0ac70000, kCsidLite0Page = 0x0acd9000, kRefgen = 0x088e7000;   // CSID-lite0 at page + 0x200
Window gPhyW, gCsidW, gRefgenW;
volatile int32_t &c3st = t970_camera[16];
bool c3fail(int s, int32_t detail) {c3st = -s; st[1] = detail; return false;}

int readU16(volatile uint32_t *c, const Sensor &s, uint16_t reg, uint32_t &out) {
    uint8_t b[2] = {};
    const int rc = readRegs(c, s.master, s.addr, reg, b, 2);
    out = uint32_t(b[0]) << 8 | b[1];
    return rc;
}

// camcc PLL0, Lucid (clk-alpha-pll.c trion/lucid register layout; camcc-sm8250.c cam_cc_pll0_config): L 0x3e + alpha 0x8000 =
// 62.5 x 19.2 = 1200 MHz, user_ctl 0x3100 = even /2 (600), odd /3 (400). Skipped when the PLL already runs.
bool pll0On() {
    const uint32_t mode = rd(gCamcc, 0x0), opmode = rd(gCamcc, 0x38);
    if (!((opmode & 1u) && (mode & 1u))) {
        wr(gCamcc, 0x04, 0x3e);
        wr(gCamcc, 0x08, 0x44);          // CAL_L
        wr(gCamcc, 0x40, 0x8000);        // ALPHA
        wr(gCamcc, 0x18, 0x20485699);    // CONFIG_CTL
        wr(gCamcc, 0x1c, 0x00002261);
        wr(gCamcc, 0x20, 0x329a699c);
        wr(gCamcc, 0x0c, 0x00003100);    // USER_CTL
        wr(gCamcc, 0x10, 0x00000805);
        wr(gCamcc, 0x00, rd(gCamcc, 0x0) | (1u << 23));     // UPDATE_BYPASS
        wr(gCamcc, 0x00, rd(gCamcc, 0x0) & ~1u);            // outputs off
        wr(gCamcc, 0x38, 0);                                // STANDBY
        wr(gCamcc, 0x00, rd(gCamcc, 0x0) | (1u << 2));      // RESET_N
        wr(gCamcc, 0x38, 1);                                // RUN
        bool locked = false;
        for (unsigned i = 0; i < 2000 && !locked; ++i) {locked = rd(gCamcc, 0x0) & (1u << 31); if (!locked) IODelay(5);}
        t970_camera[17] = int32_t(rd(gCamcc, 0x0));
        if (!locked) return c3fail(2, int32_t(rd(gCamcc, 0x0)));
        wr(gCamcc, 0x0c, rd(gCamcc, 0x0c) | 7u);           // main, even, odd outputs
        wr(gCamcc, 0x00, rd(gCamcc, 0x0) | 1u);             // OUTCTRL
    }
    t970_camera[17] = int32_t(rd(gCamcc, 0x0));
    return true;
}
void pll0Off() {
    wr(gCamcc, 0x00, rd(gCamcc, 0x0) & ~1u);
    wr(gCamcc, 0x38, 0);
}

// Roots (parent map 0: 2 = PLL0 even 600 MHz, 3 = PLL0 odd 400 MHz): cphy_rx 0xa068 400, csi3phytimer 0x6060 300 (even / 2),
// ife_lite 0xc000 400, ife_lite_csid 0xc01c 400. Branches: csiphy3 0x607c, csi3phytimer 0x6078, ife_lite_ahb 0xc040,
// ife_lite 0xc018, ife_lite_csid 0xc034, ife_lite_cphy_rx 0xc03c.
constexpr uint32_t kC3Roots[4] = {0xa068, 0x6060, 0xc000, 0xc01c};
constexpr uint32_t kC3Branches[6] = {0x607c, 0x6078, 0xc040, 0xc018, 0xc034, 0xc03c};
bool c3ClocksOn() {
    if (!rootSet(0xa068, 3, 1, 3) || !rootSet(0x6060, 2, 3, 3) || !rootSet(0xc000, 3, 1, 3) || !rootSet(0xc01c, 3, 1, 3)) {
        c3st = -3; return false;
    }
    for (uint32_t b : kC3Branches) if (!branchOn(gCamcc, b, 3)) {c3st = -3; return false;}
    return true;
}
void c3ClocksOff() {
    for (uint32_t b : kC3Branches) branchOff(gCamcc, b);
    for (uint32_t r : kC3Roots) (void)rootXo(r, 0);
}

// CSIPHY 3ph-1-0 in D-PHY mode, the SM8250 gen2 lane settings (Linux camss-csiphy-3ph-1-0.c lane_regs_sm8250): data lanes at
// 0x000/0x200/0x400/0x600 with their companion block at 0x900/0xa00/0xb00/0xc00, the clock lane at 0x700 with 0xc80.
void phyLane(volatile uint32_t *p, uint32_t b, uint32_t aux, bool clock, uint32_t settle) {
    wr(p, b + 0x30, 0x00);
    wr(p, aux + 0x0, 0x05); wr(p, aux + 0x8, 0x10); wr(p, aux + 0x4, 0x00); wr(p, aux + 0x4, 0x07);
    wr(p, b + 0x04, 0x0c); wr(p, b + 0x2c, 0x01); wr(p, b + 0x34, 0x07); wr(p, b + 0x10, 0x02);
    wr(p, b + 0x1c, 0x08); wr(p, b + 0x3c, 0xb8); wr(p, b + 0x08, settle & 0xff);
    if (clock) {
        wr(p, b + 0x00, 0x80); wr(p, b + 0x0c, 0xa5); wr(p, b + 0x38, 0x1f); wr(p, b + 0x14, 0x60); wr(p, b + 0x28, 0x04);
    } else {
        wr(p, b + 0x00, 0x8d); wr(p, b + 0x38, 0xfe); wr(p, b + 0x14, 0x60);
    }
    wr(p, 0x800, 0x02); wr(p, 0x884, 0x01);
}
void phyOn(volatile uint32_t *p, unsigned lanes, uint32_t settle) {
    constexpr uint32_t kBase[4] = {0x000, 0x200, 0x400, 0x600}, kAux[4] = {0x900, 0xa00, 0xb00, 0xc00};
    wr(p, 0x800, 1); IOSleep(6); wr(p, 0x800, 0);                  // COMMON_CTRL0 software reset
    uint32_t enable = 0x80;                                         // CTRL5: clock lane + BIT(2 * pos) per data lane
    for (unsigned i = 0; i < lanes; ++i) enable |= 1u << (2 * i);
    wr(p, 0x814, enable);
    wr(p, 0x818, 1);                                                // CTRL6 COMMON_PWRDN_B
    wr(p, 0x81c, 2);                                                // CTRL7
    wr(p, 0x800, 0);
    for (unsigned i = 0; i < 4; ++i) phyLane(p, kBase[i], kAux[i], false, settle);
    phyLane(p, 0x700, 0xc80, true, settle);
    for (unsigned n = 11; n < 22; ++n) wr(p, 0x800 + 4 * n, 0);     // PHY IRQ masks off
}
void phyOff(volatile uint32_t *p) {wr(p, 0x814, 0); wr(p, 0x818, 0);}

// CSID-lite 480 (downstream cam_ife_csid_lite480.h offsets): RST_STROBES 0x10, TOP IRQ 0x70.., IRQ_CMD 0x80, CSI2 RX IRQ 0x20..,
// RX_CFG0 0x100 (lanes - 1, DL input nibbles << 4, PHY << 20, D-PHY = 0 << 24), RX_CFG1 0x104 (ECC correction, MISR),
// capture ctrl 0x108 (long en, short en, long DT << 4), captured short 0x128, long 0x130, total packets 0x160, ECC 0x164, CRC 0x168.
void csidIrqClear(volatile uint32_t *d) {wr(d, 0x28, 0x0fffffff); wr(d, 0x78, 1); wr(d, 0x80, 1);}
bool csidOn(volatile uint32_t *d, unsigned lanes, uint32_t assign, uint32_t dt) {
    t970_camera[24] = int32_t(rd(d, 0x0));
    wr(d, 0x78, 1); wr(d, 0x80, 1);
    wr(d, 0x74, 1);                                                 // TOP IRQ mask: reset done
    wr(d, 0x10, 0x1e);                                              // hardware reset strobes
    bool done = false;
    for (unsigned i = 0; i < 1000 && !done; ++i) {done = rd(d, 0x70) & 1u; if (!done) IODelay(10);}
    wr(d, 0x78, 1); wr(d, 0x80, 1);
    if (!done) return c3fail(5, int32_t(rd(d, 0x70)));
    wr(d, 0x24, 0x0fffffff);                                        // every CSI2 RX source, so the status bits latch
    wr(d, 0x100, (lanes - 1) | (assign & 0xffff) << 4 | 3u << 20);  // PHY 3, D-PHY
    wr(d, 0x104, 1u | 1u << 6);
    wr(d, 0x108, 1u | 1u << 1 | (dt & 0x3f) << 4);                   // capture long (DT dt, VC 0) and short (VC 0) packets
    csidIrqClear(d);
    return true;
}
void csidOff(volatile uint32_t *d) {wr(d, 0x108, 0); wr(d, 0x100, 0); wr(d, 0x24, 0);}


// ---- stage C4: one frame through CSID-lite0 RDI0 and IFE-lite0 write master 0 ----
constexpr uint64_t kSmmu = 0x15000000, kSmmuBytes = 0x100000, kDefaultFramePa = 0x8e300000, kFrameBytes = 0x1a00000;
volatile int32_t &c4st = t970_camera[28];
bool c4fail(int s, int32_t detail) {c4st = -s; st[1] = detail; return false;}

Window gSmmuW, gFrameW;
// The FDT's msm_cam_smmu_ife streams as (SID, mask) pairs: 0x800/0x801/0x840/0x841, each with mask 0x400 (covering 0xc00.. too).
// combo6: one SMR 0x800 mask 0x441 did not read back (route -9), so each pair gets its own SMR as the FDT has it; a pair whose masked
// SMR does not read back is retried as the exact SID. All point at one bypass bank.
constexpr uint32_t kSids[4] = {0x800, 0x801, 0x840, 0x841}, kSidMask = 0x400;
uint32_t gShift, gPages, gBank = 0xffff, gOldBank[3], gSlots[4] = {0xffff, 0xffff, 0xffff, 0xffff}, gOldS2cr[4], gSmrSet[4];
bool gNewBank;
uint32_t sr(uint64_t off) {return gSmmuW.va[off / 4];}
void sw(uint64_t off, uint32_t v) {gSmmuW.va[off / 4] = v; barrier();}
uint64_t cbOff(uint32_t i) {return (uint64_t(gPages) + i) << gShift;}
bool overlaps(uint32_t smr, uint32_t id, uint32_t mask) {
    return ((id ^ (smr & 0x7fff)) & ~(((smr >> 16) & 0x7fff) | mask) & 0x7fff) == 0;
}
// As T970Audio.cpp routeStream (S2CR BYPASS reads back as FAULT on this SMMU, hence the bypass-emulation bank). Returns
// kind (2 shared bank, 3 new bank) | routed pairs mask << 4 (bit n: kSids[n] routed; bit n + 4: only as the exact SID), or negative.
int routeStreams() {
    if (!gSmmuW.va && !mapWindow(gSmmuW, kSmmu, kSmmuBytes)) return -1;
    gBank = 0xffff; gNewBank = false;                          // a C5 session routes again
    const uint32_t cr0 = sr(0), id0 = sr(0x20), id1 = sr(0x24);
    if (cr0 & (1u << 3)) return -2;
    const uint32_t streams = id0 & 0xff, banks = id1 & 0xff, ns2 = (id1 >> 16) & 0xff;
    gShift = (id1 >> 31) ? 16 : 12; gPages = 2u << ((id1 >> 28) & 7);
    if (!streams || streams > 128 || !banks || banks > 128 || cbOff(banks) > kSmmuBytes) return -3;
    bool used[128] = {}, freeSlot[128] = {};
    uint32_t sharedBank = 0xffff, nFree = 0;
    for (uint32_t i = 0; i < streams; ++i) {
        const uint32_t smr = sr(0x800 + 4 * i), s2cr = sr(0xc00 + 4 * i);
        if (!(smr & (1u << 31))) {freeSlot[i] = true; ++nFree; continue;}
        const bool translate = ((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) < banks;
        if (translate) used[s2cr & 0xff] = true;
        for (uint32_t sid : kSids) if (overlaps(smr, sid, kSidMask)) {t970_camera[1] = int32_t(smr); return -4;}
        if (translate && sharedBank == 0xffff && sr((1ull << gShift) + 4 * (s2cr & 0xff)) == 0x10000 && !(sr(cbOff(s2cr & 0xff)) & 1))
            sharedBank = s2cr & 0xff;
    }
    if (nFree < 4) return -5;
    if (sharedBank != 0xffff) gBank = sharedBank;
    else {
        for (uint32_t i = ns2; i < banks && gBank == 0xffff; ++i)
            if (!used[i] && !(sr(cbOff(i)) & 1) && !(sr(cbOff(i) + 0x58) & 0xc00001feu)) gBank = i;
        if (gBank == 0xffff) return -6;
        gOldBank[0] = sr(cbOff(gBank)); gOldBank[1] = sr((1ull << gShift) + 0x800 + 4 * gBank);
        gOldBank[2] = sr((1ull << gShift) + 4 * gBank);
        gNewBank = true;
        sw(cbOff(gBank), 0);                                   // SCTLR: translation off
        sw((1ull << gShift) + 0x800 + 4 * gBank, 1);           // CBA2R: VA64
        sw((1ull << gShift) + 4 * gBank, 0x10000);             // CBAR: S1 (off), S2 bypass
        if (sr(cbOff(gBank)) != 0 || sr((1ull << gShift) + 4 * gBank) != 0x10000) return -7;
    }
    uint32_t routed = 0, next = 0;
    for (uint32_t n = 0; n < 4; ++n) {
        while (next < streams && !freeSlot[next]) ++next;
        if (next >= streams) break;
        const uint32_t slot = next++;
        gSlots[n] = slot;
        gOldS2cr[n] = sr(0xc00 + 4 * slot);
        sw(0xc00 + 4 * slot, gBank);                           // S2CR first
        if ((sr(0xc00 + 4 * slot) & 0x300ff) != gBank) {sw(0xc00 + 4 * slot, gOldS2cr[n]); gSlots[n] = 0xffff; continue;}
        const uint32_t masked = 0x80000000u | kSidMask << 16 | kSids[n], exact = 0x80000000u | kSids[n];
        sw(0x800 + 4 * slot, masked);
        uint32_t back = sr(0x800 + 4 * slot);
        if (back == masked) {gSmrSet[n] = masked; routed |= 1u << n; continue;}
        if (!t970_camera[1]) t970_camera[1] = int32_t(back ? back : 0x7fffffff);   // the first refused SMR, as read back
        sw(0x800 + 4 * slot, exact);
        back = sr(0x800 + 4 * slot);
        if (back == exact) {gSmrSet[n] = exact; routed |= 1u << n | 1u << (n + 4); continue;}
        sw(0x800 + 4 * slot, 0);                               // refused both ways: give the slot back
        sw(0xc00 + 4 * slot, gOldS2cr[n]);
        gSlots[n] = 0xffff;
    }
    if (!routed) return -9;
    return int((gNewBank ? 3u : 2u) | routed << 4);
}
void unrouteStreams() {
    if (!gSmmuW.va) return;
    if (gBank != 0xffff) t970_camera[39] = int32_t(sr(cbOff(gBank) + 0x58));
    for (uint32_t n = 0; n < 4; ++n) {
        if (gSlots[n] == 0xffff) continue;
        sw(0x800 + 4 * gSlots[n], 0);                          // SMR invalid first, then the S2CR as found
        sw(0xc00 + 4 * gSlots[n], gOldS2cr[n]);
        gSlots[n] = 0xffff;
    }
    if (gNewBank) {
        sw((1ull << gShift) + 4 * gBank, gOldBank[2]); sw((1ull << gShift) + 0x800 + 4 * gBank, gOldBank[1]); sw(cbOff(gBank), gOldBank[0]);
    }
    gNewBank = false;
}

// IFE-lite 480 (Linux camss-vfe-480.c, lite offsets): reset 0x0c (BIT 1 HW and registers), REG_UPDATE 0x20 (BIT n = RDI n),
// IRQ_CMD 0x24, IRQ mask/clear/status 0x28/0x34/0x40 (BIT 17 reset ack, BIT 4 bus top), bus at 0x1a00: CGC override +0x08,
// IRQ mask/clear/status +0x18/+0x20/+0x28, global clear +0x30, test bus +0xdc, WM n at +0x200 + 0x100 n: CFG +0, IMAGE_ADDR +4,
// FRAME_INCR +8, IMAGE_CFG_0 +0xc, IMAGE_CFG_2 +0x14, PACKER +0x18, IRQ subsample period/pattern +0x30/+0x34, framedrop
// period/pattern +0x38/+0x3c, burst limit +0x64. Lite: RDI n uses WM n and composite group n (bus IRQ BIT n RUP, BIT 4 + n done).
constexpr uint32_t kBus = 0x1a00, kWm0 = kBus + 0x200;
bool ifeReset(volatile uint32_t *f) {
    t970_camera[30] = int32_t(rd(f, 0x0));
    wr(f, 0x28, 1u << 17);
    wr(f, 0x0c, 1u << 1);
    bool done = false;
    for (unsigned i = 0; i < 1000 && !done; ++i) {done = rd(f, 0x40) & (1u << 17); if (!done) IODelay(10);}
    wr(f, 0x34, rd(f, 0x40)); wr(f, 0x24, 1);
    return done ? true : c4fail(3, int32_t(rd(f, 0x40)));
}
void wmArm(volatile uint32_t *f, uint32_t pa, uint32_t stride, uint32_t lines) {
    wr(f, kBus + 0x08, 0x3ffffff);
    wr(f, kBus + 0xdc, 0);
    wr(f, kWm0 + 0x08, stride * lines);
    wr(f, kWm0 + 0x64, 0xf);
    wr(f, kWm0 + 0x0c, 0xffff);
    wr(f, kWm0 + 0x14, stride);
    wr(f, kWm0 + 0x18, 0);
    wr(f, kWm0 + 0x38, 0); wr(f, kWm0 + 0x3c, 1);
    wr(f, kWm0 + 0x30, 0); wr(f, kWm0 + 0x34, 1);
    wr(f, kWm0 + 0x00, 1u | 1u << 16);                          // enable, MIPI raw
    wr(f, kWm0 + 0x04, pa);
    wr(f, 0x28, 1u << 17 | 1u << 4);                            // top IRQ: reset ack, bus
    wr(f, kBus + 0x18, 1u << 0 | 1u << 4);                      // bus IRQ: RDI0 reg update, comp group 0 done
    wr(f, 0x20, 1u << 0);                                       // reg update RDI0
}
void wmStop(volatile uint32_t *f) {wr(f, kWm0 + 0x00, 0); wr(f, kBus + 0x18, 0); wr(f, 0x28, 0);}

// CSID-lite RDI0 (Linux camss-csid-gen2.c __csid_configure_rdi_stream, lite offsets 0x200..): payload-only decode, DT, VC 0,
// byte counter, format measure and timestamp on, vertical crop (RPP_VCROP 0x220: first line | last line << 16, CFG0 BIT 6).
void rdiOn(volatile uint32_t *d, uint32_t dt, bool crop, uint32_t first, uint32_t last) {
    wr(d, 0x34, 0x7fff);                                        // RDI0 IRQ mask, so its status latches
    wr(d, 0x220, crop ? (first | last << 16) : 0);
    wr(d, 0x200, 1u << 0 | 1u << 1 | 1u << 2 | (crop ? 1u << 6 : 0u) | 0xfu << 12 | (dt & 0x3f) << 16);
    wr(d, 0x204, 2u);
    wr(d, 0x210, 1); wr(d, 0x20c, 0);
    wr(d, 0x218, 1); wr(d, 0x214, 0);
    wr(d, 0x228, 1); wr(d, 0x224, 0);
    wr(d, 0x230, 1); wr(d, 0x22c, 0);
    wr(d, 0x208, 0);
    wr(d, 0x208, 1);                                            // resume at frame boundary
    wr(d, 0x200, rd(d, 0x200) | 1u << 31);
}
void rdiOff(volatile uint32_t *d) {wr(d, 0x208, 0); wr(d, 0x200, 0); wr(d, 0x34, 0);}

// Packed RAW10: 4 pixels in 5 bytes, bytes 0..3 the 8 MSBs, byte 4 the 2-bit LSBs. MSB of pixel x in a line at (x / 4) * 5 + x % 4.
void measure(const volatile uint8_t *b, uint32_t stride, uint32_t lineBytes, uint32_t width, uint32_t lines) {
    uint64_t written = 0, sum = 0, n = 0;
    uint32_t lo = 255, hi = 0;
    for (uint32_t y = 0; y < lines; ++y)
        for (uint32_t i = (y * 7) % 64; i < lineBytes; i += 64) if (b[y * stride + i] != 0xa5) ++written;
    for (uint32_t y = 0; y < lines; y += 8)
        for (uint32_t xp = 0; xp < width; xp += 8) {
            const uint32_t v = b[y * stride + (xp / 4) * 5 + xp % 4];
            sum += v; ++n; if (v < lo) lo = v; if (v > hi) hi = v;
        }
    t970_camera[34] = int32_t(written * 64);
    t970_camera[35] = int32_t((n ? uint32_t(sum / n) : 0) | lo << 8 | hi << 16);
    // 64 x 48 thumbnail on one Bayer site (even x, even y), 8-bit MSBs.
    for (uint32_t ty = 0; ty < 48; ++ty) {
        const uint32_t y = (ty * lines / 48) & ~1u;
        for (uint32_t tx = 0; tx < 64; ++tx) {
            const uint32_t xp = (tx * width / 64) & ~1u;
            t970_camera_thumb[ty * 64 + tx] = b[y * stride + (xp / 4) * 5 + xp % 4];
        }
    }
}

unsigned gBits;

// ---- stage C4b: exposure and analogue gain over the CCI, simple auto exposure on the RDI frames ----
// SMIA++ registers: coarse_integration_time 0x0202 (lines), analogue_gain_code_global 0x0204 (Samsung: 0x20 = 1x), grouped
// parameter hold 0x0104, gain code min/max 0x0084/0x0086, coarse integration min 0x1004 / max margin 0x1006 (max = FLL - margin).
// Status t970_camera_ae[]: [0] AE iterations | state << 8 (1 converged, 2 too dark at max, 3 too bright at min, 4 frame timeout,
// 5 iteration limit, 6 manual values only, 7 CCI error) | manual << 12; [1] sensor default coarse | gain << 16; [2] final
// coarse | gain << 16 as read back; [3] gain code min | max << 16; [4] coarse min | margin << 16; [5]/[6] mean of each AE frame
// (8-bit MSBs, all Bayer sites), one byte per frame, 8 frames; [7] target | black << 8 | AE ms << 16.
extern "C" {volatile int32_t t970_camera_ae[8];}
constexpr uint32_t kBlack = 16;                                 // 10-bit pedestal 64 as an MSB byte (combo9 min 15)
struct Ae {uint32_t coarse, gain, gMin, gMax, cMin, cMax;};

int writeU16(volatile uint32_t *c, const Sensor &s, uint16_t reg, uint32_t v) {
    const int rc = writeReg(c, s.master, s.addr, reg, uint8_t(v >> 8));
    return rc ? rc : writeReg(c, s.master, s.addr, uint16_t(reg + 1), uint8_t(v));
}
int setExposure(volatile uint32_t *c, const Sensor &s, uint32_t coarse, uint32_t gain) {
    int rc = writeReg(c, s.master, s.addr, 0x0104, 1);
    if (!rc) rc = writeU16(c, s, 0x0202, coarse);
    if (!rc) rc = writeU16(c, s, 0x0204, gain);
    const int rc2 = writeReg(c, s.master, s.addr, 0x0104, 0);
    return rc ? rc : rc2;
}
// Sensor limits and defaults; bad or missing limits fall back to 1x..16x and margin 8.
bool aeLimits(volatile uint32_t *c, const Sensor &s, uint32_t fll, Ae &a) {
    uint32_t margin = 0;
    if (readU16(c, s, 0x0202, a.coarse) || readU16(c, s, 0x0204, a.gain) || readU16(c, s, 0x0084, a.gMin) ||
        readU16(c, s, 0x0086, a.gMax) || readU16(c, s, 0x1004, a.cMin) || readU16(c, s, 0x1006, margin)) return false;
    t970_camera_ae[1] = int32_t(a.coarse | a.gain << 16);
    t970_camera_ae[3] = int32_t(a.gMin | a.gMax << 16);
    t970_camera_ae[4] = int32_t(a.cMin | margin << 16);
    if (!a.gMin || a.gMax <= a.gMin || a.gMax > 0x400) {a.gMin = 0x20; a.gMax = 0x200;}
    if (!a.cMin || a.cMin > 64) a.cMin = 1;
    if (!margin || margin > 64) margin = 8;
    a.cMax = fll > margin + a.cMin ? fll - margin : a.cMin;
    return true;
}
// Mean of the MSBs over a sparse grid that walks all four Bayer sites (odd steps), and the share of clipped samples (x1000).
uint32_t frameMean(const volatile uint8_t *b, uint32_t stride, uint32_t width, uint32_t lines, uint32_t &clip) {
    uint64_t sum = 0, n = 0, hot = 0;
    for (uint32_t y = 0; y < lines; y += 15)
        for (uint32_t xp = 0; xp < width; xp += 9) {
            const uint32_t v = b[y * stride + (xp / 4) * 5 + xp % 4];
            sum += v; ++n; if (v >= 250) ++hot;
        }
    clip = n ? uint32_t(hot * 1000 / n) : 0;
    return n ? uint32_t(sum / n) : 0;
}
// One more frame from the running RDI0 into the same buffer: re-arm WM0, wait for its done (up to 400 ms).
bool nextFrame(volatile uint32_t *f, uint32_t pa, uint32_t stride, uint32_t lines) {
    wr(f, kBus + 0x20, 0xffffffffu); wr(f, kBus + 0x30, 1);
    wmArm(f, pa, stride, lines);
    for (unsigned i = 0; i < 80; ++i) {
        IOSleep(5);
        const uint32_t b = rd(f, kBus + 0x28);
        if (b) {wr(f, kBus + 0x20, b); wr(f, kBus + 0x30, 1);}
        if (b & (1u << 4)) {wmStop(f); return true;}
    }
    wmStop(f);
    return false;
}
// Total exposure E = coarse * gain / 0x20 (line units at 1x). Each step scales E by (target - black) / (mean - black), limited
// to 1/8..8 per step; exposure time first (up to FLL - margin, so the frame rate stays), then analogue gain.
// One AE decision on a measured mean: 0 within target +- 1/8, 1 new values written, 2 too dark at max, 3 too bright at min,
// 7 CCI error.
unsigned aeStep(volatile uint32_t *c, const Sensor &s, Ae &a, uint32_t mean, uint32_t clip, uint32_t target) {
    const uint32_t tol = target / 8 + 1;
    if (mean + tol >= target && mean <= target + tol) return 0;
    const uint64_t e = uint64_t(a.coarse) * a.gain;                   // x 0x20
    uint64_t want;
    if (mean >= 240 || clip > 200) want = e / 4;
    else {
        const uint32_t sig = mean > kBlack ? mean - kBlack : 1;
        want = e * (target - kBlack) / sig;
        if (want > e * 8) want = e * 8;
        if (want < e / 8) want = e / 8;
    }
    uint32_t coarse = uint32_t(want / a.gMin);
    if (coarse > a.cMax) coarse = a.cMax;
    if (coarse < a.cMin) coarse = a.cMin;
    uint64_t g = want / coarse;
    if (g > a.gMax) g = a.gMax;
    if (g < a.gMin) g = a.gMin;
    if (coarse == a.coarse && g == a.gain) return mean < target ? 2 : 3;
    a.coarse = coarse; a.gain = uint32_t(g);
    return setExposure(c, s, a.coarse, a.gain) ? 7 : 1;
}
void autoExposure(volatile uint32_t *c, const Sensor &s, volatile uint32_t *f, uint32_t pa, uint32_t stride, uint32_t width,
                  uint32_t lines, Ae &a, uint32_t target) {
    volatile int32_t *const ae = t970_camera_ae;
    const uint64_t t0 = nowMs();
    unsigned it = 0, state = 5;
    uint32_t hist[8] = {};
    for (; it < 8; ++it) {
        uint32_t clip = 0;
        const uint32_t mean = frameMean(reinterpret_cast<const volatile uint8_t *>(gFrameW.va), stride, width, lines, clip);
        hist[it] = mean > 255 ? 255 : mean;
        const unsigned r = aeStep(c, s, a, mean, clip, target);
        if (r != 1) {state = r == 0 ? 1 : r; break;}
        IOSleep(90);                                                  // two frames at ~24 fps: the new values are in effect
        if (!nextFrame(f, pa, stride, lines)) {state = 4; break;}
    }
    ae[5] = int32_t(hist[0] | hist[1] << 8 | hist[2] << 16 | hist[3] << 24);
    ae[6] = int32_t(hist[4] | hist[5] << 8 | hist[6] << 16 | hist[7] << 24);
    ae[0] = int32_t((ae[0] & 0xf000) | (it & 0xff) | state << 8);
    ae[7] = int32_t((target & 0xff) | kBlack << 8 | uint32_t(nowMs() - t0) << 16);
}

// ---- stage C5a: continuous capture into 2..4 buffers in the carve-out (t970-camera bit 256), C5b sessions for the user client ----
IOBufferMemoryDescriptor *gCtlMem;
T970CameraControl *gCtl;
uint32_t gRun, gBusy, gManualDirty, gManExp, gManGain, gSessions, gFramePa = uint32_t(kDefaultFramePa);
bool gSession;

// Next buffer for the write master: not the newest frame and not the one the client reads; the client's flag and ours are
// checked Dekker-style (each side writes its own, barrier, reads the other's), so a buffer being read is never re-armed.
uint32_t pickBuffer(uint32_t n, uint32_t latest) {
    T970CameraControl *k = gCtl;
    uint32_t w = latest;
    for (unsigned round = 0; round < 4; ++round) {
        const uint32_t reading = k ? k->reading : T970_CAMERA_NONE;
        w = T970_CAMERA_NONE;
        for (uint32_t i = 1; i <= n && w == T970_CAMERA_NONE; ++i) {
            const uint32_t j = (latest + i) % n;
            if (j != latest && j != reading) w = j;
        }
        for (uint32_t j = 0; j < n && w == T970_CAMERA_NONE; ++j) if (j != reading) w = j;
        if (w == T970_CAMERA_NONE) w = (latest + 1) % n;
        if (!k) break;
        k->writing = w;
        barrier();
        if (k->reading != w) break;
    }
    return w;
}
// Returns the byte offset of the newest complete frame. Buffer 0 holds the C4/C4b frame on entry.
uint32_t streamLoop(volatile uint32_t *c, const Sensor &s, volatile uint32_t *f, uint32_t pa, uint32_t stride, uint32_t width,
                    uint32_t lines, uint32_t dt, Ae &a, uint32_t target, bool aeOk) {
    volatile int32_t *const v = t970_camera_c5;
    const uint32_t bytes = (stride * lines + 0xffffu) & ~0xffffu;
    uint32_t n = bytes ? uint32_t(kFrameBytes / bytes) : 0;
    if (n > T970_CAMERA_MAX_BUFFERS) n = T970_CAMERA_MAX_BUFFERS;
    v[7] = int32_t(n | (bytes >> 10) << 8);
    const uint32_t sessions = uint32_t(v[0]) & ~0xffu;
    if (n < 2) {v[0] = int32_t(sessions | 0xfe); return 0;}
    T970CameraControl *k = gCtl;
    uint32_t latest = 0, seq = 1, frames = 0, timeouts = 0, run = 0, aeChanges = 0;
    if (k) {
        k->width = width; k->height = lines; k->stride = stride; k->data_type = dt; k->buffers = n; k->buffer_bytes = bytes;
        for (uint32_t i = 0; i < T970_CAMERA_MAX_BUFFERS; ++i) k->buffer_offset[i] = i < n ? i * bytes : 0;
        k->frame_bytes = uint32_t(kFrameBytes); k->exposure = a.coarse; k->gain = a.gain; k->timeouts = 0;
        k->latest = 0; k->writing = T970_CAMERA_NONE; k->timestamp = mach_absolute_time();
        barrier();
        k->seq = seq; k->state = T970CameraStreaming;
        barrier();
    }
    unsigned secs = 5;
    if (!PE_parse_boot_argn("t970-camera-seconds", &secs, sizeof secs) || !secs || secs > 600) secs = 5;
    v[0] = int32_t(sessions | 1);
    const uint64_t t0 = nowMs();
    int state = 2;
    for (;;) {
        if (gSession ? !__atomic_load_n(&gRun, __ATOMIC_ACQUIRE) : nowMs() - t0 >= uint64_t(secs) * 1000) {state = gSession ? 3 : 2; break;}
        const uint32_t w = pickBuffer(n, latest);
        if (!nextFrame(f, pa + w * bytes, stride, lines)) {
            ++timeouts;
            if (k) {k->writing = T970_CAMERA_NONE; k->timeouts = timeouts;}
            if (++run >= 10) {state = -1; break;}
            continue;
        }
        run = 0; latest = w; ++frames; ++seq;
        if (k) {
            k->latest = w; k->timestamp = mach_absolute_time();
            barrier();
            k->seq = seq; k->writing = T970_CAMERA_NONE;
            barrier();
        }
        v[1] = int32_t(frames);
        if (__atomic_exchange_n(&gManualDirty, 0u, __ATOMIC_ACQ_REL)) {
            if (gManExp && a.gMin) {
                a.coarse = gManExp < a.cMin ? a.cMin : gManExp > a.cMax ? a.cMax : gManExp;
                a.gain = !gManGain ? a.gain : gManGain < a.gMin ? a.gMin : gManGain > a.gMax ? a.gMax : gManGain;
                (void)setExposure(c, s, a.coarse, a.gain);
            }
        } else if (aeOk && !gManExp && (frames & 7) == 0) {
            uint32_t clip = 0;
            const uint32_t mean = frameMean(reinterpret_cast<const volatile uint8_t *>(gFrameW.va) + latest * bytes, stride, width,
                                            lines, clip);
            v[6] = int32_t(mean);
            if (k) k->mean = mean;
            if (aeStep(c, s, a, mean, clip, target) == 1) ++aeChanges;
        }
        if (k) {k->exposure = a.coarse; k->gain = a.gain;}
    }
    const uint32_t ms = uint32_t(nowMs() - t0);
    if (k) k->writing = T970_CAMERA_NONE;
    v[0] = int32_t(sessions | (uint32_t(state) & 0xff));
    v[1] = int32_t(frames); v[2] = int32_t(timeouts); v[3] = int32_t(ms); v[4] = int32_t(ms ? uint64_t(frames) * 10000 / ms : 0);
    v[5] = int32_t(a.coarse | a.gain << 16); v[11] = int32_t(aeChanges);
    // Bayer site means of the newest frame (which colour sits where is not known yet; green pairs come out alike).
    const volatile uint8_t *b = reinterpret_cast<const volatile uint8_t *>(gFrameW.va) + latest * bytes;
    uint32_t site[4] = {};
    uint32_t cnt = 0;
    for (uint32_t y = 0; y + 1 < lines; y += 16)
        for (uint32_t xp = 0; xp + 1 < width; xp += 16) {
            for (uint32_t q = 0; q < 4; ++q) {
                const uint32_t yy = y + (q >> 1), xx = xp + (q & 1);
                site[q] += b[yy * stride + (xx / 4) * 5 + xx % 4];
            }
            ++cnt;
        }
    if (cnt) v[8] = int32_t(site[0] / cnt | (site[1] / cnt) << 8 | (site[2] / cnt) << 16 | (site[3] / cnt) << 24);
    return latest * bytes;
}

void capture(volatile uint32_t *c, const Sensor &s, bool frame) {
    volatile int32_t *const x = t970_camera;
    c3st = 1;
    // C5 option t970-camera-bin=1: SMIA 2x2 binning (0x0900 = 1, 0x0901 = 0x22) to 1640x1232 before the setup is read, so the
    // whole receive path follows. Untested on the S5K4HA; the stream loop then fits four buffers.
    unsigned bin = 0;
    if (frame && PE_parse_boot_argn("t970-camera-bin", &bin, sizeof bin) && bin) {
        (void)writeReg(c, s.master, s.addr, 0x0104, 1);
        (void)writeReg(c, s.master, s.addr, 0x0900, 1);
        (void)writeReg(c, s.master, s.addr, 0x0901, 0x22);
        (void)writeU16(c, s, 0x034c, 1640);
        (void)writeU16(c, s, 0x034e, 1232);
        (void)writeReg(c, s.master, s.addr, 0x0104, 0);
    }
    uint32_t fmt = 0, lanesReg = 0, xo = 0, yo = 0, llp = 0, fll = 0, rateHi = 0;
    uint8_t laneByte = 0;
    if (readU16(c, s, 0x0112, fmt) || readRegs(c, s.master, s.addr, 0x0114, &laneByte, 1) || readU16(c, s, 0x034c, xo) ||
        readU16(c, s, 0x034e, yo) || readU16(c, s, 0x0342, llp) || readU16(c, s, 0x0340, fll) || readU16(c, s, 0x0820, rateHi)) {
        c3fail(1, int32_t(st[7])); return;
    }
    lanesReg = laneByte;
    x[12] = int32_t(fmt | lanesReg << 16); x[13] = int32_t(xo | yo << 16); x[14] = int32_t(llp | fll << 16); x[15] = int32_t(rateHi);
    unsigned lanes = (lanesReg & 3) + 1, settleArg = 0, assign = 0x3210;
    if (PE_parse_boot_argn("t970-camera-lanes", &lanes, sizeof lanes) && (lanes < 1 || lanes > 4)) lanes = (lanesReg & 3) + 1;
    (void)PE_parse_boot_argn("t970-camera-assign", &assign, sizeof assign);
    // Settle count (csiphy_settle_cnt_calc): UI = bit period, T_hs_settle = 85 ns + 6 UI, over the 300 MHz timer, minus 6.
    const uint32_t mbps = rateHi ? rateHi : 1000;
    uint32_t settle = (85000 + 6 * (1000000 / mbps)) / 3333 - 6;
    if (PE_parse_boot_argn("t970-camera-settle", &settleArg, sizeof settleArg) && settleArg) settle = settleArg;
    x[25] = int32_t(settle | lanes << 8 | (assign & 0xffff) << 16);
    // DT from csi_data_format's output byte: 8 RAW8 0x2a, 10 RAW10 0x2b, 12 RAW12 0x2c (0x0112 = input << 8 | output).
    const uint32_t out = fmt & 0xff, dt = out == 8 ? 0x2a : out == 12 ? 0x2c : 0x2b;

    if (!gRefgenW.va && !mapWindow(gRefgenW, kRefgen, 0x1000)) {c3fail(4, -1); return;}
    x[26] = int32_t((rd(gRefgenW.va, 0x08) & 0xff) | (rd(gRefgenW.va, 0x14) & 0xff) << 8);   // shared with the DSI PHYs: read only

    if (!pll0On()) return;
    c3st = 2;
    if (!c3ClocksOn()) return;
    c3st = 3;
    if ((!gPhyW.va && !mapWindow(gPhyW, kCsiphy3, 0x2000)) || (!gCsidW.va && !mapWindow(gCsidW, kCsidLite0Page, 0x2000))) {
        c3fail(4, -2); c3ClocksOff(); return;
    }
    volatile uint32_t *p = gPhyW.va, *d = gCsidW.va + 0x200 / 4;
    phyOn(p, lanes, settle);
    c3st = 4;
    if (!csidOn(d, lanes, assign, dt)) {phyOff(p); c3ClocksOff(); return;}
    c3st = 5;

    // C4: buffer, SMMU route, IFE reset, write master and RDI0 armed before the sensor streams.
    volatile uint32_t *f = gCsidW.va;                           // IFE-lite0 at the page base, CSID-lite0 at + 0x200
    const uint32_t lineBytes = xo * (out == 8 ? 8 : out == 12 ? 12 : 10) / 8, stride = (lineBytes + 15) & ~15u;
    uint32_t lines = yo, cropLines = 0, framePa = uint32_t(kDefaultFramePa);
    (void)PE_parse_boot_argn("t970-camera-pa", &framePa, sizeof framePa);
    gFramePa = framePa;
    if (PE_parse_boot_argn("t970-camera-lines", &cropLines, sizeof cropLines) && cropLines && cropLines < yo) lines = cropLines;
    bool armed = false;
    uint32_t latestOff = 0;
    if (frame) {
        c4st = 0;
        x[37] = int32_t(framePa >> 12); x[38] = int32_t(stride);
        if (!lineBytes || uint64_t(stride) * lines > kFrameBytes || (framePa & 0xfff)) c4fail(1, int32_t(stride));
        else if (!gFrameW.va && !mapWindow(gFrameW, framePa, kFrameBytes)) c4fail(1, -1);
        else {
            for (uint32_t i = 0; i < 0x1000000 / 4; ++i) gFrameW.va[i] = 0xa5a5a5a5u;   // the first 16 MiB: buffer 0 and slack
            barrier();
            c4st = 1;
            const int route = routeStreams();
            // combo9: slot << 8 overlapped the routed mask's exact-SID nibble; slot and bank now sit above it.
            x[29] = route < 0 ? route : int32_t(uint32_t(route) | (gSlots[0] & 0xff) << 12 | (gBank & 0xff) << 20);
            if (route < 0) {c4st = -2; if (!st[1]) st[1] = route;}   // -4/-9 leave the SMR in det
            else {
                c4st = 2;
                // camnoc AXI to 400 MHz (PLL0 odd) for the write traffic, IFE-lite AXI branch on.
                if (!rootSet(0xc0f8, 3, 1, 3) || !branchOn(gCamcc, 0xc044, 3)) c4fail(3, -1);
                else if (ifeReset(f)) {
                    c4st = 3;
                    const uint32_t first = (yo - lines) / 2;
                    wmArm(f, framePa, stride, lines);
                    rdiOn(d, dt, lines != yo, first, first + lines - 1);
                    c4st = 4;
                    armed = true;
                }
            }
        }
    }

    // C4b: manual t970-camera-exp=<lines> / t970-camera-gain=<code> before streaming; bit 128 runs auto exposure after frame 1.
    Ae ae = {};
    uint32_t target = 72;
    const bool aeOn = armed && (gBits & 128);
    if (armed) {
        uint32_t expArg = 0, gainArg = 0;
        const bool manual = PE_parse_boot_argn("t970-camera-exp", &expArg, sizeof expArg) |
                            PE_parse_boot_argn("t970-camera-gain", &gainArg, sizeof gainArg);
        (void)PE_parse_boot_argn("t970-camera-target", &target, sizeof target);
        if (target < kBlack + 8 || target > 230) target = 72;
        if (aeOn || manual) {
            if (!aeLimits(c, s, fll, ae)) t970_camera_ae[0] = 7 << 8;
            else {
                if (manual) {
                    if (expArg) ae.coarse = expArg < ae.cMin ? ae.cMin : expArg > ae.cMax ? ae.cMax : expArg;
                    if (gainArg) ae.gain = gainArg < ae.gMin ? ae.gMin : gainArg > ae.gMax ? ae.gMax : gainArg;
                    t970_camera_ae[0] = (aeOn ? 0 : 6 << 8) | 1 << 12;
                    if (setExposure(c, s, ae.coarse, ae.gain)) t970_camera_ae[0] = 7 << 8 | 1 << 12;
                }
                if (ae.gain < ae.gMin) ae.gain = ae.gMin;               // AE math needs a sane starting gain
                if (ae.coarse < ae.cMin) ae.coarse = ae.cMin;
            }
        }
    }

    uint32_t seen = 0, top = 0, bus = 0, rdi = 0;
    int rc = writeReg(c, s.master, s.addr, 0x0100, 1);
    const uint64_t t0 = nowMs();
    for (unsigned i = 0; !rc && i < (armed ? 100u : 30u); ++i) {
        IOSleep(10);
        const uint32_t v = rd(d, 0x20);
        if (v) {seen |= v; wr(d, 0x28, v); wr(d, 0x80, 1);}
        if (armed) {
            const uint32_t r = rd(d, 0x30);
            if (r) {rdi |= r; wr(d, 0x38, r); wr(d, 0x80, 1);}
            const uint32_t t = rd(f, 0x40);
            if (t) {top |= t; wr(f, 0x34, t); wr(f, 0x24, 1);}
            const uint32_t b = rd(f, kBus + 0x28);
            if (b) {bus |= b; wr(f, kBus + 0x20, b); wr(f, kBus + 0x30, 1);}
            if (bus & (1u << 4)) {wmStop(f); x[36] = int32_t(nowMs() - t0); c4st = 5; break;}
        }
    }
    if (armed) {
        if (c4st != 5) wmStop(f);
        x[31] = int32_t((top & 0xffff) | bus << 16); x[32] = int32_t(rdi); x[33] = int32_t(rd(d, 0x278));
        if (aeOn && c4st == 5 && (t970_camera_ae[0] & 0xf00) != (7 << 8))
            autoExposure(c, s, f, framePa, stride, xo, lines, ae, target);
        if (t970_camera_ae[0] || aeOn) {
            uint32_t co = 0, ga = 0;
            if (!readU16(c, s, 0x0202, co) && !readU16(c, s, 0x0204, ga)) t970_camera_ae[2] = int32_t(co | ga << 16);
        }
        if ((gBits & 256) && c4st == 5) latestOff = streamLoop(c, s, f, framePa, stride, xo, lines, dt, ae, target, aeOn && ae.gMin);
    }
    x[18] = int32_t(seen);
    x[19] = int32_t(rd(d, 0x160)); x[20] = int32_t(rd(d, 0x164)); x[21] = int32_t(rd(d, 0x168));
    x[22] = int32_t(rd(d, 0x130)); x[23] = int32_t(rd(d, 0x128));
    x[27] = int32_t((rd(p, 0x8b0) & 0xff) | (rd(p, 0x8b4) & 0xff) << 8 | (rd(p, 0x8b8) & 0xff) << 16 | (rd(p, 0x8bc) & 0xff) << 24);
    (void)writeReg(c, s.master, s.addr, 0x0100, 0);
    IOSleep(50);
    c3st = rc ? -6 : 6;
    IOLog("t970-camera: c3 fmt %x lanes %u %ux%u llp %u fll %u rate %u settle %u rx %x pkts %u ecc %x crc %u long %x short %x\n",
          fmt, lanes, xo, yo, llp, fll, rateHi, settle, seen, unsigned(x[19]), unsigned(x[20]), unsigned(x[21]), unsigned(x[22]),
          unsigned(x[23]));
    if (armed) {
        rdiOff(d);
        unrouteStreams();
        (void)rootXo(0xc0f8, 0);
        branchOff(gCamcc, 0xc044);
        // Measured even without a done interrupt: coverage then tells whether the write master wrote anything at all.
        measure(reinterpret_cast<const volatile uint8_t *>(gFrameW.va) + latestOff, stride, lineBytes, xo, lines);
        if (c4st == 5) c4st = 6;
        IOLog("t970-camera: c4 route %x ife %x top|bus %x rdi %x measure %x written %d stats %x ms %d fault %x\n", unsigned(x[29]),
              unsigned(x[30]), unsigned(x[31]), unsigned(x[32]), unsigned(x[33]), int(x[34]), unsigned(x[35]), int(x[36]),
              unsigned(x[39]));
        if (c4st == 6) c4st = 7;
    } else if (frame) {
        unrouteStreams();
        (void)rootXo(0xc0f8, 0);
        branchOff(gCamcc, 0xc044);
    }
    csidOff(d);
    phyOff(p);
    c3ClocksOff();
    pll0Off();
    if (!rc) c3st = 7;
}

int32_t probe(const Sensor &s, unsigned index, bool keep) {
    st[0] = 5;
    if (!gCciW[s.cci].va && !mapWindow(gCciW[s.cci], kCci[s.cci], 0x1000)) {fail(5, -1); return -3;}
    volatile uint32_t *c = gCciW[s.cci].va;
    st[3] = int32_t(rd(c, 0));
    if (!function(s.sda, 1, 2, 3) || !function(s.scl, 1, 2, 3)) {fail(5, -2); return -3;}   // cciN_active: 2 mA, pull-up
    if (!cciInit(c, s.master)) return -2;

    st[0] = 6;
    parkLow(s.reset);
    output(kVio, true);
    output(s.vana, true);
    IOSleep(2);
    function(s.mclkGpio, 1, 4, 0);   // cam_sensor_mclkN_active: cam_mclk, 4 mA, no bias
    if (!branchOn(gCamcc, 0x5018 + 0x1c * s.mclk, 6)) {powerDown(s); return -3;}
    IOSleep(2);
    output(s.reset, true);
    IOSleep(10);

    st[0] = 7;
    uint8_t id[4] = {};
    int rc = readRegs(c, s.master, s.addr, 0x0000, id, 4);
    if (rc == -1) {
        // Some modules answer only after a longer reset release; one retry.
        IOSleep(20);
        rc = readRegs(c, s.master, s.addr, 0x0000, id, 4);
    }
    const int32_t result = rc ? rc : int32_t(uint32_t(id[0]) << 24 | uint32_t(id[1]) << 16 | uint32_t(id[2]) << 8 | id[3]);
    st[4 + index] = result;
    IOLog("t970-camera: %s cci%u m%u 0x%02x id %02x%02x rev %02x%02x rc %d\n", s.name, s.cci, s.master, s.addr,
          id[0], id[1], id[2], id[3], rc);
    if (!rc && index == 0 && (gBits & (32 | 64))) capture(c, s, gBits & 64);
    if (!rc && (gBits & 16)) st[9 + index] = streamTest(c, s);
    if (!keep) powerDown(s);
    return result;
}

void runOnce() {
    const uint64_t t0 = nowMs();
    if (clocksOn()) {
        int last = -1;
        for (unsigned i = 0; i < 3; ++i) if (gBits & (1u << i)) last = int(i);
        for (unsigned i = 0; i < 3; ++i) {
            if (!(gBits & (1u << i))) continue;
            (void)probe(kSensors[i], i, (gBits & 8) && int(i) == last);
            if (st[0] < 0) break;
        }
        if (st[0] > 0) st[0] = 8;
        if (!(gBits & 8)) clocksOff();
    }
    st[8] = int32_t(nowMs() - t0);
    if (!(gBits & 8)) {
        for (unsigned i = 0; i < gPins; ++i) unmap(gPin[i]);
        gPins = 0;
    }
}

// C5b: the control page, then T970CameraService.cpp's classes (registered here, between preModLoad and postModLoad, as
// T970Audio.cpp does for its service) and the service itself.
void publishService() {
    gCtlMem = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared, 4096, 4096);
    if (!gCtlMem) {t970_camera_c5[9] = -3; return;}
    gCtl = static_cast<T970CameraControl *>(gCtlMem->getBytesNoCopy());
    bzero(gCtl, 4096);
    gCtl->magic = T970_CAMERA_MAGIC; gCtl->version = T970_CAMERA_VERSION;
    gCtl->reading = T970_CAMERA_NONE; gCtl->writing = T970_CAMERA_NONE; gCtl->latest = T970_CAMERA_NONE;
    gCtl->frame_bytes = uint32_t(kFrameBytes);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {t970_camera_c5[9] = -4; return;}
    t970_camera_service_static_init();
    if (OSMetaClass::postModLoad(handle)) {t970_camera_c5[9] = -5; return;}
    t970_camera_service_start();
}

void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "t970-camera");
    IOSleep(4000);   // after the display, touch and USB log are up, so a hang here is visible in the log
    runOnce();
    if (gBits & 512) publishService();
    __atomic_store_n(&gBusy, 0u, __ATOMIC_RELEASE);
}

// One client session: the front sensor with C4 + AE + streaming until T970CameraStop or the client's close.
void sessionWorker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "t970-camera-c5");
    gSession = true;
    gBits = 1 | 64 | 128 | 256 | 512;
    st[0] = 0;
    t970_camera[28] = 0;
    gSessions = (gSessions + 1) & 0xffffff;
    t970_camera_c5[0] = int32_t(gSessions << 8);
    runOnce();
    const uint32_t c5 = uint32_t(t970_camera_c5[0]) & 0xff;
    if (gCtl) {
        gCtl->writing = T970_CAMERA_NONE;
        if (c5 == 3) gCtl->state = T970CameraStopped;
        else {
            gCtl->error = c5 == 0xff ? -1 : c5 == 0xfe ? -2 : t970_camera[28] < 0 ? t970_camera[28] : st[0] < 0 ? st[0] : 0;
            gCtl->state = T970CameraFailed;
        }
    }
    gSession = false;
    __atomic_store_n(&gRun, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&gBusy, 0u, __ATOMIC_RELEASE);
}
}

// T970CameraService.cpp's hooks. 0 accepted, 1 busy (the boot run or a session that is stopping), 2 not available.
extern "C" int t970_camera_request(uint32_t selector, uint32_t a, uint32_t b) {
    if (!gCtl) return 2;
    if (selector == T970CameraStop) {__atomic_store_n(&gRun, 0u, __ATOMIC_RELEASE); return 0;}
    if (selector == T970CameraSetExposure) {
        gManExp = a; gManGain = b;
        gCtl->manual_exposure = a; gCtl->manual_gain = b;
        __atomic_store_n(&gManualDirty, 1u, __ATOMIC_RELEASE);
        return 0;
    }
    if (selector == T970CameraReport) {
        if (a > 7) return 2;
        t970_camera_c5[12 + a] = int32_t(b);
        return 0;
    }
    if (selector != T970CameraStart) return 2;
    if (__atomic_load_n(&gBusy, __ATOMIC_ACQUIRE)) return gSession && __atomic_load_n(&gRun, __ATOMIC_ACQUIRE) ? 0 : 1;
    if (__atomic_exchange_n(&gBusy, 1u, __ATOMIC_ACQ_REL)) return 1;
    __atomic_store_n(&gRun, 1u, __ATOMIC_RELEASE);
    gCtl->error = 0; gCtl->seq = 0; gCtl->latest = T970_CAMERA_NONE; gCtl->writing = T970_CAMERA_NONE; gCtl->timeouts = 0;
    gCtl->state = T970CameraStarting;
    thread_t thread = nullptr;
    if (kernel_thread_start(sessionWorker, nullptr, &thread) != KERN_SUCCESS) {
        gCtl->state = T970CameraFailed; gCtl->error = -9;
        __atomic_store_n(&gRun, 0u, __ATOMIC_RELEASE);
        __atomic_store_n(&gBusy, 0u, __ATOMIC_RELEASE);
        return 2;
    }
    thread_deallocate(thread);
    return 0;
}
extern "C" IOMemoryDescriptor *t970_camera_memory(uint32_t type) {
    if (!gCtlMem) return nullptr;
    if (type == T970CameraMemoryControl) {gCtlMem->retain(); return gCtlMem;}
    if (type == T970CameraMemoryFrames) return IOMemoryDescriptor::withPhysicalAddress(gFramePa, kFrameBytes, kIODirectionIn);
    return nullptr;
}

extern "C" void t970_camera_start(void) {
    unsigned bits = 0;
    if (!PE_parse_boot_argn("t970-camera", &bits, sizeof bits) || !(bits & 7)) return;
    static unsigned started;
    if (__atomic_exchange_n(&started, 1u, __ATOMIC_ACQ_REL)) return;
    gBits = bits;
    st[0] = 0;
    __atomic_store_n(&gBusy, 1u, __ATOMIC_RELEASE);
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else {st[0] = -9; st[1] = 0; __atomic_store_n(&gBusy, 0u, __ATOMIC_RELEASE);}
}
