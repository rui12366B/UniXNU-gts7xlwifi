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
// Input and charge current (combo1..combo8b: CHG_DETAILS_01 0x31 fast charge with CHGIN_OK, yet the battery lost 300-800 mA at 44 %):
// after the charger watchdog reset, CHGIN_ILIM (CHG_CNFG_09 0xc0 bits 6:0, (n + 1) * 25 mA) and CHG_CC (CHG_CNFG_02 0xb9 bits 5:0,
// n * 50 mA) sit at their 500 mA power-on defaults, below the screen and GPU load. periph15's MUIC row showed the source: BC_STATUS
// 0x82 (VBUS, CDP) and CC_STATUS0 0x71 (CCIStat 3: Rp 3.0 A). Like Samsung's max77705_charger.c this unlocks the protected charger
// registers (CHG_CNFG_06 bits 3:2 CHGPROT = 11), takes the input limit from the USBC firmware (opcode 0x57 SAMSUNG_FW_AUTOIBUS, 0x01:
// firmware on, auto-IBUS off; CHG_DETAILS_00 0xb3 bit 7 then reads 1, "set by charger") and sets both limits from the cable:
// USBC 0x25 CC_STATUS0 0x0a bits 5:4 CCIStat (1 Rp default, 2 Rp 1.5 A, 3 Rp 3.0 A) and BC_STATUS 0x08 bits 1:0 ChgTyp (1 SDP,
// 2 CDP, 3 DCP), the larger one winning. Values are this tablet's own battery node (live FDT): rp_current_rp1/2/3 475/1400/3000 mA,
// default (DCP) input 1800 / charge 2100, USB 475/475, max_charging_current 3150. The gauge temperature derates the charge current
// (>= 45 C 1000 mA, >= 50 C or < 0 C 100 mA, the floor). Re-applied every 10 s and on every CHGIN/CC change, so a watchdog or
// firmware reset cannot leave the default in place. A PD contract above 5 V needs the firmware's PDO request opcodes and is not
// attempted by default: a PD source still advertises its 5 V Rp current, which this uses.
// Higher input voltage (t970-hv=<bits>, default 0; the source capabilities are read and logged in every case):
//   PD   USBC opcode 0x30 CURRENT_SRCCAP answers [1] = selected position << 3 | count, then the source PDOs (4 bytes LE each;
//        fixed: bits 31:30 = 0, 19:10 voltage in 50 mV, 9:0 current in 10 mA). bit 0: request the highest fixed PDO of at most
//        9 V (battery,max_input_voltage 9000) with opcode 0x32 SRCCAP_REQUEST [position], as max77705_select_pdo.
//   AFC  bit 1, a DCP without PD: opcode 0x21 with tx byte 0x46 (9 V, max77705_muic_afc_hv_set); the reply has [1] result (0 ok,
//        2 not HV, 3 not DCP, 4 MPing NACK, 5 unsupported byte, 6 no VBUS change) and [2] VBUS ADC.
// Both lower CHGIN_ILIM to 500 mA (battery,pre_afc_input_current) before asking, accept the new voltage only when USBC_STATUS1
// 0x06 bits 7:4 (VBADC) reads 7.5-10.5 V within 2 s, then take 1650 mA input (15 W, battery,pd_charging_charge_power 15000 /
// 9 V, and the HV TA cable group's input 1650; at most the PDO's current) and the 3150 mA charge current, still derated by
// temperature. combo10 (Mac port, Rp 3.0 A + CDP): with CHGIN_ILIM 3000 mA written, CHG_INT_OK went from 0xdf to 0x5f on the
// next sample: AICL (bit 7 clear) holds the input at the current where VBUS sags to the CHGIN regulation voltage, so a 5 V source
// behind a long or thin cable gives no more than it did; only a higher voltage helps. [25..27] measure it: the gauge's IIN
// (0xd0, 0.125 mA), VBYP (0xb3, 0.427246 mV), ISYS (0x43, 0.3125 mA less 60 % of the battery current at fg_resistor 5) and VSYS
// (0xb1, 0.15625 mV), as max77705_fuelgauge.c reads them. combo11a: VBYP read about 300 mV while VBADC said 4.5-5.5 V and IIN
// 0.6-0.8 A (ISYS and IIN agree with the battery current), so on this board VBYP is not the CHGIN voltage; trust VBADC. At 50 C or more they ask for 5 V again (PDO 1, AFC tx 0x08). One attempt per plug. PPS (the 45 W mode) feeds the
// direct charger, a different IC, and is not attempted.
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
extern volatile int32_t t970_muic[8] __attribute__((weak_import));   // T970WifiProbe.cpp: [0] != 0 once the MUIC route ran
// [0] stage (1 class registered, 2 published, negative: failed step), [1] permille, [2] mV, [3] mA, [4] gauge temperature deci-C,
// [5] flags (1 external, 2 charging, 4 full, 8 present), [6] full mAh, [7] design mAh, [8] cycles, [9] samples, [10] read failures,
// [11] CHG_DETAILS_01 | CHG_INT_OK << 8, [12] watchdog kicks, [13] input limit mA set, [14] charge mA set,
// [15] CC_STATUS0 | BC_STATUS << 8 | CHG_DETAILS_00 << 16, [16] CHG_CNFG_09 | CHG_CNFG_02 << 8 | CHG_CNFG_06 << 16 | CHG_CNFG_04 << 24
// (read back), [17] limit writes, [18] auto-IBUS opcode: 0 not yet, 1 sent and echoed, 2 already manual, -1 no echo,
// [19] HV state | source-cap reads << 8 | PDO count << 12 | selected position << 16 | last opcode result << 20,
// [20..23] PDO 1..4, [24] HV input mA | VBADC << 16 | requested position << 24,
// [25] IIN mA | VBYP mV << 16, [26] ISYS mA | VSYS mV << 16, [27] CHG_CNFG_12 | CHG_DETAILS_00 << 8 | CHG_DETAILS_02 << 16 |
// VBADC << 24
volatile int32_t t970_battery[28];
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

constexpr uint8_t kUsbc = 0x25;

// One USBC firmware command: opcode and data from AP_DATAOUT0 (0x21), 0 at AP_DATAOUT32 (0x41) ends it, the reply (echoed opcode
// first) comes back at AP_DATAIN0 (0x51). The chip keeps its last reply, so when that already echoes `op` a CONTROL2_READ (0x07)
// goes first and the echo below is a fresh one.
bool opcode(uint8_t op, const uint8_t *data, unsigned n, uint8_t *reply, unsigned replyLen, unsigned timeoutMs) {
    uint8_t echo = 0;
    if (op != 0x07 && t970_se0_read(kUsbc, 0x51, &echo, 1) && echo == op) {
        uint8_t r[2] = {};
        if (!opcode(0x07, nullptr, 0, r, 2, 200)) return false;
    }
    if (!t970_se0_write(kUsbc, 0x21, op)) return false;
    for (unsigned i = 0; i < n; ++i) if (!t970_se0_write(kUsbc, uint8_t(0x22 + i), data[i])) return false;
    if (!t970_se0_write(kUsbc, 0x41, 0x00)) return false;
    for (unsigned t = 0; t < timeoutMs / 10; ++t) {
        IOSleep(10);
        if (t970_se0_read(kUsbc, 0x51, reply, replyLen) && reply[0] == op) return true;
    }
    return false;
}

// SAMSUNG_FW_AUTOIBUS (0x57) data 0x01, as max77705_set_fw_noautoibus(MAX77705_AUTOIBUS_AT_OFF).
int autoIbusOff() {
    uint8_t d00 = 0, r[2] = {};
    if (t970_se0_read(kCharger, 0xb3, &d00, 1) && (d00 & 0x80)) return 2;
    const uint8_t on = 0x01;
    return opcode(0x57, &on, 1, r, 2, 100) ? 1 : -1;
}

unsigned vbadc() {
    uint8_t st1 = 0;
    return t970_se0_read(kUsbc, 0x06, &st1, 1) ? st1 >> 4 : 0;
}

// The high-voltage negotiation of one plug.
enum HvState {kHvIdle, kHvRead, kHvNone, kHvPd9, kHvAfc9, kHvFailed, kHvLost, kHvCooled};
struct Hv {
    int state = kHvIdle;
    unsigned reads = 0, count = 0, selected = 0, result = 0, requested = 0, inputMa = 0;
    uint32_t pdo[7] = {};
};

bool readSrcCap(Hv &h) {
    uint8_t r[32] = {};
    ++h.reads;
    if (!opcode(0x30, nullptr, 0, r, sizeof r, 200)) return false;
    h.count = r[1] & 7; h.selected = (r[1] >> 3) & 7;
    if (h.count > 7) h.count = 7;
    for (unsigned i = 0; i < h.count; ++i)
        h.pdo[i] = uint32_t(r[2 + 4 * i]) | uint32_t(r[3 + 4 * i]) << 8 | uint32_t(r[4 + 4 * i]) << 16 | uint32_t(r[5 + 4 * i]) << 24;
    return true;
}

void setInputOnly(int ma) {
    uint8_t c09 = 0;
    if (t970_se0_read(kCharger, 0xc0, &c09, 1)) (void)t970_se0_write(kCharger, 0xc0, uint8_t(0x80 | (ma / 25 - 1)));
}

bool waitHighVbus(bool high) {
    for (unsigned t = 0; t < 20; ++t) {
        IOSleep(100);
        const unsigned v = vbadc();
        if (high ? (v >= 5 && v <= 7) : (v >= 2 && v <= 4)) return true;
    }
    return false;
}

// One step per poll while plugged; returns the HV input current to use (0: the 5 V limits stand).
int hvStep(Hv &h, unsigned bits, int decic, uint8_t bc) {
    switch (h.state) {
    case kHvIdle:
        if (readSrcCap(h) && h.count) {h.state = kHvRead; break;}
        if (h.reads >= 3) h.state = kHvRead;   // no PD contract: AFC may still apply
        break;
    case kHvRead: {
        h.state = kHvNone;
        if (decic < 0 || decic >= 450) break;
        if ((bits & 1) && h.count) {
            unsigned best = 0, bestMv = 5000, bestMa = 0;
            for (unsigned i = 0; i < h.count; ++i) {
                const uint32_t o = h.pdo[i];
                const unsigned mv = ((o >> 10) & 0x3ff) * 50, ma = (o & 0x3ff) * 10;
                if ((o >> 30) == 0 && mv > bestMv && mv <= 9000) {best = i + 1; bestMv = mv; bestMa = ma;}
            }
            if (!best) break;
            setInputOnly(500);
            uint8_t r[2] = {};
            h.requested = best;
            const uint8_t pos = uint8_t(best);
            if (!opcode(0x32, &pos, 1, r, 2, 500)) {h.state = kHvFailed; h.result = 0xee; break;}
            h.result = r[1];
            if (r[1] == 0xfe || r[1] == 0xff || !waitHighVbus(true)) {h.state = kHvFailed; break;}
            (void)readSrcCap(h);
            h.inputMa = bestMa < 1650 ? bestMa : 1650;
            if (h.inputMa < 500) h.inputMa = 500;
            h.state = kHvPd9;
        } else if ((bits & 2) && !h.count && (bc & 0x83) == 0x83) {
            setInputOnly(500);
            uint8_t r[11] = {};
            const uint8_t tx = 0x46;
            if (!opcode(0x21, &tx, 1, r, sizeof r, 3000)) {h.state = kHvFailed; h.result = 0xee; break;}
            h.result = r[1];
            if (r[1] != 0 || !waitHighVbus(true)) {h.state = kHvFailed; break;}
            h.inputMa = 1650;
            h.state = kHvAfc9;
        }
        break;
    }
    case kHvPd9:
    case kHvAfc9: {
        const unsigned v = vbadc();
        if (v < 5) {h.state = kHvLost; break;}   // the source went back to 5 V (reset, hard reset)
        if (decic >= 500) {
            setInputOnly(500);
            uint8_t r[11] = {};
            if (h.state == kHvPd9) {const uint8_t one = 1; (void)opcode(0x32, &one, 1, r, 2, 500);}
            else {const uint8_t tx = 0x08; (void)opcode(0x21, &tx, 1, r, sizeof r, 3000);}
            (void)waitHighVbus(false);
            h.state = kHvCooled;
            break;
        }
        return int(h.inputMa);
    }
    default:
        break;
    }
    return 0;
}

struct Limits {int inputMa, chargeMa; uint8_t cc0, bc;};

Limits pickLimits(int decic, int hvInputMa) {
    Limits l = {500, 500, 0, 0};
    (void)t970_se0_read(kUsbc, 0x0a, &l.cc0, 1);
    (void)t970_se0_read(kUsbc, 0x08, &l.bc, 1);
    int rpIn = 0, rpChg = 0, bcIn = 0, bcChg = 0;
    switch ((l.cc0 >> 4) & 3) {
    case 1: rpIn = 475; rpChg = 475; break;
    case 2: rpIn = 1400; rpChg = 2100; break;
    case 3: rpIn = 3000; rpChg = 3150; break;
    }
    if (l.bc & 0x80) {   // VBUSDet
        switch (l.bc & 3) {
        case 1: bcIn = 475; bcChg = 475; break;      // SDP
        case 2: bcIn = 1500; bcChg = 1500; break;    // CDP
        case 3: bcIn = 1800; bcChg = 2100; break;    // DCP
        }
    }
    const int in = rpIn > bcIn ? rpIn : bcIn, chg = rpChg > bcChg ? rpChg : bcChg;
    if (in) {l.inputMa = in; l.chargeMa = chg;}
    if (hvInputMa) {l.inputMa = hvInputMa; l.chargeMa = 3150;}
    if (decic >= 500 || decic < 0) l.chargeMa = 100;
    else if (decic >= 450 && l.chargeMa > 1000) l.chargeMa = 1000;
    return l;
}

// CHGPROT unlock, then CHGIN_ILIM with CHG_EN kept on and CHG_CC with OTG_ILIM kept.
bool applyLimits(const Limits &l) {
    uint8_t c06 = 0, c09 = 0, c02 = 0;
    if (!t970_se0_read(kCharger, 0xbd, &c06, 1)) return false;
    if ((c06 & 0x0c) != 0x0c && !t970_se0_write(kCharger, 0xbd, uint8_t(c06 | 0x0c))) return false;
    int in = l.inputMa > 3200 ? 3200 : l.inputMa;
    int chg = l.chargeMa > 3150 ? 3150 : l.chargeMa;
    const uint8_t ilim = uint8_t(in / 25 - 1), cc = uint8_t(chg / 50);
    if (!t970_se0_read(kCharger, 0xc0, &c09, 1) || !t970_se0_read(kCharger, 0xb9, &c02, 1)) return false;
    bool ok = true;
    if (uint8_t(c09 & 0x7f) != ilim || !(c09 & 0x80)) ok = t970_se0_write(kCharger, 0xc0, uint8_t(0x80 | ilim)) && ok;
    if (uint8_t(c02 & 0x3f) != cc) ok = t970_se0_write(kCharger, 0xb9, uint8_t((c02 & 0xc0) | cc)) && ok;
    // combo11a: CHG_CNFG_12 read 0x68, VCHGIN_REG (bits 4:3) = 01, 4.6 V; AICL held the input at 0.6-0.8 A with VBADC 4.5-5.5 V on a
    // 5 V 3 A source. 00 (4.5 V, UVLO 4.7 V: max77705_charger_initialize's setting without CHGIN) lets a sagging 5 V source give
    // a little more before AICL steps in.
    uint8_t c12 = 0;
    if (t970_se0_read(kCharger, 0xc3, &c12, 1) && (c12 & 0x18)) ok = t970_se0_write(kCharger, 0xc3, uint8_t(c12 & ~0x18u)) && ok;
    return ok;
}

void readPower(volatile int32_t *b, int batteryMa) {
    uint16_t iin = 0, vbyp = 0, isys = 0, vsys = 0;
    (void)word(0xd0, &iin); (void)word(0xb3, &vbyp); (void)word(0x43, &isys); (void)word(0xb1, &vsys);
    const uint32_t iinMa = iin / 8u, vbypMv = uint32_t(vbyp) * 427246u / 1000000u, vsysMv = uint32_t(vsys) * 15625u / 100000u;
    int isysMa = int(uint32_t(isys) * 3125u / 10000u) - batteryMa * 6 / 10;
    if (isysMa < 0) isysMa = 0;
    b[25] = int32_t((iinMa & 0xffff) | (vbypMv & 0xffff) << 16);
    b[26] = int32_t((uint32_t(isysMa) & 0xffff) | (vsysMv & 0xffff) << 16);
    uint8_t c12 = 0, d00 = 0, d02 = 0;
    (void)t970_se0_read(kCharger, 0xc3, &c12, 1); (void)t970_se0_read(kCharger, 0xb3, &d00, 1);
    (void)t970_se0_read(kCharger, 0xb5, &d02, 1);
    b[27] = int32_t(uint32_t(c12) | uint32_t(d00) << 8 | uint32_t(d02) << 16 | vbadc() << 24);
}

void readBack(volatile int32_t *b) {
    uint8_t c09 = 0, c02 = 0, c06 = 0, c04 = 0;
    (void)t970_se0_read(kCharger, 0xc0, &c09, 1); (void)t970_se0_read(kCharger, 0xb9, &c02, 1);
    (void)t970_se0_read(kCharger, 0xbd, &c06, 1); (void)t970_se0_read(kCharger, 0xbb, &c04, 1);
    b[16] = int32_t(uint32_t(c09) | uint32_t(c02) << 8 | uint32_t(c06) << 16 | uint32_t(c04) << 24);
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
        setExternalChargeCapable(s.external);
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
    uint8_t lastCc0 = 0xff, lastBc = 0xff;
    unsigned hvBits = 0;
    (void)PE_parse_boot_argn("t970-hv", &hvBits, sizeof hvBits);
    Hv hv;
    int hvMa = 0;
    unsigned pluggedPolls = 0;
    for (;;) {
        Sample s;
        if (readSample(s)) {
            ++b[9];
            b[1] = s.permille; b[2] = s.mv; b[3] = s.ma; b[4] = s.decic;
            b[5] = (s.external ? 1 : 0) | (s.charging ? 2 : 0) | (s.full ? 4 : 0) | 8;
            b[6] = s.fullMah; b[7] = s.designMah; b[8] = s.cycles; b[11] = s.details | s.intOk << 8;
            if (s.external) {
                // The MUIC route (T970WifiProbe.cpp routeUsb, about 25 s) sends its own opcodes; wait for it before ours.
                // combo10: CHG_DETAILS_00 bit 7 already read 1 (charger-set limit), so no opcode is needed then.
                if (b[18] == 0) {
                    uint8_t d00 = 0;
                    if (t970_se0_read(kCharger, 0xb3, &d00, 1) && (d00 & 0x80)) b[18] = 2;
                    else if (!&t970_muic || t970_muic[0] != 0 || b[9] > 60) b[18] = autoIbusOff();
                }
                ++pluggedPolls;
                // Source capabilities a few seconds after the plug (the contract takes about a second), then once per 10 s.
                if (b[18] != 0 && pluggedPolls >= 3 && (hv.state == kHvIdle || hv.state == kHvRead || sinceFull >= 10)) {
                    uint8_t bc = 0;
                    (void)t970_se0_read(kUsbc, 0x08, &bc, 1);
                    const int before = hv.state;
                    hvMa = hvStep(hv, hvBits, s.decic, bc);
                    b[19] = int32_t(uint32_t(hv.state) | (hv.reads & 15) << 8 | hv.count << 12 | hv.selected << 16 | (hv.result & 0xff) << 20);
                    for (unsigned i = 0; i < 4; ++i) b[20 + i] = int32_t(hv.pdo[i]);
                    b[24] = int32_t(hv.inputMa | vbadc() << 16 | hv.requested << 24);
                    if (hv.state != before) sinceFull = 10;   // re-apply the limits now
                }
                Limits l = pickLimits(s.decic, hvMa);
                if (sinceFull >= 10 || s.external != lastExternal || l.cc0 != lastCc0 || l.bc != lastBc) {
                    uint8_t d00 = 0;
                    (void)t970_se0_read(kCharger, 0xb3, &d00, 1);
                    b[13] = l.inputMa; b[14] = l.chargeMa;
                    b[15] = int32_t(uint32_t(l.cc0) | uint32_t(l.bc) << 8 | uint32_t(d00) << 16);
                    if (applyLimits(l)) ++b[17];
                    readBack(b);
                }
                lastCc0 = l.cc0; lastBc = l.bc;
                if (sinceFull % 5 == 0) readPower(b, s.ma);
            } else {
                lastCc0 = lastBc = 0xff;
                hv = Hv(); hvMa = 0; pluggedPolls = 0;
            }
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
