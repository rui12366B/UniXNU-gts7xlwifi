// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi S Pen: the Wacom EMR digitizer (W9021, "wacom,w90xx") at I2C 0x56 on QUP SE17 (live FDT /soc/i2c@88c000/wacom@56).
//
//   bus        QUP SE17 pins GPIO 52 (SDA) and 53 (SCL), TLMM SOUTH (pinctrl-kona.c PINGROUP(52/53, SOUTH, qup17)), driven here as a
//              GPIO I2C master like the touch core (T970FtsTouch.cpp): no QUP clock, GPI or SMMU is touched
//   irq        GPIO 136, WEST, active low while a packet is pending (wacom,irq-gpio; IRQF_TRIGGER_LOW), polled
//   pdct       GPIO 7, NORTH, pen detect: high = pen in its garage (wacom,pdct-gpio; wacom_interrupt_pdct)
//   fwe        GPIO 6, NORTH, firmware-update enable, held low for normal mode (wacom_compulsory_flash_mode(false) at probe)
//   power      wacom_avdd 3.3 V, a fixed regulator enabled by GPIO 93 (NORTH, active high, regulator-boot-on)
//   hall       GPIO 110, NORTH, the pen-garage hall sensor (hall_wacom, hall,gpio_wacom_cover), read only
//   protocol   Samsung's driver (github.com/ianmacd/gts7xl drivers/input/wacom/wacom_i2c.c, read for reference only): plain
//              I2C reads, no register address. Query = 31 bytes, query block at 16 with header 0x0f, max x/y, max pressure, fw
//              version, MPU/BL version, max tilt, max height. Event = 17 bytes on IRQ low, packet type = byte0 & 15: 1 coordinate
//              (byte0 bit7 ready, bit6 rubber, bit5 side button, bit4 tip; x = b1:b2, y = b3:b4, pressure = (b5&15):b6, height
//              b7, tilt x/y = s8 b8/b9), 3 AOP, 13 notification, 14 reply. Commands are single bytes (0x2d survey exit, 0x31
//              sample rate start).
//
// None of these GPIOs is on the TrustZone-secure list of this board (20-23, 28-31, 40-43); securePin() refuses them anyway.
//
// t970-pen=<bits> (off by default), started from native_legacy_display_late_probe:
//   1    stage 1: map, power (GPIO 93 high if it is not), FWE low, read the query block and the firmware version
//   2    stage 2: stream: poll the IRQ line, read packets, keep counters and the last raw packets for the boot log
//   4    stage 3: publish to HID (needs offline-hid-input=1): a Stylus collection inside the touch screen's own report 3 (NativeHidInput's
//        touch device; combo23/24b showed a separate pen service never reaches backboardd)
//   8    stage 3 fallback: the pen tip as one more finger in the touch screen's report 3 (no descriptor change) instead of a stylus
//   16   stage 4: pen garage: log PDCT / hall changes (counted always; this adds the text lines)
//   32   power-cycle the digitizer first (GPIO 93 low 100 ms, high, 200 ms; wacom_reset_hw)
//   64   do not send survey exit (0x2d) + sample rate start (0x31) before streaming
//   128  log every coordinate packet (default: the first 16, then every 256th)
//   256  separate device (2048) only: application collection Pen (0x02) instead of Digitizer (0x01); combo23's layout
//   512  separate device only: stamp the event driver PrimaryUsage 4 (touch screen)
//   1024 separate device only: no DigitizerCollectionDispatch (the stylus event on its own instead of as a collection's child)
//   2048 the old separate HID pen device (HidInputProbe.cpp, combo23/24b) instead of the touch device's report
//   4096 separate device only: application collection Stylus (0x20), backboardd's pencil digitizer pair (0x0D/0x20)
//   8192 separate device only: keyboard collection first (like the working touch device), stamped to 0x0D/0x20 (or 1)
// t970-pen-xform=<bits>: 1 swap the raw axes, 2 mirror x, 4 mirror y, applied to the raw sensor axes before scaling to the frame
// buffer (2800 x 1752 landscape). Default 0: the raw axes already match the frame buffer (raw x along the long side, raw y down).
// main3 (default was 4, from Android's y-invert + xy-switch and the touch core's swap) put the pen point mirrored top/bottom on the
// device; the user confirmed a vertical flip is right, so Android's y-invert does not carry over to this frame buffer.
// t970-pen-i2c=<us>: half clock period in microseconds (default 4, about 120 kHz).
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

extern "C" {
// Boot log rows (BootLog.c): see the index list in t970PenRow below.
volatile int32_t t970_pen[32];
// Start-up lines (query bytes, first packets, garage changes) and the last four raw packets.
char t970_pen_text[768];
char t970_pen_last[4][64];
volatile uint32_t t970_pen_last_n;
}

namespace {
// ---- t970_pen index ---------------------------------------------------------------------------------------------------
enum : unsigned {
    kStage = 0,      // 1 mapped, 2 powered, 3 bus idle, 4 query ok, 5 streaming; negative: failed at that stage
    kErr = 1,        // failure detail (see fail())
    kPins = 2,       // pin snapshot at start: 1 sda, 2 scl, 4 irq, 8 pdct, 16 fwe, 32 ldo, 64 hall; <<8 the same after setup
    kFw = 3,         // firmware version (query 7:8)
    kMpu = 4,        // MPU version | BL version << 8 | format rev << 16
    kMaxX = 5, kMaxY = 6, kMaxP = 7,
    kTiltMax = 8,    // max tilt x | max tilt y << 8 | max height << 16
    kPackets = 9,    // packets read
    kCoords = 10,    // coordinate packets
    kOther = 11,     // other packet types: count; the type histogram is in kTypes
    kI2cErr = 12,    // failed transfers
    kLastX = 13, kLastY = 14, kLastP = 15,
    kLastTilt = 16,  // (uint8)tilt x | (uint8)tilt y << 8 | height << 16
    kLastFlags = 17, // byte 0 of the last coordinate packet
    kPdct = 18,      // current PDCT level (1 = pen in garage)
    kPdctEdges = 19, // PDCT changes since start
    kHall = 20,      // current hall level | hall changes << 8
    kSink = 21,      // packets handed to the HID sink
    kHover = 22,     // proximity entries (in range)
    kTips = 23,      // tip-down transitions
    kSide = 24,      // side-button presses
    kTypes = 25,     // bits: packet types seen (1 << type)
    kQueryTries = 26,// query attempts | boot-address ack << 8 (1 = the boot loader answered at 0x09)
    kTime = 27,      // ms since start at the last stage change
    kIrqStuck = 28,  // IRQ low but the read failed or returned nothing new
    kBits = 29,      // t970-pen bits | xform << 16
    kLdo = 30,       // GPIO 93 before: ctl | io << 16
    kRate = 31,      // coordinate packets in the last second | the most in any second << 16
};

struct Region {uintptr_t physical; unsigned length; IOMemoryMap *mapping;};
// TLMM: SOUTH 52..53, NORTH 6..7, NORTH 93, NORTH 110, WEST 136 (base 0x0f000000 + tile, 0x1000 per pin).
constexpr uintptr_t kTlmm = 0x0f000000, kNorth = 0x00900000, kSouth = 0x00500000, kWest = 0x00100000;
constexpr unsigned kSda = 52, kScl = 53, kIrq = 136, kPdctPin = 7, kFwe = 6, kLdoPin = 93, kHallPin = 110;
constexpr uint8_t kAddr = 0x56, kBootAddr = 0x09;
Region regions[] = {
    {kTlmm + kSouth + kSda * 0x1000, 0x2000, nullptr},
    {kTlmm + kNorth + kFwe * 0x1000, 0x2000, nullptr},
    {kTlmm + kNorth + kLdoPin * 0x1000, 0x1000, nullptr},
    {kTlmm + kNorth + kHallPin * 0x1000, 0x1000, nullptr},
    {kTlmm + kWest + kIrq * 0x1000, 0x1000, nullptr},
};

void (*gSink)(void *, uint64_t) = nullptr;
void *gSinkContext = nullptr;
unsigned gBits, gXform = 0, gHalf = 4;
uint64_t gFrequency, gEpoch;

uint64_t now() {uint64_t n; __asm__ volatile("isb\n\tmrs %0,cntpct_el0" : "=r"(n)); return n;}
unsigned msSince(uint64_t t) {return gFrequency ? (unsigned)((now() - t) * 1000 / gFrequency) : 0;}
void udelay(unsigned us) {
    if (us > 50 || !gFrequency) {IODelay(us); return;}
    const uint64_t start = now(), ticks = (gFrequency * us + 999999) / 1000000;
    while (now() - start < ticks) {}
}

// ---- text ------------------------------------------------------------------------------------------------------------
unsigned gTextAt;
void note(const char *format, ...) __printflike(1, 2);
void note(const char *format, ...) {
    char line[160];
    va_list a; va_start(a, format); vsnprintf(line, sizeof line, format, a); va_end(a);
    IOLog("T970 pen: %s\n", line);
    const size_t n = strlen(line);
    if (gTextAt + n + 2 >= sizeof t970_pen_text) return;   // keep the start-up lines; the rows carry the rest
    memcpy(t970_pen_text + gTextAt, line, n);
    gTextAt += (unsigned)n;
    t970_pen_text[gTextAt++] = ';';
    t970_pen_text[gTextAt] = 0;
}
void stage(int s) {t970_pen[kStage] = s; t970_pen[kTime] = (int32_t)msSince(gEpoch);}
bool fail(int s, int err) {t970_pen[kStage] = -s; t970_pen[kErr] = err; t970_pen[kTime] = (int32_t)msSince(gEpoch); note("fail stage %d err %d", s, err); return false;}

// ---- MMIO / GPIO ------------------------------------------------------------------------------------------------------
bool securePin(unsigned pin) {return (pin >= 20 && pin <= 23) || (pin >= 28 && pin <= 31) || (pin >= 40 && pin <= 43);}
uintptr_t tile(unsigned pin) {
    switch (pin) {
    case kSda: case kScl: return kTlmm + kSouth;
    case kIrq: return kTlmm + kWest;
    default: return kTlmm + kNorth;
    }
}
volatile uint32_t *reg(unsigned pin, unsigned offset) {
    if (securePin(pin)) panic("T970 pen: secure GPIO %u", pin);
    const uintptr_t physical = tile(pin) + pin * 0x1000 + offset;
    for (auto &r : regions)
        if (r.mapping && physical >= r.physical && physical - r.physical < r.length)
            return reinterpret_cast<volatile uint32_t *>(r.mapping->getVirtualAddress() + (physical - r.physical));
    panic("T970 pen: unmapped GPIO %u", pin);
}
uint32_t rd(unsigned pin, unsigned offset) {return *reg(pin, offset);}
void wr(unsigned pin, unsigned offset, uint32_t value) {__asm__ volatile("dsb sy" ::: "memory"); *reg(pin, offset) = value; __asm__ volatile("dsb sy" ::: "memory");}
constexpr unsigned kCtl = 0, kIo = 4;
constexpr uint32_t kPullMask = 3, kPullUp = 3, kFuncMask = 0x3c, kOe = 1U << 9, kIn = 1U << 0, kOut = 1U << 1;
bool high(unsigned pin) {return rd(pin, kIo) & kIn;}
// Function 0 (GPIO), input, with the given pull (0 none, 3 up).
void asInput(unsigned pin, uint32_t pull) {wr(pin, kCtl, (rd(pin, kCtl) & ~(kFuncMask | kPullMask | kOe)) | pull);}
void asOutput(unsigned pin, bool level) {
    wr(pin, kIo, level ? (rd(pin, kIo) | kOut) : (rd(pin, kIo) & ~kOut));
    wr(pin, kCtl, (rd(pin, kCtl) & ~(kFuncMask | kPullMask)) | kOe);
}
unsigned pinSnapshot() {
    return (high(kSda) ? 1 : 0) | (high(kScl) ? 2 : 0) | (high(kIrq) ? 4 : 0) | (high(kPdctPin) ? 8 : 0) | (high(kFwe) ? 16 : 0) |
           (high(kLdoPin) ? 32 : 0) | (high(kHallPin) ? 64 : 0);
}

// ---- GPIO I2C (the touch core's master, on 52/53) --------------------------------------------------------------------
void release(unsigned pin) {wr(pin, kCtl, rd(pin, kCtl) & ~kOe);}
void drive(unsigned pin) {wr(pin, kIo, rd(pin, kIo) & ~kOut); wr(pin, kCtl, rd(pin, kCtl) | kOe);}
unsigned gNacks, gStretch, gBus;
bool sclUp() {release(kScl); for (unsigned i = 0; i < 2000; ++i) {if (high(kScl)) return true; udelay(1);} gStretch++; return false;}
void recover() {for (unsigned i = 0; i < 9 && !high(kSda); ++i) {drive(kScl); udelay(gHalf); sclUp(); udelay(gHalf);}}
bool start() {
    release(kSda); if (!sclUp()) return false; udelay(gHalf);
    if (!high(kSda)) {recover(); if (!high(kSda)) {gBus++; return false;}}
    drive(kSda); udelay(gHalf); drive(kScl); udelay(gHalf); return true;
}
void stop() {drive(kSda); udelay(gHalf); sclUp(); udelay(gHalf); release(kSda); udelay(gHalf);}
bool writeByte(uint8_t b) {
    for (int bit = 7; bit >= 0; --bit) {
        if (b & (1U << bit)) release(kSda); else drive(kSda);
        udelay(gHalf); if (!sclUp()) return false; udelay(gHalf); drive(kScl);
    }
    release(kSda); udelay(gHalf); if (!sclUp()) return false;
    const bool ack = !high(kSda); udelay(gHalf); drive(kScl);
    if (!ack) gNacks++;
    return ack;
}
uint8_t readByte(bool ack) {
    uint8_t b = 0; release(kSda);
    for (int bit = 7; bit >= 0; --bit) {udelay(gHalf); sclUp(); if (high(kSda)) b |= 1U << bit; udelay(gHalf); drive(kScl);}
    if (ack) drive(kSda); else release(kSda);
    udelay(gHalf); sclUp(); udelay(gHalf); drive(kScl); release(kSda);
    return b;
}
bool i2cWrite(uint8_t address, const uint8_t *w, unsigned n) {
    bool ok = start() && writeByte(uint8_t(address << 1));
    for (unsigned i = 0; ok && i < n; ++i) ok = writeByte(w[i]);
    stop();
    if (!ok) t970_pen[kI2cErr]++;
    return ok;
}
// A plain read (i2c_master_recv): no register address, the controller sends its current packet.
bool i2cRead(uint8_t address, uint8_t *r, unsigned n) {
    bool ok = start() && writeByte(uint8_t((address << 1) | 1));
    for (unsigned i = 0; ok && i < n; ++i) r[i] = readByte(i + 1 < n);
    stop();
    if (!ok) t970_pen[kI2cErr]++;
    return ok;
}
// Address-only probe (no data): true when something acknowledges.
bool i2cProbe(uint8_t address) {const bool ok = start() && writeByte(uint8_t(address << 1)); stop(); return ok;}
bool command(uint8_t c) {const bool ok = i2cWrite(kAddr, &c, 1); IOSleep(50); return ok;}   // wacom_start_stop_cmd's 50 ms

// ---- W9021 -----------------------------------------------------------------------------------------------------------
constexpr unsigned kCoordNum = 16, kQueryNum = 15, kQueryPos = kCoordNum;
unsigned gMaxX = 0x5880, gMaxY = 0x3750, gMaxP = 0xfff;   // DT wacom,max_coords / max_pressure, replaced by the query

void hex(char *out, size_t cap, const uint8_t *b, unsigned n) {
    size_t at = 0;
    for (unsigned i = 0; i < n && at + 3 < cap; ++i) at += (size_t)snprintf(out + at, cap - at, "%02x", b[i]);
}

bool query() {
    uint8_t d[kCoordNum + kQueryNum] = {};
    for (unsigned i = 0; i < 5; ++i) {
        t970_pen[kQueryTries] = (t970_pen[kQueryTries] & ~0xff) | int32_t(i + 1);
        memset(d, 0, sizeof d);
        if (!i2cRead(kAddr, d, sizeof d)) {IOSleep(50); continue;}
        const uint8_t *q = d + kQueryPos;
        char text[2 * kQueryNum + 1] = {};
        hex(text, sizeof text, q, kQueryNum);
        note("query %u %s", i, text);
        if (q[0] != 0x0f) {IOSleep(50); continue;}
        t970_pen[kFw] = (q[7] << 8) | q[8];
        t970_pen[kMpu] = q[9] | (q[10] << 8) | (q[14] << 16);   // the last query byte (EPEN_REG_FMTREV 0x0f lies past the 15 read)
        const unsigned mx = (unsigned(q[1]) << 8) | q[2], my = (unsigned(q[3]) << 8) | q[4], mp = (unsigned(q[5]) << 8) | q[6];
        if (mx) gMaxX = mx;
        if (my) gMaxY = my;
        if (mp) gMaxP = mp;
        t970_pen[kMaxX] = (int32_t)gMaxX; t970_pen[kMaxY] = (int32_t)gMaxY; t970_pen[kMaxP] = (int32_t)gMaxP;
        t970_pen[kTiltMax] = q[11] | (q[12] << 8) | (q[13] << 16);
        note("fw %04x mpu %02x bl %02x max %ux%u p %u tilt %u/%u h %u", (unsigned)t970_pen[kFw], q[9], q[10], gMaxX, gMaxY, gMaxP,
             q[11], q[12], q[13]);
        return true;
    }
    // Nothing valid at 0x56: is the boot loader answering (FWE high or a broken image)?
    if (i2cProbe(kBootAddr)) {t970_pen[kQueryTries] |= 1 << 8; note("boot loader acks at 0x%02x", kBootAddr);}
    return false;
}

// Scale a raw coordinate to 0..32767 along a frame axis.
unsigned scale(unsigned value, unsigned maximum) {
    if (!maximum) return 0;
    if (value > maximum) value = maximum;
    return (unsigned)((uint64_t)value * 32767 / maximum);
}

// The sink format (NativeHidInput penAction): x 0..14, y 15..29, pressure 30..41 (0..4095), tilt x 42..48 and tilt y 49..55 (7-bit
// two's complement, -63..63), 56 tip, 57 barrel, 58 in range, 59 invert/eraser.
uint64_t pack(unsigned x, unsigned y, unsigned p, int tx, int ty, bool tip, bool barrel, bool range, bool eraser) {
    if (tx < -63) tx = -63; if (tx > 63) tx = 63; if (ty < -63) ty = -63; if (ty > 63) ty = 63;
    return uint64_t(x & 0x7fff) | uint64_t(y & 0x7fff) << 15 | uint64_t(p & 0xfff) << 30 | uint64_t(unsigned(tx) & 0x7f) << 42 |
           uint64_t(unsigned(ty) & 0x7f) << 49 | uint64_t(tip) << 56 | uint64_t(barrel) << 57 | uint64_t(range) << 58 |
           uint64_t(eraser) << 59;
}
void toSink(uint64_t v) {
    if (!(gBits & 4)) return;
    void (*sink)(void *, uint64_t) = __atomic_load_n(&gSink, __ATOMIC_ACQUIRE);
    if (!sink) return;
    sink(gSinkContext, v);
    t970_pen[kSink]++;
}

bool gProx, gTip, gSideDown;
unsigned gLastFx, gLastFy;
void rawLine(const uint8_t *d, unsigned n) {
    char text[64] = {};
    hex(text, sizeof text, d, n);
    const uint32_t i = t970_pen_last_n;
    strlcpy(t970_pen_last[i & 3], text, sizeof t970_pen_last[0]);
    t970_pen_last_n = i + 1;
}

void coordinate(const uint8_t *d) {
    t970_pen[kCoords]++;
    const uint32_t c = (uint32_t)t970_pen[kCoords];
    if ((gBits & 128) || c <= 16 || !(c & 255)) rawLine(d, kCoordNum + 1);
    const bool ready = d[0] & 0x80;
    if (!ready) {   // out of range (wacom_i2c_coord_handler's else branch)
        if (gProx) toSink(pack(gLastFx, gLastFy, 0, 0, 0, false, false, false, false));
        gProx = gTip = gSideDown = false;
        return;
    }
    const bool tip = d[0] & 0x10, side = d[0] & 0x20, rubber = d[0] & 0x40;
    unsigned rx = (unsigned(d[1]) << 8) | d[2], ry = (unsigned(d[3]) << 8) | d[4];
    const unsigned p = ((unsigned(d[5]) & 15) << 8) | d[6];
    int tx = (int8_t)d[8], ty = (int8_t)d[9];
    t970_pen[kLastX] = (int32_t)rx; t970_pen[kLastY] = (int32_t)ry; t970_pen[kLastP] = (int32_t)p;
    t970_pen[kLastTilt] = d[8] | (d[9] << 8) | (d[7] << 16);
    t970_pen[kLastFlags] = d[0];
    if (!gProx) t970_pen[kHover]++;
    if (tip && !gTip) t970_pen[kTips]++;
    if (side && !gSideDown) t970_pen[kSide]++;
    // Raw axes to the frame buffer: raw x along the long side unless swapped.
    unsigned ax = rx, ay = ry, mx = gMaxX, my = gMaxY;
    if (gXform & 1) {ax = ry; ay = rx; mx = gMaxY; my = gMaxX; const int t = tx; tx = ty; ty = t;}
    unsigned fx = scale(ax, mx), fy = scale(ay, my);
    if (gXform & 2) {fx = 32767 - fx; tx = -tx;}
    if (gXform & 4) {fy = 32767 - fy; ty = -ty;}
    gLastFx = fx; gLastFy = fy;
    // The first tip-downs as frame-buffer pixels (2800 x 1752 landscape, the units of the [t970 touch] xy field), raw too: a pen tap
    // and a finger tap on the same spot must match (main2: backboardd delivered 3 pencil taps to Preferences, nothing visibly hit).
    if (tip && !gTip && t970_pen[kTips] <= 12) note("tip %u,%u px raw %u,%u", fx * 2800 / 32768, fy * 1752 / 32768, rx, ry);
    // The first packet in range only marks proximity in Samsung's handler; the stylus gets it as hover with the position.
    const unsigned pressure = gMaxP ? (unsigned)((uint64_t)(p > gMaxP ? gMaxP : p) * 4095 / gMaxP) : 0;
    toSink(pack(fx, fy, tip ? pressure : 0, tx, ty, tip, side, true, rubber));
    gProx = true; gTip = tip; gSideDown = side;
}

void packet() {
    uint8_t d[kCoordNum + 1] = {};
    if (!i2cRead(kAddr, d, sizeof d)) {t970_pen[kIrqStuck]++; IOSleep(2); return;}
    t970_pen[kPackets]++;
    const unsigned type = d[0] & 15;
    t970_pen[kTypes] |= int32_t(1u << type);
    if (type == 1) {coordinate(d); return;}
    t970_pen[kOther]++;
    rawLine(d, kCoordNum + 1);
    if (t970_pen[kOther] <= 8) {char text[2 * (kCoordNum + 1) + 1] = {}; hex(text, sizeof text, d, kCoordNum + 1); note("pkt %u %s", type, text);}
}

int gPdctLast = -1, gHallLast = -1;
void garage() {
    const int pdct = high(kPdctPin) ? 1 : 0, hall = high(kHallPin) ? 1 : 0;
    t970_pen[kPdct] = pdct;
    if (gPdctLast >= 0 && pdct != gPdctLast) {
        t970_pen[kPdctEdges]++;
        if (gBits & 16) note("pen %s (pdct %d) t=%ums", pdct ? "in" : "out", pdct, msSince(gEpoch));
        // Leaving the garage while the stylus was in range cannot happen; entering it ends any proximity.
        if (pdct && gProx) {toSink(pack(gLastFx, gLastFy, 0, 0, 0, false, false, false, false)); gProx = gTip = gSideDown = false;}
    }
    if (gHallLast >= 0 && hall != gHallLast) {
        t970_pen[kHall] = (t970_pen[kHall] & ~0xff) + 0x100;
        if (gBits & 16) note("hall %d t=%ums", hall, msSince(gEpoch));
    }
    t970_pen[kHall] = (t970_pen[kHall] & ~0xff) | hall;
    gPdctLast = pdct; gHallLast = hall;
}

bool bringUp() {
    for (auto &r : regions) {
        IODeviceMemory *mem = IODeviceMemory::withRange(r.physical, r.length);
        r.mapping = mem ? mem->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (mem) mem->release();
        if (!r.mapping) return fail(1, (int)(r.physical >> 12));
    }
    stage(1);
    const unsigned before = pinSnapshot();
    t970_pen[kPins] = (int32_t)before;
    t970_pen[kLdo] = int32_t((rd(kLdoPin, kCtl) & 0xffff) | (rd(kLdoPin, kIo) << 16));
    note("pins %02x ldo ctl %x io %x fwe ctl %x irq ctl %x", before, rd(kLdoPin, kCtl), rd(kLdoPin, kIo), rd(kFwe, kCtl), rd(kIrq, kCtl));
    // wacom_i2c_probe: FWE output low (normal mode), then power. An FWE found high is the reset_hw path (power cycle).
    const bool fweHigh = high(kFwe);
    asOutput(kFwe, false);
    const bool ldoOn = (rd(kLdoPin, kCtl) & kOe) && (rd(kLdoPin, kIo) & kOut);
    if ((gBits & 32) || fweHigh) {
        asOutput(kLdoPin, false); IOSleep(100);
        asOutput(kLdoPin, true); IOSleep(200);
        note("power cycled (fwe was %d)", fweHigh);
    } else if (!ldoOn) {
        asOutput(kLdoPin, true); IOSleep(200);
        note("ldo on");
    }
    // The pen-detect, IRQ and hall lines are inputs without pull (DT bias-disable); the bus pins GPIO with the pad pull-up.
    asInput(kIrq, 0); asInput(kPdctPin, 0); asInput(kHallPin, 0);
    const unsigned bus[2] = {kSda, kScl};
    for (unsigned pin : bus) {wr(pin, kIo, rd(pin, kIo) & ~kOut); asInput(pin, kPullUp);}
    IOSleep(20);
    stage(2);
    const unsigned after = pinSnapshot();
    t970_pen[kPins] = int32_t(before | (after << 8));
    if (!(after & 1) || !(after & 2)) {recover(); if (!high(kSda) || !high(kScl)) return fail(3, (int)after);}
    stage(3);
    if (!query()) return fail(4, (int)(gNacks | (gBus << 8) | (gStretch << 16)));
    stage(4);
    garage();
    note("pdct %d hall %d irq %d", t970_pen[kPdct], t970_pen[kHall] & 1, high(kIrq) ? 1 : 0);
    return true;
}

void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "t970-pen");
    gEpoch = now();
    if (!bringUp() || !(gBits & 2)) return;
    if (!(gBits & 64)) {
        const bool exitOk = command(0x2d), startOk = command(0x31);
        note("survey exit %d rate start %d", exitOk, startOk);
    }
    stage(5);
    unsigned idle = 0, garageTick = 0, burst = 0;
    uint32_t rateFrom = (uint32_t)t970_pen[kCoords];
    uint64_t rateAt = now();
    for (;;) {
        if (!high(kIrq)) {
            packet();
            idle = 0;
            // A line that stays low (read failing, or not this controller's IRQ) must not spin a CPU: yield every 8 reads in a row.
            if (++burst >= 8) {burst = 0; IOSleep(1);}
            continue;
        }
        burst = 0;
        if (++garageTick >= 25) {garageTick = 0; garage();}
        if (now() - rateAt >= gFrequency) {
            const uint32_t last = (uint32_t)t970_pen[kCoords] - rateFrom, most = (uint32_t)t970_pen[kRate] >> 16;
            t970_pen[kRate] = int32_t((last & 0xffff) | ((last > most ? last : most) << 16));
            rateFrom = (uint32_t)t970_pen[kCoords]; rateAt = now();
        }
        // In range the controller reports every few ms; out of range it is quiet, so poll slower.
        ++idle;
        IOSleep(gProx || idle < 200 ? 1 : 8);
    }
}
}  // namespace

// A line from the HID side (NativeHidInput: which driver took the separate pen device, and who opened it) into [t970 pen log].
extern "C" void t970_pen_note(const char *line) {if (line) note("%s", line);}
extern "C" void native_pen_set_sink(void (*sink)(void *, uint64_t), void *context) {
    gSinkContext = context;
    __atomic_store_n(&gSink, sink, __ATOMIC_RELEASE);
}

// Called from native_legacy_display_late_probe; returns after starting the worker.
extern "C" void t970_pen_start(void) {
    static bool started;
    if (started) return;
    unsigned bits = 0;
    if (!PE_parse_boot_argn("t970-pen", &bits, sizeof bits) || !bits) return;
    started = true;
    gBits = bits;
    unsigned x = 0;
    if (PE_parse_boot_argn("t970-pen-xform", &x, sizeof x)) gXform = x & 7;
    unsigned half = 0;
    if (PE_parse_boot_argn("t970-pen-i2c", &half, sizeof half) && half >= 1 && half <= 50) gHalf = half;
    t970_pen[kBits] = int32_t((bits & 0xffff) | (gXform << 16));
    uint64_t f; __asm__ volatile("mrs %0,cntfrq_el0" : "=r"(f)); gFrequency = f;
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) != KERN_SUCCESS) {t970_pen[kStage] = -9; return;}
    thread_deallocate(thread);
}
