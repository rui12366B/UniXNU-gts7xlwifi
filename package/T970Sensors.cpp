// SPDX-License-Identifier: BSD-2-Clause-Patent
// gts7xlwifi sensors: the accelerometer for screen rotation (later the light sensors), boot argument t970-sensors (off by default).
//
// Wiring (Samsung kernel sources, LineageOS android_kernel_samsung_sm8250): the sensors are not on an APPS I2C/I3C bus. They hang off
// the sensor hub, the SSC on the SLPI (qcom,ssc@5c00000, sensor_vdd = pm8150a L8 1.8 V, proximity interrupt GPIO 67), and Android only
// talks to them through the hub (CONFIG_SENSORS_SSC, CONFIG_SUPPORT_SSC_SPU, the adsp_factory drivers LSM6DSO accel/gyro, AK09918
// magnetometer, VEML3328 light main and sub). So this driver boots the SLPI the way T970Adsp.cpp boots the ADSP and becomes an SSC
// client: kona.dtsi facts:
//   PAS id 12, firmware "slpi" (slpi.mdt + .bNN squashed into slpi.mbn by scripts/squash-mdt.py), carveout pil_slpi_region
//   0x89200000 + 0x1500000, proxy votes vdd_cx L11A_LEVEL (lcx) and vdd_mx L4A_LEVEL (lmx) as the ADSP, AOP load_state "slpi",
//   SMEM crash reason item 424, SMP2P items 481 (in) / 430 (out), remote pid 3, IPCC client 4 (SLPI) signals 2 (SMP2P) and 0 (GLINK);
//   glink edge "dsps" with channels IPCRTR (QRTR, net id 2), fastrpcglink-apps-dsp and glink_ssr.
// On top of IPCRTR: QRTR, the QMI sensor client service 400 and the SSC protobuf API (T970Ssc.c): a SUID lookup for "accel", then
// sns_std_sensor_config at t970-sensors-hz, then sns_std_sensor_event samples (m/s^2, Android axes). Samples are logged ([sensors]
// rows in the boot log, and lines on the USB log) and fed, in iPadOS axes (mg), to the two HID devices HidInputProbe.cpp starts:
// the accelerometer (Sensor page 0x20/0x73, Acceleration Axis X/Y/Z) and the device orientation (0x20/0x8a, Apple motion orientation
// flags, sent on every change decided here from gravity), which IOHIDEventDriver turns into accelerometer and orientation events.
//
// Expected blocker: on SM8250 the SSC sensor drivers run in the "sensorspd" user PD, which Android starts with sscrpcd over FastRPC
// (fastrpcglink-apps-dsp, apps SMMU streams 0x541-0x543) and which reads its registry and JSON configs from APPS files through the
// FastRPC reverse (listener) calls. Without that the sensor service 400 may never appear; level 3 records which QRTR services do.
//
// t970-sensors=<bits>:
//   low nibble (level): 1 survey only (reads), 2 boot the SLPI and GLINK, 3 + QRTR (HELLO, servers), 4 + sensor client (accel stream)
//   16  publish the accelerometer HID device (HidInputProbe.cpp)
//   32  synthetic gravity instead of the SLPI (tests the iPadOS hookup): t970-sensors-fake=0 cycles portrait, landscape left, upside
//       down, landscape right every 8 s; 1..4 holds one of them
// t970-sensors-fw=DIR (default /private/var/ngpu/firmware/slpi), t970-sensors-hz (default 25), t970-sensors-axes (see axes()).
// The ADSP: with t970-adsp >= 2 it owns the IPCC interrupt and the AOSS mailbox. Then this driver waits for its ready stage, gets its
// doorbells through t970_ipcc_forward (called from T970Adsp's interrupt handler, if that hook is there; it polls every 20 ms anyway)
// and sends load_state through t970_adsp_qmp_send (weak here; T970Adsp's strong one replaces it).
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/thread.h>
#include <stdarg.h>
#include <sys/errno.h>
#include <pexpert/pexpert.h>
extern "C" {
#include "QcomGlink.h"
#include "QcomMdt.h"
#include "QcomQmp.h"
#include "QcomRpmh.h"
#include "QcomScm.h"
#include "QcomSmem.h"
#include "QcomSmp2p.h"
#include "T970Ssc.h"
int sheng_fw_open(const char *path, void **handle, uint64_t *size);
int sheng_fw_read(void *handle, uint64_t offset, void *to, size_t length);
void sheng_fw_close(void *handle);
}

extern "C" int t970_rpmh_write(const char *name, uint32_t word, uint32_t value) __attribute__((weak_import));
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" size_t qcom_usb_debug_write(const void *, size_t) __attribute__((weak_import));
extern "C" {extern volatile int32_t native_adsp_stats[16];}
// The ADSP's AOSS mailbox, used while T970Adsp owns it. This weak stand-in says "no hook"; T970Adsp.cpp's definition replaces it.
extern "C" __attribute__((weak)) int t970_adsp_qmp_send(const char *) {return -95;}

// [0] stage (negative: the step that failed), [1] last error, [2] doorbells seen, [3] the SLPI's slave-kernel bits (-1 no item),
// [4] items (bit 0 SMP2P in 481, 1 out 430, 2 GLINK 478, 3 479, 4 480, 8 our partition, 9 global, 12 PAS 12 supported, 13 IRQ ours,
// 14 IRQ forwarded seen), [5] QMP state (ours) or the hook's result, [6] slpi.mbn KiB, [7] ms from start to ready, [8] GLINK messages
// in, [9] out, [10] IPCRTR up | version agreed << 14 | link broken << 15, [11] channel opens nobody serves, [12] QRTR packets in,
// [13] remote node | servers seen << 16, [14] sensor service node << 16 | port, [15] last request: QMI result << 16 | error, or
// 0x10000000 | SSC result
// t970_accel: [0] samples, [1..3] x, y, z in mg (iPadOS axes), [4..6] raw x, y, z in mm/s^2 (sensor axes), [7] sample status |
// accel SUIDs << 8 | other events << 16
extern "C" {
volatile int32_t t970_sensors[16];
volatile int32_t t970_accel[8];
volatile uint32_t t970_sensors_srv[8];  // the first services announced over QRTR: service | instance << 16 (low 16 bits of each)
char t970_sensors_text[96];
}

namespace {
enum Stage {
    StMapped = 1, StSurveyed = 2, StFirmware = 3, StSmp2p = 4, StDoorbells = 5, StLoadState = 6, StRails = 7, StLoaded = 8,
    StStarted = 9, StReady = 10, StGlink = 11, StChannel = 12, StRouter = 13, StService = 14, StSuid = 15, StStream = 16,
    StFake = 20,
};
constexpr uint64_t kSmem = 0x80900000, kSmemSize = 0x200000, kMutex = 0x1f40000 + 3 * 0x1000, kIpcc = 0x408000, kAoss = 0xc300000;
constexpr uint64_t kCmdDb = 0x80860000, kCmdDbSize = 0x20000;
constexpr uint64_t kSlpiBase = 0x89200000, kSlpiSize = 0x1500000;
constexpr uint32_t kPasSlpi = 12, kRemotePid = 3, kItemIn = 481, kItemOut = 430, kItemCrash = 424;
constexpr uint32_t kItemGlinkDesc = 478, kItemGlinkTx = 479, kItemGlinkRx = 480, kGlinkFifo = 0x4000;
constexpr uint32_t kSpiIpcc = 229, kClientSlpi = 4;
constexpr uint32_t kFatal = 1u << 0, kReady = 1u << 1, kHandover = 1u << 2;
constexpr uint32_t kEvSmp2p = 1, kEvGlink = 2, kEvAop = 4;
constexpr uint32_t kPortSuid = 0x4001, kPortAccel = 0x4002;
constexpr uint32_t kIntentSize = 0x4400, kIntentCount = 6, kTxSlots = 8, kTxBytes = 1280;

struct Window {IOMemoryMap *map; volatile uint8_t *va;};
bool mapWindow(Window &w, uint64_t base, uint64_t size, IOOptionBits cache = kIOMapInhibitCache) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | cache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint8_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void unmap(Window &w) {if (w.map) w.map->release(); w = {};}
uint32_t rd(const Window &w, uint64_t at) {return *reinterpret_cast<volatile uint32_t *>(w.va + at);}
void wr(const Window &w, uint64_t at, uint32_t value) {*reinterpret_cast<volatile uint32_t *>(w.va + at) = value; __asm__ volatile("dsb sy" ::: "memory");}
void fail(int step, int error) {t970_sensors[1] = error; t970_sensors[0] = -step;}
void stage(int value) {if (t970_sensors[0] >= 0 && value > t970_sensors[0]) t970_sensors[0] = value;}
void note(const char *prefix, const char *text) {
    snprintf(t970_sensors_text, sizeof t970_sensors_text, "%s%s", prefix, text);
}
void usbLog(const char *format, ...) __attribute__((format(printf, 1, 2)));
void usbLog(const char *format, ...) {
    if (!&qcom_usb_debug_write) return;
    char line[160];
    va_list args;
    va_start(args, format);
    int n = vsnprintf(line, sizeof line - 1, format, args);
    va_end(args);
    if (n < 0) return;
    if (n > (int)sizeof line - 2) n = sizeof line - 2;
    line[n++] = '\n';
    qcom_usb_debug_write(line, (size_t)n);
}

Window gSmemW, gMutex, gIpcc, gAoss;
QcomSmem gSmem;
QcomSmemPartition gPart;
QcomSmp2p gP2p;
QcomQmp gQmp;
QcomGlink gGlink;
bool gGlinkStarted, gOwnIrq, gOwnQmp;
volatile uint32_t gPending;

void wake(uint32_t bits) {
    __atomic_fetch_or(&gPending, bits, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gPending);
}
uint32_t eventFor(uint32_t id) {
    if (id == qcom_ipcc_id(kClientSlpi, QCOM_IPCC_SIGNAL_SMP2P)) return kEvSmp2p;
    if (id == qcom_ipcc_id(kClientSlpi, QCOM_IPCC_SIGNAL_GLINK_QMP)) return kEvGlink;
    if (gOwnQmp && id == qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP)) return kEvAop;
    return 0;
}
// Our own interrupt handler (only when the ADSP driver does not own SPI 229).
void ipccIrq(void *) {
    uint32_t bits = 0;
    for (unsigned n = 0; n < 32; ++n) {
        const uint32_t id = rd(gIpcc, QCOM_IPCC_RECV_ID);
        if (id == QCOM_IPCC_NONE) break;
        wr(gIpcc, QCOM_IPCC_RECV_CLEAR, id);
        bits |= eventFor(id);
    }
    t970_sensors[2] = t970_sensors[2] + 1;
    if (bits) wake(bits);
}
uint32_t waitEvents(uint32_t ms) {
    uint32_t bits = __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
    if (bits) return bits;
    assert_wait_timeout((event_t)&gPending, THREAD_UNINT, ms, kMillisecondScale);
    bits = __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
    if (bits) {clear_wait(current_thread(), THREAD_AWAKENED); return bits;}
    thread_block(THREAD_CONTINUE_NULL);
    return __atomic_exchange_n(&gPending, 0, __ATOMIC_ACQ_REL);
}
void ring(uint32_t client, uint32_t signal) {wr(gIpcc, QCOM_IPCC_SEND, qcom_ipcc_id(client, signal));}
void kickAop(void *) {ring(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP);}
void kickGlink(void *) {ring(kClientSlpi, QCOM_IPCC_SIGNAL_GLINK_QMP);}
void kickSmp2p() {ring(kClientSlpi, QCOM_IPCC_SIGNAL_SMP2P);}

// ---- SMEM (allocation under the TCSR hardware mutex, shared with the other processors and the ADSP driver) ----
bool lockSmem() {
    for (unsigned i = 0; i < 1000; ++i) {
        wr(gMutex, 0, 1);
        if (rd(gMutex, 0) == 1) return true;
        IODelay(10);
    }
    return false;
}
void unlockSmem() {wr(gMutex, 0, 0);}
int allocItem(uint16_t item, uint32_t size) {
    if (!lockSmem()) return -16;
    const int rc = qcom_smem_alloc(&gSmem, &gPart, item, size);
    unlockSmem();
    return rc == QcomSmemExists ? 0 : rc;
}
volatile uint8_t *item(uint16_t id, uint32_t *size) {
    uint32_t at = 0, bytes = 0;
    if (qcom_smem_find(&gSmem, &gPart, id, &at, &bytes)) return nullptr;
    if (size) *size = bytes;
    return gSmemW.va + at;
}
void readCrashReason() {
    QcomSmemPartition global;
    uint32_t at = 0, size = 0;
    if (qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global) ||
        qcom_smem_find(&gSmem, &global, kItemCrash, &at, &size) || !size)
        return;
    char text[80];
    size_t n = 0;
    for (; n < sizeof text - 1 && n < size; ++n) {const char c = (char)gSmemW.va[at + n]; if (!c) break; text[n] = c >= 32 && c < 127 ? c : '.';}
    text[n] = 0;
    if (n) {note("crash: ", text); usbLog("[sensors] slpi crash: %s", text);}
}

// ---- secure world calls ----
struct ScmResult {int64_t x0; uint64_t x1;};
ScmResult scm(uint32_t command, uint32_t arginfo, uint64_t a, uint64_t b, uint64_t c) {
    ScmResult result = {-1, 0};
    for (unsigned busy = 0; busy < 20; ++busy) {
        uint64_t function = qcom_scm_fn(QCOM_SCM_SVC_PIL, command), session = 0;
        unsigned resumes = 0;
        for (;;) {
            register uint64_t x0 __asm__("x0") = function, x1 __asm__("x1") = arginfo, x2 __asm__("x2") = a, x3 __asm__("x3") = b,
                              x4 __asm__("x4") = c, x5 __asm__("x5") = 0, x6 __asm__("x6") = session, x7 __asm__("x7") = 0;
            __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                             :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
            result = {(int64_t)x0, x1};
            if ((int64_t)x0 != QCOM_SCM_INTERRUPTED || ++resumes > 100000) break;
            function = QCOM_SCM_INTERRUPTED;
            session = x6;
        }
        if (result.x0 != QCOM_SCM_V2_EBUSY) break;
        IOSleep(30);
    }
    return result;
}
bool scmOk(const ScmResult &r) {return r.x0 == 0 && r.x1 == 0;}

// ---- the survey: reads only (and the PAS "is supported" query) ----
void survey() {
    int32_t bits = 0;
    if (!qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_APPS, kRemotePid, &gPart)) {
        bits |= 1 << 8;
        const uint16_t ids[5] = {kItemIn, kItemOut, kItemGlinkDesc, kItemGlinkTx, kItemGlinkRx};
        for (unsigned i = 0; i < 5; ++i) if (item(ids[i], nullptr)) bits |= 1 << i;
    }
    QcomSmemPartition global;
    if (!qcom_smem_partition(&gSmem, QCOM_SMEM_HOST_GLOBAL, QCOM_SMEM_HOST_GLOBAL, &global)) bits |= 1 << 9;
    const ScmResult pas = scm(QCOM_SCM_PAS_IS_SUPPORTED, qcom_scm_args(1, 0, 0, 0), kPasSlpi, 0, 0);
    if (pas.x0 == 0 && pas.x1 == 1) bits |= 1 << 12;
    t970_sensors[4] = bits;
    t970_sensors[3] = -1;
    if (volatile uint8_t *in = item(kItemIn, nullptr)) {
        QcomSmp2p view = {};
        view.in = reinterpret_cast<volatile uint32_t *>(in);
        view.local_pid = 0; view.remote_pid = kRemotePid;
        const int index = qcom_smp2p_check_in(view.in, 0, kRemotePid) ? -1 : qcom_smp2p_find_in(&view, "slave-kernel");
        if (index >= 0) t970_sensors[3] = (int32_t)qcom_smp2p_in_value(&view, index);
    }
    readCrashReason();
    usbLog("[sensors] survey items=%x slave=%x", (unsigned)bits, (unsigned)t970_sensors[3]);
}

// ---- rails (the board's one RPMh writer, as T970Adsp) ----
QcomRpmhResource gLcx, gLmx;
int vote(uint32_t lcx, uint32_t lmx) {
    if (!&t970_rpmh_write) return -100;
    int rc = t970_rpmh_write("lcx.lvl", 0, lcx);
    if (!rc) rc = t970_rpmh_write("lmx.lvl", 0, lmx);
    return rc;
}
bool readCmdDb(uint8_t *copy) {
    Window db = {};
    if (!mapWindow(db, kCmdDb, kCmdDbSize)) return false;
    for (uint64_t i = 0; i < kCmdDbSize; i += 4) {const uint32_t w = rd(db, i); memcpy(copy + i, &w, 4);}
    unmap(db);
    return true;
}

void cleanToPoc(const void *start, size_t length) {
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const uintptr_t line = static_cast<uintptr_t>(4) << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(start) & ~(line - 1); a < reinterpret_cast<uintptr_t>(start) + length; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

// ---- the firmware image ----
struct Image {void *file; uint64_t size; QcomMdtImage mdt; IOBufferMemoryDescriptor *meta;};
Image gImage;
int fileRead(void *context, uint64_t offset, void *to, size_t length) {return sheng_fw_read(context, offset, to, length);}
void release(Image &image) {
    if (image.file) sheng_fw_close(image.file);
    if (image.meta) image.meta->release();
    image.file = nullptr; image.meta = nullptr;
}
int openImage(Image &image, const char *path) {
    int rc = sheng_fw_open(path, &image.file, &image.size);
    if (rc) {image.file = nullptr; return rc;}
    rc = qcom_mdt_parse(&image.mdt, image.size, fileRead, image.file);
    return rc ? 1000 - rc : 0;
}
int openFirmware(Image &image, char *path, size_t size, const char *chosen) {
    static const uint32_t pauses[] = {1, 2, 4, 8, 16, 30, 30, 30};
    snprintf(path, size, "%s/slpi.mbn", chosen ? chosen : "/private/var/ngpu/firmware/slpi");
    int rc = 0;
    for (unsigned i = 0;; ++i) {
        rc = openImage(image, path);
        if (!rc || (rc != ENOENT && rc != ENXIO && rc != EIO)) return rc;
        if (i == sizeof pauses / sizeof pauses[0]) return rc;
        IOSleep(pauses[i] * 1000);
    }
}
int authenticateHeader(Image &image) {
    const uint64_t bytes = qcom_mdt_metadata_size(&image.mdt);
    image.meta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, (IOOptionBits)kIODirectionInOut |
                                                                  (IOOptionBits)kIOMemoryPhysicallyContiguous, bytes, 0xfffffffffffff000ull);
    if (!image.meta || image.meta->prepare() != kIOReturnSuccess) return -12;
    uint8_t *va = static_cast<uint8_t *>(image.meta->getBytesNoCopy());
    if (qcom_mdt_metadata(&image.mdt, fileRead, image.file, va, bytes)) return -5;
    cleanToPoc(va, bytes);
    const uint64_t phys = image.meta->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    ScmResult r = scm(QCOM_SCM_PAS_INIT_IMAGE, qcom_scm_args(2, QCOM_SCM_VAL, QCOM_SCM_RW, 0), kPasSlpi, phys, 0);
    if (!scmOk(r)) return -1000 - (int)r.x1;
    if (image.mdt.relocatable &&
        !scmOk(r = scm(QCOM_SCM_PAS_MEM_SETUP, qcom_scm_args(3, 0, 0, 0), kPasSlpi, kSlpiBase, qcom_mdt_span(&image.mdt))))
        return -2000 - (int)r.x1;
    return 0;
}
int loadSegments(Image &image) {
    Window region = {};
    if (!mapWindow(region, kSlpiBase, kSlpiSize, kIOMapWriteCombineCache)) return -12;
    constexpr size_t kChunk = 256 * 1024;
    uint8_t *bounce = static_cast<uint8_t *>(IOMallocData(kChunk));
    int rc = bounce ? 0 : -12;
    for (unsigned i = 0; !rc && i < image.mdt.count; ++i) {
        const QcomMdtSegment &s = image.mdt.segment[i];
        uint64_t at = 0;
        if (!s.load) continue;
        if (qcom_mdt_place(&image.mdt, i, kSlpiBase, kSlpiSize, &at)) {rc = -7; break;}
        uint8_t *to = const_cast<uint8_t *>(region.va) + at;
        for (uint64_t done = 0; !rc && done < s.file_size;) {
            const size_t n = s.file_size - done < kChunk ? (size_t)(s.file_size - done) : kChunk;
            if (sheng_fw_read(image.file, s.file_offset + done, bounce, n)) {rc = -5; break;}
            memcpy(to + done, bounce, n);
            done += n;
        }
        if (!rc && s.memory_size > s.file_size) bzero(to + s.file_size, (size_t)(s.memory_size - s.file_size));
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (bounce) IOFreeData(bounce, kChunk);
    unmap(region);
    return rc;
}
void shutdownSlpi() {scm(QCOM_SCM_PAS_SHUTDOWN, qcom_scm_args(1, 0, 0, 0), kPasSlpi, 0, 0);}

// ---- QRTR and the sensor client (all on this thread: GLINK callbacks run inside qcom_glink_poll) ----
struct Tx {uint32_t length; uint8_t data[kTxBytes];};
Tx *gTx;  // kTxSlots
uint32_t gTxHead, gTxCount;
QcomGlinkChannel *gRouter;
uint8_t *gRouterStorage;
unsigned gLevel;
uint32_t gRemoteNode = T970_QRTR_NODE_BCAST;
bool gHelloSent, gHelloAnswered;
uint32_t gServers;
uint32_t gSnsNode, gSnsPort;
bool gSnsFound;
uint16_t gTxn = 1;
int gClientState;  // 0 idle, 1 SUID asked, 2 config sent
T970Suid gAccel;
uint32_t gHz = 25;
uint32_t gAxes;

bool queue(const uint8_t *data, size_t length) {
    if (!gTx || gTxCount == kTxSlots || length > kTxBytes) return false;
    Tx &t = gTx[(gTxHead + gTxCount) % kTxSlots];
    memcpy(t.data, data, length);
    t.length = (uint32_t)length;
    gTxCount++;
    return true;
}
void pump() {
    while (gTxCount && gRouter && gRouter->up) {
        Tx &t = gTx[gTxHead];
        const int rc = qcom_glink_send(&gGlink, gRouter, t.data, t.length);
        if (rc != QcomGlinkOk) break;
        gTxHead = (gTxHead + 1) % kTxSlots;
        gTxCount--;
    }
}
void sendHello(uint32_t node) {
    uint8_t p[64];
    const size_t n = t970_qrtr_ctrl(p, sizeof p, T970_QRTR_HELLO, T970_QRTR_NODE_APPS, node, 0, 0, 0, 0);
    if (n) queue(p, n);
}
bool sendSns(uint32_t port, T970Suid suid, uint32_t msg_id, const uint8_t *payload, size_t payload_length) {
    uint8_t request[512], qmi[600], packet[kTxBytes];
    const size_t r = t970_sns_request(request, sizeof request, suid, msg_id, payload, payload_length);
    const size_t q = r ? t970_sns_qmi_request(qmi, sizeof qmi, gTxn++, request, r) : 0;
    const size_t p = q ? t970_qrtr_pack(packet, sizeof packet, T970_QRTR_DATA, T970_QRTR_NODE_APPS, port, gSnsNode, gSnsPort, 0, qmi,
                                        (uint32_t)q) : 0;
    return p && queue(packet, p);
}
void askSuid() {
    uint8_t payload[64];
    const size_t n = t970_sns_suid_req(payload, sizeof payload, "accel", 1, 1);
    const T970Suid suidSensor = {T970_SUID_SENSOR_LOW, T970_SUID_SENSOR_HIGH};
    if (n && sendSns(kPortSuid, suidSensor, T970_SNS_SUID_REQ, payload, n)) {gClientState = 1; usbLog("[sensors] suid lookup accel sent");}
}
void configure() {
    uint8_t payload[16];
    const size_t n = t970_sns_sensor_config(payload, sizeof payload, gHz);
    if (n && sendSns(kPortAccel, gAccel, T970_SNS_STD_SENSOR_CONFIG, payload, n)) {
        gClientState = 2;
        usbLog("[sensors] accel config %u Hz sent", (unsigned)gHz);
    }
}

// ---- the HID sinks and the axes ----
void (*volatile gSink)(void *, uint64_t);
void *volatile gSinkContext;
void (*volatile gOrientSink)(void *, uint64_t);
void (*volatile gGyroSink)(void *, uint64_t);
void *volatile gGyroContext;
void *volatile gOrientContext;
// The orientation last sent (bit index: 0 ambiguous, 1 portrait, 2 upside down, 3 landscape left, 4 landscape right, 5 face up,
// 6 face down), -1 none yet. Decided from gravity in iPadOS axes with hysteresis: the dominant axis must exceed 700 mg and beat the
// other two by 300 mg, otherwise the last one stays (a tablet held at 45 degrees does not flip back and forth).
int gOrientation = -1;
extern "C" {volatile int32_t t970_orient[4];}  // [0] current (bit index, -1 none), [1] changes sent, [2] sink calls, [3] last change ms
const char *const kOrientNames[7] = {"ambiguous", "portrait", "upside down", "landscape left", "landscape right", "face up", "face down"};
void orient(int32_t x, int32_t y, int32_t z) {
    const int32_t ax = x < 0 ? -x : x, ay = y < 0 ? -y : y, az = z < 0 ? -z : z;
    int next = -1;
    if (ax > 700 && ax > ay + 300 && ax > az + 300) next = x < 0 ? 3 : 4;      // gravity along -x: landscape left
    else if (ay > 700 && ay > ax + 300 && ay > az + 300) next = y < 0 ? 1 : 2; // gravity along -y: portrait
    else if (az > 700 && az > ax + 300 && az > ay + 300) next = z < 0 ? 5 : 6; // gravity along -z: screen up
    if (next < 0 || next == gOrientation) return;
    gOrientation = next;
    t970_orient[0] = next;
    t970_orient[1] = t970_orient[1] + 1;
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    t970_orient[3] = (int32_t)(ns / 1000000);
    usbLog("[sensors] orientation -> %s", kOrientNames[next]);
    void (*sink)(void *, uint64_t) = gOrientSink;
    if (sink) {t970_orient[2] = t970_orient[2] + 1; sink(gOrientContext, 1ull << next);}
}
// t970-sensors-axes: three 3-bit fields (X bits 0-2, Y 3-5, Z 6-8), each the source axis (0..2) in its low two bits and bit 2 to
// negate. 0 = the default 0x1ac: X = -x, Y = -y, Z = -z of the sensor's Android frame (Android reports the reaction to gravity,
// +9.8 up; iPadOS reports gravity, -1 g down). The frame itself (the T970's Android natural orientation is landscape, iPadOS on this
// board treats the panel as portrait) is what this argument is for if rotation comes out turned.
// Samples go to the accelerometer device in mg (its report: int16 mg, unit exponent -3) and decide the orientation device's state.
void feed(int32_t xMilliG, int32_t yMilliG, int32_t zMilliG) {
    orient(xMilliG, yMilliG, zMilliG);
    void (*sink)(void *, uint64_t) = gSink;
    if (!sink) return;
    auto clamp = [](int32_t mg) -> uint16_t {
        if (mg > 32767) mg = 32767;
        if (mg < -32768) mg = -32768;
        return (uint16_t)(int16_t)mg;
    };
    const uint64_t packed = (uint64_t)clamp(xMilliG) | (uint64_t)clamp(yMilliG) << 16 | (uint64_t)clamp(zMilliG) << 32;
    sink(gSinkContext, packed);
    // The gyro alongside every accelerometer sample. No gyro stream from the hub yet (and none in the fake mode), so the device reports
    // a tablet at rest (0 rad/s), which is what locationd's orientation detector needs to see to run at all.
    void (*gyro)(void *, uint64_t) = gGyroSink;
    if (gyro) gyro(gGyroContext, 0);
}
void sample(const int32_t raw[3], int32_t status) {
    const uint32_t axes = gAxes ? gAxes : 0x1ac;
    int32_t out[3];
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t f = (axes >> (3 * i)) & 7;
        const int32_t v = raw[(f & 3) < 3 ? (f & 3) : i];
        out[i] = (int32_t)((int64_t)(f & 4 ? -v : v) * 1000 / 9807);  // mm/s^2 -> mg
    }
    const int32_t n = t970_accel[0] + 1;
    t970_accel[0] = n;
    for (unsigned i = 0; i < 3; ++i) {t970_accel[1 + i] = out[i]; t970_accel[4 + i] = raw[i];}
    t970_accel[7] = (t970_accel[7] & ~0xff) | (status & 0xff);
    stage(StStream);
    feed(out[0], out[1], out[2]);
    if (n <= 10 || n % 250 == 0)
        usbLog("[sensors] accel #%d raw %d %d %d mm/s2 -> %d %d %d mg st %d", (int)n, (int)raw[0], (int)raw[1], (int)raw[2], (int)out[0],
               (int)out[1], (int)out[2], (int)status);
}

void onEvent(void *, T970Suid suid, uint32_t msg_id, uint64_t, const uint8_t *payload, size_t length) {
    if (msg_id == T970_SNS_SUID_EVENT) {
        char type[24];
        T970Suid found[4];
        const int n = t970_sns_suid_event(payload, length, type, sizeof type, found, 4);
        usbLog("[sensors] suid event '%s' n=%d", type, n);
        if (n > 0 && !strcmp(type, "accel") && gClientState < 2) {
            gAccel = found[0];
            t970_accel[7] = (t970_accel[7] & ~0xff00) | (n & 0xff) << 8;
            char text[64];
            snprintf(text, sizeof text, "%llx/%llx", (unsigned long long)gAccel.high, (unsigned long long)gAccel.low);
            note("accel suid ", text);
            stage(StSuid);
            if (gLevel >= 4) configure();
        }
        return;
    }
    if (msg_id == T970_SNS_STD_SENSOR_EVENT && suid.low == gAccel.low && suid.high == gAccel.high) {
        uint32_t data[4];
        int32_t status = -1;
        if (t970_sns_sensor_event(payload, length, data, 4, &status) >= 3) {
            const int32_t raw[3] = {t970_f32_scale(data[0], 1000), t970_f32_scale(data[1], 1000), t970_f32_scale(data[2], 1000)};
            sample(raw, status);
        }
        return;
    }
    t970_accel[7] = (t970_accel[7] & 0xffff) | (((t970_accel[7] >> 16) + 1) & 0x7fff) << 16;
    if (msg_id == T970_SNS_STD_ERROR_EVENT) usbLog("[sensors] error event from %llx", (unsigned long long)suid.low);
}

void onData(const T970QrtrPacket &p) {
    if (p.confirm_rx) {
        // Flow control: RESUME_TX from the port the data came to, naming that port, so the sender may go on.
        uint8_t body[20] = {}, r[64];
        const uint32_t words[3] = {T970_QRTR_RESUME_TX, T970_QRTR_NODE_APPS, p.dst_port};
        memcpy(body, words, sizeof words);
        const size_t n = t970_qrtr_pack(r, sizeof r, T970_QRTR_RESUME_TX, T970_QRTR_NODE_APPS, p.dst_port, p.src_node, p.src_port, 0,
                                        body, sizeof body);
        if (n) queue(r, n);
    }
    T970QmiMessage m;
    if (t970_qmi_parse(p.payload, p.size, &m)) return;
    if (m.type == T970_QMI_RESPONSE && m.msg == T970_SNS_CLIENT_REQ) {
        uint16_t result = 0, error = 0; uint64_t client = 0; int32_t sns = -1;
        if (!t970_sns_qmi_response(&m, &result, &error, &client, &sns)) {
            t970_sensors[15] = result || error ? (int32_t)((uint32_t)result << 16 | error) : (int32_t)(0x10000000u | (uint32_t)(sns & 0xffff));
            usbLog("[sensors] response port %x qmi %u/%u client %llx sns %d", (unsigned)p.dst_port, result, error,
                   (unsigned long long)client, (int)sns);
        }
        return;
    }
    if (m.type == T970_QMI_INDICATION && (m.msg == T970_SNS_CLIENT_REPORT_IND || m.msg == T970_SNS_CLIENT_JUMBO_REPORT_IND)) {
        uint64_t client = 0; const uint8_t *event = nullptr; uint16_t length = 0;
        if (!t970_sns_qmi_report(&m, &client, &event, &length)) t970_sns_events(event, length, onEvent, nullptr);
    }
}

void routerReceive(void *, QcomGlinkChannel *, const uint8_t *data, uint32_t length) {
    T970QrtrPacket p;
    if (t970_qrtr_parse(data, length, &p)) return;
    t970_sensors[12] = t970_sensors[12] + 1;
    if (p.type == T970_QRTR_DATA && p.dst_port != T970_QRTR_PORT_CTRL) {onData(p); return;}
    T970QrtrCtrl c;
    if (t970_qrtr_parse_ctrl(&p, &c)) return;
    switch (p.type) {
    case T970_QRTR_HELLO:
        gRemoteNode = p.src_node;
        if (!gHelloAnswered) {gHelloAnswered = true; sendHello(gRemoteNode);}
        stage(StRouter);
        usbLog("[sensors] qrtr hello from node %u", (unsigned)p.src_node);
        break;
    case T970_QRTR_NEW_SERVER: {
        if (gServers < 8) t970_sensors_srv[gServers] = (c.a & 0xffff) | (c.b & 0xffff) << 16;
        gServers++;
        usbLog("[sensors] server %u inst %x at %u:%u", (unsigned)c.a, (unsigned)c.b, (unsigned)c.c, (unsigned)c.d);
        if (c.a == T970_SNS_SERVICE && !gSnsFound) {
            gSnsFound = true; gSnsNode = c.c; gSnsPort = c.d;
            t970_sensors[14] = (int32_t)(gSnsNode << 16 | (gSnsPort & 0xffff));
            stage(StService);
            if (gLevel >= 4 && !gClientState) askSuid();
        }
        break;
    }
    case T970_QRTR_DEL_SERVER:
        if (c.a == T970_SNS_SERVICE && gSnsFound && c.c == gSnsNode && c.d == gSnsPort) {gSnsFound = false; gClientState = 0;}
        break;
    default:
        break;
    }
    t970_sensors[13] = (int32_t)((gRemoteNode & 0xffff) | (gServers & 0x7fff) << 16);
}
void routerState(void *, QcomGlinkChannel *, int up) {
    usbLog("[sensors] IPCRTR %s", up ? "up" : "down");
    if (up && gLevel >= 3 && !gHelloSent) {gHelloSent = true; sendHello(T970_QRTR_NODE_BCAST);}
}

// ---- events ----
uint32_t gSlave;
bool gHandedOver;
void serviceSmp2p() {
    if (!gP2p.out) return;
    if (!gP2p.in) {
        if (volatile uint8_t *in = item(kItemIn, nullptr)) gP2p.in = reinterpret_cast<volatile uint32_t *>(in);
        else return;
    }
    uint32_t value = 0, changed = 0;
    if (qcom_smp2p_service(&gP2p, "slave-kernel", &value, &changed)) kickSmp2p();
    gSlave = value;
    t970_sensors[3] = (int32_t)value;
    if (changed & value & kFatal) readCrashReason();
    if ((value & kHandover) && !gHandedOver) {gHandedOver = true; vote(0, 0);}
}
void serviceGlink() {
    if (!gGlinkStarted) return;
    if (!gGlink.rx.data) {
        uint32_t size = 0;
        if (volatile uint8_t *rx = item(kItemGlinkRx, &size)) qcom_glink_set_rx(&gGlink, rx, size);
    }
    qcom_glink_poll(&gGlink);
    pump();
    t970_sensors[8] = (int32_t)gGlink.rx_messages;
    t970_sensors[9] = (int32_t)gGlink.tx_messages;
    t970_sensors[10] = (int32_t)((gRouter && gRouter->up ? 1u : 0u) | (uint32_t)gGlink.version_agreed << 14 | (uint32_t)gGlink.broken << 15);
    t970_sensors[11] = (int32_t)gGlink.unknown_opens;
    if (gGlink.unknown_opens && strncmp(t970_sensors_text, "crash: ", 7) && strncmp(t970_sensors_text, "accel", 5))
        note("open: ", gGlink.unknown_names);
    if (gGlink.version_agreed) stage(StGlink);
    if (gRouter && gRouter->up) stage(StChannel);
}
void service(uint32_t bits) {
    if (gOwnQmp && (bits & kEvAop)) qcom_qmp_step(&gQmp);
    serviceSmp2p();  // cheap: every wake, so a missed doorbell costs at most one poll period
    serviceGlink();
}
template <typename F> bool waitUntil(uint32_t ms, F done) {
    uint64_t deadline;
    clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
    while (!done()) {
        const uint64_t now = mach_absolute_time();
        if (now >= deadline) return done();
        uint64_t left = 0;
        absolutetime_to_nanoseconds(deadline - now, &left);
        uint32_t wait = (uint32_t)(left / 1000000) + 1;
        if (wait > 20) wait = 20;
        service(waitEvents(wait) | (gOwnQmp ? kEvAop : 0));
    }
    return true;
}

// ---- the bring-up ----
bool attachIrqs() {
    const uint32_t ids[3] = {qcom_ipcc_id(kClientSlpi, QCOM_IPCC_SIGNAL_SMP2P), qcom_ipcc_id(kClientSlpi, QCOM_IPCC_SIGNAL_GLINK_QMP),
                             qcom_ipcc_id(QCOM_IPCC_CLIENT_AOP, QCOM_IPCC_SIGNAL_GLINK_QMP)};
    if (gOwnIrq) {
        if (!armcpu_spi_attach) {fail(StDoorbells, -2); return false;}
        int rc = 0;
        for (unsigned tries = 0;; ++tries) {
            rc = armcpu_spi_attach(32 + kSpiIpcc, false, ipccIrq, nullptr, nullptr, nullptr);
            if (rc != kIOReturnNotReady || tries == 240) break;
            IOSleep(250);
        }
        if (rc) {fail(StDoorbells, rc); return false;}
        t970_sensors[4] = t970_sensors[4] | 1 << 13;
    }
    for (unsigned i = 0; i < (gOwnQmp ? 3u : 2u); ++i) wr(gIpcc, QCOM_IPCC_RECV_ENABLE, ids[i]);
    return true;
}

bool loadState() {
    const char *text = "{class: image, res: load_state, name: slpi, val: on}";
    if (!gOwnQmp) {
        const int rc = t970_adsp_qmp_send(text);
        t970_sensors[5] = rc;
        if (rc) {fail(StLoadState, rc); return false;}
        return true;
    }
    qcom_qmp_init(&gQmp, reinterpret_cast<volatile uint32_t *>(gAoss.va), 0x400, kickAop, nullptr);
    for (int phase = 0; phase < 4 && qcom_qmp_step(&gQmp) > 0 && gQmp.state != QcomQmpOpen; ++phase)
        waitUntil(1000, [] {return qcom_qmp_step(&gQmp) == QcomQmpOpen || gQmp.state < 0;});
    t970_sensors[5] = gQmp.state;
    if (gQmp.state != QcomQmpOpen) {fail(StLoadState, gQmp.state); return false;}
    qcom_qmp_send(&gQmp, text);
    if (!waitUntil(1000, [] {return qcom_qmp_step(&gQmp) == QcomQmpOpen;})) {qcom_qmp_abandon(&gQmp); fail(StLoadState, -60); return false;}
    return true;
}

bool start() {
    char dir[80] = "";
    const bool chosen = PE_parse_boot_argn("t970-sensors-fw", dir, sizeof dir) && dir[0];
    char path[112];
    int rc = openFirmware(gImage, path, sizeof path, chosen ? dir : nullptr);
    if (rc) {note("fw: ", path); fail(StFirmware, rc); return false;}
    t970_sensors[6] = (int32_t)(gImage.size >> 10);
    stage(StFirmware);
    if (t970_sensors[3] > 0) {fail(StFirmware, -114); release(gImage); return false;}  // running already: left alone

    if ((rc = allocItem(kItemOut, QCOM_SMP2P_ITEM_BYTES))) {fail(StSmp2p, rc); release(gImage); return false;}
    qcom_smp2p_init_out(&gP2p, reinterpret_cast<volatile uint32_t *>(item(kItemOut, nullptr)), 0, kRemotePid);
    qcom_smp2p_add_entry(&gP2p, "master-kernel");
    stage(StSmp2p);
    if (!attachIrqs()) {release(gImage); return false;}
    kickSmp2p();
    stage(StDoorbells);
    if (!loadState()) {release(gImage); return false;}
    stage(StLoadState);

    struct CmdDbCopy {uint8_t *bytes = static_cast<uint8_t *>(IOMallocData(kCmdDbSize)); ~CmdDbCopy() {if (bytes) IOFreeData(bytes, kCmdDbSize);}} copy;
    uint32_t lcx = 0, lmx = 0;
    if (!copy.bytes || !readCmdDb(copy.bytes) || qcom_cmd_db_find(copy.bytes, kCmdDbSize, "lcx.lvl", &gLcx) ||
        qcom_cmd_db_find(copy.bytes, kCmdDbSize, "lmx.lvl", &gLmx) || qcom_rpmh_level_index(&gLcx, 0xffffffffu, &lcx) ||
        qcom_rpmh_level_index(&gLmx, 0xffffffffu, &lmx)) {
        fail(StRails, -2); release(gImage); return false;
    }
    if ((rc = vote(lcx, lmx))) {fail(StRails, rc); release(gImage); return false;}
    stage(StRails);

    if ((rc = authenticateHeader(gImage)) || (rc = loadSegments(gImage))) {
        fail(StLoaded, rc); shutdownSlpi(); vote(0, 0); release(gImage); return false;
    }
    stage(StLoaded);
    const uint64_t began = mach_absolute_time();
    const ScmResult reset = scm(QCOM_SCM_PAS_AUTH_AND_RESET, qcom_scm_args(1, 0, 0, 0), kPasSlpi, 0, 0);
    if (!scmOk(reset)) {fail(StStarted, -3000 - (int)reset.x1); shutdownSlpi(); vote(0, 0); release(gImage); return false;}
    stage(StStarted);
    usbLog("[sensors] slpi started (%d KiB)", (int)t970_sensors[6]);
    const bool ready = waitUntil(5000, [] {return (gSlave & (kReady | kFatal)) != 0;});
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    t970_sensors[7] = (int32_t)(ns / 1000000);
    release(gImage);
    if (!ready || (gSlave & kFatal)) {
        readCrashReason();
        fail(StReady, ready ? -5 : -60);
        shutdownSlpi();
        if (!gHandedOver) vote(0, 0);
        return false;
    }
    stage(StReady);
    usbLog("[sensors] slpi ready after %d ms, slave %x", (int)t970_sensors[7], (unsigned)gSlave);
    return true;
}

bool startGlink() {
    int rc = allocItem(kItemGlinkDesc, 32);
    if (!rc) rc = allocItem(kItemGlinkTx, kGlinkFifo);
    uint32_t size = 0;
    volatile uint8_t *desc = item(kItemGlinkDesc, nullptr), *txf = item(kItemGlinkTx, &size);
    if (rc || !desc || !txf) {fail(StGlink, rc ? rc : -2); return false;}
    volatile uint32_t *d = reinterpret_cast<volatile uint32_t *>(desc);
    const QcomGlinkFifo tx = {&d[1], &d[0], txf, size}, rx = {&d[3], &d[2], nullptr, 0};
    qcom_glink_init(&gGlink, tx, rx, kickGlink, nullptr);
    gGlinkStarted = true;
    // Registered at once, before the first poll: QcomGlink forgets an OPEN for a name nobody registered.
    static const QcomGlinkClient router = {routerState, routerReceive};
    gRouter = qcom_glink_channel(&gGlink, "IPCRTR", kIntentSize, kIntentCount, gRouterStorage, &router, nullptr, 0);
    if (!gRouter) {fail(StGlink, -4); return false;}
    serviceGlink();
    return true;
}

// ---- synthetic gravity (t970-sensors bit 32) ----
void fakeLoop() {
    unsigned fixed = 0;
    PE_parse_boot_argn("t970-sensors-fake", &fixed, sizeof fixed);
    static const int32_t vectors[4][3] = {{0, -1000, 0}, {-1000, 0, 0}, {0, 1000, 0}, {1000, 0, 0}};
    static const char *const names[4] = {"portrait", "landscape left", "upside down", "landscape right"};
    stage(StFake);
    unsigned last = 99;
    for (uint64_t tick = 0;; ++tick) {
        const unsigned which = fixed >= 1 && fixed <= 4 ? fixed - 1 : (unsigned)((tick / 80) % 4);  // 10 Hz, 8 s each
        const int32_t *v = vectors[which];
        // a little noise, so nothing downstream treats the stream as stuck
        const int32_t jitter = (int32_t)(tick % 7) - 3;
        t970_accel[0] = t970_accel[0] + 1;
        t970_accel[1] = v[0] + jitter; t970_accel[2] = v[1] - jitter; t970_accel[3] = v[2] + jitter;
        feed(v[0] + jitter, v[1] - jitter, v[2] + jitter);
        if (which != last) {last = which; note("fake: ", names[which]); usbLog("[sensors] fake gravity: %s", names[which]);}
        IOSleep(100);
    }
}

void sensorsThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "t970-sensors");
    unsigned mode = 0, adsp = 0;
    PE_parse_boot_argn("t970-sensors", &mode, sizeof mode);
    PE_parse_boot_argn("t970-adsp", &adsp, sizeof adsp);
    PE_parse_boot_argn("t970-sensors-hz", &gHz, sizeof gHz);
    PE_parse_boot_argn("t970-sensors-axes", &gAxes, sizeof gAxes);
    if (!gHz || gHz > 400) gHz = 25;
    gLevel = mode & 0xf;
    if (mode & 32) {fakeLoop(); return;}
    if (!gLevel) return;
    if (!mapWindow(gSmemW, kSmem, kSmemSize) || !mapWindow(gMutex, kMutex, 0x1000) || !mapWindow(gIpcc, kIpcc, 0x1000) ||
        !mapWindow(gAoss, kAoss, 0x1000)) {
        fail(StMapped, -12);
        return;
    }
    gSmem = {gSmemW.va, (uint32_t)kSmemSize};
    stage(StMapped);
    survey();
    stage(StSurveyed);
    if (gLevel < 2) return;
    gOwnIrq = gOwnQmp = adsp < 2;
    if (!gOwnIrq) {
        // The ADSP driver owns the doorbell interrupt and the AOP mailbox: wait until its DSP is up and holds its own rail votes.
        uint64_t deadline;
        clock_interval_to_deadline(180, kSecondScale, &deadline);
        while (native_adsp_stats[0] >= 0 && native_adsp_stats[0] < 11 && mach_absolute_time() < deadline) IOSleep(250);
        if (native_adsp_stats[0] < 11) {fail(StDoorbells, -11); return;}
    }
    gTx = static_cast<Tx *>(IOMallocData(sizeof(Tx) * kTxSlots));
    gRouterStorage = static_cast<uint8_t *>(IOMallocData((vm_size_t)kIntentSize * kIntentCount));
    if (!gTx || !gRouterStorage) {fail(StMapped, -12); return;}
    if (!start() || !startGlink()) return;
    // From here: doorbells (ours or forwarded) and a 20 ms poll while anything waits; otherwise once a second.
    for (;;) service(waitEvents(gTxCount || !gOwnIrq ? 20 : 1000));
}
}  // namespace

// Doorbells for the SLPI that T970Adsp's interrupt handler read and acknowledged (interrupt context: record and wake only).
extern "C" void t970_ipcc_forward(uint32_t id) {
    const uint32_t bits = eventFor(id);
    t970_sensors[2] = t970_sensors[2] + 1;
    if (bits) {t970_sensors[4] = t970_sensors[4] | 1 << 14; wake(bits);}
}

// The accelerometer HID device (NativeHidInput accel mode) registers here; null unregisters.
extern "C" void native_accel_set_sink(void (*sink)(void *, uint64_t), void *context) {
    gSink = nullptr;
    __asm__ volatile("dmb ish" ::: "memory");
    gSinkContext = context;
    __asm__ volatile("dmb ish" ::: "memory");
    gSink = sink;
}
// The device orientation HID device registers here; it is sent the current orientation at once, then every change.
// The gyro HID device registers here (samples packed as the accelerometer's, in mrad/s).
extern "C" void native_gyro_set_sink(void (*sink)(void *, uint64_t), void *context) {
    gGyroSink = nullptr;
    __asm__ volatile("dmb ish" ::: "memory");
    gGyroContext = context;
    __asm__ volatile("dmb ish" ::: "memory");
    gGyroSink = sink;
}
extern "C" void native_orient_set_sink(void (*sink)(void *, uint64_t), void *context) {
    gOrientSink = nullptr;
    __asm__ volatile("dmb ish" ::: "memory");
    gOrientContext = context;
    __asm__ volatile("dmb ish" ::: "memory");
    gOrientSink = sink;
    gOrientation = -1;  // the next sample re-sends the current orientation to the new device
}

extern "C" void t970_sensors_start(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("t970-sensors", &mode, sizeof mode) || !(mode & 0x2f) || t970_sensors[0]) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(sensorsThread, nullptr, &thread) != KERN_SUCCESS) {fail(StMapped, -7); return;}
    thread_deallocate(thread);
}
