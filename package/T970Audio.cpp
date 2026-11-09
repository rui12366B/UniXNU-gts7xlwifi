// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi audio stage A2: a kernel test tone through the four speaker amplifiers (boot argument t970-audio=1; needs t970-adsp=2).
// One kernel thread, in order (each stage leaves its number in t970_audio_stats[0], negative when it failed):
//   1  "apr_audio_svc" registered with the ADSP GLINK client table (T970Adsp.cpp). This happens in t970_audio_start, before the
//      ADSP thread loads the firmware: QcomGlink drops an OPEN for a name nobody has registered, and the DSP opens the channel ~140
//      ms after it starts.
//   2  channel up (the DSP opened it, we answered and gave it receive intents)
//   3  APR core: AVCS_GET_STATE answers 1 (ready), then the framework version (AVCS_GET_FWK_VERSION, or AVCS_GET_VERSIONS)
//   4  DMA: the DSP reads shared buffers through apps-SMMU stream 0x1801 (live FDT msm-audio-ion iommus, sid mask 0xf, so the
//      DSP takes address | 1 << 32). Nothing in this kernel maps that stream, and this SMMU stores S2CR BYPASS as FAULT, so the
//      stream is routed (S2CR then SMR) to a context bank with translation off and CBAR S1_TRANS_S2_BYPASS (Linux qcom bypass
//      quirk): a bank of that kind already in use is shared, otherwise the first free bank from NUMS2CB up (bottom up, as the
//      Wi-Fi and USB code found safe; a top-down pick froze this board). The buffer is 512 KiB of pil_cvp_region (0x8de00000, a
//      no-map carveout whose processor nobody starts here; t970-audio-pa moves it), mapped write-combined.
//   5  pins and amplifiers: primary TDM pins (GPIO 138 sck, 141 ws, 139 din, 140 dout) to mi2s0; the CS35L41 reset (GPIO 69) low
//      2 ms then high; the four CS35L41 on QUP SE11's pins (GPIO 60 SDA, 61 SCL, TLMM south) as a GPIO I2C master (~100 kHz),
//      addresses 0x40..0x43: OTP boot, identity, errata and OTP trim unpack (Linux cs35l41-lib.c), DSP_A consumer, 32-bit slots
//      with 16-bit data, 48 kHz, PLL from the 6.144 MHz SCLK, RX1 in slot n. No amplifier firmware (CSPL) is loaded.
//   6  AFE: LPASS core hardware votes, PRI_TDM_IBIT 6.144 MHz, TDM group 0x9100 config + enable, port 0x9000 TDM config
//      (4 x 32-bit slots, short sync, internal sync source, 1-bit delay, as /soc/qcom,msm-dai-tdm-pri-rx), slot mapping, start
//   7  ADM: COPP on port 0x9000 with the null topology, 4 channels, 16 bits, 48 kHz
//   8  ASM: session 1 open (PCM, null post-processing), matrix map session -> COPP, media format, buffer map
//   9  amplifiers on (GLOBAL_EN, PUP_DONE), run, and a 1 kHz sine for t970-audio-ms (default 1500) in 20 ms periods, four queued
//  10  done: session paused and closed, COPP closed, port stopped, amplifiers off, buffer unmapped, stream route removed
// t970-audio bits: 1 run, 2 internal boost OFF (bypass, VBST = VP; default since audio5: boost on, as Samsung runs it), 4 leave the amplifiers alone, 8 keep the path up after the tone (no teardown), 16 send the TDM group config and
// enable (downstream msm-dai-q6-v2.c does that only for groups of more than one port; this board's group has one, and audio1
// with the group enabled saw DEVICE_START fail with ADSP_EFAILED), 32 device mode (below), 64 slot mapping v1 only (default v2,
// then v1 if refused). t970-audio-gain=N (amplifier gain step, default 6 = 6.5 dB until the volume is proven (int13: loud at iOS's lowest step at 10); Samsung runs 17 behind its protection DSP; int9's distortion was boost
// bypass, not gain: clean with boost on, int11), t970-audio-amp=N (tone amplitude, default 2000
// of 32767; 20000 was startling). The tone plays only with bit 1 or 128: t970-audio=32 is the device alone (no tone), 1 the tone
// alone, 544 device mode plus the slot sweep (sweepSample), 288 device mode plus the kernel's own sine ladder through it 5 s after it serves (see selfSample), 128 the tone as a
// four-step level ladder (-36, -24, -12, 0 dBFS; t970-audio-ms default 4000 then), 160 ladder then device.
// Device mode (bit 32), stage 20 = serving: the iOS output device. After stage 5 (and the tone, if asked) the service class T970Audio
// (T970AudioService.cpp, interface T970AudioAbi.h) is published and this thread serves it: the audio server's plug-in
// (hal/T970AudioPlugIn.c, loaded from the payload addon's audio/ directory) mixes into a shared 48 kHz stereo ring; on its start
// request the path opens (stages 6-8: AFE port, COPP, ASM session, buffer map), the amplifiers power up, the session runs and four
// 10 ms periods stay queued at the DSP, each copied from the ring as the previous one returns (left to slots 0 and 2, right to
// slots 1 and 3: Samsung's FL/FR/RL/RR slot order; 0x41 FL, 0x40 FR, 0x43 RL, 0x42 RR). The timeline, lead and drift handling are
// UniXNU-sheng's AudioProbe.cpp. Five seconds after the last stop request the session closes, the port stops and the amplifiers
// power down; the next start opens it again. The SMMU route and the amplifier configuration stay for the whole boot.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
#include <string.h>
extern "C" {
#include "ShengAdsp.h"
#include "T970Apr.h"
#include "T970AudioAbi.h"
}
extern kmod_info_t g_kernel_kmod_info;
// T970AudioService.cpp: its static initializer, taken out of __mod_init_func by the build, and the publication of the service.
extern "C" void t970_audio_service_static_init(void) __asm__("__GLOBAL__sub_I_T970AudioService.cpp");
extern "C" void t970_audio_service_start(void);

extern "C" ShengAdspChannel *sheng_adsp_channel(const char *, uint32_t, uint32_t, int, const ShengAdspClient *, void *) __attribute__((weak_import));
extern "C" int sheng_adsp_send(ShengAdspChannel *, const void *, uint32_t) __attribute__((weak_import));

// [0] stage, [1] last error (DSP status, -60 timeout, or a local code), [2] APR answers, [3] APR packets sent, [4] last opcode
// received, [5] core state, [6] framework major << 16 | minor (or build id), [7] branch << 16 | services, [8] ms to channel up,
// [9] ms from channel up to the tone, [10] COPP id, [11] buffer map handle, [12] periods sent, [13] periods returned, [14] periods
// returned with an error, [15] answers nobody waited for
extern "C" {volatile int32_t t970_audio_stats[16];}
// The status of each DSP command, in order (99 not sent, -60 no answer): votes macro/dcodec, clock, group config/enable, TDM config,
// slot mapping, port start, ADM open, matrix map, ASM open, media format, buffer map, run, close, port stop
extern "C" {volatile int32_t t970_audio_cmd[16];}
// [0] route kind (1 a mapping existed, 2 shared bypass bank, 3 new bypass bank) | SMR slot << 8 | bank << 16 (negative: the failing
// check), [1] sCR0, [2] IDR1, [3] SMR, [4] S2CR, [5] bank CBAR, [6] bank SCTLR, [7] bank FSR after the tone
extern "C" {volatile uint32_t t970_audio_dma[8];}
// Per amplifier: DEVID, REVID | OTPID << 8 | last step << 16 (bit 31: failed there), IRQ1_STATUS1 after power-up, IRQ1_STATUS3
// during the tone (bit 1 PLL lock)
extern "C" {volatile uint32_t t970_audio_amp[4][4];}
// I2C: NACKs, SCL held low, transfers that failed
extern "C" {volatile uint32_t t970_audio_i2c[3];}
// Every DSP command in order: opcode << 8 | (status & 0xff) (status 0xc4 = -60 no answer, 0xc3 = -61 not sent); [0] count
extern "C" {volatile uint32_t t970_audio_trace[25];}
// Per amplifier right after the AFE port start (whether or not it started): IRQ1_STATUS1, IRQ1_STATUS3 (bit 1 PLL lock), and
// the same once more 100 ms later
extern "C" {volatile uint32_t t970_audio_clk[4][4];}
// Device mode: [0] output starts, [1] stops, [2] starts that failed, [3] periods sent, [4] returned, [5] returned with an error,
// [6] periods taken before the plug-in wrote them (out_late), [7] published lead (frames), [8] peak sample, [9] drift resyncs,
// [10] settled DSP lead (frames), [11] the last failed start: stage << 16 | error
// [12] samples at or above 30000 (every 16th frame looked at), [13] peak of the current run, [14] runs whose amplifier status was
// read (t970_audio_clk then holds IRQ1_STATUS1..4 per amplifier, read 1 s into the run and again at its stop), [15] largest
// playback drift from the settled lead (frames; a resync at 1440)
extern "C" {volatile uint32_t t970_audio_stream[16];}
// The plug-in's diagnostics from the control page (see noteHal)
extern "C" {volatile uint32_t t970_audio_hal[12];}
// The plug-in's volume trace: last set kind (1 scalar, 2 dB), raw x1000, element, applied gain x10000 now / least / most, sets so far,
// then the last 8 sets (kind << 30 | raw x1000 + 2^29)
extern "C" {volatile int32_t t970_audio_vol[16];}
// Exactly what went into the DSP buffers in device mode, every sample: [0..3] peak per slot (FL, FR, RL, RR), [4..7] steps of more
// than 8192 between neighbouring samples per slot (a byte-split or misaligned sample shows as many of these), [8] XOR of every 32-bit
// word written, [9] frames copied, [10] frames with any nonzero slot
extern "C" {volatile uint32_t t970_audio_bytes[12];}
// The boost and class-H registers of amplifier i (t970-audio-bst-amp, default 1 = 0x41) as found after OTP and errata, before this
// driver writes them: VCTRL1 (BST_CTL target), VCTRL2 (BST_CTL_SEL), COEFF, SLOPE_LBST, PEAK_CUR, CLASSH_CFG, WKFET_CFG, NG_CFG;
// then VCTRL1, VCTRL2, PWR_CTRL2 as configured.
extern "C" {volatile uint32_t t970_audio_bst[12];}
// t970-audio bit 262144: the capture (T970AudioCapture): header t (low 32), write frames << 16 | index in the write, peak, kernel state
// (0 waiting, 1 taking, 2 done, 3 missed: the frames had been taken before the capture was seen); the plug-in's samples, the ring's.
// [4] where the captured frames really turned up: the timeline position at which the service read 16 frames equal to the capture's
// frames 64..79, minus t (0x7fffffff: not seen within 10 s); [5] t minus the service's next period position when it found the
// capture (how far ahead of its reads the plug-in wrote); [6]/[7] the plug-in's least and most output time minus now (frames).
extern "C" {volatile uint32_t t970_audio_cap[8] = {0, 0, 0, 0, 0x7fffffff}; volatile int16_t t970_audio_cap_p[512], t970_audio_cap_k[512];}
uint32_t gMatch;
// Playback timing, per boot: [0] least and [1] most frames the plug-in had written past the end of a period when the service took it
// (signed; negative: not written yet), [2] periods taken before the plug-in wrote them while it was running (any age), [3] plug-in
// writes that began before the service's take position (their start was lost), [4] runs, [5] the lead each run settled at
// (frames), [6] test flags sent to the plug-in
extern "C" {volatile int32_t t970_audio_timing[8] = {0x7fffffff, -0x7fffffff};}

namespace {
constexpr uint64_t kTlmm = 0x0f000000, kWest = 0x100000, kSouth = 0x500000, kSmmu = 0x15000000, kSmmuBytes = 0x100000;
constexpr uint64_t kDefaultPa = 0x8de00000;   // pil_cvp_region
constexpr uint32_t kBufferBytes = 512 * 1024;
constexpr uint16_t kStream = 0x1801, kSession = 1, kPort = 0x9000, kGroup = 0x9100;
constexpr uint32_t kRate = 48000, kChannels = 4, kBits = 16, kSlotBits = 32, kSclk = 6144000, kClkPriTdmIbit = 0x200;
// The buffer's first half is playback's, the second the capture's, each mapped to its own ASM session.
constexpr uint32_t kPlayBytes = kBufferBytes / 2;
constexpr uint32_t kPeriodFrames = 960, kPeriodBytes = kPeriodFrames * kChannels * 2, kSlots = kPlayBytes / kPeriodBytes, kInFlight = 4;
constexpr unsigned kSda = 60, kScl = 61, kReset = 69, kHalf = 5;
constexpr unsigned kTdmPins[4] = {138, 139, 140, 141};
constexpr uint32_t kRing = 16, kMessage = 128;
const int16_t kSine[48] = {0, 4277, 8481, 12539, 16383, 19947, 23170, 25996, 28377, 30273, 31650, 32487, 32767, 32487, 31650, 30273,
    28377, 25996, 23170, 19947, 16383, 12539, 8481, 4277, 0, -4277, -8481, -12539, -16383, -19947, -23170, -25996, -28377, -30273,
    -31650, -32487, -32767, -32487, -31650, -30273, -28377, -25996, -23170, -19947, -16384, -12539, -8481, -4277};

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void unmap(Window &w) {if (w.map) w.map->release(); w = {};}
void barrier() {__asm__ volatile("dsb sy" ::: "memory");}
uint64_t nowMs() {uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns / 1000000;}
void stage(int s) {t970_audio_stats[0] = s;}
bool fail(int s, int err) {t970_audio_stats[0] = -s; t970_audio_stats[1] = err; return false;}

// ---- pins ----
Window gPin[8];
unsigned gPinGpio[8], gPins;
volatile uint32_t *pin(unsigned gpio) {
    for (unsigned i = 0; i < gPins; ++i) if (gPinGpio[i] == gpio) return gPin[i].va;
    if (gPins == 8) return nullptr;
    const uint64_t tile = gpio >= 138 ? kWest : kSouth;   // GPIO 60, 61, 69 south; 138..141 west (pinctrl-sm8250.c)
    if (!mapWindow(gPin[gPins], kTlmm + tile + gpio * 0x1000ull, 0x1000)) return nullptr;
    gPinGpio[gPins] = gpio;
    return gPin[gPins++].va;
}

// ---- I2C on SE11's pins (open drain by output enable, as T970WifiProbe.cpp drives SE0) ----
volatile uint32_t *gSda, *gScl;
void release(volatile uint32_t *p) {p[0] &= ~(1u << 9); barrier();}
void drive(volatile uint32_t *p) {p[1] &= ~2u; p[0] |= 1u << 9; barrier();}
bool high(volatile uint32_t *p) {return p[1] & 1u;}
bool sclUp() {
    release(gScl);
    for (unsigned i = 0; i < 2000; ++i) {if (high(gScl)) return true; IODelay(1);}
    t970_audio_i2c[1] = t970_audio_i2c[1] + 1;
    return false;
}
void recover() {for (unsigned i = 0; i < 9 && !high(gSda); ++i) {drive(gScl); IODelay(kHalf); sclUp(); IODelay(kHalf);}}
bool start() {
    release(gSda); if (!sclUp()) return false; IODelay(kHalf);
    if (!high(gSda)) {recover(); if (!high(gSda)) return false;}
    drive(gSda); IODelay(kHalf); drive(gScl); IODelay(kHalf); return true;
}
void stop() {drive(gSda); IODelay(kHalf); sclUp(); IODelay(kHalf); release(gSda); IODelay(kHalf);}
bool writeByte(uint8_t b) {
    for (int bit = 7; bit >= 0; --bit) {
        if (b & (1u << bit)) release(gSda); else drive(gSda);
        IODelay(kHalf); if (!sclUp()) return false; IODelay(kHalf); drive(gScl);
    }
    release(gSda); IODelay(kHalf); if (!sclUp()) return false;
    const bool ack = !high(gSda); IODelay(kHalf); drive(gScl);
    if (!ack) t970_audio_i2c[0] = t970_audio_i2c[0] + 1;
    return ack;
}
uint8_t readByte(bool ack) {
    uint8_t b = 0; release(gSda);
    for (int bit = 7; bit >= 0; --bit) {IODelay(kHalf); sclUp(); if (high(gSda)) b |= uint8_t(1u << bit); IODelay(kHalf); drive(gScl);}
    if (ack) drive(gSda); else release(gSda);
    IODelay(kHalf); sclUp(); IODelay(kHalf); drive(gScl); release(gSda);
    return b;
}
// One transaction at a time: the audio thread and the halt path (t970_audio_quiesce) share the bus. Once halting, only the halt
// path uses it; it waits at most 5 ms for a transaction in flight (its owner may be preempted on this CPU) and goes on anyway.
volatile uint32_t gBus, gHalting;
struct BusHold {
    bool held = false;
    explicit BusHold(bool halt) {
        if (!halt && __atomic_load_n(&gHalting, __ATOMIC_ACQUIRE)) return;
        for (unsigned t = 0; t < 5000; ++t) {
            uint32_t free = 0;
            if (__atomic_compare_exchange_n(&gBus, &free, 1u, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {held = true; return;}
            if (!halt && t > 1000) return;
            IODelay(1);
        }
        held = halt;   // the halt path takes the bus whatever its owner left
    }
    ~BusHold() {if (held) __atomic_store_n(&gBus, 0u, __ATOMIC_RELEASE);}
};
IOThread gHaltThread;   // the thread running t970_audio_quiesce
bool onHaltPath() {return gHaltThread && IOThreadSelf() == gHaltThread;}
// CS35L41: 32-bit register address and value, both big endian.
bool ampWrite(uint8_t addr, uint32_t reg, uint32_t value) {
    BusHold bus(onHaltPath());
    if (!bus.held) return false;
    const uint8_t w[8] = {uint8_t(reg >> 24), uint8_t(reg >> 16), uint8_t(reg >> 8), uint8_t(reg),
                          uint8_t(value >> 24), uint8_t(value >> 16), uint8_t(value >> 8), uint8_t(value)};
    bool ok = start() && writeByte(uint8_t(addr << 1));
    for (unsigned i = 0; ok && i < 8; ++i) ok = writeByte(w[i]);
    stop();
    if (!ok) t970_audio_i2c[2] = t970_audio_i2c[2] + 1;
    return ok;
}
bool ampRead(uint8_t addr, uint32_t reg, uint32_t *value) {
    BusHold bus(onHaltPath());
    if (!bus.held) return false;
    bool ok = start() && writeByte(uint8_t(addr << 1));
    for (int s = 24; ok && s >= 0; s -= 8) ok = writeByte(uint8_t(reg >> s));
    if (ok) {release(gSda); IODelay(kHalf); ok = sclUp(); IODelay(kHalf);}
    ok = ok && start() && writeByte(uint8_t(addr << 1 | 1));
    uint32_t v = 0;
    for (unsigned i = 0; ok && i < 4; ++i) v = v << 8 | readByte(i < 3);
    stop();
    if (!ok) t970_audio_i2c[2] = t970_audio_i2c[2] + 1;
    else *value = v;
    return ok;
}
bool ampUpdate(uint8_t addr, uint32_t reg, uint32_t mask, uint32_t value) {
    uint32_t v = 0;
    return ampRead(addr, reg, &v) && ampWrite(addr, reg, (v & ~mask) | (value & mask));
}

// ---- CS35L41 (Linux sound/soc/codecs/cs35l41.c, cs35l41-lib.c, include/sound/cs35l41.h) ----
enum : uint32_t {
    DEVID = 0x0, REVID = 0x4, OTPID = 0x10, TEST_KEY = 0x40, OTP_MEM0 = 0x400, PWR_CTRL1 = 0x2014, PWR_CTRL2 = 0x2018,
    PLL_CLK_CTRL = 0x2c04, GLOBAL_CLK_CTRL = 0x2c0c, TST_FS_MON0 = 0x2d10, BST_PEAK_CUR = 0x3808, BST_DCM_CTRL = 0x381c,
    VIMON_SPKMON_RESYNC = 0x4100, VPVBST_FS_SEL = 0x4400, SP_ENABLES = 0x4800, SP_FORMAT = 0x4808, SP_HIZ = 0x480c,
    SP_RX_SLOT = 0x4820, SP_RX_WL = 0x4840, DAC_PCM1_SRC = 0x4c00, ASP_TX3_SRC = 0x4c28, ASP_TX4_SRC = 0x4c2c,
    AMP_DIG_VOL = 0x6000, AMP_GAIN = 0x6c04, IRQ1_STATUS1 = 0x10010, IRQ1_STATUS3 = 0x10018, IRQ1_STATUS4 = 0x1001c,
    IRQ1_MASK1 = 0x10110, CCM_CORE_CTRL = 0x02bc1000, XM_ACCEL_PL0 = 0x02bc2020, YM_ACCEL_PL0 = 0x02bc20e0,
    PUP_DONE = 0x01000000, PDN_DONE = 0x00800000, OTP_BOOT_DONE = 0x2, OTP_BOOT_ERR = 0x80000000,
};
struct Reg {uint32_t reg, value;};
// The B0 and B2 errata list (the A0 list adds writes for engineering silicon only; not applied here).
const Reg kErrata[] = {{VIMON_SPKMON_RESYNC, 0}, {0x4310, 0}, {VPVBST_FS_SEL, 0}, {BST_DCM_CTRL, 0x51}, {YM_ACCEL_PL0, 0},
                       {XM_ACCEL_PL0, 0}, {PWR_CTRL2, 0}, {AMP_GAIN, 0}, {ASP_TX3_SRC, 0}, {ASP_TX4_SRC, 0}};
struct Otp {uint32_t reg; uint8_t shift, size;};
// otp_map_1 / otp_map_2 of cs35l41-lib.c; they differ in one element (index 92): an unused bit (map 1) or VMON_POL (map 2).
const Otp kOtp[] = {
    {0x2030, 0, 4}, {0x2030, 7, 1}, {0x208c, 24, 6}, {0x2090, 14, 4}, {0x2090, 10, 4}, {0x300C, 11, 4}, {0x394C, 23, 2},
    {0x3950, 0, 7}, {0x3950, 8, 7}, {0x3950, 16, 8}, {0x3950, 24, 8}, {0x3954, 0, 7}, {0x3954, 8, 7}, {0x3954, 16, 8},
    {0x3954, 24, 8}, {0x3958, 0, 7}, {0x3958, 8, 7}, {0x3958, 16, 8}, {0x3958, 24, 8}, {0x395C, 0, 7}, {0x395C, 8, 7},
    {0x395C, 16, 8}, {0x395C, 24, 8}, {0x416C, 0, 8}, {0x4160, 0, 7}, {0x416C, 8, 8}, {0x4160, 16, 10}, {0x416C, 16, 12},
    {0x416C, 28, 1}, {0x4170, 0, 6}, {0x4170, 6, 1}, {0x4170, 8, 6}, {0x4170, 14, 1}, {0x4170, 16, 9}, {0x4360, 0, 5},
    {0x4360, 6, 9}, {0x4448, 0, 8}, {0x4448, 8, 8}, {0x4448, 16, 8}, {0x4448, 24, 8}, {0x444C, 0, 3}, {0x6E30, 0, 5},
    {0x6E30, 8, 5}, {0x6E30, 16, 5}, {0x6E30, 24, 5}, {0x6E34, 0, 5}, {0x6E34, 8, 5}, {0x6E34, 16, 5}, {0x6E34, 24, 5},
    {0x6E38, 0, 5}, {0x6E38, 8, 5}, {0x6E38, 16, 5}, {0x6E38, 24, 5}, {0x6E3C, 0, 5}, {0x6E3C, 8, 5}, {0x6E3C, 16, 5},
    {0x6E3C, 24, 5}, {0x6E40, 0, 5}, {0x6E40, 8, 5}, {0x6E40, 16, 5}, {0x6E40, 24, 5}, {0x6E44, 0, 5}, {0x6E48, 0, 10},
    {0x6E48, 10, 10}, {0x6E48, 20, 10}, {0x6E4C, 0, 10}, {0x6E4C, 10, 10}, {0x6E4C, 20, 10}, {0x6E50, 0, 10}, {0x6E50, 10, 10},
    {0x6E50, 20, 10}, {0x6E54, 0, 10}, {0x6E54, 10, 10}, {0x6E54, 20, 10}, {0x6E58, 0, 10}, {0x6E58, 10, 10}, {0x6E58, 20, 10},
    {0x6E5C, 0, 10}, {0x6E5C, 10, 10}, {0x6E5C, 20, 10}, {0x6E60, 0, 10}, {0x6E60, 10, 10}, {0x6E60, 20, 10}, {0x6E64, 0, 10},
    {0x7418, 7, 5}, {0x741C, 0, 5}, {0x741C, 11, 4}, {0x741C, 19, 4}, {0x7434, 17, 1}, {0x7434, 18, 7}, {0x7068, 0, 9},
    {0x410C, 7, 1}, {0x400C, 0, 7}, {0x0, 0, 1} /* map 2: {0x4000, 11, 1} */, {0x17040, 0, 8}, {0x17040, 8, 8},
    {0x17040, 16, 8}, {0x17040, 24, 8}, {0x17044, 0, 24},
};
constexpr unsigned kOtpPolIndex = 92;
uint32_t bits(unsigned hi, unsigned lo) {return (hi >= 31 ? 0xffffffffu : ((1u << (hi + 1)) - 1)) & ~((1u << lo) - 1);}

bool testKey(uint8_t a, bool unlock) {
    return unlock ? ampWrite(a, TEST_KEY, 0x55) && ampWrite(a, TEST_KEY, 0xaa) : ampWrite(a, TEST_KEY, 0xcc) && ampWrite(a, TEST_KEY, 0x33);
}
bool otpUnpack(uint8_t a, uint32_t otpId) {
    if (otpId != 1 && otpId != 2 && otpId != 3 && otpId != 6 && otpId != 8) return false;
    const bool map2 = otpId == 2 || otpId == 3 || otpId == 6;
    uint32_t mem[32];
    for (unsigned i = 0; i < 32; ++i) if (!ampRead(a, OTP_MEM0 + 4 * i, &mem[i])) return false;
    unsigned bit = 16, word = 2;
    for (unsigned i = 0; i < sizeof kOtp / sizeof kOtp[0]; ++i) {
        Otp e = kOtp[i];
        if (i == kOtpPolIndex && map2) e = {0x4000, 11, 1};
        uint32_t v = 0;
        if (bit + e.size - 1 >= 32) {
            v = (mem[word] & bits(31, bit)) >> bit;
            ++word;
            if (word >= 32) return false;
            v |= (mem[word] & bits(bit + e.size - 33, 0)) << (32 - bit);
            bit += e.size - 32;
        } else {
            v = (mem[word] & bits(bit + e.size - 1, bit)) >> bit;
            bit += e.size;
        }
        if (bit == 32) {bit = 0; ++word;}
        if (e.reg && !ampUpdate(a, e.reg, bits(e.shift + e.size - 1, e.shift), v << e.shift)) return false;
    }
    return true;
}

struct AmpCfg {unsigned mode, gain;};
// The RX slot of each amplifier, nibble i for the one at 0x40 + i (t970-audio-slots=0xNNNN; default 0x2301: Samsung's FL 0, FR 1,
// RL 2, RR 3 with 0x41 FL, 0x40 FR, 0x43 RL, 0x42 RR). Swapping it against the slot sweep (bit 512) tells a bad slot from a bad
// amplifier.
uint32_t gAmpSlots = 0x2301;
void ampStep(unsigned i, unsigned s, bool failed = false) {
    t970_audio_amp[i][1] = (t970_audio_amp[i][1] & 0xffff) | (s & 0x7fff) << 16 | (failed ? 0x80000000u : 0);
}
// Identity, errata, trims, interface and clocking; the amplifier stays off.
bool ampConfigure(unsigned i, const AmpCfg &cfg) {
    const uint8_t a = uint8_t(0x40 + i);
    uint32_t v = 0;
    ampStep(i, 1);
    bool booted = false;
    for (unsigned t = 0; t < 100 && !booted; ++t) {if (ampRead(a, IRQ1_STATUS4, &v) && (v & OTP_BOOT_DONE)) booted = true; else IOSleep(1);}
    if (!booted) {ampStep(i, 1, true); return false;}
    ampStep(i, 2);
    if (!ampRead(a, IRQ1_STATUS3, &v) || (v & OTP_BOOT_ERR)) {ampStep(i, 2, true); return false;}
    uint32_t dev = 0, rev = 0, otp = 0;
    if (!ampRead(a, DEVID, &dev) || !ampRead(a, REVID, &rev) || !ampRead(a, OTPID, &otp)) {ampStep(i, 3, true); return false;}
    t970_audio_amp[i][0] = dev;
    t970_audio_amp[i][1] = (rev & 0xff) | (otp & 0xff) << 8;
    ampStep(i, 3);
    if (dev != 0x35a40 && dev != 0x35b40) {ampStep(i, 3, true); return false;}
    bool ok = testKey(a, true);
    for (const Reg &r : kErrata) ok = ok && ampWrite(a, r.reg, r.value);
    ok = ok && ampWrite(a, CCM_CORE_CTRL, 0);
    if (!ok) {ampStep(i, 4, true); return false;}
    ampStep(i, 5);
    if (!otpUnpack(a, otp & 0xf)) {testKey(a, false); ampStep(i, 5, true); return false;}
    ok = testKey(a, false) && ampWrite(a, IRQ1_MASK1, 0x7FFCFE3F);
    // Samsung's fsync errata (its DT sets cirrus,use-fsync-errata; Samsung cs35l41.c cs35l41_fsync_errata_patch): with the other
    // three registers of that patch already in kErrata, ASP_CONTROL4 (0x4854) = 0x01010000, behind the test key.
    if (!(cfg.mode & 2097152)) ok = ok && testKey(a, true) && ampWrite(a, 0x4854, 0x01010000) && testKey(a, false);
    // The hardware noise gate as Android leaves it (live dump 2026-11-08: NG_CFG 0x6808 = 0x3f26 on all four; DT hw-noise-gate-select
    // 0x3f, delay 2, threshold 6): it mutes the output stage while the input stays below the threshold. Reset is 0x33 (off).
    if (!(cfg.mode & 2097152)) ok = ok && ampWrite(a, 0x6808, 0x3f26);
    // Brownout protection (VPBR) as Android runs it (live dump: VPBR_CFG 0x6404 = 0x02005206, i.e. threshold 2.687 V, max
    // attenuation 2, attack 1 dB; PWR_CTRL3 0x201c bit 12 on): with the louder default gain the amplifiers turn themselves down for a
    // moment when the battery sags instead of browning out. t970-audio bit 4194304 leaves it off.
    if (!(cfg.mode & 4194304)) ok = ok && ampWrite(a, 0x6404, 0x02005206) && ampUpdate(a, 0x201c, 1u << 12, 1u << 12);
    ampStep(i, 6);
    // DSP_A (TDM 1.5: frame sync one SCLK before slot 0), we are the clock consumer; slots 32 SCLKs, data 16 bits. Bit clock
    // INVERTED as Samsung's machine driver sets the speaker link (kona.c PRI TDM RX: DSP_A | CBS_CFS | IB_NF, so cs35l41_set_dai_fmt
    // sets SCLK_INV); audio1..12 ran with normal polarity, the amplifiers sampling on the edge the DSP changes the data on.
    // t970-audio bit 2097152 goes back to that (normal polarity, no fsync errata).
    ok = ok && ampUpdate(a, SP_FORMAT, 0xff000000u | 0x700 | 0x10 | 0x01 | 0x04 | 0x40,
                         kSlotBits << 24 | ((cfg.mode & 2097152) ? 0 : 0x40));
    ok = ok && ampUpdate(a, SP_RX_WL, 0x3f, kBits);
    ok = ok && ampUpdate(a, GLOBAL_CLK_CTRL, 0x1f, 0x03);                 // 48 kHz
    // Samsung's slots (vendor mixer_paths: ASPRX1 FL 0, FR 1, RL 2, RR 3; names from mfd-suffix "", "_r", "_b", "_br" and
    // cirrus,right-channel-amp): 0x41 FL, 0x40 FR, 0x43 RL, 0x42 RR. ASPRX2 at slot 7 (unused), as Samsung sets it.
    ok = ok && ampUpdate(a, SP_RX_SLOT, 0x3f3f, ((gAmpSlots >> (4 * i)) & 3u) | 7u << 8);
    ok = ok && ampUpdate(a, SP_HIZ, 0x3, 0x3);                           // cirrus,asp-sdout-hiz = 3
    ok = ok && ampWrite(a, DAC_PCM1_SRC, 0x08);                          // PCM from ASPRX1
    // PLL from SCLK: open loop, reference 6.144 MHz (0x28), disable, source SCLK, close the loop, enable.
    ok = ok && ampUpdate(a, PLL_CLK_CTRL, 1u << 11, 1u << 11);
    ok = ok && ampUpdate(a, PLL_CLK_CTRL, 0x3fu << 5, 0x28u << 5);
    ok = ok && ampUpdate(a, PLL_CLK_CTRL, 1u << 4, 0);
    ok = ok && ampUpdate(a, PLL_CLK_CTRL, 0x7, 0);
    ok = ok && ampUpdate(a, PLL_CLK_CTRL, 1u << 11, 0);
    ok = ok && ampUpdate(a, PLL_CLK_CTRL, 1u << 4, 1u << 4);
    ok = ok && ampWrite(a, TST_FS_MON0, 16 | 24u << 12);                 // fs monitor windows for a 6.144 MHz SCLK
    // Boost: peak current 4100 mA (cirrus,boost-peak-milliamp) and on only with t970-audio bit 2; otherwise bypass (VBST = VP).
    // Boost on, as Samsung's "spk" path runs: the DT gives only cirrus,boost-peak-milliamp (4100), so Samsung's driver leaves the
    // converter's coefficients at their reset values, which are Linux cs35l41_boost_config's for 1.0 uH and up to 19 uF (K1 0x24,
    // K2 0x24, slope 0x75, LBST 0); they are written explicitly here. t970-audio bit 2 turns boost off (bypass, VBST = VP) instead,
    // which int9 ran with: rough sound at low levels.
    if (i == 1) {
        static const uint32_t regs[8] = {0x3800, 0x3804, 0x3810, 0x3814, BST_PEAK_CUR, 0x6800, 0x6804, 0x6808};
        for (unsigned k = 0; k < 8; ++k) {uint32_t v = 0; if (ampRead(a, regs[k], &v)) t970_audio_bst[k] = v;}
    }
    ok = ok && ampUpdate(a, 0x3810, 0xffff, 0x24u | 0x24u << 8);        // BSTCVRT_COEFF
    ok = ok && ampUpdate(a, 0x3814, 0xff03, 0x75u << 8);                 // BSTCVRT_SLOPE_LBST
    // t970-audio bit 16384: a fixed boost voltage instead of whatever BST_CTL_SEL selects after reset (class-H tracking needs the
    // amplifier DSP Samsung runs; without it the target may lag transients): BST_CTL_SEL = control port (VCTRL2 bits 1:0 = 0), VBST
    // 8.0 V (VCTRL1 = (8000 - 2550) / 50 + 1 = 0x6e; 0x01 = 2.55 V .. 0xaa = 11 V in 50 mV steps).
    if (cfg.mode & 16384) {
        ok = ok && ampUpdate(a, 0x3804, 0x3, 0);
        ok = ok && ampUpdate(a, 0x3800, 0xff, 0x6e);
    }
    ok = ok && ampUpdate(a, BST_PEAK_CUR, 0x7f, (4100 - 1600) / 50 + 0x10);
    ok = ok && ampUpdate(a, PWR_CTRL2, 0x30, (cfg.mode & 2 ? 1u : 2u) << 4);
    ok = ok && ampUpdate(a, AMP_GAIN, 0x3e0, (cfg.gain & 0x1f) << 5);
    ok = ok && ampUpdate(a, AMP_DIG_VOL, 0x7ffu << 3 | 7u, 1);           // 0 dB, unmuted, PCM soft ramp 0.5 ms (Samsung's)
    if (i == 1) {
        uint32_t v = 0;
        if (ampRead(a, 0x3800, &v)) t970_audio_bst[8] = v;
        if (ampRead(a, 0x3804, &v)) t970_audio_bst[9] = v;
        if (ampRead(a, PWR_CTRL2, &v)) t970_audio_bst[10] = v;
    }
    if (!ok) {ampStep(i, 7, true); return false;}
    ampStep(i, 8);
    return true;
}
const Reg kPup[] = {{TEST_KEY, 0x55}, {TEST_KEY, 0xaa}, {0x2084, 0x002F1AA0}, {TEST_KEY, 0xcc}, {TEST_KEY, 0x33}};
const Reg kPdn[] = {{TEST_KEY, 0x55}, {TEST_KEY, 0xaa}, {0x2084, 0x002F1AA3}, {TEST_KEY, 0xcc}, {TEST_KEY, 0x33}};
bool ampPower(unsigned i, bool on) {
    const uint8_t a = uint8_t(0x40 + i);
    bool ok = true;
    if (on) {
        ok = ampUpdate(a, SP_ENABLES, 1u << 16, 1u << 16) && ampUpdate(a, PWR_CTRL2, 1, 1);
        for (const Reg &r : kPup) ok = ok && ampWrite(a, r.reg, r.value);
    }
    ok = ok && ampUpdate(a, PWR_CTRL1, 1, on ? 1 : 0);
    const uint32_t done = on ? PUP_DONE : PDN_DONE;
    uint32_t v = 0;
    bool seen = false;
    for (unsigned t = 0; ok && t < 100 && !seen; ++t) {if (ampRead(a, IRQ1_STATUS1, &v) && (v & done)) seen = true; else IOSleep(1);}
    if (on) t970_audio_amp[i][2] = v;
    if (ok) ampWrite(a, IRQ1_STATUS1, done);
    if (!on) {
        for (const Reg &r : kPdn) ok = ok && ampWrite(a, r.reg, r.value);
        ok = ok && ampUpdate(a, PWR_CTRL2, 1, 0) && ampUpdate(a, SP_ENABLES, 1u << 16, 0);
    }
    ampStep(i, on ? (seen ? 9 : 10) : 11, !seen);
    return ok && seen;
}
// Halt and restart (ShengPlatformPower's t970_halt_restart, before the PSCI call): the amplifiers go quiet before their supply drops,
// or the speakers pop (int16b, "like an old CRT switching off"). Per amplifier: digital volume to mute (0.5 ms soft ramp), 10 ms,
// GLOBAL_EN off and the PDN_DONE wait (up to 20 ms), boost off. Busy-waits only (preemption may be off there); the audio thread's
// I2C stops once gHalting is set. t970_audio_halt: 1 entered, bit 8+i amplifier i quiet, 0x10000 already done.
uint32_t gAmpsFound;
extern "C" {volatile uint32_t t970_audio_halt;}
extern "C" void t970_audio_quiesce(void) {
    if (__atomic_exchange_n(&gHalting, 1u, __ATOMIC_ACQ_REL)) {t970_audio_halt = t970_audio_halt | 0x10000; return;}
    t970_audio_halt = 1;
    const uint32_t found = __atomic_load_n(&gAmpsFound, __ATOMIC_ACQUIRE);
    if (!found || !gSda || !gScl) return;
    gHaltThread = IOThreadSelf();
    for (unsigned i = 0; i < 4; ++i) if (found & (1u << i)) ampUpdate(uint8_t(0x40 + i), AMP_DIG_VOL, 0x7ffu << 3, 0x400u << 3);
    IODelay(10000);
    for (unsigned i = 0; i < 4; ++i) {
        if (!(found & (1u << i))) continue;
        const uint8_t a = uint8_t(0x40 + i);
        bool ok = ampUpdate(a, PWR_CTRL1, 1, 0);
        uint32_t v = 0;
        for (unsigned t = 0; ok && t < 20; ++t) {if (ampRead(a, IRQ1_STATUS1, &v) && (v & PDN_DONE)) break; IODelay(1000);}
        ok = ampUpdate(a, PWR_CTRL2, 0x31, 0) && ok;   // boost off (BST_EN 0), amplifier off
        if (ok) t970_audio_halt = t970_audio_halt | 1u << (8 + i);
    }
    gHaltThread = nullptr;
}

// ---- pins ----
bool setupPins() {
    for (unsigned g : kTdmPins) {   // function 1 (mi2s0_*), 2 mA, no pull
        volatile uint32_t *p = pin(g);
        if (!p) return false;
        p[0] = 1u << 2;
        barrier();
    }
    gSda = pin(kSda); gScl = pin(kScl);
    volatile uint32_t *reset = pin(kReset);
    if (!gSda || !gScl || !reset) return false;
    volatile uint32_t *const bus[2] = {gSda, gScl};
    for (volatile uint32_t *p : bus) {p[1] &= ~2u; p[0] = (p[0] & ~(0x3cu | 3u | (1u << 9))) | 3u; barrier();}   // GPIO, pull-up, released
    reset[1] = 0; reset[0] = 1u << 9; barrier();     // GPIO, output low, 2 mA, no pull
    IOSleep(3);
    reset[1] = 2; barrier();                          // release
    IOSleep(3);
    return true;
}

// ---- the stream route in the apps SMMU ----
Window gSmmu;
uint32_t gShift, gPages, gSlot = 0xffff, gBank = 0xffff;
uint32_t gOld[4];   // the new bank's SCTLR, CBA2R, CBAR and the free slot's S2CR before we wrote them
bool gNewBank, gRouted, gS2cr;
uint32_t sr(uint64_t off) {return gSmmu.va[off / 4];}
void sw(uint64_t off, uint32_t v) {gSmmu.va[off / 4] = v; barrier();}
uint64_t cb(uint32_t i) {return (uint64_t(gPages) + i) << gShift;}
int routeStream(uint64_t pa) {
    volatile uint32_t *d = t970_audio_dma;
    if (!mapWindow(gSmmu, kSmmu, kSmmuBytes)) return -1;
    const uint32_t cr0 = sr(0), id0 = sr(0x20), id1 = sr(0x24), id2 = sr(0x28);
    d[1] = cr0; d[2] = id1;
    if (cr0 & (1u << 3)) return -2;
    const uint32_t streams = id0 & 0xff, banks = id1 & 0xff, ns2 = (id1 >> 16) & 0xff;
    gShift = (id1 >> 31) ? 16 : 12; gPages = 2u << ((id1 >> 28) & 7);
    if (!streams || streams > 128 || !banks || banks > 128 || cb(banks) > kSmmuBytes) return -3;
    if (pa + kBufferBytes > (1ull << 32) || !(id2 & 0xfff)) return -4;
    bool used[128] = {};
    uint32_t freeSmr = 0xffff, sharedBank = 0xffff;
    for (uint32_t i = 0; i < streams; ++i) {
        const uint32_t smr = sr(0x800 + 4 * i), s2cr = sr(0xc00 + 4 * i);
        if (!(smr & (1u << 31))) {if (freeSmr == 0xffff) freeSmr = i; continue;}
        const uint32_t id = smr & 0x7fff, mask = (smr >> 16) & 0x7fff;
        const bool translate = ((s2cr >> 16) & 3) == 0 && (s2cr & 0xff) < banks;
        if (translate) used[s2cr & 0xff] = true;
        if (((kStream ^ id) & ~mask & 0x7fff) == 0) {   // a mapping exists already (firmware or an earlier run): use it as it is
            d[3] = smr; d[4] = s2cr; gSlot = i;
            if (translate) {gBank = s2cr & 0xff; d[5] = sr((1ull << gShift) + 4 * gBank); d[6] = sr(cb(gBank));}
            d[0] = 1 | i << 8 | (gBank & 0xff) << 16;
            return 0;
        }
        if (translate && sharedBank == 0xffff && sr((1ull << gShift) + 4 * (s2cr & 0xff)) == 0x10000 && !(sr(cb(s2cr & 0xff)) & 1))
            sharedBank = s2cr & 0xff;   // a bypass-emulation bank another stream uses (UFS does, on sheng)
    }
    if (freeSmr == 0xffff) return -5;
    if (sharedBank != 0xffff) gBank = sharedBank;
    else {
        for (uint32_t i = ns2; i < banks && gBank == 0xffff; ++i)
            if (!used[i] && !(sr(cb(i)) & 1) && !(sr(cb(i) + 0x58) & 0xc00001feu)) gBank = i;
        if (gBank == 0xffff) return -6;
        gOld[0] = sr(cb(gBank)); gOld[1] = sr((1ull << gShift) + 0x800 + 4 * gBank); gOld[2] = sr((1ull << gShift) + 4 * gBank);
        gNewBank = true;
        sw(cb(gBank), 0);                                   // SCTLR: translation off
        sw((1ull << gShift) + 0x800 + 4 * gBank, 1);        // CBA2R: VA64
        sw((1ull << gShift) + 4 * gBank, 0x10000);          // CBAR: S1 translate (off), S2 bypass
        if (sr(cb(gBank)) != 0 || sr((1ull << gShift) + 4 * gBank) != 0x10000) return -7;
    }
    gSlot = freeSmr;
    gOld[3] = sr(0xc00 + 4 * gSlot);
    gS2cr = true;
    sw(0xc00 + 4 * gSlot, gBank);                           // S2CR first: translate through the bank
    if ((sr(0xc00 + 4 * gSlot) & 0x300ff) != gBank) return -8;
    sw(0x800 + 4 * gSlot, 0x80000000u | kStream);           // then the exact SMR
    gRouted = true;
    d[3] = sr(0x800 + 4 * gSlot); d[4] = sr(0xc00 + 4 * gSlot);
    d[5] = sr((1ull << gShift) + 4 * gBank); d[6] = sr(cb(gBank));
    d[0] = (gNewBank ? 3 : 2) | gSlot << 8 | gBank << 16;
    return d[3] == (0x80000000u | kStream) ? 0 : -9;
}
void unrouteStream() {
    if (!gSmmu.va) return;
    if (gBank != 0xffff) t970_audio_dma[7] = sr(cb(gBank) + 0x58);
    // As found: SMR invalid first, then the slot's S2CR, then the bank (CBAR, CBA2R, SCTLR).
    if (gRouted) sw(0x800 + 4 * gSlot, 0);
    if (gS2cr) sw(0xc00 + 4 * gSlot, gOld[3]);
    if (gNewBank) {sw((1ull << gShift) + 4 * gBank, gOld[2]); sw((1ull << gShift) + 0x800 + 4 * gBank, gOld[1]); sw(cb(gBank), gOld[0]);}
    gRouted = gS2cr = gNewBank = false;
}

// ---- APR ----
ShengAdspChannel *gChannel;
IOLock *gLock;
IOSimpleLock *gRingLock;
struct {uint32_t length; uint8_t data[kMessage];} gRing[kRing];
uint32_t gHead, gCount;
volatile uint32_t gUp;
void dspState(void *, int up) {
    __atomic_store_n(&gUp, up ? 1u : 0u, __ATOMIC_RELEASE);
    IOLockWakeup(gLock, &gCount, false);
}
void dspReceive(void *, const uint8_t *data, uint32_t length) {
    IOSimpleLockLock(gRingLock);
    if (gCount < kRing) {
        auto &slot = gRing[(gHead + gCount) % kRing];
        slot.length = length < kMessage ? length : kMessage;
        memcpy(slot.data, data, slot.length);
        if (length >= 4 && length > kMessage) {slot.data[2] = uint8_t(kMessage); slot.data[3] = 0;}   // clipped: the size says so
        gCount++;
    }
    IOSimpleLockUnlock(gRingLock);
    IOLockWakeup(gLock, &gCount, false);
}
bool nextAnswer(AprAnswer &a, int &kind, uint64_t endMs) {
    for (;;) {
        uint8_t data[kMessage];
        uint32_t length = 0;
        IOSimpleLockLock(gRingLock);
        if (gCount) {
            auto &slot = gRing[gHead];
            length = slot.length;
            memcpy(data, slot.data, length);
            gHead = (gHead + 1) % kRing;
            gCount--;
        }
        IOSimpleLockUnlock(gRingLock);
        if (length) {
            kind = apr_parse(data, length, &a);
            if (kind == AprBad) continue;
            t970_audio_stats[2] = t970_audio_stats[2] + 1;
            t970_audio_stats[4] = (int32_t)a.opcode;
            return true;
        }
        const uint64_t now = nowMs();
        if (now >= endMs) return false;
        uint64_t deadline;
        clock_interval_to_deadline((uint32_t)(endMs - now), kMillisecondScale, &deadline);
        IOLockLock(gLock);
        if (!gCount) IOLockSleepDeadline(gLock, &gCount, deadline, THREAD_UNINT);
        IOLockUnlock(gLock);
    }
}
bool send(const uint8_t *p, uint32_t n) {
    for (unsigned t = 0; t < 50; ++t) {   // the client queue holds 8; it drains as GLINK intents come back
        if (n && sheng_adsp_send(gChannel, p, n) == 0) {t970_audio_stats[3] = t970_audio_stats[3] + 1; return true;}
        if (!n) return false;
        IOSleep(10);
    }
    return false;
}
uint64_t gPa, gDsp;          // the buffer: physical, and as the DSP takes it
uint32_t gHandle;
Window gBuf;
uint32_t gDone, gErrors, gInFlight;
bool gServing;                       // device mode: write answers belong to the stream (streamDone), not the tone
void streamDone(const AprAnswer &a);
void micDone(const AprAnswer &a);   // capture (below)
void handleWriteDone(const AprAnswer &a) {
    if (gServing) {streamDone(a); return;}
    gDone++; if (gInFlight) gInFlight--;
    if (a.status) gErrors++;
    t970_audio_stats[13] = (int32_t)gDone; t970_audio_stats[14] = (int32_t)gErrors;
}
// Sends one command and waits for its answer: the basic result for that opcode, or the matching specific answer (want). Returns
// the status (0 success; -60 no answer, -61 not sent) and stores it in t970_audio_cmd[index].
int command(unsigned index, const uint8_t *p, uint32_t n, uint32_t opcode, int want = AprBasic, AprAnswer *out = nullptr) {
    int status = -61;
    if (send(p, n)) {
        status = -60;
        const uint64_t end = nowMs() + 3000;
        AprAnswer a;
        int kind = 0;
        while (nextAnswer(a, kind, end)) {
            if (kind == AprWriteDone) {handleWriteDone(a); continue;}
            if (kind == AprReadDone) {micDone(a); continue;}
            if (kind == AprBasic && a.result_opcode == opcode) {status = (int)a.status; if (out) *out = a; break;}
            if (want != AprBasic && kind == want) {status = kind == AprAdmOpened ? (int)a.status : 0; if (out) *out = a; break;}
            t970_audio_stats[15] = t970_audio_stats[15] + 1;
        }
    }
    {
        const uint32_t n = t970_audio_trace[0];
        if (n < 24) {t970_audio_trace[1 + n] = opcode << 8 | (uint32_t(status) & 0xff); t970_audio_trace[0] = n + 1;}
    }
    if (index < 16) t970_audio_cmd[index] = status;
    if (status) t970_audio_stats[1] = status;
    return status;
}

// ---- the stages ----
bool coreReady() {
    uint8_t p[APR_MAX_PACKET];
    AprAnswer a;
    for (unsigned t = 0; t < 30; ++t) {   // q6core polls the same way while the services come up
        int st = command(99, p, apr_core(p, 0, AVCS_GET_STATE), AVCS_GET_STATE, AprCoreState, &a);
        if (!st && a.opcode == AVCS_GET_STATE_RSP) {
            t970_audio_stats[5] = (int32_t)a.status;
            if (a.status == 1) break;
        }
        IOSleep(100);
    }
    if (t970_audio_stats[5] != 1) return fail(3, t970_audio_stats[1]);
    int st = command(99, p, apr_core(p, 1, AVCS_GET_FWK_VERSION), AVCS_GET_FWK_VERSION, AprCoreFwk, &a);
    if (!st && a.opcode == AVCS_GET_FWK_VERSION_RSP) {
        t970_audio_stats[6] = (int32_t)(a.words[0] << 16 | (a.words[1] & 0xffff));
        t970_audio_stats[7] = (int32_t)(a.words[2] << 16 | (a.words[4] & 0xffff));
    } else if (!command(99, p, apr_core(p, 2, AVCS_GET_VERSIONS), AVCS_GET_VERSIONS, AprCoreVersions, &a) &&
               a.opcode == AVCS_GET_VERSIONS_RSP) {
        t970_audio_stats[6] = (int32_t)a.words[0];
        t970_audio_stats[7] = (int32_t)(a.words[1] & 0xffff);
    }
    t970_audio_stats[1] = 0;
    stage(3);
    return true;
}

const AprTdm kTdm = {kPort, kGroup, kRate, kChannels, kBits, 4, kSlotBits, 0 /* short sync */, 1 /* internal */, 0, 0, 1, 0xf};

void sampleAmps(unsigned column) {
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t v = 0;
        if (ampRead(uint8_t(0x40 + i), IRQ1_STATUS1, &v)) t970_audio_clk[i][column] = v;
        if (ampRead(uint8_t(0x40 + i), IRQ1_STATUS3, &v)) t970_audio_clk[i][column + 1] = v;
    }
}
// Port configuration and start as afe_tdm_port_start: TDM config, slot mapping (v2 on AFE API >= V3, v1 otherwise), start.
int portStart(uint8_t *p, unsigned mode) {
    if (command(5, p, apr_afe_tdm_config(p, 4, &kTdm), AFE_PORT_CMD_SET_PARAM_V2)) return t970_audio_cmd[5];
    if ((mode & 64) || command(6, p, apr_afe_slot_mapping(p, 5, &kTdm, 2), AFE_PORT_CMD_SET_PARAM_V2))
        command(6, p, apr_afe_slot_mapping(p, 5, &kTdm, 1), AFE_PORT_CMD_SET_PARAM_V2);
    return command(7, p, apr_afe_port(p, 6, AFE_PORT_CMD_DEVICE_START, kPort), AFE_PORT_CMD_DEVICE_START);
}
bool gClockOn, gGroupOn, gPortOn, gSampled;
bool afeStart(unsigned mode) {
    uint8_t p[APR_MAX_PACKET];
    AprAnswer a;
    // Votes (the board's lpass_audio_hw_vote / core hw vote clocks): failure here is noted, not fatal.
    command(0, p, apr_afe_hw_vote(p, AFE_HW_BLOCK_MACRO, AFE_HW_BLOCK_MACRO, "LPASS_HW_MACRO"), AFE_CMD_HW_VOTE, AprVote, &a);
    command(1, p, apr_afe_hw_vote(p, AFE_HW_BLOCK_DCODEC, AFE_HW_BLOCK_DCODEC, "LPASS_HW_DCODEC"), AFE_CMD_HW_VOTE, AprVote, &a);
    // The bit clock, service-level as downstream afe_set_lpass_clock_v2 -> afe_set_lpass_clk_cfg (clk-attribute 1, root 0).
    const bool svcClock = !command(2, p, apr_afe_clock(p, 1024, 0, kClkPriTdmIbit, kSclk, 1), AFE_SVC_CMD_SET_PARAM);
    if (!svcClock && command(2, p, apr_afe_clock(p, 1024, kPort, kClkPriTdmIbit, kSclk, 1), AFE_PORT_CMD_SET_PARAM_V2))
        return fail(6, t970_audio_cmd[2]);
    gClockOn = true;
    if (mode & 16) {
        if (!command(3, p, apr_afe_group_tdm(p, 2, &kTdm), AFE_SVC_CMD_SET_PARAM))
            gGroupOn = !command(4, p, apr_afe_group_enable(p, 3, kGroup, 1), AFE_SVC_CMD_SET_PARAM);
    }
    int rc = portStart(p, mode);
    if (rc && svcClock) {   // once more with the clock set on the port as well (Linux q6afe_set_lpass_clock_v2)
        command(99, p, apr_afe_clock(p, 1024, kPort, kClkPriTdmIbit, kSclk, 1), AFE_PORT_CMD_SET_PARAM_V2);
        rc = portStart(p, mode);
    }
    if (!(mode & 4) && !gSampled) {gSampled = true; sampleAmps(0); IOSleep(100); sampleAmps(2);}
    if (rc) return fail(6, rc);
    gPortOn = true;
    t970_audio_stats[1] = 0;
    stage(6);
    return true;
}
uint16_t gCopp = 0xffff;
bool admOpen() {
    uint8_t p[APR_MAX_PACKET];
    AprAnswer a;
    if (command(8, p, apr_adm_open(p, kPort << 16, kPort, kRate, kChannels, kBits), ADM_CMD_DEVICE_OPEN_V5, AprAdmOpened, &a) ||
        a.opcode != ADM_CMDRSP_DEVICE_OPEN_V5 || a.value == 0xff)
        return fail(7, t970_audio_cmd[8] ? t970_audio_cmd[8] : -62);
    gCopp = uint16_t(a.value);
    t970_audio_stats[10] = gCopp;
    stage(7);
    return true;
}
bool gAsmOpen;
bool asmOpen() {
    uint8_t p[APR_MAX_PACKET];
    AprAnswer a;
    if (command(10, p, apr_asm_open_write(p, kSession, kBits), ASM_STREAM_CMD_OPEN_WRITE_V3)) return fail(8, t970_audio_cmd[10]);
    gAsmOpen = true;
    if (command(9, p, apr_adm_map(p, 0, kSession, gCopp), ADM_CMD_MATRIX_MAP_V5)) return fail(8, t970_audio_cmd[9]);
    if (command(11, p, apr_asm_media_format(p, kSession, kRate, kChannels, kBits), ASM_DATA_CMD_MEDIA_FMT_V2)) return fail(8, t970_audio_cmd[11]);
    if (command(12, p, apr_asm_map(p, kSession, gDsp, kPlayBytes), ASM_CMD_MEM_MAP, AprMapped, &a) || a.opcode != ASM_CMDRSP_MEM_MAP)
        return fail(8, t970_audio_cmd[12] ? t970_audio_cmd[12] : -62);
    gHandle = a.value;
    t970_audio_stats[11] = (int32_t)gHandle;
    stage(8);
    return true;
}

// The tone: period k of the run fills slot k % kSlots with frames [k * kPeriodFrames, ...), every channel the same sample, with
// 10 ms ramps at both ends.
// Level ladder (t970-audio bit 128): the tone in four steps of t970-audio-ms / 4, each 12 dB louder than the one before, with a
// 10 ms ramp at both ends of each step and 50 ms of silence between them: a pure sine that is clean when quiet and breaks up only
// when loud is clipping (gain, supply); one that is rough at every level is the data format or the amplifier's processing.
bool gLadder;
const uint32_t kLadder[4] = {500, 2000, 8000, 32000};
void fillPeriod(uint32_t k, uint32_t totalFrames, uint32_t amplitude) {
    volatile uint32_t *to = gBuf.va + (k % kSlots) * (kPeriodBytes / 4);
    const uint32_t ramp = kRate / 100, steps = gLadder ? 4 : 1, span = totalFrames / steps, gap = gLadder ? kRate / 20 : 0;
    for (uint32_t f = 0; f < kPeriodFrames; ++f) {
        const uint32_t n = k * kPeriodFrames + f;
        int32_t s = 0;
        const uint32_t step = n / span, at = n % span, len = span - gap;
        if (n < totalFrames && step < steps && at < len) {
            uint32_t g = gLadder ? kLadder[step] : amplitude;
            if (g > 32767) g = 32767;
            if (at < ramp) g = g * at / ramp;
            else if (len - at < ramp) g = g * (len - at) / ramp;
            s = (int32_t)kSine[n % 48] * (int32_t)g / 32767;
        }
        const uint32_t pair = uint16_t(s) | uint32_t(uint16_t(s)) << 16;
        to[2 * f] = pair; to[2 * f + 1] = pair;
    }
    barrier();
}
bool playTone(const AmpCfg &cfg, uint32_t ms, uint32_t amplitude) {
    uint8_t p[APR_MAX_PACKET];
    if (!(cfg.mode & 4)) for (unsigned i = 0; i < 4; ++i) ampPower(i, true);
    if (command(13, p, apr_asm_session(p, kSession, ASM_SESSION_CMD_RUN_V2), ASM_SESSION_CMD_RUN_V2)) return fail(9, t970_audio_cmd[13]);
    stage(9);
    const uint32_t totalFrames = kRate / 1000 * ms, periods = (totalFrames + kPeriodFrames - 1) / kPeriodFrames + 2;
    uint32_t sent = 0;
    const uint64_t end = nowMs() + ms + 3000;
    bool sampled = false;
    while (gDone < periods && nowMs() < end) {
        while (sent < periods && gInFlight < kInFlight && sent - gDone < kSlots) {
            fillPeriod(sent, totalFrames, amplitude);
            if (!send(p, apr_asm_write(p, kSession, sent, gDsp + (sent % kSlots) * kPeriodBytes, gHandle, kPeriodBytes, sent))) break;
            sent++; gInFlight++;
            t970_audio_stats[12] = (int32_t)sent;
        }
        AprAnswer a;
        int kind = 0;
        if (nextAnswer(a, kind, nowMs() + 200)) {
            if (kind == AprWriteDone) handleWriteDone(a);
            else t970_audio_stats[15] = t970_audio_stats[15] + 1;
        }
        if (!sampled && gDone >= periods / 2 && !(cfg.mode & 4)) {   // halfway: the amplifiers' state while they play
            sampled = true;
            for (unsigned i = 0; i < 4; ++i) {uint32_t v = 0; if (ampRead(uint8_t(0x40 + i), IRQ1_STATUS3, &v)) t970_audio_amp[i][3] = v;}
        }
    }
    return gDone >= periods;
}
// Closes whatever of the session is open (amplifiers off first, so the end makes no click), leaving it ready to open again. The
// SMMU route is not touched.
void closeSession(const AmpCfg &cfg) {
    uint8_t p[APR_MAX_PACKET];
    if (!(cfg.mode & 4)) for (unsigned i = 0; i < 4; ++i) ampPower(i, false);
    if (gAsmOpen) {
        command(99, p, apr_asm_session(p, kSession, ASM_SESSION_CMD_PAUSE), ASM_SESSION_CMD_PAUSE);
        command(14, p, apr_asm_session(p, kSession, ASM_STREAM_CMD_CLOSE), ASM_STREAM_CMD_CLOSE);
    }
    if (gHandle) command(99, p, apr_asm_unmap(p, kSession, gHandle), ASM_CMD_MEM_UNMAP);
    if (gCopp != 0xffff) command(99, p, apr_adm_close(p, kPort << 16, kPort, gCopp), ADM_CMD_DEVICE_CLOSE_V5);
    if (gPortOn) command(15, p, apr_afe_port(p, 7, AFE_PORT_CMD_DEVICE_STOP, kPort), AFE_PORT_CMD_DEVICE_STOP);
    if (gGroupOn) command(99, p, apr_afe_group_enable(p, 8, kGroup, 0), AFE_SVC_CMD_SET_PARAM);
    if (gClockOn) command(99, p, apr_afe_clock(p, 1024, 0, kClkPriTdmIbit, 0, 1), AFE_SVC_CMD_SET_PARAM);
    gAsmOpen = gPortOn = gGroupOn = gClockOn = false;
    gHandle = 0;
    gCopp = 0xffff;
}
void teardown(const AmpCfg &cfg) {closeSession(cfg); unrouteStream();}

// ---- device mode (bit 32): the shared ring and the output stream ----
constexpr uint32_t kOutFrames = T970_AUDIO_PERIOD_FRAMES, kOutBytes = kOutFrames * kChannels * 2, kOutSlots = kPlayBytes / kOutBytes;
constexpr uint32_t kRingFrames = T970_AUDIO_RING_FRAMES, kOutCh = T970_AUDIO_OUT_CHANNELS, kLeadFrames = 30 * kOutFrames;
constexpr uint32_t kStreamInFlight = 4, kKeepMs = 5000;
// The DSP answers a buffer once it has copied it into its own queue, and that queue fills over the first hundreds of ms of a run: a
// lead taken after 8 periods (sheng's) was still growing, crossed a 3-period limit and snapped the period index back again and again
// (int10: sync=9, crackle in real sound while the kernel's own sine, which ignores the timeline, stayed clean). The lead is now taken
// after 1 s and corrected only past 20 periods (the plug-in writes about 600 ms ahead, so a 200 ms drift still finds written frames).
constexpr int64_t kDriftLimit = 20 * kOutFrames;
constexpr uint64_t kSettlePeriods = 100;
IOBufferMemoryDescriptor *gControlMem, *gRingMem, *gRingInMem;
int16_t *gRingIn;   // the capture ring (on by default; t970-audio bit 8388608 turns it off), mono
T970AudioControl *gCtl;
int16_t *gRingOut;
volatile uint32_t gWant, gPublished;
bool gRunning;
uint64_t gNext, gDoneP, gFirst, gStopAsked;
uint32_t gStreamInFlight;
int64_t gBaseLead;
AmpCfg gCfg;

bool setupShared() {
    const IOOptionBits shared = static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryKernelUserShared);
    gControlMem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared, 4096, 4096);
    gRingMem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared, kRingFrames * kOutCh * 2, 4096);
    if (!gControlMem || !gRingMem) return false;
    if (!(gCfg.mode & 8388608)) {   // capture (default on since combo23, Voice Memos worked; bit 8388608 turns it off): the mono input ring
        gRingInMem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, shared, kRingFrames * 2, 4096);
        if (!gRingInMem) return false;
        gRingIn = static_cast<int16_t *>(gRingInMem->getBytesNoCopy());
        memset(gRingIn, 0, kRingFrames * 2);
    }
    gCtl = static_cast<T970AudioControl *>(gControlMem->getBytesNoCopy());
    gRingOut = static_cast<int16_t *>(gRingMem->getBytesNoCopy());
    memset(gCtl, 0, 4096);
    memset(gRingOut, 0, kRingFrames * kOutCh * 2);
    mach_timebase_info_data_t tb;
    clock_timebase_info(&tb);
    T970AudioControl *c = gCtl;
    c->rate = kRate; c->out_channels = kOutCh; c->in_channels = gRingIn ? T970_AUDIO_IN_CHANNELS : 0;
    c->period_frames = kOutFrames; c->ring_frames = kRingFrames; c->lead_frames = kLeadFrames;
    // 128-bit: with the 19.2 MHz timer 1e9 * denom << 32 overflows 64 bits (sheng i535).
    c->ticks_per_frame_q32 = (uint64_t)(((__uint128_t)1000000000ULL * tb.denom << 32) / ((uint64_t)kRate * tb.numer));
    c->out_anchor_host = mach_absolute_time();
    c->in_anchor_host = c->out_anchor_host;
    c->version = T970_AUDIO_VERSION;
    __atomic_store_n(&c->magic, T970_AUDIO_MAGIC, __ATOMIC_RELEASE);
    return true;
}
uint64_t framesNow() {
    const uint64_t now = mach_absolute_time(), anchor = gCtl->out_anchor_host;
    return now > anchor ? (uint64_t)(((__uint128_t)(now - anchor) << 32) / gCtl->ticks_per_frame_q32) : 0;
}
void notePeak(int32_t v) {
    if (v < 0) v = -v;
    if (v > (int32_t)t970_audio_stream[8]) t970_audio_stream[8] = (uint32_t)v;
    if (v > (int32_t)t970_audio_stream[13]) t970_audio_stream[13] = (uint32_t)v;
    if (v >= 30000) t970_audio_stream[12] = t970_audio_stream[12] + 1;
}
// The amplifiers' four interrupt status words (errors: STATUS1 bit 31 amp short, 8 boost short, 15 temperature warning, 17
// temperature error, 6/7 boost OVP/UVP; STATUS3 bit 1 PLL lock) into t970_audio_clk.
void readAmpStatus() {
    if (gCfg.mode & 4) return;
    static const uint32_t regs[4] = {IRQ1_STATUS1, 0x10014, IRQ1_STATUS3, IRQ1_STATUS4};
    for (unsigned i = 0; i < 4; ++i)
        for (unsigned r = 0; r < 4; ++r) {uint32_t v = 0; if (ampRead(uint8_t(0x40 + i), regs[r], &v)) t970_audio_clk[i][r] = v;}
    // TEMP_WARN (STATUS1 bit 15) latches: clear it and release the error as Linux cs35l41_irq does (PROTECT_REL_ERR_IGN 0x2034
    // TEMP_WARN_ERR_RLS 0, 1, 0), so the next read shows whether it came back. Count per amplifier in [audio bytes] tw= (byte i).
    for (unsigned i = 0; i < 4; ++i) {
        if (!(t970_audio_clk[i][0] & 0x8000u)) continue;
        const uint8_t a = uint8_t(0x40 + i);
        ampWrite(a, IRQ1_STATUS1, 0x8000u);
        ampWrite(a, 0x2034, 0); ampWrite(a, 0x2034, 0x20); ampWrite(a, 0x2034, 0);
        const uint32_t c = (t970_audio_bytes[11] >> (8 * i)) & 0xff;
        if (c < 0xff) t970_audio_bytes[11] = t970_audio_bytes[11] + (1u << (8 * i));
    }
    t970_audio_stream[14] = t970_audio_stream[14] + 1;
}
// The DSP takes a buffer into its own queue before it answers, so the distance between the next period and the play position settles
// at whatever the run gives (sheng i534); it is taken once the queue has filled, the published lead follows it, and when the two clocks
// drift three periods from it the next period is set back to it (one click).
void syncPlayback() {
    const uint64_t now = framesNow(), played = gDoneP - gFirst;
    const int64_t lead = (int64_t)(gNext * kOutFrames) - (int64_t)now;
    if (played < kSettlePeriods) return;
    if (played == kSettlePeriods) {
        gBaseLead = lead;
        t970_audio_stream[10] = (uint32_t)lead;
        t970_audio_timing[5] = (int32_t)lead;
        const int64_t want = lead + 2 * (int64_t)kOutFrames;
        gCtl->lead_frames = (uint32_t)(want < (int64_t)kLeadFrames ? kLeadFrames : want > 24000 ? 24000 : want);
        return;
    }
    const int64_t drift = lead - gBaseLead;
    const uint32_t mag = (uint32_t)(drift < 0 ? -drift : drift);
    if (mag > t970_audio_stream[15]) t970_audio_stream[15] = mag;
    if (drift > kDriftLimit || drift < -kDriftLimit) {
        gNext = (uint64_t)((int64_t)now + gBaseLead + kOutFrames / 2) / kOutFrames;
        t970_audio_stream[9] = t970_audio_stream[9] + 1;
    }
}
// Period n (timeline frames n * kOutFrames on) from the ring into DSP slot n % kOutSlots, the ring frames cleared once taken (a frame
// the plug-in did not write in time plays as silence, not as what the ring held one turn earlier).
// t970-audio bit 256, the kernel's own sine through device mode: 5 s after the service is up, the path starts as for the plug-in
// and gSelfFrames frames of a 1 kHz sine replace the ring's samples in these same 10 ms periods: four 1 s steps of 500, 2000, 8000
// and 16000 (-36, -24, -12, -6 dBFS), left channel only in the first half of each step, both after. Clean here but rough from the
// plug-in: the plug-in or the audio server's format; rough here too: the DSP, TDM or amplifier side.
uint32_t gSelfFrames, gSelfAt;
int16_t selfSample(bool left) {
    static const uint32_t levels[4] = {500, 2000, 8000, 16000};
    const uint32_t n = gSelfAt, step = n / kRate, at = n % kRate, len = kRate - kRate / 20, ramp = kRate / 100;
    if (step >= 4 || at >= len || (!left && at < len / 2)) return 0;
    uint32_t g = levels[step];
    if (at < ramp) g = g * at / ramp;
    else if (len - at < ramp) g = g * (len - at) / ramp;
    return (int16_t)((int32_t)kSine[n % 48] * (int32_t)g / 32767);
}
// t970-audio bit 512, the slot sweep: 5 s after device mode serves, seven 1 kHz beeps of 0.7 s at 4000 (-18 dBFS) with 0.3 s
// gaps: slot 0 alone, 1, 2, 3, then 0+2, 1+3, all four. With the default slots the first four are the amplifiers at 0x41 (FL),
// 0x40 (FR), 0x43 (RL), 0x42 (RR) one at a time.
bool gSweep;
int16_t sweepSample(int16_t slot[4]) {
    static const uint8_t masks[7] = {1, 2, 4, 8, 5, 10, 15};
    const uint32_t n = gSelfAt, step = n / kRate, at = n % kRate, len = kRate * 7 / 10, ramp = kRate / 100;
    for (unsigned k = 0; k < 4; ++k) slot[k] = 0;
    if (step >= 7 || at >= len) return 0;
    uint32_t g = 4000;
    if (at < ramp) g = g * at / ramp;
    else if (len - at < ramp) g = g * (len - at) / ramp;
    const int16_t v = (int16_t)((int32_t)kSine[n % 48] * (int32_t)g / 32767);
    for (unsigned k = 0; k < 4; ++k) if (masks[step] & (1u << k)) slot[k] = v;
    return v;
}
// A 2nd-order Butterworth high-pass at 200 Hz on the ring's samples (Q29 coefficients, direct form I, output state with 8 extra
// fraction bits): these small speakers have no protection DSP or tuning here (Samsung runs the amplifiers' CSPL firmware and its own
// EQ), and bass they cannot move breaks up into buzz on real sound while a 1 kHz sine stays clean. t970-audio bit 2048 turns it off.
struct Hpf {int32_t x1, x2; int64_t y1, y2;};
Hpf gHpf[2];
// t970-audio bit 131072: the corner at 400 Hz instead (same Butterworth design).
const int64_t kHpf200[5] = {527023770, -1054047540, 527023770, -1053866911, 517357256};
const int64_t kHpf400[5] = {517357138, -1034714276, 517357138, -1034004770, 498552871};
const int64_t *gHpfC = kHpf200;
int16_t hpf(Hpf &h, int16_t x) {
    const int64_t b0 = gHpfC[0], b1 = gHpfC[1], b2 = gHpfC[2], a1 = gHpfC[3], a2 = gHpfC[4];
    const int64_t acc = b0 * x + b1 * h.x1 + b2 * h.x2 - ((a1 * h.y1 + a2 * h.y2) >> 8);   // Q29
    const int64_t y = acc >> 21;                                                         // Q8
    h.x2 = h.x1; h.x1 = x; h.y2 = h.y1; h.y1 = y;
    const int64_t s = (y + 128) >> 8;
    return (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}
// t970-audio bit 4096, the kernel's chirp through device mode (5 s after the service is up, in place of the ring's samples, both
// channels): 4 s log sweep 100 Hz -> 16 kHz, 0.5 s silence, 2 s of 440 Hz + 3520 Hz; amplitude 2000 (-24 dBFS), 10 ms ramps. The
// plug-in plays the same signal with t970-audio bit 8192 (test_flags bit 1), through the ring. Clean from the kernel but crushed from
// the plug-in: the ring or timeline; crushed from both: the DSP/TDM/amplifier path for wide-band content; clean from both: the audio
// server's mix itself. A sweep that dies or turns back down above some frequency names the rate actually played.
constexpr uint32_t kChirpFrames = 6 * kRate + kRate / 2;
const int16_t kQuarter[257] = {0,201,402,603,804,1005,1206,1407,1608,1809,2009,2210,2410,2611,2811,3012,3212,3412,3612,3811,4011,4210,4410,4609,4808,5007,5205,5404,5602,5800,5998,6195,6393,6590,6786,6983,7179,7375,7571,7767,7962,8157,8351,8545,8739,8933,9126,9319,9512,9704,9896,10087,10278,10469,10659,10849,11039,11228,11417,11605,11793,11980,12167,12353,12539,12725,12910,13094,13279,13462,13645,13828,14010,14191,14372,14553,14732,14912,15090,15269,15446,15623,15800,15976,16151,16325,16499,16673,16846,17018,17189,17360,17530,17700,17869,18037,18204,18371,18537,18703,18868,19032,19195,19357,19519,19680,19841,20000,20159,20317,20475,20631,20787,20942,21096,21250,21403,21554,21705,21856,22005,22154,22301,22448,22594,22739,22884,23027,23170,23311,23452,23592,23731,23870,24007,24143,24279,24413,24547,24680,24811,24942,25072,25201,25329,25456,25582,25708,25832,25955,26077,26198,26319,26438,26556,26674,26790,26905,27019,27133,27245,27356,27466,27575,27683,27790,27896,28001,28105,28208,28310,28411,28510,28609,28706,28803,28898,28992,29085,29177,29268,29358,29447,29534,29621,29706,29791,29874,29956,30037,30117,30195,30273,30349,30424,30498,30571,30643,30714,30783,30852,30919,30985,31050,31113,31176,31237,31297,31356,31414,31470,31526,31580,31633,31685,31736,31785,31833,31880,31926,31971,32014,32057,32098,32137,32176,32213,32250,32285,32318,32351,32382,32412,32441,32469,32495,32521,32545,32567,32589,32609,32628,32646,32663,32678,32692,32705,32717,32728,32737,32745,32752,32757,32761,32765,32766,32767};
int32_t sine32(uint32_t phase) {   // Q15, phase 2^32 = one turn, linear interpolation on a quarter wave
    const uint32_t q = phase >> 30, i = (phase >> 22) & 0xff, frac = (phase >> 6) & 0xffff;
    const uint32_t a = (q & 1) ? 256 - i : i, b = (q & 1) ? a - 1 : a + 1;
    const int32_t v = kQuarter[a] + (int32_t)(((int64_t)(kQuarter[b] - kQuarter[a]) * frac) >> 16);
    return q >= 2 ? -v : v;
}
uint32_t gChirpPhase, gChirpPhase2, gChirpLevel = 2000;
// t970-audio bit 32768, the level ladder: the chirp four times, at 2000, 8000, 16000 and 30000 (-24, -12, -6, -0.8 dBFS), 1.5 s apart
// (32 s in all, 45 s after the service is up, through the amplifiers' volume as iOS set it): the step where it breaks up names a
// level-dependent fault (clipping or overload after the samples) rather than a format one.
const uint32_t kChirpLadder[4] = {2000, 8000, 16000, 30000};
constexpr uint32_t kLadderStep = kChirpFrames + kRate * 3 / 2;
uint64_t gChirpInc;   // Q30 of the per-sample phase increment, during the sweep
int16_t chirpSample(uint32_t n) {
    const uint32_t sweep = 4 * kRate, gapEnd = sweep + kRate / 2, ramp = kRate / 100;
    if (n == 0) {gChirpPhase = gChirpPhase2 = 0; gChirpInc = (uint64_t)8947849 << 30;}   // 100 Hz
    int32_t g = (int32_t)gChirpLevel, v;
    if (n < sweep) {
        if (n % 48 == 0 && n) gChirpInc = (gChirpInc >> 30) * 1075105045ull + (((gChirpInc & ((1u << 30) - 1)) * 1075105045ull) >> 30);
        gChirpPhase += (uint32_t)(gChirpInc >> 30);
        if (n < ramp) g = g * (int32_t)n / (int32_t)ramp; else if (sweep - n < ramp) g = g * (int32_t)(sweep - n) / (int32_t)ramp;
        v = sine32(gChirpPhase) * g / 32767;
    } else if (n < gapEnd) {
        return 0;
    } else {
        const uint32_t at = n - gapEnd, len = kChirpFrames - gapEnd;
        gChirpPhase += 39370534u; gChirpPhase2 += 314964268u;   // 440 Hz, 3520 Hz
        if (at < ramp) g = g * (int32_t)at / (int32_t)ramp; else if (len - at < ramp) g = g * (int32_t)(len - at) / (int32_t)ramp;
        v = (sine32(gChirpPhase) + sine32(gChirpPhase2)) * (g / 2) / 32767;
    }
    return (int16_t)v;
}
bool gChirp;
uint32_t gMarkLeft;      // A/B cycle marker frames still to add (markSample, below)
uint32_t gAbSeg = 0xff;  // the A/B cycle's segment now (0xff: none set in this run)
int16_t markSample();
const T970AudioCapture *gCapture;   // set once the plug-in stored its capture (looked for at every period: it writes ~300 ms ahead)
void findCapture() {
    if (gCapture || !(gCfg.mode & 262144)) return;
    const T970AudioCapture *c = reinterpret_cast<const T970AudioCapture *>(reinterpret_cast<const uint8_t *>(gCtl) + T970_AUDIO_CAP_OFFSET);
    if (__atomic_load_n(&c->state, __ATOMIC_ACQUIRE) != 1) return;
    for (unsigned k = 0; k < 512; ++k) t970_audio_cap_p[k] = c->s[k];
    t970_audio_cap[0] = (uint32_t)c->t; t970_audio_cap[1] = c->write_frames << 16 | (c->at & 0xffff);
    t970_audio_cap[2] = (uint32_t)c->peak;
    t970_audio_cap[5] = (uint32_t)(int32_t)((int64_t)c->t - (int64_t)(gNext * kOutFrames));
    gCapture = c;
}
bool submit(uint64_t n) {
    findCapture();
    uint8_t p[APR_MAX_PACKET];
    const uint32_t slot = (uint32_t)(n % kOutSlots);
    volatile uint32_t *dst = gBuf.va + slot * (kOutBytes / 4);
    uint32_t at = (uint32_t)((n * kOutFrames) % kRingFrames);
    // holes: a silent run of 64+ frames with sound on both sides inside one period (a sound that starts or ends in it is not one;
    // combo1b's holes=7 over 5 runs counted those).
    static bool soundBefore;
    uint32_t zeroRun = 0, holes = 0, nonzero = 0;
    for (uint32_t f = 0; f < kOutFrames; ++f, at = (at + 1) % kRingFrames) {
        int16_t *src = gRingOut + kOutCh * at;
        int16_t l = src[0], r = src[1];
        src[0] = src[1] = 0;
        if (gCapture && t970_audio_cap[3] < 2) {
            const uint64_t pos = n * kOutFrames + f;
            if (pos >= gCapture->t && pos < gCapture->t + T970_AUDIO_CAP_FRAMES) {
                const uint32_t k = (uint32_t)(pos - gCapture->t);
                if (k == 0 || t970_audio_cap[3] == 1) {
                    t970_audio_cap[3] = 1;
                    t970_audio_cap_k[2 * k] = l; t970_audio_cap_k[2 * k + 1] = r;
                    if (k == T970_AUDIO_CAP_FRAMES - 1) t970_audio_cap[3] = 2;
                } else {
                    t970_audio_cap[3] = 3;
                }
            } else if (pos >= gCapture->t + T970_AUDIO_CAP_FRAMES && t970_audio_cap[3] == 0) {
                t970_audio_cap[3] = 3;
            }
        }
        if (gCapture && t970_audio_cap[4] == 0x7fffffff && n * kOutFrames < gCapture->t + 10 * kRate) {
            const volatile int16_t *want = t970_audio_cap_p + 2 * 64;
            if (l == want[2 * gMatch] && r == want[2 * gMatch + 1]) gMatch++;
            else gMatch = (l == want[0] && r == want[1]) ? 1 : 0;
            if (gMatch == 16) t970_audio_cap[4] = (uint32_t)(int32_t)((int64_t)(n * kOutFrames + f) - 79 - (int64_t)gCapture->t);
        }
        if (!(gCfg.mode & 2048)) {l = hpf(gHpf[0], l); r = hpf(gHpf[1], r);}
        // t970-audio bit 65536: the iOS samples at -12 dB (a quarter) before the DSP: a fault that goes away is level-dependent and
        // downstream (DSP, TDM, amplifier, speaker); the amplifiers' volume still applies on top.
        if (gCfg.mode & 65536) {l = (int16_t)(l / 4); r = (int16_t)(r / 4);}
        if (gMarkLeft) {
            const int32_t m = markSample(), ml = l + m, mr = r + m;
            l = (int16_t)(ml > 32767 ? 32767 : ml < -32768 ? -32768 : ml);
            r = (int16_t)(mr > 32767 ? 32767 : mr < -32768 ? -32768 : mr);
        }
        int16_t slot[4] = {l, r, l, r};   // slots 0 FL, 1 FR, 2 RL, 3 RR
        if (gChirp && gSelfFrames) {
            if (gCfg.mode & 32768) {
                const uint32_t step = gSelfAt / kLadderStep, at = gSelfAt % kLadderStep;
                gChirpLevel = kChirpLadder[step < 4 ? step : 3];
                l = r = at < kChirpFrames ? chirpSample(at) : 0;
            } else {
                l = r = chirpSample(gSelfAt);
            }
            gSelfAt++; gSelfFrames--;
            slot[0] = slot[1] = slot[2] = slot[3] = l;
        } else if (gSweep && gSelfFrames) {
            sweepSample(slot);
            gSelfAt++; gSelfFrames--;
        } else if (!gSweep && !gChirp && gSelfFrames) {
            l = selfSample(true); r = selfSample(false); gSelfAt++; gSelfFrames--;
            slot[0] = slot[2] = l; slot[1] = slot[3] = r;
        }
        const uint32_t w0 = uint16_t(slot[0]) | uint32_t(uint16_t(slot[1])) << 16, w1 = uint16_t(slot[2]) | uint32_t(uint16_t(slot[3])) << 16;
        dst[2 * f] = w0;
        dst[2 * f + 1] = w1;
        if (w0 | w1) {if (soundBefore && zeroRun >= 64) holes++; nonzero++; zeroRun = 0; soundBefore = true;}
        else if (++zeroRun >= 4800) soundBefore = false;   // 100 ms of silence ends a sound
        if (f % 16 == 0) {notePeak(l); notePeak(r);}
        {
            static int16_t last[4];
            volatile uint32_t *b = t970_audio_bytes;
            for (unsigned k = 0; k < 4; ++k) {
                const int32_t v = slot[k], m = v < 0 ? -v : v, d = v - last[k];
                if ((uint32_t)m > b[k]) b[k] = (uint32_t)m;
                if (d > 8192 || d < -8192) b[4 + k] = b[4 + k] + 1;
                last[k] = slot[k];
            }
            b[8] = b[8] ^ w0 ^ w1;
            b[9] = b[9] + 1;
            if (w0 | w1) b[10] = b[10] + 1;
        }
    }
    barrier();
    if (holes) {gCtl->out_holes = gCtl->out_holes + holes; t970_audio_timing[7] = (int32_t)gCtl->out_holes;}
    (void)nonzero;
    uint64_t recent = 0;
    nanoseconds_to_absolutetime(100000000ULL, &recent);
    if ((gWant & T970AudioOutputRunning) && mach_absolute_time() - gCtl->out_written_host < recent && gCtl->out_written < (n + 1) * kOutFrames)
        gCtl->out_late = gCtl->out_late + 1;
    // Only once the plug-in wrote into this run: before its first write out_written is still the last run's end, which counted every
    // start's first periods as "not yet" (int15n notyet=28, ahead -37552 with late=0; those periods play silence, the ring is cleared).
    if ((gWant & T970AudioOutputRunning) && gCtl->out_written > gFirst * kOutFrames) {
        const int64_t ahead = (int64_t)gCtl->out_written - (int64_t)((n + 1) * kOutFrames);
        if (ahead < t970_audio_timing[0]) t970_audio_timing[0] = (int32_t)(ahead < -0x7fffffff ? -0x7fffffff : ahead);
        if (ahead > t970_audio_timing[1]) t970_audio_timing[1] = (int32_t)(ahead > 0x7fffffff ? 0x7fffffff : ahead);
        if (ahead < 0) t970_audio_timing[2] = t970_audio_timing[2] + 1;
    }
    if (!send(p, apr_asm_write(p, kSession, (uint32_t)n, gDsp + slot * kOutBytes, gHandle, kOutBytes, (uint32_t)n))) return false;
    t970_audio_stream[3] = t970_audio_stream[3] + 1;
    gCtl->out_taken = (n + 1) * kOutFrames;   // the plug-in counts writes that start before this (t970_audio_timing[3])
    gStreamInFlight++;
    return true;
}
void streamDone(const AprAnswer &a) {
    t970_audio_stream[4] = t970_audio_stream[4] + 1;
    if (a.status) {t970_audio_stream[5] = t970_audio_stream[5] + 1; t970_audio_stats[1] = (int32_t)a.status;}
    if (gStreamInFlight) gStreamInFlight--;
    gDoneP++;
    gCtl->out_frames = (gDoneP - gFirst) * kOutFrames;
    if (gRunning && gDoneP - gFirst == 100) readAmpStatus();   // 1 s into the run, while it plays
    if (gRunning && gDoneP - gFirst == 200) __atomic_or_fetch(&gCtl->test_flags, 1u << 8, __ATOMIC_ACQ_REL);   // steady (capture)
    if (gRunning) {
        syncPlayback();
        while (gStreamInFlight < kStreamInFlight && submit(gNext)) gNext++;
    }
}
void startOutput() {
    if (gRunning) return;
    uint8_t p[APR_MAX_PACKET];
    const int keep = t970_audio_stats[0];
    gServing = false;   // the session's own answers first
    const bool ok = afeStart(gCfg.mode) && admOpen() && asmOpen();
    gServing = true;
    if (ok && !(gCfg.mode & 4)) for (unsigned i = 0; i < 4; ++i) ampPower(i, true);
    if (!ok || command(13, p, apr_asm_session(p, kSession, ASM_SESSION_CMD_RUN_V2), ASM_SESSION_CMD_RUN_V2)) {
        t970_audio_stream[2] = t970_audio_stream[2] + 1;
        t970_audio_stream[11] = (uint32_t)t970_audio_stats[0] << 16 | (uint32_t(t970_audio_stats[1]) & 0xffff);
        closeSession(gCfg);
        t970_audio_stats[0] = keep;
        return;
    }
    t970_audio_stats[0] = keep;
    // The DSP plays the first period at once, so it is the period of the timeline that plays now. The ring is NOT cleared here: the
    // plug-in has been writing the new sound since its StartIO, while the path above came up (AFE, ADM, ASM, amplifiers: hundreds of
    // ms), and clearing it wiped the start of every sound (combo5 capture: the plug-in's frames read back as zeros). Frames left from
    // the last run are cleared when it stops (stopOutput).
    memset(gHpf, 0, sizeof gHpf);
    gNext = gDoneP = gFirst = (framesNow() + kOutFrames - 1) / kOutFrames;
    gCtl->out_taken = gNext * kOutFrames;
    t970_audio_timing[4] = t970_audio_timing[4] + 1;
    gCtl->out_frames = 0;
    gStreamInFlight = 0;
    gRunning = true;
    gAbSeg = 0xff;   // the A/B cycle sets its segment again (and announces it) on each run
    while (gStreamInFlight < kStreamInFlight && submit(gNext)) gNext++;
    __atomic_or_fetch(&gCtl->state, T970AudioOutputRunning, __ATOMIC_ACQ_REL);
    t970_audio_stream[0] = t970_audio_stream[0] + 1;
    t970_audio_stream[13] = 0;
}
void stopOutput() {
    if (!gRunning) return;
    gRunning = false;
    readAmpStatus();
    closeSession(gCfg);
    // Whatever the plug-in wrote beyond the last period taken (the plug-in adds into the ring) must not play in the next run; it is
    // idle now (it stopped kKeepMs ago).
    if (!(__atomic_load_n(&gWant, __ATOMIC_ACQUIRE) & T970AudioOutputRunning)) memset(gRingOut, 0, kRingFrames * kOutCh * 2);
    gStreamInFlight = 0;
    __atomic_and_fetch(&gCtl->state, ~T970AudioOutputRunning, __ATOMIC_ACQ_REL);
    __atomic_and_fetch(&gCtl->test_flags, ~(1u << 8), __ATOMIC_ACQ_REL);
    t970_audio_stream[1] = t970_audio_stream[1] + 1;
}
// ---- capture (on by default; t970-audio bit 8388608 turns it off): the built-in digital microphones into the plug-in's input stream ----
// Live FDT: DMIC0..3 on the LPASS codec (bolero) TX macro, clock/data on LPI GPIO 6/7 (DMIC0/1) and 8/9 (DMIC2/3); their clocks
// are generated in the VA macro's top CSR. Sequence from downstream techpack/audio bolero (tx-macro.c tx_macro_mclk_enable,
// tx_macro_put_dec_enum, tx_macro_enable_dec, bolero-cdc.c bolero_dmic_clk_enable, bolero-clk-rsc.c fs_gen): the TX core and NPL
// clocks (AFE clock ids 0x30c/0x30d, 19.2 MHz) and the macro's 9.6 MHz MCLK, the board's fs-gen sequence (VA MCLK, FS counter,
// TOP_CFG0), the DMIC mux and clock (2.4 MHz: MCLK / 4), decimator 0 at 48 kHz with its HPF settle and unmute, then the codec DMA
// port TX_CODEC_DMA_TX_3 (0xB037, active decimator mask 1), an ADM TX COPP and ASM session 2 reading 10 ms periods into the second
// half of the shared buffer; each period goes to the input ring at the timeline position the plug-in reads.
// t970-audio-mic=N: the DMIC (0..3), default 3: Android's camcorder recording (live mixer dump 2026-11-09) takes DEC0 from DMIC3
// and DEC1 from DMIC1 on TX_CODEC_DMA_TX_3, decimator volume 84 (0 dB), ADC default mode; DMIC3 is the first channel. Its supply
// is the DT's "MIC BIAS External1", the TX macro's sub-mic-bias PM8150A L5 ("ldoc5", 1.8 V): voted on (HPM) for DMIC3 while capturing.
// t970-audio-micgain=dB (decimator volume, -84..40, default 0).
// t970_audio_mic: [0] stage (negative: failed there) [1] error [2] starts [3] stops [4] reads sent [5] reads done [6] read errors
// [7] frames [8] peak [9] resyncs [10] TX0_PATH_CTL [11] VA DMIC CTL [12] TX core id / reads of the macro, [13] AFE port start,
// [14] ADM COPP, [15] capture position minus now (frames) at the last period.
// [16] the mic supply vote's result (ldoc5, DMIC3 only; -100 no RPMh writer, -1 not voted).
extern "C" {volatile int32_t t970_audio_mic[20];}
extern "C" int t970_rpmh_write(const char *name, uint32_t word, uint32_t value) __attribute__((weak_import));
// The plug-in's input reads: reads, reads past the captured end (silence), least and most captured end minus the read's end (frames).
extern "C" {volatile int32_t t970_audio_micp[4];}
constexpr uint64_t kTxMacro = 0x3220000, kVaMacro = 0x3370000, kLpiGpio6 = 0x33c6000;
constexpr uint16_t kMicSession = 2, kMicPort = AFE_PORT_ID_TX_CODEC_DMA_TX_3;
constexpr uint32_t kMicFrames = T970_AUDIO_PERIOD_FRAMES, kMicBytes = kMicFrames * 2, kMicSlots = 64, kMicInFlight = 4;
constexpr uint32_t kTxPathCtl = 0x400, kTxPathCfg0 = 0x404, kTxVolCtl = 0x40c, kTxPathSec2 = 0x418, kTxPathSec7 = 0x42c;
Window gTxW, gVaW, gLpiW;
bool gMicRunning, gMicVoted;
uint16_t gMicCopp = 0xffff;
uint32_t gMicHandle, gMicInFlight, gMicSeq, gMicDmic;
int32_t gMicGain;
uint64_t gMicPos;
void micFail(int stage, int err) {t970_audio_mic[0] = -stage; t970_audio_mic[1] = err;}
void rUpd(Window &w, uint32_t off, uint32_t mask, uint32_t v) {w.va[off / 4] = (w.va[off / 4] & ~mask) | (v & mask); barrier();}
uint32_t rRd(Window &w, uint32_t off) {return w.va[off / 4];}
bool micClocks(bool on) {
    uint8_t p[APR_MAX_PACKET];
    int rc = command(99, p, apr_afe_clock(p, 2048, 0, 0x30c, on ? 19200000 : 0, 1), AFE_SVC_CMD_SET_PARAM);
    const int rc2 = command(99, p, apr_afe_clock(p, 2049, 0, 0x30d, on ? 19200000 : 0, 1), AFE_SVC_CMD_SET_PARAM);
    if (on && (rc || rc2)) {micFail(2, rc ? rc : rc2); return false;}
    return true;
}
// LPI pin n (6..9) as the DMIC function: function 1, 2 mA, data pins pulled down.
void micPin(unsigned n, bool data) {
    volatile uint32_t *cfg = gLpiW.va + (n - 6) * (0x1000 / 4);
    cfg[0] = (data ? 1u : 0u) | 1u << 2;
    barrier();
}
bool startCapture() {
    if (gMicRunning) return true;
    uint8_t p[APR_MAX_PACKET];
    AprAnswer a;
    t970_audio_mic[2] = t970_audio_mic[2] + 1;
    // 1: the LPASS core and audio votes the codec registers need (refused votes are noted, not fatal: the playback path votes too)
    if (!gMicVoted) {
        command(99, p, apr_afe_hw_vote(p, AFE_HW_BLOCK_MACRO, AFE_HW_BLOCK_MACRO, "MIC_MACRO"), AFE_CMD_HW_VOTE, AprVote, &a);
        command(99, p, apr_afe_hw_vote(p, AFE_HW_BLOCK_DCODEC, AFE_HW_BLOCK_DCODEC, "MIC_DCODEC"), AFE_CMD_HW_VOTE, AprVote, &a);
        gMicVoted = true;
    }
    t970_audio_mic[0] = 1;
    t970_audio_mic[16] = -1;
    if (gMicDmic == 3) {
        int r = -100;
        if (&t970_rpmh_write) {
            r = t970_rpmh_write("ldoc5", 0, 1800);
            if (!r) r = t970_rpmh_write("ldoc5", 8, 7);
            if (!r) r = t970_rpmh_write("ldoc5", 4, 1);
        }
        t970_audio_mic[16] = r;   // not fatal: the rail may be on already
        IODelay(2000);
    }
    // 2: the TX core / NPL clocks; no codec register is touched before they run (the macro's bus would not answer)
    if (!micClocks(true)) return false;
    t970_audio_mic[0] = 2;
    if (!gTxW.va && (!mapWindow(gTxW, kTxMacro, 0x1000) || !mapWindow(gVaW, kVaMacro, 0x1000) || !mapWindow(gLpiW, kLpiGpio6, 0x4000))) {
        micFail(3, -12); micClocks(false); return false;
    }
    // 3: fs-gen sequence (VA MCLK, VA FS counter, VA TOP_CFG0 bit 1), the TX MCLK at 9.6 MHz, the decimator's SEC7 default
    rUpd(gVaW, 0x00, 1, 1); rUpd(gVaW, 0x04, 1, 1); rUpd(gVaW, 0x80, 2, 2);
    rUpd(gTxW, 0x90, 1, 1); rUpd(gTxW, 0x00, 1, 1); rUpd(gTxW, 0x04, 1, 1);
    rUpd(gTxW, kTxPathSec7, 0x3f, 0x0a);
    t970_audio_mic[12] = (int32_t)rRd(gVaW, 0xc0);
    t970_audio_mic[0] = 3;
    // 4: pins, DMIC mux (ADC mux 0 CFG0 bits 7:4 = DMIC + 1, CFG1 = MSM DMIC), path CFG0 DMIC select, DMIC clock MCLK/4 on
    const uint32_t d = gMicDmic;
    micPin(6 + (d / 2) * 2, false); micPin(7 + (d / 2) * 2, true);
    rUpd(gTxW, 0x100, 0xf0, (d + 1) << 4);
    rUpd(gTxW, 0x104, 0x03, 0);
    rUpd(gTxW, kTxPathCfg0, 0x86, 0x80);
    rUpd(gVaW, 0x94, 0x80, 0);
    rUpd(gVaW, 0x84 + 4 * (d / 2), 0x0e, 2u << 1);
    rUpd(gVaW, 0x84 + 4 * (d / 2), 0x01, 1);
    t970_audio_mic[11] = (int32_t)rRd(gVaW, 0x84 + 4 * (d / 2));
    // 5: decimator 0: 48 kHz, muted, enabled, HPF gate released, settle, gain
    rUpd(gTxW, kTxPathCtl, 0x0f, 4);
    rUpd(gTxW, kTxPathCtl, 0x10, 0x10);
    rUpd(gTxW, kTxPathCtl, 0x20, 0x20);
    rUpd(gTxW, kTxPathSec2, 0x01, 0);
    IODelay(1000);
    rUpd(gTxW, kTxPathSec2, 0x03, 0x02); rUpd(gTxW, kTxPathSec2, 0x03, 0x00); rUpd(gTxW, kTxPathSec2, 0x03, 0x01);
    IODelay(6000);
    gTxW.va[kTxVolCtl / 4] = (uint32_t)(uint8_t)(int8_t)gMicGain;
    barrier();
    t970_audio_mic[10] = (int32_t)rRd(gTxW, kTxPathCtl);
    t970_audio_mic[0] = 5;
    // 6: the codec DMA TX port
    const int cfg = command(99, p, apr_afe_cdc_dma(p, 2050, kMicPort, kRate, 16, 1, 1), AFE_PORT_CMD_SET_PARAM_V2);
    const int st = cfg ? cfg : command(99, p, apr_afe_port(p, 2051, AFE_PORT_CMD_DEVICE_START, kMicPort), AFE_PORT_CMD_DEVICE_START);
    t970_audio_mic[13] = cfg ? 0x100 | (cfg & 0xff) : st;
    if (st) {micFail(6, st); return false;}
    t970_audio_mic[0] = 6;
    // 7: ADM TX COPP, ASM read session, matrix, PCM encoder, memory, run
    if (command(99, p, apr_adm_open_path(p, (uint32_t)kMicPort << 16 | 2, kMicPort, 2, kRate, 1, 16), ADM_CMD_DEVICE_OPEN_V5, AprAdmOpened, &a) ||
        a.opcode != ADM_CMDRSP_DEVICE_OPEN_V5 || a.value == 0xff) {micFail(7, -62); return false;}
    gMicCopp = uint16_t(a.value);
    t970_audio_mic[14] = gMicCopp;
    int rc = command(99, p, apr_asm_open_read(p, kMicSession, 16), ASM_STREAM_CMD_OPEN_READ_V3);
    if (!rc) rc = command(99, p, apr_adm_map_matrix(p, 2052, ADM_MATRIX_ID_AUDIO_TX, kMicSession, gMicCopp), ADM_CMD_MATRIX_MAP_V5);
    if (!rc) rc = command(99, p, apr_asm_enc_pcm(p, kMicSession, kRate, 1, 16), ASM_STREAM_CMD_SET_ENCDEC_PARAM);
    if (rc) {micFail(8, rc); return false;}
    if (command(99, p, apr_asm_map(p, kMicSession, gDsp + kPlayBytes, kPlayBytes), ASM_CMD_MEM_MAP, AprMapped, &a) || a.opcode != ASM_CMDRSP_MEM_MAP) {
        micFail(9, -62); return false;
    }
    gMicHandle = a.value;
    if ((rc = command(99, p, apr_asm_session(p, kMicSession, ASM_SESSION_CMD_RUN_V2), ASM_SESSION_CMD_RUN_V2))) {micFail(10, rc); return false;}
    gMicRunning = true;
    gMicInFlight = 0;
    gMicPos = framesNow();
    gCtl->in_written = gMicPos;
    for (unsigned i = 0; i < kMicInFlight; ++i) {
        const uint32_t slot = gMicSeq % kMicSlots;
        if (!send(p, apr_asm_read(p, kMicSession, gMicSeq, gDsp + kPlayBytes + slot * kMicBytes, gMicHandle, kMicBytes, gMicSeq))) break;
        gMicSeq++; gMicInFlight++;
        t970_audio_mic[4] = t970_audio_mic[4] + 1;
    }
    __atomic_or_fetch(&gCtl->state, T970AudioInputRunning, __ATOMIC_ACQ_REL);
    IOSleep(100);                                     // the DMIC and HPF settle with the decimator muted, then unmute
    rUpd(gTxW, kTxPathCtl, 0x10, 0);
    t970_audio_mic[10] = (int32_t)rRd(gTxW, kTxPathCtl);
    t970_audio_mic[0] = 11;
    return true;
}
void stopCapture() {
    uint8_t p[APR_MAX_PACKET];
    const bool wasRunning = gMicRunning;
    gMicRunning = false;
    __atomic_and_fetch(&gCtl->state, ~T970AudioInputRunning, __ATOMIC_ACQ_REL);
    if (wasRunning) {
        command(99, p, apr_asm_session(p, kMicSession, ASM_SESSION_CMD_PAUSE), ASM_SESSION_CMD_PAUSE);
        command(99, p, apr_asm_session(p, kMicSession, ASM_STREAM_CMD_CLOSE), ASM_STREAM_CMD_CLOSE);
    }
    if (gMicHandle) command(99, p, apr_asm_unmap(p, kMicSession, gMicHandle), ASM_CMD_MEM_UNMAP);
    if (gMicCopp != 0xffff) command(99, p, apr_adm_close(p, (uint32_t)kMicPort << 16 | 2, kMicPort, gMicCopp), ADM_CMD_DEVICE_CLOSE_V5);
    if (t970_audio_mic[0] >= 6 || t970_audio_mic[0] <= -7)
        command(99, p, apr_afe_port(p, 2053, AFE_PORT_CMD_DEVICE_STOP, kMicPort), AFE_PORT_CMD_DEVICE_STOP);
    if (gTxW.va && (t970_audio_mic[0] >= 3 || t970_audio_mic[0] <= -4)) {
        rUpd(gTxW, kTxPathCtl, 0x10, 0x10);
        rUpd(gTxW, kTxPathCtl, 0x20, 0);
        rUpd(gTxW, kTxPathCfg0, 0x06, 0);
        rUpd(gVaW, 0x84 + 4 * (gMicDmic / 2), 0x01, 0);
        rUpd(gTxW, 0x04, 1, 0); rUpd(gTxW, 0x00, 1, 0);
    }
    if (t970_audio_mic[0] >= 2 || t970_audio_mic[0] <= -3) micClocks(false);
    if (t970_audio_mic[16] == 0 && &t970_rpmh_write) t970_rpmh_write("ldoc5", 4, 0);
    gMicHandle = 0; gMicCopp = 0xffff; gMicInFlight = 0;
    t970_audio_mic[3] = t970_audio_mic[3] + 1;
}
// A captured period: its frames go to the input ring at the capture position (the timeline frame the first one was taken at); the
// position follows the DSP and is set back to now when the two drift more than 200 ms apart.
void micDone(const AprAnswer &a) {
    uint8_t p[APR_MAX_PACKET];
    if (gMicInFlight) gMicInFlight--;
    t970_audio_mic[5] = t970_audio_mic[5] + 1;
    if (a.status) t970_audio_mic[6] = t970_audio_mic[6] + 1;
    if (!gMicRunning) return;
    const uint64_t base = gDsp + kPlayBytes, at = a.address >= base ? a.address - base : 0;
    const uint32_t bytes = a.words[0], offset = a.words[1];
    if (!a.status && at + offset + bytes <= kPlayBytes && bytes <= kMicBytes) {
        const volatile int16_t *src = reinterpret_cast<volatile int16_t *>(reinterpret_cast<volatile uint8_t *>(gBuf.va) + kPlayBytes + at + offset);
        const uint32_t frames = bytes / 2;
        const int64_t now = (int64_t)framesNow(), drift = (int64_t)gMicPos - now;
        if (drift > 9600 || drift < -9600) {gMicPos = (uint64_t)(now - (int64_t)kMicFrames); t970_audio_mic[9] = t970_audio_mic[9] + 1;}
        int32_t peak = t970_audio_mic[8];
        for (uint32_t f = 0; f < frames; ++f) {
            const int16_t v = src[f];
            gRingIn[(gMicPos + f) % kRingFrames] = v;
            const int32_t m = v < 0 ? -(int32_t)v : v;
            if (m > peak) peak = m;
        }
        t970_audio_mic[8] = peak;
        gMicPos += frames;
        __atomic_store_n(&gCtl->in_written, gMicPos, __ATOMIC_RELEASE);
        t970_audio_mic[7] = t970_audio_mic[7] + (int32_t)frames;
        t970_audio_mic[15] = (int32_t)((int64_t)gMicPos - (int64_t)framesNow());
    }
    while (gMicInFlight < kMicInFlight) {
        const uint32_t slot = gMicSeq % kMicSlots;
        if (!send(p, apr_asm_read(p, kMicSession, gMicSeq, base + slot * kMicBytes, gMicHandle, kMicBytes, gMicSeq))) break;
        gMicSeq++; gMicInFlight++;
        t970_audio_mic[4] = t970_audio_mic[4] + 1;
    }
}
// The output volume: the plug-in's setting (milli-dB) as the amplifiers' AMP_VOL_PCM, 11-bit two's complement in 0.125 dB steps
// (+12..-102 dB, 0x400 mutes), applied with their 0.5 ms soft ramp; the samples stay at full 16-bit resolution (int15n: scaling them
// to -57 dB left 3-4 bits, a harsh sound). Until the plug-in publishes a setting the amplifiers stay at 0 dB.
uint32_t gVolSeq, gVolCode;
uint32_t volumeCode(int32_t mdb) {
    if (mdb <= T970_AUDIO_MUTE_MDB) return 0x400;
    if (mdb > 0) mdb = 0;
    int32_t steps = (mdb * 8 - 500) / 1000;   // 1/8 dB, rounded
    if (steps < -816) steps = -816;
    return (uint32_t)steps & 0x7ff;
}
int32_t gVolTrim;    // milli-dB added to the plug-in's setting (the A/B cycle's -6 dB segment)
uint32_t gAmpMute;   // amplifiers (bit i = 0x40 + i) held muted (the format cycle's one-pair segments)
bool gVolForce;      // apply again even without a new setting
void applyVolume() {
    const uint32_t seq = __atomic_load_n(&gCtl->out_volume_seq, __ATOMIC_ACQUIRE);
    if ((seq == gVolSeq && !gVolForce) || (gCfg.mode & 4) || !seq) return;
    gVolSeq = seq;
    gVolForce = false;
    const int32_t mdb = gCtl->out_volume_mdb;
    const uint32_t code = volumeCode(mdb <= T970_AUDIO_MUTE_MDB ? mdb : mdb + gVolTrim);
    if (code == gVolCode) return;
    unsigned ok = 0;
    for (unsigned i = 0; i < 4; ++i)
        ok += ampUpdate(uint8_t(0x40 + i), AMP_DIG_VOL, 0x7ffu << 3, ((gAmpMute >> i) & 1 ? 0x400u : code) << 3) ? 1 : 0;
    gVolCode = code;
    t970_audio_vol[7] = (int32_t)(code | ok << 12 | (seq & 0xffff) << 16);
}
// t970-audio bit 524288, the A/B cycle: while the path plays, every 20 s the next of four amplifier set-ups, announced by 1..4 quiet
// beeps (1 kHz, 60 ms, amplitude 1500, added to the sound) so the listener can say which segment sounded clean:
//   1 boost as after reset (BST_CTL_SEL class-H, BST_EN on)   2 fixed VBST 8.0 V (BST_CTL_SEL port, VCTRL1 0x6e)
//   3 boost off (bypass, VBST = VP)                            4 as 1, amplifier digital volume 6 dB lower
// Each change mutes the amplifiers (0.5 ms ramp) first; the boost enable changes only with the amplifiers powered down.
// t970_audio_ab: segment (1..4), changes, ms since the cycle began (the first run) at the last change, failed changes.
extern "C" {volatile uint32_t t970_audio_ab[4];}
uint64_t gAbStart;
uint32_t gMarkAt, gMarkBeeps;
void abApply(uint32_t seg) {
    bool ok = true;
    for (unsigned i = 0; i < 4; ++i) ok = ampUpdate(uint8_t(0x40 + i), AMP_DIG_VOL, 0x7ffu << 3, 0x400u << 3) && ok;
    IOSleep(2);
    const uint32_t bstEn = seg == 2 ? 1u : 2u;
    uint32_t cur = 0;
    if (ampRead(0x40, PWR_CTRL2, &cur) && ((cur >> 4) & 3) != bstEn) {
        for (unsigned i = 0; i < 4; ++i) ampPower(i, false);
        for (unsigned i = 0; i < 4; ++i) ok = ampUpdate(uint8_t(0x40 + i), PWR_CTRL2, 0x30, bstEn << 4) && ok;
        for (unsigned i = 0; i < 4; ++i) ok = ampPower(i, true) && ok;
    }
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t a = uint8_t(0x40 + i);
        if (seg == 1) {ok = ampUpdate(a, 0x3804, 0x3, 0) && ampUpdate(a, 0x3800, 0xff, 0x6e) && ok;}
        else ok = ampUpdate(a, 0x3804, 0x3, 1) && ok;
    }
    gVolTrim = seg == 3 ? -6000 : 0;
    gVolForce = true;
    gVolCode = 0xffffffffu;
    if (__atomic_load_n(&gCtl->out_volume_seq, __ATOMIC_ACQUIRE)) applyVolume();
    else for (unsigned i = 0; i < 4; ++i) ampUpdate(uint8_t(0x40 + i), AMP_DIG_VOL, 0x7ffu << 3, 0);   // no setting yet: 0 dB
    t970_audio_ab[0] = seg + 1; t970_audio_ab[1] = t970_audio_ab[1] + 1; t970_audio_ab[2] = (uint32_t)(nowMs() - gAbStart);
    if (!ok) t970_audio_ab[3] = t970_audio_ab[3] + 1;
    gMarkBeeps = seg + 1; gMarkAt = 0; gMarkLeft = (seg + 1) * (kRate * 15 / 100);
}
// t970-audio bit 1048576, the format cycle (in place of the A/B cycle): every 15 s, announced by 1..6 beeps, boost as after reset:
//   1 reference (as configured)                 2 only the amplifiers at 0x40 and 0x41 (0x42, 0x43 muted)
//   3 only 0x42 and 0x43                         4 SP_RX_WL 24 (24-bit words read from each 32-bit slot instead of 16)
//   5 the other bit-clock edge than configured (SCLK_INV toggled)   6 PCM polarity inverted on 0x42 and 0x43 (AMP_INV_PCM)
// Changes to the serial port are made with the amplifiers powered down (muted first); each segment starts from the reference.
void fmtApply(uint32_t seg) {
    bool ok = true;
    for (unsigned i = 0; i < 4; ++i) ok = ampUpdate(uint8_t(0x40 + i), AMP_DIG_VOL, 0x7ffu << 3, 0x400u << 3) && ok;
    IOSleep(2);
    for (unsigned i = 0; i < 4; ++i) ampPower(i, false);
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t a = uint8_t(0x40 + i);
        ok = ampUpdate(a, SP_RX_WL, 0x3f, seg == 3 ? 24 : kBits) && ok;
        const uint32_t sclkInv = (gCfg.mode & 2097152) ? 0 : 0x40;   // segment 5: the other bit-clock edge than configured
        ok = ampUpdate(a, SP_FORMAT, 0x40, seg == 4 ? (sclkInv ^ 0x40) : sclkInv) && ok;
        ok = ampUpdate(a, AMP_DIG_VOL, 1u << 14, seg == 5 && i >= 2 ? 1u << 14 : 0) && ok;
    }
    for (unsigned i = 0; i < 4; ++i) ok = ampPower(i, true) && ok;
    gAmpMute = seg == 1 ? 0xcu : seg == 2 ? 0x3u : 0;
    gVolForce = true;
    gVolCode = 0xffffffffu;
    if (__atomic_load_n(&gCtl->out_volume_seq, __ATOMIC_ACQUIRE)) applyVolume();
    else for (unsigned i = 0; i < 4; ++i) ampUpdate(uint8_t(0x40 + i), AMP_DIG_VOL, 0x7ffu << 3, ((gAmpMute >> i) & 1 ? 0x400u : 0) << 3);
    t970_audio_ab[0] = 0x10 | (seg + 1); t970_audio_ab[1] = t970_audio_ab[1] + 1; t970_audio_ab[2] = (uint32_t)(nowMs() - gAbStart);
    if (!ok) t970_audio_ab[3] = t970_audio_ab[3] + 1;
    gMarkBeeps = seg + 1; gMarkAt = 0; gMarkLeft = (seg + 1) * (kRate * 15 / 100);
}
void abStep() {
    const bool fmt = (gCfg.mode & 1048576) != 0;
    if (!(gCfg.mode & (524288 | 1048576)) || (gCfg.mode & 4) || !gRunning) return;
    if (!gAbStart) gAbStart = nowMs();
    const uint32_t seg = fmt ? (uint32_t)((nowMs() - gAbStart) / 15000) % 6 : (uint32_t)((nowMs() - gAbStart) / 20000) % 4;
    if (seg != gAbSeg) {gAbSeg = seg; if (fmt) fmtApply(seg); else abApply(seg);}
}
// The marker beeps, added to the frame being sent: 150 ms per beep, 60 ms of tone with 5 ms ramps.
int16_t markSample() {
    if (!gMarkLeft) return 0;
    const uint32_t n = gMarkAt++, in = n % (kRate * 15 / 100), on = kRate * 6 / 100, ramp = kRate / 200;
    gMarkLeft--;
    if (in >= on) return 0;
    int32_t g = 1500;
    if (in < ramp) g = g * (int32_t)in / (int32_t)ramp; else if (on - in < ramp) g = g * (int32_t)(on - in) / (int32_t)ramp;
    return (int16_t)((int32_t)kSine[n % 48] * g / 32767);
}
// The plug-in's side, from the control page, for the log: loads, unknown properties, volume x1000, volume sets, mix peak, writes,
// frames written, gaps, overlaps, host clock (Hz), its result, IO thread priority.
void noteHal() {
    const T970AudioDiag *d = reinterpret_cast<const T970AudioDiag *>(reinterpret_cast<const uint8_t *>(gCtl) + T970_AUDIO_DIAG_OFFSET);
    volatile uint32_t *h = t970_audio_hal;
    h[0] = d->loads; h[1] = d->count; h[2] = d->volume_milli; h[3] = d->volume_sets; h[4] = d->mix_peak; h[5] = d->writes;
    h[6] = d->frames_total; h[7] = d->gaps; h[8] = d->overlaps; h[9] = d->host_clock; h[10] = (uint32_t)d->host_clock_result;
    h[11] = (uint32_t)d->io_pri;
    t970_audio_timing[3] = (int32_t)d->in_misses;
    t970_audio_cap[6] = (uint32_t)d->ahead_min; t970_audio_cap[7] = (uint32_t)d->ahead_max;
    t970_audio_micp[0] = (int32_t)d->mic_reads; t970_audio_micp[1] = (int32_t)d->mic_short;
    t970_audio_micp[2] = d->mic_behind_min; t970_audio_micp[3] = d->mic_behind_max;
    volatile int32_t *v = t970_audio_vol;
    v[0] = d->vol_kind; v[1] = d->vol_raw_milli; v[2] = d->vol_element; v[3] = (int32_t)d->gain_now; v[4] = (int32_t)d->gain_min;
    v[5] = (int32_t)d->gain_max; v[6] = (int32_t)d->vol_history_count;
    for (unsigned k = 0; k < 8; ++k) v[8 + k] = (int32_t)d->vol_history[k];
    t970_audio_stream[6] = gCtl->out_late;
    t970_audio_stream[7] = gCtl->lead_frames;
}
bool publishService() {
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return false;
    t970_audio_service_static_init();
    if (OSMetaClass::postModLoad(handle)) return false;
    t970_audio_service_start();
    return true;
}
void serveDevice(const AmpCfg &cfg) {
    gCfg = cfg;
    {
        unsigned dmic = 3; int gainDb = 0;
        PE_parse_boot_argn("t970-audio-mic", &dmic, sizeof dmic);
        PE_parse_boot_argn("t970-audio-micgain", &gainDb, sizeof gainDb);
        gMicDmic = dmic & 3;
        gMicGain = gainDb < -84 ? -84 : gainDb > 40 ? 40 : gainDb;
    }
    if (!setupShared()) {fail(20, -12); return;}
    gServing = true;
    __atomic_store_n(&gPublished, 1u, __ATOMIC_RELEASE);
    if (!publishService()) {__atomic_store_n(&gPublished, 0u, __ATOMIC_RELEASE); fail(20, -7); return;}
    // t970-audio bit 1024: the plug-in replaces the audio server's mix with its own 1 kHz sine at -18 dBFS (volume not applied), on the
    // sample times the audio server gives it: clean then means the timeline and ring are right and the mix's format is not.
    gCtl->test_flags = ((gCfg.mode & 1024) ? 1u : 0u) | ((gCfg.mode & 8192) ? 2u : 0u) | ((gCfg.mode & 262144) ? 4u : 0u);
    t970_audio_timing[6] = (int32_t)gCtl->test_flags;
    __atomic_or_fetch(&gCtl->state, T970AudioOutputReady, __ATOMIC_ACQ_REL);
    t970_audio_stats[1] = 0;
    stage(20);
    uint64_t noted = 0;
    // The chirp (4096) 45 s after the service is up (the lock screen is up by then); the plug-in's chirp (8192) comes at 75 s or later,
    // so one boot can play both in turn.
    if (gCfg.mode & 131072) gHpfC = kHpf400;
    const uint64_t selfAt = (gCfg.mode & (768 | 4096 | 32768)) ? nowMs() + ((gCfg.mode & (4096 | 32768)) ? 45000 : 5000) : 0;
    bool selfDone = !(gCfg.mode & (768 | 4096 | 32768));
    for (;;) {
        if (!selfDone && nowMs() >= selfAt) {
            selfDone = true; gSelfAt = 0;
            gChirp = (gCfg.mode & (4096 | 32768)) != 0;
            gSweep = !gChirp && (gCfg.mode & 512) != 0;
            gSelfFrames = gChirp ? ((gCfg.mode & 32768) ? 4 * kLadderStep : kChirpFrames) : gSweep ? 7 * kRate + kRate / 10 : 4 * kRate + kRate / 10;
        }
        if (gSweep && !gSelfFrames) gSweep = false;
        if (gChirp && !gSelfFrames) gChirp = false;
        const uint32_t want = __atomic_load_n(&gWant, __ATOMIC_ACQUIRE) | (gSelfFrames ? T970AudioOutputRunning : 0u);
        // The path stays up kKeepMs after the plug-in stops: the amplifiers' power-up would cut the start of the next sound.
        if (want & T970AudioOutputRunning) {gStopAsked = 0; if (!gRunning) startOutput();}
        else if (gRunning && !gStopAsked) gStopAsked = nowMs();
        else if (gRunning && nowMs() - gStopAsked >= kKeepMs) {gStopAsked = 0; stopOutput();}
        // Capture follows the plug-in's input requests at once (no amplifiers to keep warm); a failed start is retried only on the
        // next request.
        if (gRingIn) {
            static uint32_t lastWant;
            const uint32_t wantIn = want & T970AudioInputRunning;
            if (wantIn && !lastWant && !gMicRunning) {if (!startCapture()) stopCapture();}
            else if (!wantIn && lastWant) stopCapture();
            lastWant = wantIn;
        }
        AprAnswer a;
        int kind = 0;
        if (nextAnswer(a, kind, nowMs() + (gRunning ? 100 : 1000))) {
            if (kind == AprWriteDone) streamDone(a);
            else if (kind == AprReadDone) micDone(a);
            else t970_audio_stats[15] = t970_audio_stats[15] + 1;
        }
        applyVolume();
        abStep();
        if (nowMs() - noted >= 1000) {noted = nowMs(); noteHal();}
    }
}


void audioThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "t970-audio");
    const uint64_t began = nowMs();
    // Amplifier PCM gain (AMP_GAIN_PCM, code n = n + 0.5 dB): 12 since audio14 ("good, just a bit quiet" at 6 in combo7). Android
    // runs 17 behind its CSPL limiter; without it 12 keeps 5 dB of that headroom. t970-audio-gain=N overrides (0..20).
    unsigned mode = 0, gain = 12, ms = 1500, amplitude = 2000;
    uint64_t pa = kDefaultPa;
    PE_parse_boot_argn("t970-audio", &mode, sizeof mode);
    PE_parse_boot_argn("t970-audio-gain", &gain, sizeof gain);
    if (gain > 20) gain = 20;
    PE_parse_boot_argn("t970-audio-ms", &ms, sizeof ms);
    PE_parse_boot_argn("t970-audio-amp", &amplitude, sizeof amplitude);
    PE_parse_boot_argn("t970-audio-pa", &pa, sizeof pa);
    PE_parse_boot_argn("t970-audio-slots", &gAmpSlots, sizeof gAmpSlots);
    if ((mode & 128) && !PE_parse_boot_argn("t970-audio-ms", &ms, sizeof ms)) ms = 4000;
    if (amplitude > 32767) amplitude = 32767;
    if (ms > 10000) ms = 10000;
    const AmpCfg cfg = {mode, gain};

    // 2: the DSP boots on its own thread (firmware from the Data volume, up to about two minutes); wait for the channel.
    const uint64_t upBy = nowMs() + 240000;
    while (!__atomic_load_n(&gUp, __ATOMIC_ACQUIRE) && nowMs() < upBy) {
        uint64_t deadline;
        clock_interval_to_deadline(1000, kMillisecondScale, &deadline);
        IOLockLock(gLock);
        if (!gUp) IOLockSleepDeadline(gLock, &gCount, deadline, THREAD_UNINT);
        IOLockUnlock(gLock);
    }
    if (!gUp) {fail(2, -60); return;}
    t970_audio_stats[8] = (int32_t)(nowMs() - began);
    stage(2);
    const uint64_t upAt = nowMs();
    if (!coreReady()) return;

    // 4: the buffer and the stream route.
    if ((pa & 0xfff) || !mapWindow(gBuf, pa, kBufferBytes, kIOMapWriteCombineCache)) {fail(4, -12); return;}
    gPa = pa;
    gDsp = pa | uint64_t(kStream & 0xf) << 32;
    for (uint32_t i = 0; i < kBufferBytes / 4; ++i) gBuf.va[i] = 0;
    barrier();
    if (int rc = routeStream(pa)) {t970_audio_dma[0] = uint32_t(rc); unrouteStream(); fail(4, rc); return;}
    stage(4);

    // 5: pins and amplifiers.
    if (!setupPins()) {fail(5, -12); unrouteStream(); return;}
    unsigned found = 0;
    if (!(mode & 4)) for (unsigned i = 0; i < 4; ++i) if (ampConfigure(i, cfg)) found |= 1u << i;
    __atomic_store_n(&gAmpsFound, found, __ATOMIC_RELEASE);
    if (!(mode & 4) && !found) {fail(5, -5); unrouteStream(); return;}
    stage(5);

    // 6..9: the boot tone (bit 1)
    gLadder = (mode & 128) != 0;
    if (mode & 129) {
        bool ok = afeStart(mode) && admOpen() && asmOpen();
        t970_audio_stats[9] = (int32_t)(nowMs() - upAt);
        if (ok && playTone(cfg, ms, amplitude)) stage(10);
        else if (ok) fail(9, gDone ? -63 : -60);
        if (t970_audio_stats[0] == 10) t970_audio_stats[1] = 0;
        if (!(mode & 32)) {
            if (!(mode & 8)) {teardown(cfg); unmap(gBuf); unmap(gSmmu);}
            return;
        }
        closeSession(cfg);
    }
    // 20: the iOS output device (bit 32)
    serveDevice(cfg);
}
}  // namespace

extern "C" void t970_audio_start(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("t970-audio", &mode, sizeof mode) || !(mode & 1953) || t970_audio_stats[0]) return;
    for (unsigned i = 0; i < 16; ++i) t970_audio_cmd[i] = 99;
    gLock = IOLockAlloc();
    gRingLock = IOSimpleLockAlloc();
    if (!gLock || !gRingLock || !&sheng_adsp_channel || !&sheng_adsp_send) {fail(1, -7); return;}
    // Registered now, before the ADSP thread loads the firmware (see stage 1 above). 1 KiB receive intents: the framework version
    // answer lists every service.
    static const ShengAdspClient client = {dspState, dspReceive};
    gChannel = sheng_adsp_channel("apr_audio_svc", 1024, 12, 0, &client, nullptr);
    if (!gChannel) {fail(1, -12); return;}
    stage(1);
    thread_t thread = nullptr;
    if (kernel_thread_start(audioThread, nullptr, &thread) != KERN_SUCCESS) {fail(1, -7); return;}
    thread_deallocate(thread);
}

extern "C" IOBufferMemoryDescriptor *t970_audio_memory(uint32_t type) {
    if (!__atomic_load_n(&gPublished, __ATOMIC_ACQUIRE)) return nullptr;
    IOBufferMemoryDescriptor *m = type == T970AudioMemoryControl ? gControlMem : type == T970AudioMemoryOutput ? gRingMem :
                                  type == T970AudioMemoryInput ? gRingInMem : nullptr;
    if (m) m->retain();
    return m;
}
extern "C" int t970_audio_request(uint32_t selector) {
    if (!__atomic_load_n(&gPublished, __ATOMIC_ACQUIRE)) return -1;
    switch (selector) {
    case T970AudioStartOutput: __atomic_or_fetch(&gWant, T970AudioOutputRunning, __ATOMIC_ACQ_REL); break;
    case T970AudioStopOutput: __atomic_and_fetch(&gWant, ~T970AudioOutputRunning, __ATOMIC_ACQ_REL); break;
    case T970AudioStartInput:
        if (!gRingIn) return 0;   // capture off (t970-audio bit 8388608): accepted, nothing to do
        __atomic_or_fetch(&gWant, T970AudioInputRunning, __ATOMIC_ACQ_REL); break;
    case T970AudioStopInput:
        if (!gRingIn) return 0;
        __atomic_and_fetch(&gWant, ~T970AudioInputRunning, __ATOMIC_ACQ_REL); break;
    default: return -1;
    }
    IOLockWakeup(gLock, &gCount, false);
    return 0;
}
