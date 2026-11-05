// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off CPU frequency control for this board's EPSS frequency domains (kona).
// Adapted from the reference board's CpuFreqProbe (UniXNU-sheng, SM8550): the EPSS register
// layout is the same qcom-cpufreq-hw-epss interface, and the SM8250 addresses and domain CPU
// counts come from this board's own device tree (kona.dtsi:325-330) and the same SoC's pipa
// port, which votes the identical hardware. This board's device tree declares no EPSS L3, so
// the L3 window the SM8550 version maps is absent here. `sheng-cpufreq=<n>`:
//   1  read-only: report each domain's enable bit, current/requested level and its highest LUT entry
//   2  additionally request that highest non-turbo level in domain 0 (the boot CPU's cluster)
//   3  the same in every domain once it reads enabled, and the highest L3 level
// Optional `sheng-cpufreq-cap=<MHz>`: mode 3 requests the highest non-turbo level at or below it (domains and L3).
// Register layout is the qcom-cpufreq-hw "epss" data of the pinned mainline tree (reg_enable 0x0, reg_domain_state 0x20,
// reg_dcvs_ctrl 0xb0, reg_freq_lut 0x100 with 4-byte rows, reg_perf_state 0x320, per-core perf_state rows +4*i for the
// policy's other CPUs when dcvs_ctrl bit 0 is set); the three domain windows come from the cpufreq@17d91000 node of
// sm8550.dtsi, their CPU counts from the cpus' qcom,freq-domain (3, 4, 1). The L3 is osm-l3.c "qcom,epss-l3" (the sm8550
// node interconnect@17d90000): REG_ENABLE 0x0, LUT 0x100 with 4-byte rows, vote EPSS_REG_L3_VOTE 0x90; only those three
// offsets are touched there. LUT row: bits 31:30 source, 18:16 core count (1 = turbo), 7:0 L value; an L value is a
// multiple of the 19.2 MHz bi_tcxo_div2 clock, source 0 is GPLL0/2.
// A cluster whose cores are all off reads enable 0 and state 0 (int-143 read at 7 s, before the secondary cores ran), so the
// probe maps the windows once and sheng_cpufreq_poll(), called every log round, votes each domain the first time it reads
// enabled and refreshes the row. Nothing here is validated on the device until a boot has shown it.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <mach/processor_info.h>
#include <machine/machine_routines.h>

// periph14: mode 4 is load-driven DVFS (SM-T733 package/T733CpuPower.cpp + T733CpuPowerStartup.c, same EPSS windows on SM7325):
// a 10 ms worker per domain takes the larger of (a) the busiest core's scheduler load over 200 ms, scaled so that 70 % load holds the
// current frequency and 80 % goes to the top, and (b) the scheduler's perfcontrol class of what runs (UI 75 %, real time 100 %, a
// thread that waited >= 1 ms 85 %, >= 5 ms 100 %, held for three passes), as a fraction of the top non-turbo level; it steps up at
// once and down after 300 ms. sheng-cpufreq-cap and the TSENS throttle below bound the top level.
extern "C" kern_return_t processor_info(processor_t, processor_flavor_t, host_t *, processor_info_t, mach_msg_type_number_t *);
extern "C" processor_t cpu_to_processor(int cpu);

extern "C" {
char native_cpufreq_row[120];
volatile uint32_t native_cpufreq_stage;
}

namespace {
constexpr uint64_t kDomainBase[3] = {0x18591000, 0x18592000, 0x18593000};
constexpr unsigned kDomainCpus[3] = {4, 3, 1};
constexpr uint64_t kWindowSize = 0x1000;
constexpr uint32_t kRegEnable = 0x0, kRegDomainState = 0x20, kRegDcvsCtrl = 0xb0, kRegFreqLut = 0x100, kRegPerfState = 0x320;
constexpr unsigned kLutEntries = 40;
constexpr uint32_t kXoKHz = 19200;        // bi_tcxo_div2
constexpr uint32_t kGpllHalfKHz = 300000; // GPLL0 600 MHz / CLK_HW_DIV 2

struct Window {
    IOMemoryMap *map;
    uintptr_t va;
    int voted;          // level written, -1 none
    uint32_t votedKHz;
    uint32_t preKHz;    // domain state the first time it read enabled, before the vote
};
Window gDomain[3];
unsigned gMode;
// gts7xlwifi thermal step-down (v49: eight cores at their top levels reached 84-86 C within 60 s and the tablet reset). Every poll
// (one log round, 2 s) reads the hottest TSENS sensor; at or above kHotC every voted domain drops two LUT levels, at or above
// kWarmC one, below kCoolC it climbs back one. gThrottle is the drop applied below each domain's chosen level.
constexpr int kHotC = 80, kWarmC = 72, kCoolC = 65;
constexpr unsigned kMaxThrottle = 12;
unsigned gThrottle;
int gTempC = -1000;
volatile uint8_t *gTsens[2];
uint32_t gCapKHz;
IOLock *gLock;

uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t lutKHz(uint32_t row) {return (row >> 30) ? kXoKHz * (row & 0xff) : kGpllHalfKHz;}

// Highest non-turbo LUT level (at or below the cap when one is set); the table ends at two equal frequencies.
int pickLevel(uintptr_t va, bool turboMatters, uint32_t *khzOut, unsigned *rowsOut) {
    int best = -1; uint32_t bestKHz = 0, prev = 0; unsigned rows = 0;
    for (unsigned i = 0; i < kLutEntries; ++i) {
        uint32_t row = rd32(va, kRegFreqLut + i * 4);
        uint32_t khz = lutKHz(row);
        if (i > 0 && khz == prev) break;
        rows = i + 1;
        prev = khz;
        if (turboMatters && ((row >> 16) & 7) == 1) continue;
        if (gCapKHz && khz > gCapKHz) continue;
        if (khz >= bestKHz) {best = (int)i; bestKHz = khz;}
    }
    if (khzOut) *khzOut = bestKHz;
    if (rowsOut) *rowsOut = rows;
    return best;
}

bool mapWindow(Window &w, uint64_t base) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, kWindowSize);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? static_cast<uintptr_t>(w.map->getVirtualAddress()) : 0;
    w.voted = -1;
    return w.va != 0;
}

void voteDomain(unsigned d) {
    Window &w = gDomain[d];
    uint32_t khz = 0;
    int level = pickLevel(w.va, true, &khz, nullptr);
    if (level < 0) return;
    w.preKHz = (rd32(w.va, kRegDomainState) & 0xff) * kXoKHz;
    wr32(w.va, kRegPerfState, (uint32_t)level);
    if (rd32(w.va, kRegDcvsCtrl) & 1)
        for (unsigned c = 1; c < kDomainCpus[d]; ++c) wr32(w.va, kRegPerfState + c * 4, (uint32_t)level);
    w.voted = level;
    w.votedKHz = khz;
}

// TSENS v2 (live FDT tsens0 tm 0xc263000, tsens1 tm 0xc265000): sensor n at tm + 0xa0 + 4n, bits 11:0 signed tenths, bit 21 valid.
int hottestC() {
    static const uint64_t phys[2] = {0xc263000, 0xc265000};
    int hottest = -1000;
    for (unsigned b = 0; b < 2; ++b) {
        if (!gTsens[b]) {
            IODeviceMemory *memory = IODeviceMemory::withRange(phys[b], 0x1000);
            IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
            if (memory) memory->release();
            if (map) gTsens[b] = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());   // kept for the boot
        }
        if (!gTsens[b]) continue;
        for (unsigned i = 0; i < 16; ++i) {
            const uint32_t v = *reinterpret_cast<volatile uint32_t *>(gTsens[b] + 0xa0 + 4 * i);
            if (!(v & (1u << 21))) continue;
            int t = (int)(v & 0xfff); if (t & 0x800) t -= 0x1000;
            if (t / 10 > hottest) hottest = t / 10;
        }
    }
    return hottest;
}

// Writes the domain's level less the current throttle (never below the LUT's first row).
void applyLevel(unsigned d) {
    Window &w = gDomain[d];
    if (!w.va || w.voted < 0) return;
    const int level = w.voted > (int)gThrottle ? w.voted - (int)gThrottle : 0;
    wr32(w.va, kRegPerfState, (uint32_t)level);
    if (rd32(w.va, kRegDcvsCtrl) & 1)
        for (unsigned c = 1; c < kDomainCpus[d]; ++c) wr32(w.va, kRegPerfState + c * 4, (uint32_t)level);
}

void thermalStep() {
    gTempC = hottestC();
    if (gTempC <= -1000) return;
    unsigned next = gThrottle;
    if (gTempC >= kHotC) next = gThrottle + 2 > kMaxThrottle ? kMaxThrottle : gThrottle + 2;
    else if (gTempC >= kWarmC) next = gThrottle + 1 > kMaxThrottle ? kMaxThrottle : gThrottle + 1;
    else if (gTempC < kCoolC && gThrottle) next = gThrottle - 1;
    if (next == gThrottle) return;
    gThrottle = next;
    for (unsigned d = 0; d < 3; ++d) applyLevel(d);
}


// ---- mode 4: DVFS ---------------------------------------------------------------------------------------------------------
struct Demand {uint32_t state, peak; uint64_t latency, thread, notes;};
Demand gDemand[8];
uint64_t gOneMs, gFiveMs;
unsigned classPercent(unsigned cls) {
    switch (cls) {
    case PERFCONTROL_CLASS_IDLE: return 0;
    case PERFCONTROL_CLASS_REALTIME: case PERFCONTROL_CLASS_ABOVEUI: return 100;
    case PERFCONTROL_CLASS_UI: return 75;
    case PERFCONTROL_CLASS_USER_INITIATED: return 70;
    case PERFCONTROL_CLASS_NONUI: return 50;
    case PERFCONTROL_CLASS_UTILITY: return 35;
    default: return 20;
    }
}
void notePeak(unsigned cpu, uint32_t percent) {
    uint32_t old = __atomic_load_n(&gDemand[cpu].peak, __ATOMIC_RELAXED);
    while (old < percent && !__atomic_compare_exchange_n(&gDemand[cpu].peak, &old, percent, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}
void noteThread(unsigned cpu, struct perfcontrol_thread_data *data) {
    if (cpu >= 8 || !data) return;
    const unsigned cls = data->perfctl_class, percent = classPercent(cls);
    __atomic_store_n(&gDemand[cpu].state, (cls != PERFCONTROL_CLASS_IDLE ? 1u : 0u) | percent << 16, __ATOMIC_RELEASE);
    notePeak(cpu, percent);
    __atomic_fetch_add(&gDemand[cpu].notes, 1, __ATOMIC_RELAXED);
}
// Scheduler callbacks: atomics only, never the worker's lock.
void perfSwitch(perfcontrol_event, uint32_t cpu, uint64_t, uint32_t, struct perfcontrol_thread_data *, struct perfcontrol_thread_data *on,
                struct perfcontrol_cpu_counters *, void *) {
    if (cpu < 8 && on) __atomic_store_n(&gDemand[cpu].thread, on->thread_id, __ATOMIC_RELAXED);
    noteThread(cpu, on);
}
void perfUpdate(perfcontrol_event, uint32_t cpu, uint64_t, uint32_t, struct perfcontrol_thread_data *data, void *) {
    if (cpu < 8 && data && data->thread_id == __atomic_load_n(&gDemand[cpu].thread, __ATOMIC_RELAXED)) noteThread(cpu, data);
}
void perfOncore(perfcontrol_state_t, going_on_core_t data) {
    const unsigned cpu = (unsigned)cpu_number();
    if (cpu >= 8 || !data) return;
    const uint64_t latency = data->scheduling_latency_at_same_basepri;
    __atomic_store_n(&gDemand[cpu].latency, latency, __ATOMIC_RELAXED);
    if (latency >= gFiveMs) notePeak(cpu, 100); else if (latency >= gOneMs) notePeak(cpu, 85);
}

struct Dvfs {
    uint32_t khz[kLutEntries]; uint8_t turbo[kLutEntries];
    unsigned rows, top, requested, load, demand, held, hold, low;
    uint64_t changes;
    bool ready;
};
Dvfs gDvfs[3];
uint32_t gPrevBusy[8], gPrevIdle[8];
bool gHaveLoad[8];
uint64_t gDvfsPasses;
bool gDvfsStarted;

void writeLevel(unsigned d, unsigned level) {
    Window &w = gDomain[d];
    wr32(w.va, kRegPerfState, level);
    if (rd32(w.va, kRegDcvsCtrl) & 1)
        for (unsigned c = 1; c < kDomainCpus[d]; ++c) wr32(w.va, kRegPerfState + c * 4, level);
}
bool readTable(unsigned d) {
    Dvfs &v = gDvfs[d]; Window &w = gDomain[d];
    v.rows = 0; v.top = 0;
    bool any = false;
    for (unsigned i = 0; i < kLutEntries; ++i) {
        const uint32_t row = rd32(w.va, kRegFreqLut + i * 4), khz = lutKHz(row);
        if (i && khz == v.khz[i - 1]) break;
        if (!khz || (i && khz < v.khz[i - 1])) return false;
        v.khz[i] = khz; v.turbo[i] = ((row >> 16) & 7) == 1; v.rows = i + 1;
        if (!v.turbo[i] && (!gCapKHz || khz <= gCapKHz)) {v.top = i; any = true;}
    }
    v.requested = rd32(w.va, kRegPerfState);
    if (v.requested >= v.rows) v.requested = 0;
    return any;
}
// Lowest ordinary level at or above target, at most `limit`.
unsigned choose(const Dvfs &v, uint32_t target, unsigned limit) {
    unsigned chosen = 0;
    for (unsigned i = 0; i <= limit && i < v.rows; ++i) {
        if (v.turbo[i]) continue;
        chosen = i;
        if (v.khz[i] >= target) break;
    }
    return chosen;
}
const unsigned kFirstCpu[3] = {0, 4, 7};
void updateDomain(unsigned d, bool sample) {
    Dvfs &v = gDvfs[d];
    if (!v.ready) return;
    unsigned demand = 0, peak = 0;
    for (unsigned c = 0; c < kDomainCpus[d]; ++c) {
        const unsigned cpu = kFirstCpu[d] + c;
        const uint32_t st = __atomic_load_n(&gDemand[cpu].state, __ATOMIC_ACQUIRE);
        if ((st & 1) && (st >> 16) > demand) demand = st >> 16;
        const uint32_t p = __atomic_exchange_n(&gDemand[cpu].peak, 0u, __ATOMIC_RELAXED);
        if (p > peak) peak = p;
    }
    if (peak >= v.held) {v.held = peak; v.hold = 3;} else if (v.hold && --v.hold == 0) v.held = peak;
    if (v.held > demand) demand = v.held;
    v.demand = demand;
    const unsigned limit = v.top > gThrottle ? v.top - gThrottle : 0;
    if (sample) {
        unsigned load = 0;
        for (unsigned c = 0; c < kDomainCpus[d]; ++c) {
            const unsigned cpu = kFirstCpu[d] + c;
            processor_t processor = cpu_to_processor((int)cpu);
            processor_cpu_load_info_data_t ticks = {};
            host_t host = HOST_NULL;
            mach_msg_type_number_t count = PROCESSOR_CPU_LOAD_INFO_COUNT;
            if (!processor || processor_info(processor, PROCESSOR_CPU_LOAD_INFO, &host, (processor_info_t)&ticks, &count) != KERN_SUCCESS) {
                gHaveLoad[cpu] = false;
                continue;
            }
            const uint32_t busy = ticks.cpu_ticks[CPU_STATE_USER] + ticks.cpu_ticks[CPU_STATE_SYSTEM], idle = ticks.cpu_ticks[CPU_STATE_IDLE];
            if (gHaveLoad[cpu]) {
                const uint32_t db = busy - gPrevBusy[cpu], di = idle - gPrevIdle[cpu];
                if (db + di) {const unsigned l = (unsigned)((uint64_t)db * 100 / (db + di)); if (l > load) load = l;}
            }
            gPrevBusy[cpu] = busy; gPrevIdle[cpu] = idle; gHaveLoad[cpu] = true;
        }
        v.load = load;
    }
    uint32_t target = v.load >= 80 ? v.khz[limit] : (uint32_t)((uint64_t)v.khz[v.requested] * v.load / 70);
    const uint32_t scheduler = (uint32_t)((uint64_t)v.khz[limit] * v.demand / 100);
    if (scheduler > target) target = scheduler;
    const unsigned row = choose(v, target, limit);
    const bool above = v.requested > limit;
    if (row < v.requested && !above) {if (++v.low < 30) return;} else v.low = 0;
    if (row == v.requested) return;
    writeLevel(d, row);
    v.requested = row;
    ++v.changes;
}
void dvfsWorker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "t970-cpu-dvfs");
    nanoseconds_to_absolutetime(1000000, &gOneMs);
    nanoseconds_to_absolutetime(5000000, &gFiveMs);
    // The secondary clusters read enabled only once their cores run; wait for every domain, as sheng_cpufreq_poll waits for each.
    for (unsigned tries = 0; tries < 600; ++tries) {
        unsigned ready = 0;
        IOLockLock(gLock);
        for (unsigned d = 0; d < 3; ++d) {
            if (!gDvfs[d].ready && gDomain[d].va && (rd32(gDomain[d].va, kRegEnable) & 1)) gDvfs[d].ready = readTable(d);
            ready += gDvfs[d].ready;
        }
        IOLockUnlock(gLock);
        if (ready == 3) break;
        IOSleep(100);
    }
    static struct sched_perfcontrol_callbacks callbacks = {};
    callbacks.version = SCHED_PERFCONTROL_CALLBACKS_VERSION_6;
    callbacks.oncore = perfOncore;
    callbacks.csw = perfSwitch;
    callbacks.state_update = perfUpdate;
    sched_perfcontrol_register_callbacks(&callbacks, 0);
    native_cpufreq_stage = 4;
    unsigned ticks = 0;
    for (;;) {
        IOSleep(10);
        const bool sample = ++ticks % 20 == 0;
        IOLockLock(gLock);
        for (unsigned d = 0; d < 3; ++d) {
            if (!gDvfs[d].ready && gDomain[d].va && (rd32(gDomain[d].va, kRegEnable) & 1)) gDvfs[d].ready = readTable(d);
            updateDomain(d, sample);
        }
        ++gDvfsPasses;
        IOLockUnlock(gLock);
    }
}
// Mode 4: the TSENS throttle lowers each domain's top level (the worker's `limit`) by gThrottle instead of rewriting the levels.
void dvfsThermal() {
    if (gTempC <= -1000) return;
    if (gTempC >= kHotC) gThrottle = gThrottle + 2 > kMaxThrottle ? kMaxThrottle : gThrottle + 2;
    else if (gTempC >= kWarmC) gThrottle = gThrottle + 1 > kMaxThrottle ? kMaxThrottle : gThrottle + 1;
    else if (gTempC < kCoolC && gThrottle) --gThrottle;
}
void refreshRow() {
    char *out = native_cpufreq_row;
    const size_t cap = sizeof(native_cpufreq_row);
    unsigned n = (unsigned)snprintf(out, cap, "CPUFREQ t%dC thr%u", gTempC, gThrottle);
    for (unsigned d = 0; d < 3 && n < cap; ++d) {
        Window &w = gDomain[d];
        if (!w.va) {n += (unsigned)snprintf(out + n, cap - n, " d%u:nomap", d); continue;}
        uint32_t enable = rd32(w.va, kRegEnable) & 1, state = (rd32(w.va, kRegDomainState) & 0xff) * kXoKHz / 1000;
        if (gMode >= 4 && gDvfs[d].ready)
            n += (unsigned)snprintf(out + n, cap - n, " d%u:%u@%u l%u q%u n%llu", d, gDvfs[d].requested, gDvfs[d].khz[gDvfs[d].requested] / 1000,
                                    gDvfs[d].load, gDvfs[d].demand, (unsigned long long)gDvfs[d].changes);
        else if (w.voted >= 0)
            n += (unsigned)snprintf(out + n, cap - n, " d%u:e%u c%u v%d@%u pre%u", d, enable, state, w.voted, w.votedKHz / 1000,
                                    w.preKHz / 1000);
        else
            n += (unsigned)snprintf(out + n, cap - n, " d%u:e%u c%u p%u", d, enable, state, rd32(w.va, kRegPerfState));
    }

}
}

extern "C" void sheng_cpufreq_poll(void) {
    if (!gMode || !gLock) return;
    IOLockLock(gLock);
    for (unsigned d = 0; d < 3; ++d) {
        Window &w = gDomain[d];
        bool wanted = gMode == 3 || (gMode == 2 && d == 0);
        if (w.va && wanted && w.voted < 0 && (rd32(w.va, kRegEnable) & 1)) {voteDomain(d); applyLevel(d);}
    }
    if (gMode >= 4) gTempC = hottestC(), dvfsThermal(); else thermalStep();
    refreshRow();
    native_cpufreq_stage = 2;
    IOLockUnlock(gLock);
}

extern "C" void sheng_cpufreq_probe(void) {
    unsigned mode = 0, capMHz = 0;
    PE_parse_boot_argn("sheng-cpufreq", &mode, sizeof(mode));
    PE_parse_boot_argn("sheng-cpufreq-cap", &capMHz, sizeof(capMHz));
    if (!mode || gLock) return;
    gLock = IOLockAlloc();
    if (!gLock) return;
    native_cpufreq_stage = 1;
    gCapKHz = capMHz * 1000;
    for (unsigned d = 0; d < 3; ++d) mapWindow(gDomain[d], kDomainBase[d]);
    gMode = mode;
    sheng_cpufreq_poll();
    if (gMode >= 4 && !gDvfsStarted) {
        thread_t thread = nullptr;
        if (kernel_thread_start(dvfsWorker, nullptr, &thread) == KERN_SUCCESS) {thread_deallocate(thread); gDvfsStarted = true;}
        else native_cpufreq_stage = 0xe4;
    }
}