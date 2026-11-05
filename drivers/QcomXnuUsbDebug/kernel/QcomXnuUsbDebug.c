/* SPDX-License-Identifier: MIT
 * Native XNU backend for the freestanding DWC3 core. No EFI state,
 * Android, pstore, partition, filesystem, allocation or scheduler dependency.
 */
#include "QcomXnuUsbDebug.h"
#include "QcomUsbBuffer.h"
#include "QcomUsbControl.h"
#include "QcomUsbScreenshot.h"
#include "../usb/Dwc3.h"
#include "../soc/QcomUsbQualify.h"
#include "../soc/QcomUsbDmaDomain.h"
#include <machine/machine_routines.h>
#include <kern/cpu_number.h>
#include <pexpert/pexpert.h>

#define NATIVE_CONTEXT_BYTES DWC3_CONTEXT_BYTES
#define GCC_BYTES QUD_GCC_BYTES
#define USB_BYTES QUD_DWC3_BYTES
#define PAGE_BYTES 4096u
#define USB_GCTL 0xc110u
#define USB_DCTL 0xc704u
#define USB_DSTS 0xc70cu
#define USB_GHWPARAMS0 0xc140u
#define USB_GUSB2PHYCFG 0xc200u
#define USB_PHY_KEEP_AWAKE_MASK ((1u << 6) | (1u << 8))
#define USB_RUN_STOP (1u << 31)
#define USB_HALTED (1u << 22)
#define USB_HS_PHY_CTRL QUD_QSCRATCH_HS_PHY_CTRL_OFFSET
#define USB_SW_VBUS_MASK QUD_QSCRATCH_VBUS_MASK
#define COUNTER_MAX_FREQUENCY UINT64_C(1000000000)
#define CONTROL_PE_RESTART_CPU 1u

/* The platform callback ABI is unchanged from Darwin22 IOPlatformExpert.h.
 * Only the control opt-in may atomically install into this NULL global. */
extern int (*PE_halt_restart)(unsigned int);

static uint8_t usb_dma[DWC3_DMA_BYTES] __attribute__((aligned(DWC3_DMA_ALIGNMENT)));
static uint8_t usb_tables[QUSB_DOMAIN_PT_BYTES] __attribute__((aligned(PAGE_BYTES)));
static uint8_t usb_context[NATIVE_CONTEXT_BYTES] __attribute__((aligned(DWC3_CONTEXT_ALIGNMENT)));
static volatile unsigned int usb_busy;
static volatile unsigned int usb_diag_enabled, usb_diag_checkpoint, usb_diag_resetting, usb_diag_target;
static volatile unsigned int usb_diag_mode, usb_diag_polling;
static uint8_t usb_log_bytes[SOC_USB_LOG_BUFFER_BYTES];
static struct soc_usb_buffer usb_log_buffer = {
    .data = usb_log_bytes, .capacity = sizeof(usb_log_bytes)
};
static uint64_t usb_log_replayed, usb_log_loss_reported;
static struct {
    struct qcom_usb_debug_status status;
    vm_offset_t gcc_va, usb_va, smmu_va;
    struct dwc3 *core;
    QUSB_DOMAIN_LEASE lease;
    QUSB_DOMAIN_PLATFORM domain_platform;
    uint64_t deadline;
    unsigned int deadline_active, deadline_expired, core_initialized;
} native;
static struct {
    struct sxnu_control_parser parser;
    struct sxnu_control_state state;
    uint64_t seed;
} usb_control_context;
static volatile unsigned int control_started, control_enabled;
static volatile unsigned int control_hook_owned, control_reboot_requested;
static volatile unsigned int control_reboot_taken, control_reboot_armed;
static volatile uint32_t control_rejected, control_reply_count, control_failures;
static volatile uint32_t control_reboot_seq, control_reboot_generation;
static volatile uint64_t control_reboot_session;

static void platform_smc_reset(void) __attribute__((noreturn));
static int qcom_usb_control_halt_restart(unsigned int type);

static void bytes_zero(void *pointer, size_t bytes)
{
    volatile uint8_t *p = pointer;
    while (bytes--) *p++ = 0;
}
static void bytes_copy(void *to, const void *from, size_t bytes)
{
    volatile uint8_t *d = to;
    const volatile uint8_t *s = from;
    while (bytes--) *d++ = *s++;
}
static uint64_t counter(void)
{
    uint64_t value;
    /* Match XNU nonspeculative_timebase and the hardware-qualified UEFI
     * TimerLib: intervals use CNTVCT with CNTFRQ, not the physical counter. */
    __asm__ volatile("isb\nmrs %0, cntvct_el0" : "=r"(value) :: "memory");
    return value;
}
static void diag_reset(unsigned int seconds);
static void platform_smc_reset(void)
{
    /* Existing hardware-qualified final reset sequence. The diagnostic path
     * and the opted-in control callback share this exact clobber contract. */
#if QUD_PSCI_SMC_RESET
    __asm__ volatile("movz w0, #9\nmovk w0, #0x8400, lsl #16\n"
                     "mov x1, xzr\nmov x2, xzr\nmov x3, xzr\ndsb sy\nsmc #0"
                     ::: "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7",
                         "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
                         "x16", "x17", "memory");
#endif
    for (;;) __asm__ volatile("wfe" ::: "memory");
}
/* Reset straight to the bootloader, skipping the orderly shutdown path.
 *
 * reboot_kernel(0, NULL) unmounts every filesystem first, and measured 2026-10-27 that
 * path panics on this board: vfs_unmountall -> dounmount -> apfs_unmount -> vflush ->
 * vnode_rele_ext reports "usecount -ve : 4294967295" at vfs_subr.c:2692, because the
 * SystemOS union mount drops one more reference on the root vnode than it took. The
 * panic lands before the reset, so the IMEM reason cell is never written and the phone
 * cannot come back to fastboot.
 *
 * This path writes the reason cell and issues PSCI SYSTEM_RESET with no filesystem work
 * at all. A reset discards nothing that the volumes hold, which is what a reset is. */
void qcom_usb_debug_control_reset_now(void)
{
    /* Hand the reset to the board's own implementation instead of writing IMEM here.
     *
     * BootReset.c maps the devicetree IMEM window with ml_io_map_unmappable(..., 7)
     * because ml_io_map hands out permanent io_submap mappings that ml_io_unmap cannot
     * free, and it reads the cell back and compares against the values the bootloader is
     * known to accept. Measured 2026-10-27: this module's own 4-byte ml_io_map write
     * restarted the kernel instead of returning to fastboot, twice.
     *
     * boot_reset_bootloader writes the cell, verifies it, then issues PSCI SYSTEM_RESET,
     * and returns only when something refused; text says what. */
    extern int boot_reset_bootloader(char *, unsigned) __attribute__((weak_import));
    if (&boot_reset_bootloader) {
        char text[160];
        int rc = boot_reset_bootloader(text, sizeof(text));
        /* Only reached when it refused, since success resets the SoC. */
        {
            char line[192];
            char *q = line;
            const char *pre = "QCOM USB REBOOT refused: rc=";
            while (*pre) *q++ = *pre++;
            if (rc < 0 || rc > 99) rc = 99;
            *q++ = (char)('0' + (rc / 10) % 10);
            *q++ = (char)('0' + rc % 10);
            *q++ = ' ';
            for (const char *s = text; *s && q < line + sizeof(line) - 2; ++s) *q++ = *s;
            *q++ = '\n';
            (void)qcom_usb_debug_enqueue(line, (size_t)(q - line));
        }
        (void)qcom_usb_debug_wait_drained(200);
    }
    platform_smc_reset();
}
static int qcom_usb_control_halt_restart(unsigned int type)
{
    if (type == CONTROL_PE_RESTART_CPU &&
        __atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&control_reboot_armed, __ATOMIC_ACQUIRE))
        platform_smc_reset();
    return -1;
}
static void diag_checkpoint(unsigned int checkpoint)
{
    if (!usb_diag_enabled) return;
    usb_diag_checkpoint = checkpoint;
    native.status.diag_checkpoint = checkpoint;
    __asm__ volatile("dsb ishst" ::: "memory");
    if (usb_diag_target == checkpoint) diag_reset(0);
}
static void diag_reset(unsigned int seconds)
{
    uint64_t frequency, start, ticks;
    if (!usb_diag_enabled) return;
    /* Standalone early-panic path: no CPU-data, USB guard, EFI or deadline
     * dependency. A second panic while timing goes directly to reset. */
    __asm__ volatile("msr daifset, #15" ::: "memory");
    if (__atomic_exchange_n(&usb_diag_resetting, 1u, __ATOMIC_RELAXED)) seconds = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    /* Probe204 uses the separately device-verified 19.2MHz reference so
     * an incorrect advertised CNTFRQ cannot corrupt its own classification. */
    if (usb_diag_mode == 204u) frequency = UINT64_C(19200000);
    if (frequency >= 1000u && frequency <= COUNTER_MAX_FREQUENCY && seconds <= 256u) {
        start = counter(); ticks = frequency * seconds;
        while (counter() - start < ticks) __asm__ volatile("yield" ::: "memory");
    }
    platform_smc_reset();
}
static void barrier(void *cookie)
{
    (void)cookie;
    __asm__ volatile("dsb sy" ::: "memory");
}
static int deadline_expired(void)
{
    if (native.deadline_active && counter() >= native.deadline) {
        native.deadline_expired = 1;
        return 1;
    }
    return 0;
}
static int acquire(uint64_t *saved_daif)
{
    if (cpu_number() != master_cpu) return 0;
    __asm__ volatile("mrs %0, daif\nmsr daifset, #3" : "=r"(*saved_daif) :: "memory");
    if (__atomic_exchange_n(&usb_busy, 1u, __ATOMIC_ACQUIRE)) {
        __asm__ volatile("msr daif, %0" :: "r"(*saved_daif) : "memory");
        return 0;
    }
    return 1;
}
static void release(uint64_t saved_daif)
{
    __atomic_store_n(&usb_busy, 0u, __ATOMIC_RELEASE);
    __asm__ volatile("msr daif, %0" :: "r"(saved_daif) : "memory");
}
static size_t backlog_write(const void *bytes, size_t count)
{
    uint64_t flags;
    size_t accepted;
    /* No CPU-data access: console hooks may run before PE_init_kprintf.
     * Local IRQ/FIQ masking prevents self-interruption inside the ring guard;
     * other CPUs and recursive producers never spin waiting for this CPU. */
    __asm__ volatile("mrs %0, daif\nmsr daifset, #3" : "=r"(flags) :: "memory");
    accepted = soc_usb_buffer_write(&usb_log_buffer, bytes, count);
    __asm__ volatile("msr daif, %0" :: "r"(flags) : "memory");
    return accepted;
}
static size_t backlog_sink(void *cookie, const void *bytes, size_t count)
{
    (void)cookie;
    return dwc3_queue(native.core, bytes, count);
}
static void backlog_drain_owned(void)
{
    size_t free_bytes = DWC3_TX_BYTES - dwc3_queued(native.core);
    /* usb_busy already owns DWC3 and masks local IRQ/FIQ. Only queue CPU
     * memory while the backlog guard is held; MMIO is serviced afterwards. */
    if (free_bytes)
        usb_log_replayed += soc_usb_buffer_drain(&usb_log_buffer, backlog_sink, 0, free_bytes);
}
static void buffer_status_owned(void)
{
    native.status.buffer_capacity = sizeof(usb_log_bytes);
    native.status.buffer_pending = soc_usb_buffer_pending(&usb_log_buffer);
    native.status.buffer_high_water = __atomic_load_n(&usb_log_buffer.high_water, __ATOMIC_RELAXED);
    native.status.buffer_accepted = __atomic_load_n(&usb_log_buffer.accepted, __ATOMIC_RELAXED);
    native.status.buffer_overflow = __atomic_load_n(&usb_log_buffer.overflow, __ATOMIC_RELAXED);
    native.status.buffer_contended = __atomic_load_n(&usb_log_buffer.contended, __ATOMIC_RELAXED);
    native.status.buffer_replayed = usb_log_replayed;
    native.status.transport_pending = native.core_initialized ? dwc3_queued(native.core) : 0;
    native.status.dropped = soc_usb_buffer_dropped(&usb_log_buffer) +
                           (native.core_initialized ? dwc3_dropped(native.core) : 0);
    native.status.rx_pending = native.core_initialized ? (uint32_t)dwc3_rx_pending(native.core) : 0;
    native.status.rx_overflow = native.core_initialized ? dwc3_rx_overflow(native.core) : 0;
    native.status.rx_generation = native.core_initialized ? dwc3_rx_generation(native.core) : 0;
    native.status.control_enabled = __atomic_load_n(&control_enabled, __ATOMIC_ACQUIRE);
    native.status.control_hook_owned = __atomic_load_n(&control_hook_owned, __ATOMIC_ACQUIRE);
    native.status.control_rejected = __atomic_load_n(&control_rejected, __ATOMIC_RELAXED);
    native.status.control_replies = __atomic_load_n(&control_reply_count, __ATOMIC_RELAXED);
    native.status.control_reboot_state = !__atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE) ? 0u :
        (__atomic_load_n(&control_reboot_taken, __ATOMIC_ACQUIRE) ? 2u : 1u);
}
static char *append_text(char *to, const char *text)
{
    while (*text) *to++ = *text++;
    return to;
}
static char *append_decimal(char *to, uint64_t value)
{
    char digits[20];
    unsigned int n = 0;
    do { digits[n++] = (char)('0' + value % 10u); value /= 10u; } while (value);
    while (n) *to++ = digits[--n];
    return to;
}
/* Registers are read as words; decimal would make them unreadable against the manual. */
static char *append_hex(char *to, uint64_t value)
{
    static const char digits[16] = "0123456789abcdef";
    int shift;
    for (shift = 60; shift > 0 && !((value >> shift) & 0xfu); shift -= 4) { }
    for (; shift >= 0; shift -= 4) *to++ = digits[(value >> shift) & 0xfu];
    return to;
}
/* Small in-band accounting records use no printf/console recursion. */
static size_t loss_record(char *text, int final)
{
    char *p = append_text(text, final ? "\nQCOM USB DRAIN pending=0" : "\nQCOM USB LOSS");
    p = append_text(p, " overflow=");
    p = append_decimal(p, __atomic_load_n(&usb_log_buffer.overflow, __ATOMIC_RELAXED));
    p = append_text(p, " contention=");
    p = append_decimal(p, __atomic_load_n(&usb_log_buffer.contended, __ATOMIC_RELAXED));
    p = append_text(p, " transport=");
    p = append_decimal(p, dwc3_dropped(native.core));
    *p++ = '\n';
    return (size_t)(p - text);
}
static void loss_notice_owned(void)
{
    char text[160];
    size_t bytes;
    uint64_t dropped = soc_usb_buffer_dropped(&usb_log_buffer) + dwc3_dropped(native.core);
    if (dropped == usb_log_loss_reported || soc_usb_buffer_pending(&usb_log_buffer)) return;
    bytes = loss_record(text, 0);
    if (DWC3_TX_BYTES - dwc3_queued(native.core) >= bytes &&
        dwc3_queue(native.core, text, bytes) == bytes) usb_log_loss_reported = dropped;
}
/* Panel-visible state. The console cannot report its own failure: once dwc3 stops being
 * serviced the CDC device leaves the bus and every later line is lost, so the only channel
 * that survives is the frame buffer overlay. BootProgress reads these four. */
volatile uint32_t qcom_usb_diag_phase, qcom_usb_diag_failed, qcom_usb_diag_recoveries, qcom_usb_diag_dropped;
static uint32_t usb_recoveries;
static void usb_diag_update(void)
{
    qcom_usb_diag_phase = (uint32_t)native.status.phase;
    qcom_usb_diag_recoveries = usb_recoveries;
    qcom_usb_diag_dropped = native.status.dropped;
    if (native.core_initialized && native.core) {
        struct dwc3_snapshot snap;
        dwc3_snapshot(native.core, &snap);
        qcom_usb_diag_failed = (uint32_t)snap.failed;
    } else qcom_usb_diag_failed = 0;
}
static void fail(unsigned int error, int core_result)
{
    native.status.error = error;
    native.status.core_result = core_result;
    native.status.phase = QCOM_USB_DEBUG_REFUSED;
    native.status.dma_lease_active = native.lease.active;
    /* Say which check refused, on the panel. The module otherwise gives up silently: no USB
     * device appears and nothing anywhere says why, because the console it would have reported
     * on is the one that failed. boot_progress_stage writes into the diagnostic page the board
     * already draws, so one photograph names the failing precondition. Weak, so a board without
     * that page still links and simply loses the line. */
    /* A persistent row, not boot_progress_stage: that ring keeps six lines, and this runs at
     * time=0s, so the exit log overwrites the line within seconds and a refused controller then
     * looks exactly like one that never ran. boot_progress_persist redraws every frame.
     * Built by hand: the module's import audit allows only a fixed set of XNU symbols and
     * snprintf is not one of them, so the in-band append helpers format the line. */
    extern void boot_progress_persist(const char *, uint32_t) __attribute__((weak_import));
    if (&boot_progress_persist) {
        char line[384];
        char *p = append_text(line, "QCOM USB REFUSED error=");
        p = append_decimal(p, error);
        p = append_text(p, " core=");
        p = append_decimal(p, (uint64_t)(unsigned int)core_result);
        /* The domain refusal at error 10 is one of twelve distinct results (QusbDomainOkay=0
         * through QusbDomainCleanFailed=12 in QcomUsbDmaDomain.h), and which one it is decides
         * the fix: StreamExists means firmware still owns the stream, NoSlot means the SMMU has
         * no free context, ReadFailed means the registers did not answer at all. */
        p = append_text(p, " domain=");
        p = append_decimal(p, (uint64_t)native.status.domain_result);
        /* For StreamExists, scan() records what firmware mapped SID 0 to: smr and s2cr.
         * s2cr bits 17:16 are the memory-access type (0 translation, 1 bypass, 2 fault), and
         * that decides the fix, so both words are printed. */
        p = append_text(p, " smr=");
        p = append_hex(p, native.status.domain_last_address);
        p = append_text(p, " s2cr=");
        p = append_hex(p, native.status.domain_last_readback & 0xffffffffu);
        /* The endpoint-command timeout that returns -4 from dwc3_init leaves no trace of why:
         * the controller answered the reset and passed every parameter check, then a DEPSTARTCFG
         * or DEPCFG never completed. Those commands share exactly one dependency, the event ring
         * the controller writes completions into, so the register set that decides it is the
         * event base and its count next to the device state. Read them here, while the MMIO is
         * still ours: restore_after_failure() runs right after this and gives the lease back. */
        if (native.core_initialized && native.core) {
            struct dwc3_snapshot snap;
            dwc3_snapshot(native.core, &snap);
            p = append_text(p, " gctl="); p = append_hex(p, snap.gctl);
            p = append_text(p, " dctl="); p = append_hex(p, snap.dctl);
            p = append_text(p, " dsts="); p = append_hex(p, snap.dsts);
            p = append_text(p, " evt="); p = append_decimal(p, snap.event_count);
            p = append_text(p, " eps="); p = append_decimal(p, snap.enabled_endpoints);
            p = append_text(p, " iova="); p = append_hex(p, native.status.dma_iova);
            /* ep_command() reports a non-zero completion status by calling trace() with
             * (ep << 16) | cmd and the raw DEPCMD result, and core_trace() stores both. That is
             * the pair that says which endpoint command the controller refused and why, so it
             * belongs on this line rather than needing a new capture path. */
            p = append_text(p, " cmd="); p = append_hex(p, native.status.trace_a);
            p = append_text(p, " st="); p = append_hex(p, native.status.trace_b);
            p = append_text(p, " tr="); p = append_decimal(p, native.status.traces);
        }
        p = append_text(p, " sctlr=");
        p = append_hex(p, native.status.domain_last_readback >> 32);
        p = append_text(p, " ttbr0=");
        p = append_hex(p, native.status.domain_last_expected);
        *p = 0;
        /* 0xffff6600: the amber refusal colour the panel already uses for failures. */
        boot_progress_persist(line, 0xffff6600u);
    }
}
static int in_range(uint64_t address, uint64_t base, uint64_t bytes)
{
    return !(address & 3u) && address >= base && address - base <= bytes - 4;
}
static volatile uint32_t *mmio(uint64_t physical)
{
    vm_offset_t base = 0;
    uint64_t offset = 0;
    if (in_range(physical, QUSB_GCC_BASE, GCC_BYTES)) {
        base = native.gcc_va; offset = physical - QUSB_GCC_BASE;
    } else if (in_range(physical, QUSB_DWC3_BASE, USB_BYTES)) {
        base = native.usb_va; offset = physical - QUSB_DWC3_BASE;
    } else if (in_range(physical, QUSB_SMMU_BASE, QUSB_SMMU_BYTES)) {
        base = native.smmu_va; offset = physical - QUSB_SMMU_BASE;
    }
    if (!base) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return 0;
    }
    return (volatile uint32_t *)(uintptr_t)(base + offset);
}
static int physical_read(void *cookie, QUSB_U64 address, QUSB_U32 *value)
{
    volatile uint32_t *p;
    (void)cookie;
    if (!value || deadline_expired()) {
        native.status.io_failed = 1;
        return 0;
    }
    p = mmio(address);
    if (!p) return 0;
    barrier(0); *value = *p; barrier(0);
    return 1;
}
static int physical_write(void *cookie, QUSB_U64 address, QUSB_U32 value)
{
    volatile uint32_t *p;
    (void)cookie;
    if (deadline_expired()) { native.status.io_failed = 1; return 0; }
    /* Only USB registers or shared-policy-approved selected-domain registers
     * can be written. Private CB, local TLB and exact SID40 SMR/S2CR writes
     * must match the recorded phase/value. Global SMMU/GCC writes are denied. */
    if (!in_range(address, QUSB_DWC3_BASE, USB_BYTES) &&
        !in_range(address, QUSB_SMMU_BASE, QUSB_SMMU_BYTES)) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return 0;
    }
    if (in_range(address, QUSB_SMMU_BASE, QUSB_SMMU_BYTES) &&
        !QcomUsbDomainWriteAllowed(&native.lease, address, value, 4)) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return 0;
    }
    if (in_range(address, QUSB_SMMU_BASE, QUSB_SMMU_BYTES)) {
        QUSB_U32 cr0;
        if (!physical_read(0, QUSB_SMMU_BASE, &cr0) ||
            (cr0 & 0x409u) != (native.lease.original_cr0 & 0x409u)) {
            native.status.io_failed = 1;
            native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
            return 0;
        }
    }
    p = mmio(address);
    if (!p) return 0;
    barrier(0); *p = value; barrier(0);
    return 1;
}
static int physical_read64(void *cookie, QUSB_U64 address, QUSB_U64 *value)
{
    volatile QUSB_U64 *p;
    (void)cookie;
    if (!value || (address & 7u) || address < QUSB_SMMU_BASE ||
        address - QUSB_SMMU_BASE > QUSB_SMMU_BYTES - 8u || !native.smmu_va || deadline_expired()) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return 0;
    }
    p = (volatile QUSB_U64 *)(uintptr_t)(native.smmu_va + address - QUSB_SMMU_BASE);
    barrier(0); *value = *p; barrier(0);
    return 1;
}
static int physical_write64(void *cookie, QUSB_U64 address, QUSB_U64 value)
{
    volatile QUSB_U64 *p;
    QUSB_U32 cr0;
    (void)cookie;
    if ((address & 7u) || address < QUSB_SMMU_BASE ||
        address - QUSB_SMMU_BASE > QUSB_SMMU_BYTES - 8u || !native.smmu_va || deadline_expired() ||
        !QcomUsbDomainWriteAllowed(&native.lease, address, value, 8) ||
        !physical_read(0, QUSB_SMMU_BASE, &cr0) ||
        (cr0 & 0x409u) != (native.lease.original_cr0 & 0x409u)) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return 0;
    }
    /* One aligned volatile 64-bit access: never split a high TTBR into two
     * 32-bit writes with an intermediate truncated translation-table base. */
    p = (volatile QUSB_U64 *)(uintptr_t)(native.smmu_va + address - QUSB_SMMU_BASE);
    barrier(0); *p = value; barrier(0);
    return 1;
}
static uint32_t core_read(void *cookie, uint32_t offset)
{
    QUSB_U32 value;
    if (offset >= USB_BYTES || (offset & 3u) ||
        !physical_read(cookie, QUSB_DWC3_BASE + offset, &value)) return UINT32_MAX;
    if (usb_diag_polling && usb_diag_mode == 201u && offset == 0xc40cu)
        diag_reset(value == 0u ? 0u : (value <= 4096u && !(value & 3u)) ? 4u : 8u);
    return value;
}
static void core_write(void *cookie, uint32_t offset, uint32_t value)
{
    if (offset >= USB_BYTES || (offset & 3u)) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return;
    }
    (void)physical_write(cookie, QUSB_DWC3_BASE + offset, value);
}
static void delay_us(void *cookie, uint32_t usec)
{
    uint64_t start, ticks, iterations;
    (void)cookie;
    if (!usec || deadline_expired()) return;
    /* Core and qualification delays are 1us or 1ms; refuse unbounded input. */
    if (usec > 10000u || !native.status.counter_frequency) {
        native.status.io_failed = 1;
        return;
    }
    start = counter();
    ticks = (native.status.counter_frequency * usec + 999999u) / 1000000u;
    iterations = (uint64_t)usec * 1024u + 1024u;
    while (counter() - start < ticks && iterations--) {
        if (deadline_expired()) return;
        __asm__ volatile("yield" ::: "memory");
    }
    if (counter() - start < ticks) native.status.io_failed = 1;
}
static int cache_sync(void *cpu, size_t bytes, enum dwc3_sync_direction direction,
                       const void *arena, size_t arena_bytes)
{
    uintptr_t address = (uintptr_t)cpu, start, end;
    uintptr_t base = (uintptr_t)arena;
    size_t line = (size_t)native.status.cache_line;
    if (!bytes) return 1;
    if (!line || address < base || address - base >= arena_bytes ||
        bytes > arena_bytes - (address - base)) {
        native.status.io_failed = 1;
        native.status.error = QCOM_USB_DEBUG_CALLBACK_RANGE_FAILED;
        return 0;
    }
    start = address & ~(line - 1u);
    end = (address + bytes + line - 1u) & ~(line - 1u);
    for (; start < end; start += line) {
        if (direction == DWC3_TO_DEVICE)
            __asm__ volatile("dc cvac, %0" :: "r"(start) : "memory");
        else
            __asm__ volatile("dc ivac, %0" :: "r"(start) : "memory");
    }
    barrier(0);
    return 1;
}
static void dma_sync(void *cookie, void *cpu, size_t bytes,
                     enum dwc3_sync_direction direction)
{
    (void)cookie;
    if (usb_diag_polling && usb_diag_mode == 202u && direction == DWC3_FROM_DEVICE)
        diag_reset(0);
    if (cache_sync(cpu, bytes, direction, usb_dma, sizeof(usb_dma)) &&
        usb_diag_polling && usb_diag_mode == 203u && direction == DWC3_FROM_DEVICE)
        diag_reset(0);
}
static int clean_tables(void *cookie, void *cpu, QUSB_U32 bytes)
{
    (void)cookie;
    if (deadline_expired()) return 0;
    return cache_sync(cpu, bytes, DWC3_TO_DEVICE, usb_tables, sizeof(usb_tables));
}
static void core_trace(void *cookie, enum dwc3_trace event, uint32_t a, uint32_t b)
{
    (void)cookie;
    native.status.last_trace = (uint32_t)event;
    native.status.trace_a = a; native.status.trace_b = b;
    native.status.traces++;
}
static int halt_inherited(void)
{
    QUSB_U32 control, status;
    unsigned int i;
    if (!physical_read(0, QUSB_DWC3_BASE + USB_DCTL, &control) ||
        !physical_write(0, QUSB_DWC3_BASE + USB_DCTL, control & ~USB_RUN_STOP)) return 0;
    for (i = 0; i < 10000u; i++) {
        if (!physical_read(0, QUSB_DWC3_BASE + USB_DCTL, &control) ||
            !physical_read(0, QUSB_DWC3_BASE + USB_DSTS, &status)) return 0;
        if (!(control & USB_RUN_STOP) && (status & USB_HALTED)) return 1;
        delay_us(0, 1);
    }
    return 0;
}
static int restore_lease(void)
{
    QUSB_DOMAIN_RESULT result;
    if (!native.lease.active) return 1;
    if (!halt_inherited()) return 0;
    result = QcomUsbDomainRestore(&native.domain_platform, &native.lease);
    native.status.restore_result = (uint32_t)result;
    native.status.dma_lease_active = native.lease.active;
    native.status.domain_stage = native.lease.stage;
    native.status.domain_last_address = native.lease.last_address;
    native.status.domain_last_expected = native.lease.last_expected;
    native.status.domain_last_readback = native.lease.last_readback;
    return result == QusbDomainOkay;
}
static int acquire_vbus(void)
{
    QUSB_U32 saved, check;
    if (!halt_inherited() ||
        !physical_read(0, QUSB_DWC3_BASE + USB_HS_PHY_CTRL, &saved)) return 0;
    native.status.vbus_saved = saved;
    if ((saved & USB_SW_VBUS_MASK) == USB_SW_VBUS_MASK) return 1;
    /* The pinned dwc3-msm-core.c uses QSCRATCH software session-valid/VBUS.
     * Mark ownership before a write because even a failed readback can leave
     * a side effect. Only these two bits will be restored after a proven halt. */
    native.status.vbus_owned = 1;
    if (!physical_write(0, QUSB_DWC3_BASE + USB_HS_PHY_CTRL, saved | USB_SW_VBUS_MASK) ||
        !physical_read(0, QUSB_DWC3_BASE + USB_HS_PHY_CTRL, &check)) return 0;
    return (check & USB_SW_VBUS_MASK) == USB_SW_VBUS_MASK;
}
static int restore_vbus(void)
{
    QUSB_U32 current, wanted, check;
    if (!native.status.vbus_owned) return 1;
    if (!halt_inherited() ||
        !physical_read(0, QUSB_DWC3_BASE + USB_HS_PHY_CTRL, &current)) return 0;
    /* Preserve all unrelated bits. If our failed set never took effect, the
     * original selected bits already prove no restoration write is needed. */
    if ((current & USB_SW_VBUS_MASK) == (native.status.vbus_saved & USB_SW_VBUS_MASK)) {
        native.status.vbus_owned = 0;
        return 1;
    }
    if ((current & USB_SW_VBUS_MASK) != USB_SW_VBUS_MASK) return 0;
    wanted = (current & ~USB_SW_VBUS_MASK) | (native.status.vbus_saved & USB_SW_VBUS_MASK);
    if (!physical_write(0, QUSB_DWC3_BASE + USB_HS_PHY_CTRL, wanted) ||
        !physical_read(0, QUSB_DWC3_BASE + USB_HS_PHY_CTRL, &check) ||
        (check & USB_SW_VBUS_MASK) != (wanted & USB_SW_VBUS_MASK)) return 0;
    native.status.vbus_owned = 0;
    return 1;
}
static int phy_core_halted(void)
{
    QUSB_U32 control, status;
    return physical_read(0, QUSB_DWC3_BASE + USB_DCTL, &control) &&
           physical_read(0, QUSB_DWC3_BASE + USB_DSTS, &status) &&
           !(control & USB_RUN_STOP) && (status & USB_HALTED);
}
static int acquire_phy(void)
{
    QUSB_U32 current, check;
    if (native.status.phy_owned || !phy_core_halted() ||
        !physical_read(0, QUSB_DWC3_BASE + USB_GUSB2PHYCFG, &current)) return 0;
    native.status.phy_saved = current & USB_PHY_KEEP_AWAKE_MASK;
    native.status.phy_applied = native.status.phy_restoring = 0;
    /* DT dis_u2_susphy requires bit6 clear. Holding bit8 clear is this
     * HS-only, non-LPM polling diagnostic's keep-awake policy. Save only the
     * selected controls and record a possible side effect before writing. */
    native.status.phy_owned = 1;
    if (!physical_write(0, QUSB_DWC3_BASE + USB_GUSB2PHYCFG,
                         current & ~USB_PHY_KEEP_AWAKE_MASK) ||
        !physical_read(0, QUSB_DWC3_BASE + USB_GUSB2PHYCFG, &check) ||
        (check & USB_PHY_KEEP_AWAKE_MASK)) return 0;
    native.status.phy_applied = 1;
    return 1;
}
static int restore_phy(void)
{
    QUSB_U32 current, selected, wanted, check;
    if (!native.status.phy_owned) return 1;
    if (native.status.stop_failed || native.status.vbus_owned || native.lease.active ||
        !phy_core_halted() ||
        !physical_read(0, QUSB_DWC3_BASE + USB_GUSB2PHYCFG, &current)) return 0;
    selected = current & USB_PHY_KEEP_AWAKE_MASK;
    if ((!native.status.phy_applied || native.status.phy_restoring) &&
        selected == native.status.phy_saved) {
        /* Failed acquire never changed the controls, or a prior restore
         * write completed despite its callback/readback failing. */
        native.status.phy_owned = native.status.phy_applied = native.status.phy_restoring = 0;
        return 1;
    }
    if (selected) return 0; /* Unexpected owner changed our active lease. */
    wanted = (current & ~USB_PHY_KEEP_AWAKE_MASK) | native.status.phy_saved;
    native.status.phy_restoring = 1;
    if (!physical_write(0, QUSB_DWC3_BASE + USB_GUSB2PHYCFG, wanted) ||
        !physical_read(0, QUSB_DWC3_BASE + USB_GUSB2PHYCFG, &check) ||
        (check & USB_PHY_KEEP_AWAKE_MASK) != native.status.phy_saved) return 0;
    native.status.phy_owned = native.status.phy_applied = native.status.phy_restoring = 0;
    return 1;
}
static int restore_after_failure(void)
{
    /* A failed stop must retain both leases and all static DMA buffers.
     * Never change VBUS or SMMU merely because another status bit looked idle. */
    if (native.status.stop_failed) return 0;
    if ((native.core_initialized && dwc3_stop(native.core)) || !halt_inherited()) {
        native.status.stop_failed = 1; return 0;
    }
    if (!restore_vbus() || !restore_lease() || !restore_phy()) return 0;
    return 1;
}
static int dma_addressable(uint64_t physical, size_t bytes, uint32_t width)
{
    uint64_t last;
    if (!bytes || width < 32u || width > 64u || physical > UINT64_MAX - (bytes - 1u)) return 0;
    last = physical + bytes - 1u;
    return width == 64u || (last >> width) == 0;
}
static int validate_static_pages(const uint8_t *arena, size_t arena_bytes, uint64_t *address)
{
    uint64_t base, physical;
    size_t offset;
    if (!arena_bytes || (arena_bytes & (PAGE_BYTES - 1u))) return 0;
    base = (uint64_t)ml_static_vtop((vm_offset_t)(uintptr_t)arena);
    if ((base & (PAGE_BYTES - 1u)) || base > UINT64_MAX - arena_bytes) return 0;
    for (offset = 0; offset < arena_bytes; offset += PAGE_BYTES) {
        physical = (uint64_t)ml_static_vtop((vm_offset_t)(uintptr_t)(arena + offset));
        if (physical != base + offset) return 0;
        physical = (uint64_t)ml_static_vtop((vm_offset_t)(uintptr_t)(arena + offset + PAGE_BYTES - 1));
        if (physical != base + offset + PAGE_BYTES - 1) return 0;
    }
    *address = base;
    return 1;
}

/* gts7xlwifi: the inherited state the qualify step saw, for the BootLog page: [0] result | resumed mask << 8 | rounds << 16,
 * [1..5] the five CBCRs, [6] BCR, [7] GDSCR, [8] CFG_GDSCR, [9] GCTL, [10] DCTL, [11] DSTS; the second round at [12..23]. */
volatile uint32_t t970_qud_inherited[24];
static void t970_qud_record(const QUSB_INHERITED *in, uint32_t result, uint32_t resumed)
{
    const unsigned int base = (t970_qud_inherited[0] >> 16) ? 12u : 0u;
    volatile uint32_t *r = t970_qud_inherited + base;
    r[0] = result | (resumed & 0xff) << 8 | (base ? 2u : 1u) << 16 | (resumed >> 8) << 20;
    for (unsigned int i = 0; i < 5; ++i) r[1 + i] = in->gcc_cbcr[i];
    r[6] = in->gcc_bcr; r[7] = in->gcc_gdscr; r[8] = in->gcc_cfg_gdscr;
    r[9] = in->gctl; r[10] = in->dctl; r[11] = in->dsts;
}

/* gts7xlwifi: the SM8250 USB2 PHY (usb_1_hsphy, "qcom,usb-hsphy-snps-femto", 0x88e3000 + 0x110) after UEFI has powered it down
 * (periph3: DEPSTARTCFG on 0xc80c never completes, no PHY clock). Sequence of Linux phy-qcom-snps-femto-v2.c / msm
 * phy-msm-snps-hs.c init: block reset (GCC_QUSB2PHY_PRIM_BCR 0x12000), common override + POR, FSEL, PLLBTUNE, VBUS valid
 * external select, REFCLK, VREGBYPASS, SUSPEND_N, the board's qcom,param-override-seq (value, offset pairs: e7@6c 0b@70
 * 2f@74 03@78), SLEEPM, SIDDQ off, POR off, override off. Only when the late retry asks for it (t970_qud_phy_wanted).
 * t970_qud_phy: [0] step reached (negative: failed) | 0x100 done, [1..8] CFG0 UTMI_CTRL0 UTMI_CTRL5 COMMON0 COMMON1 COMMON2
 * CTRL1 CTRL2 at the end, [9] retry calls, [10] retry result (phase << 8 | error), [11] init's core result. */
volatile int32_t t970_qud_phy[15];
static volatile uint32_t t970_qud_phy_wanted;
static void t970_wait_us(uint64_t frequency, uint64_t us)
{
    const uint64_t t0 = counter(), span = frequency / 1000000u * us + 1u;
    while (counter() - t0 < span) __asm__ volatile("yield" ::: "memory");
}
static vm_offset_t t970_phy_va;   /* mapped by qcom_usb_debug_t970_retry with interrupts on */
static int t970_hsphy_init(uint64_t frequency)
{
    volatile uint32_t *gcc = (volatile uint32_t *)(uintptr_t)native.gcc_va;
    vm_offset_t va = t970_phy_va;
    volatile uint32_t *phy = (volatile uint32_t *)(uintptr_t)va;
#define T970_RMW(off, clear, set) do { phy[(off) / 4] = (phy[(off) / 4] & ~(uint32_t)(clear)) | (uint32_t)(set); \
        __asm__ volatile("dsb sy" ::: "memory"); } while (0)
    if (!va || !gcc) { t970_qud_phy[0] = -1; return -1; }
    t970_qud_phy[0] = 1;
    gcc[0x12000u / 4] |= 1u;  __asm__ volatile("dsb sy" ::: "memory"); t970_wait_us(frequency, 100);
    gcc[0x12000u / 4] &= ~1u; __asm__ volatile("dsb sy" ::: "memory"); t970_wait_us(frequency, 100);
    t970_qud_phy[0] = 2;
    T970_RMW(0x94, 0, 1u << 1);                    /* CFG0: UTMI_PHY_CMN_CTRL_OVERRIDE_EN */
    T970_RMW(0x50, 0, 1u << 1);                    /* UTMI_CTRL5: POR */
    T970_RMW(0x54, 7u << 4, 3u << 4);              /* HS_PHY_CTRL_COMMON0: FSEL 19.2 MHz */
    T970_RMW(0x58, 0, 1u << 5 | 1u << 4);          /* COMMON1: PLLBTUNE, VBUSVLDEXTSEL0 */
    T970_RMW(0xa0, 3u, 2u);                        /* REFCLK_CTRL: REFCLK_SEL default */
    T970_RMW(0x5c, 0, 1u << 0);                    /* COMMON2: VREGBYPASS */
    T970_RMW(0x60, 0, 1u << 0);                    /* HS_PHY_CTRL1: VBUSVLDEXT0 */
    T970_RMW(0x64, 0, 1u << 3 | 1u << 2);          /* HS_PHY_CTRL2: USB2_SUSPEND_N_SEL, USB2_SUSPEND_N */
    phy[0x6c / 4] = 0xe7; phy[0x70 / 4] = 0x0b; phy[0x74 / 4] = 0x2f; phy[0x78 / 4] = 0x03;   /* param-override-seq */
    t970_qud_phy[0] = 3;
    T970_RMW(0x3c, 0, 1u << 0);                    /* UTMI_CTRL0: SLEEPM */
    T970_RMW(0x54, 1u << 2, 0);                    /* COMMON0: SIDDQ off (power the analog block) */
    t970_wait_us(frequency, 20);
    T970_RMW(0x50, 1u << 1, 0);                    /* UTMI_CTRL5: POR off */
    t970_wait_us(frequency, 20);
    T970_RMW(0x64, 1u << 3, 0);                    /* HS_PHY_CTRL2: SUSPEND_N_SEL off */
    T970_RMW(0x94, 1u << 1, 0);                    /* CFG0: override off */
    t970_wait_us(frequency, 200);
    {
        static const uint16_t regs[8] = {0x94, 0x3c, 0x50, 0x54, 0x58, 0x5c, 0x60, 0x64};
        for (unsigned int i = 0; i < 8; ++i) t970_qud_phy[1 + i] = (int32_t)phy[regs[i] / 4];
    }
#undef T970_RMW
    /* periph7: the PHY ran and DEPSTARTCFG still timed out: the core has no PIPE clock, because UEFI left the SuperSpeed PHY off. A
     * high-speed-only DWC3 then takes the UTMI clock as its PIPE clock (Linux dwc3-qcom.c dwc3_qcom_select_utmi_clk,
     * qcom,select-utmi-as-pipe-clk): QSCRATCH (0x0a6f8800) GENERAL_CFG (+0x08) PIPE_UTMI_CLK_DIS (bit 8) set, PIPE_UTMI_CLK_SEL (bit 0)
     * and PIPE3_PHYSTATUS_SW (bit 3) set, PIPE_UTMI_CLK_DIS cleared, 100 us apart. [12] reads GENERAL_CFG back. */
    {
        volatile uint32_t *cfg = (volatile uint32_t *)(uintptr_t)(native.usb_va + 0xf8800u + 0x08u);
        *cfg |= 1u << 8;  __asm__ volatile("dsb sy" ::: "memory"); t970_wait_us(frequency, 100);
        *cfg |= 1u << 0 | 1u << 3; __asm__ volatile("dsb sy" ::: "memory"); t970_wait_us(frequency, 100);
        *cfg &= ~(1u << 8); __asm__ volatile("dsb sy" ::: "memory"); t970_wait_us(frequency, 100);
        t970_qud_phy[12] = (int32_t)*cfg;
        /* periph8: ACTIVE but no USB event (ev 0): the host never saw a device, because nothing says VBUS is present. Linux
         * dwc3-qcom.c dwc3_qcom_vbus_override_enable for peripheral mode: QSCRATCH HS_PHY_CTRL (+0x10) UTMI_OTG_VBUS_VALID (bit 20) and
         * SW_SESSVLD_SEL (bit 28), SS_PHY_CTRL (+0x30) LANE0_PWR_PRESENT (bit 24). [13] HS_PHY_CTRL, [14] SS_PHY_CTRL read back. */
        volatile uint32_t *hs = (volatile uint32_t *)(uintptr_t)(native.usb_va + 0xf8800u + 0x10u);
        volatile uint32_t *ss = (volatile uint32_t *)(uintptr_t)(native.usb_va + 0xf8800u + 0x30u);
        *hs |= 1u << 20 | 1u << 28; __asm__ volatile("dsb sy" ::: "memory");
        *ss |= 1u << 24; __asm__ volatile("dsb sy" ::: "memory");
        t970_wait_us(frequency, 100);
        t970_qud_phy[13] = (int32_t)*hs;
        t970_qud_phy[14] = (int32_t)*ss;
    }
    t970_qud_phy[0] = 5 | 0x100;
    return 0;
}

int qcom_usb_debug_init(void)
{
    uint64_t flags, frequency, ctr;
    int enabled = 0, ret = 0;
    QUSB_RESULT result;
    QUSB_DOMAIN_RESULT domain_result;
    QUSB_INHERITED inherited;
    struct dwc3_platform platform;
    static const struct dwc3_identity identity = {
        QUD_USB_VID, QUD_USB_PID, "UniXNU", QUD_USB_PRODUCT, QUD_USB_SERIAL
    };
    if (native.status.phase == QCOM_USB_DEBUG_NOT_INITIALIZED) {
        int diagnostic = 0;
        if (QUD_PSCI_SMC_RESET &&
            PE_parse_boot_argn("qcom-usb-diag", &diagnostic, sizeof(diagnostic)) &&
            (diagnostic == 1 || diagnostic == 2 || (diagnostic >= 101 && diagnostic <= 119) ||
             (diagnostic >= 201 && diagnostic <= 205))) {
            usb_diag_enabled = 1;
            usb_diag_mode = (unsigned int)diagnostic;
            native.status.diag_enabled = 1;
            usb_diag_target = diagnostic >= 101 && diagnostic <= 119 ? (unsigned int)diagnostic - 100u : 0u;
            diag_checkpoint(QCOM_USB_DEBUG_DIAG_BEFORE_ACQUIRE);
            /* Mode2 measures this exact kernel/handoff path's zero-delay
             * reboot baseline before interpreting mode1's encoded delays. */
            if (diagnostic == 2) diag_reset(0);
        }
    }
    if (!acquire(&flags)) {
        if (usb_diag_enabled && native.status.phase == QCOM_USB_DEBUG_NOT_INITIALIZED) {
            fail(QCOM_USB_DEBUG_ACQUIRE_REFUSED, 0);
            diag_reset(12u + 4u * native.status.error);
        }
        return 0;
    }
    if (native.status.phase != QCOM_USB_DEBUG_NOT_INITIALIZED) {
        ret = native.status.phase == QCOM_USB_DEBUG_ACTIVE; goto out;
    }
    if (!PE_parse_boot_argn("qcom-usb-debug", &enabled, sizeof(enabled)) || enabled != 1) {
        native.status.phase = QCOM_USB_DEBUG_DISABLED; goto out;
    }
    /* gts7xlwifi: with the late retry on, the early attempt stays out of the hardware. It cannot power the USB2 PHY, fails at the
     * core init and leaves a DMA domain lease the retry's qualification then refuses (periph5: error 10 DMA_REFUSED). The retry
     * resets the phase and makes the one attempt. */
    {
        int retry = 0;
        if (!t970_qud_phy_wanted && PE_parse_boot_argn("t970-qud-retry", &retry, sizeof(retry)) && retry == 1) {
            native.status.phase = QCOM_USB_DEBUG_DISABLED; goto out;
        }
    }
    native.status.phase = QCOM_USB_DEBUG_PREPARING;
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_AFTER_ACQUIRE);
    if (dwc3_context_bytes() > sizeof(usb_context)) {
        fail(QCOM_USB_DEBUG_CONTEXT_TOO_SMALL, 0); goto out;
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_COUNTER_CACHE);
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    if (usb_diag_mode == 204u)
        diag_reset(frequency == UINT64_C(19200000) ? 0u :
                   frequency == UINT64_C(24000000) ? 4u :
                   frequency == UINT64_C(1000000000) ? 8u : 12u);
    if (frequency < 1000u || frequency > COUNTER_MAX_FREQUENCY) {
        fail(QCOM_USB_DEBUG_BAD_COUNTER, 0); goto out;
    }
    if (usb_diag_mode == 205u) {
        uint64_t v0 = counter(), p0, p1, vd, pd;
        unsigned int samples = 100000000u;
        __asm__ volatile("isb\nmrs %0, cntpct_el0" : "=r"(p0) :: "memory");
        while (samples-- && counter() - v0 < frequency / 4u) __asm__ volatile("yield" ::: "memory");
        __asm__ volatile("isb\nmrs %0, cntpct_el0" : "=r"(p1) :: "memory");
        vd = counter() - v0; pd = p1 - p0;
        /*4s=matching progress;8s=physical frozen;12s=different rate;
         *16s=virtual sampling budget exhausted. Waits use virtual time. */
        diag_reset(vd < frequency / 4u ? 16u : !pd ? 8u :
                   (pd > vd ? pd - vd : vd - pd) > vd / 10u ? 12u : 4u);
    }
    native.status.counter_frequency = frequency;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    native.status.cache_line = UINT64_C(4) << ((ctr >> 16) & 15u);
    if (native.status.cache_line > 64u || native.status.cache_line < 16u) {
        fail(QCOM_USB_DEBUG_CACHE_GRANULE_UNSUPPORTED, 0); goto out;
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_DATA_VTOP);
    if (!validate_static_pages(usb_dma, sizeof(usb_dma), &native.status.dma_pa)) {
        fail(QCOM_USB_DEBUG_DMA_NOT_CONTIGUOUS, 0); goto out;
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_TABLE_VTOP);
    if (!validate_static_pages(usb_tables, sizeof(usb_tables), &native.status.tables_pa)) {
        fail(QCOM_USB_DEBUG_TABLES_NOT_CONTIGUOUS, 0); goto out;
    }
    native.status.dma_iova = QUSB_DOMAIN_IOVA;
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_MAP_GCC);
    /* gts7xlwifi: the late retry runs this with interrupts masked (acquire()) on a live system, where ml_io_map can wait on VM
     * locks forever (periph4: the retry never returned, phase stayed PREPARING). The early attempt's windows are kept and reused. */
    if (!native.gcc_va) native.gcc_va = ml_io_map((vm_offset_t)QUSB_GCC_BASE, GCC_BYTES);
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_MAP_USB);
    if (!native.usb_va) native.usb_va = ml_io_map((vm_offset_t)QUSB_DWC3_BASE, USB_BYTES);
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_MAP_SMMU);
    if (!native.smmu_va) native.smmu_va = ml_io_map((vm_offset_t)QUSB_SMMU_BASE, QUSB_SMMU_BYTES);
    if (!native.gcc_va || !native.usb_va || !native.smmu_va) {
        fail(QCOM_USB_DEBUG_MAP_FAILED, 0); goto out;
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_INSPECT);
    result = QcomUsbInspectInherited(physical_read, 0, &inherited);
    t970_qud_record(&inherited, (uint32_t)result, 0);
    /* gts7xlwifi: the T970's UEFI leaves one or more USB30_PRIM branches gated at hand-off (qcom 3, 2026-11-05). With
     * t970-qud-resume=1, turn each gated CBCR's enable bit on and inspect again. Only the branches are touched; GDSC,
     * resets and the core are left as found, and every later check still applies. */
    if (result == QusbClockOff) {
        int resume = 0;
        static const QUSB_U32 cbcr[5] = QUD_GCC_CBCR_OFFSETS;
        if (PE_parse_boot_argn("t970-qud-resume", &resume, sizeof(resume)) && resume == 1) {
            /* physical_write() refuses GCC on purpose, so this goes through the GCC window init mapped above. The mock UTMI
             * branch (0xf01c) runs from RCG 0xf038 (CFG 0xf03c: TCXO, no divider) through the post-divider 0xf050 (/1); the
             * source is set first, as gcc-kona.c chains them. 2026-11-05 periph2: only that branch was gated (cbcr 80000000). */
            volatile uint32_t *gcc = (volatile uint32_t *)(uintptr_t)native.gcc_va;
            uint32_t mask = 0;
            for (unsigned int i = 0; i < 5; ++i) {
                if (!(inherited.gcc_cbcr[i] & 0x80000000U)) continue;
                if (cbcr[i] == 0xf01cu) {
                    gcc[0xf03cu / 4] = 0;
                    gcc[0xf038u / 4] |= 1u;
                    for (unsigned int k = 0; k < 1000 && (gcc[0xf038u / 4] & 1u); ++k) __asm__ volatile("yield" ::: "memory");
                    gcc[0xf050u / 4] = 0;
                    mask |= 1u << 8;
                }
                gcc[cbcr[i] / 4] |= 1u;
                mask |= 1u << i;
            }
            native.status.io_failed = 0;
            __asm__ volatile("dsb sy" ::: "memory");
            for (uint64_t t0 = counter(); counter() - t0 < frequency / 1000u;) __asm__ volatile("yield" ::: "memory");
            result = QcomUsbInspectInherited(physical_read, 0, &inherited);
            t970_qud_record(&inherited, (uint32_t)result, mask);
        }
    }
    native.status.qcom_result = (uint32_t)result;
    if (result == QusbQualified && t970_qud_phy_wanted) (void)t970_hsphy_init(frequency);
    if (result != QusbQualified) {
        /* In particular do not wake a clock-gated old DMA engine: the native
         * kernel cannot prove ABL's original DMA buffers are still retained. */
        fail(QCOM_USB_DEBUG_INHERITED_STATE_REFUSED, 0); goto out;
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_GATES_WIDTH);
    /* GDSCR HW_CONTROL is bit1; CFG_GDSCR.POWER_UP_COMPLETE is a separate
     * register's bit16, already checked by QcomUsbInspectInherited. */
    if (inherited.gcc_gdscr & 3u) {
        fail(QCOM_USB_DEBUG_UNSTABLE_POWER_STATE, 0); goto out;
    }
    if (((inherited.gctl >> 12) & 3u) != 2u) {
        fail(QCOM_USB_DEBUG_CORE_NOT_DEVICE, 0); goto out;
    }
    {
        QUSB_U32 hwparams0;
        if (!physical_read(0, QUSB_DWC3_BASE + USB_GHWPARAMS0, &hwparams0)) {
            fail(QCOM_USB_DEBUG_DMA_WIDTH_UNSUPPORTED, 0); goto out;
        }
        native.status.hwparams0 = hwparams0;
        /* Linux drivers/usb/dwc3/core.h DWC3_GHWPARAMS0_AWIDTH and TI
         * SPRUHJ7A table3-91: bits31:24 are master address width in bits.
         * DWC3 sees only the fixed IOVA span. The domain backend separately
         * checks high physical DMA/table addresses against SMMU output width.
         * No truncation, identity mapping or BYPASS fallback is used. */
        native.status.dma_address_width = (hwparams0 >> 24) & 0xffu;
        if (!dma_addressable(native.status.dma_iova, sizeof(usb_dma), native.status.dma_address_width)) {
            fail(QCOM_USB_DEBUG_DMA_WIDTH_UNSUPPORTED, 0); goto out;
        }
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_HALT);
    if (!halt_inherited()) { fail(QCOM_USB_DEBUG_HALT_TIMEOUT, 0); goto out; }
    bytes_zero(&native.domain_platform, sizeof(native.domain_platform));
    native.domain_platform.read32 = physical_read;
    native.domain_platform.write32 = physical_write;
    native.domain_platform.read64 = physical_read64;
    native.domain_platform.write64 = physical_write64;
    native.domain_platform.barrier = barrier;
    native.domain_platform.delay_us = delay_us;
    native.domain_platform.clean_tables = clean_tables;
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_DOMAIN);
    domain_result = QcomUsbDomainInstall(&native.domain_platform, usb_tables, native.status.tables_pa,
                                         sizeof(usb_tables), native.status.dma_pa, sizeof(usb_dma), &native.lease);
    native.status.domain_result = (uint32_t)domain_result;
    native.status.domain_stage = native.lease.stage;
    native.status.domain_bank = native.lease.cb;
    native.status.domain_slot = native.lease.index;
    native.status.domain_asid = native.lease.asid;
    native.status.domain_last_address = native.lease.last_address;
    native.status.domain_last_expected = native.lease.last_expected;
    native.status.domain_last_readback = native.lease.last_readback;
    native.status.dma_lease_active = native.lease.active;
    if (domain_result != QusbDomainOkay) {
        fail(QCOM_USB_DEBUG_DMA_REFUSED, 0); (void)restore_lease(); goto out;
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_VERIFY_DOMAIN);
    {
        QUSB_U32 sctlr, s2cr;
        if (!native.lease.active || native.status.io_failed ||
            native.lease.iova != native.status.dma_iova ||
            native.lease.dma_pa != native.status.dma_pa || native.lease.table_pa != native.status.tables_pa ||
            !in_range(native.lease.cb_base, QUSB_SMMU_BASE, QUSB_SMMU_BYTES) ||
            native.lease.index >= native.lease.stream_count ||
            !physical_read(0, native.lease.cb_base, &sctlr) || !(sctlr & 1u) ||
            !physical_read(0, QUSB_SMMU_BASE + 0xc00u + 4u * native.lease.index, &s2cr) ||
            ((s2cr >> 16) & 3u) != 0u || (s2cr & 0xffu) != native.lease.cb) {
            fail(QCOM_USB_DEBUG_DOMAIN_STATE_INVALID, 0); (void)restore_lease(); goto out;
        }
    }
    native.status.dma_mode = QCOM_USB_DEBUG_DMA_PRIVATE_TRANSLATED;
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_VBUS);
    if (!acquire_phy()) {
        fail(QCOM_USB_DEBUG_PHY_LEASE_FAILED, 0); (void)restore_after_failure(); goto out;
    }
    if (!acquire_vbus()) {
        fail(QCOM_USB_DEBUG_VBUS_FAILED, 0); (void)restore_after_failure(); goto out;
    }
    bytes_zero(&platform, sizeof(platform));
    platform.read32 = core_read; platform.write32 = core_write;
    platform.delay_us = delay_us; platform.dma_sync = dma_sync;
    platform.barrier = barrier; platform.trace = core_trace;
    native.core = (struct dwc3 *)(void *)usb_context;
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_CORE_INIT);
    ret = dwc3_init(usb_context, sizeof(usb_context), &platform, &identity, usb_dma,
                         native.status.dma_iova, sizeof(usb_dma));
    if (ret || native.status.io_failed) {
        fail(QCOM_USB_DEBUG_CORE_INIT_FAILED, ret); (void)restore_after_failure(); ret = 0; goto out;
    }
    native.core_initialized = 1;
    {
        static const char banner[] = "QCOM XNU USB CDC ACM v1\n";
        (void)backlog_write(banner, sizeof(banner) - 1);
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_CORE_START);
    ret = dwc3_start(native.core);
    if (ret || native.status.io_failed) {
        fail(QCOM_USB_DEBUG_CORE_START_FAILED, ret);
        (void)restore_after_failure(); ret = 0; goto out;
    }
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_CORE_STARTED);
    usb_diag_polling = 1;
    /* Let the host complete EP0 enumeration before early kernel startup can
     * reach a path with no more log output. Once-per-character polling alone
     * may stop before the host has even requested the device descriptor.
     * This is bounded without installing an asynchronous worker or IRQ. */
    {
        uint64_t start = counter();
        unsigned int attempts = 50000u;
        while (attempts-- && counter() - start < frequency * 5u) {
            int events;
            backlog_drain_owned();
            events = dwc3_poll(native.core, 64);
            native.status.poll_calls++;
            native.status.last_poll_events = events < 0 ? 0u : (uint32_t)events;
            if (events < 0 || native.status.io_failed) {
                usb_diag_polling = 0;
                fail(QCOM_USB_DEBUG_CORE_POLL_FAILED, events);
                (void)restore_after_failure(); ret = 0; goto out;
            }
            if (native.status.poll_calls == 1u) diag_checkpoint(QCOM_USB_DEBUG_DIAG_FIRST_POLL);
            if (dwc3_ready(native.core)) break;
            delay_us(0, 100);
        }
    }
    usb_diag_polling = 0;
    if (usb_diag_mode >= 201u && usb_diag_mode <= 203u) diag_reset(12);
    diag_checkpoint(QCOM_USB_DEBUG_DIAG_ENUM_DONE);
    native.status.phase = QCOM_USB_DEBUG_ACTIVE;
    /* The success counterpart of fail()'s persistent row. Without it a working controller and a
     * controller that never ran both leave the row empty, which is the ambiguity this whole
     * change exists to remove. */
    {
        extern void boot_progress_persist(const char *, uint32_t) __attribute__((weak_import));
        if (&boot_progress_persist)
            boot_progress_persist("QCOM USB ACTIVE (CDC-ACM up)", 0xff00ff88u);
    }
    ret = 1;
out:
    buffer_status_owned();
    release(flags);
    if (usb_diag_enabled && native.status.phase == QCOM_USB_DEBUG_REFUSED)
        diag_reset(12u + 4u * native.status.error);
    return ret;
}

static int poll_owned(void)
{
    int ret;
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE) return 0;
    backlog_drain_owned();
    loss_notice_owned();
    ret = dwc3_poll(native.core, 1);
    native.status.poll_calls++;
    native.status.last_poll_events = ret < 0 ? 0u : (uint32_t)ret;
    buffer_status_owned();
    usb_diag_update();
    if (ret < 0 || native.status.io_failed) {
        /* Recover before reporting. dwc3_poll latches d->failed and nothing cleared it, so a
         * single bad poll used to end the console permanently: the phase became IO_FAILED, the
         * worker thread exited on its next status read, nothing drained the event ring or
         * answered IN tokens, and the host reset the port (the CDC device left the bus at
         * t=5 s, 35 s and 110 s in different boots while the system kept running). Bringing the
         * controller back here keeps the phase ACTIVE, so the worker never sees the failure.
         * The budget stops a genuinely dead controller from spinning forever. */
        if (usb_recoveries < 64u && dwc3_recover(native.core) == 0) {
            usb_recoveries++;
            native.status.io_failed = 0;
            native.status.core_result = 0;
            native.status.error = 0;
            return 1;
        }
        native.status.core_result = ret;
        native.status.error = native.deadline_active && native.deadline_expired ? QCOM_USB_DEBUG_DEADLINE_EXPIRED :
                                                      QCOM_USB_DEBUG_CORE_POLL_FAILED;
        native.status.phase = QCOM_USB_DEBUG_IO_FAILED;
        return 0;
    }
    return 1;
}
static uint64_t control_mix(uint64_t value)
{
    value ^= value >> 30;
    value *= UINT64_C(0xbf58476d1ce4e5b9);
    value ^= value >> 27;
    value *= UINT64_C(0x94d049bb133111eb);
    value ^= value >> 31;
    return value;
}
static void control_rotate_session_owned(void)
{
    qcom_usb_shot_cancel();
    uint64_t next = 0;
    unsigned int attempt;
    for (attempt = 0; attempt < 4u; attempt++) {
        usb_control_context.seed += UINT64_C(0x9e3779b97f4a7c15);
        next = control_mix(counter() ^ usb_control_context.seed);
        if (!next) next = UINT64_C(0x53584e55314b4559); /* "SXNU1KEY" */
        if (next != usb_control_context.state.session) break;
    }
    /* A transport generation change must never restart sequence numbering in
     * the same session. The bounded fallback also covers a theoretical mix
     * collision with the previous value. */
    if (next == usb_control_context.state.session)
        next = next == UINT64_MAX ? 1u : next + 1u;
    sxnu_control_state_reset(&usb_control_context.state, next,
                             usb_control_context.state.generation);
    sxnu_control_parser_reset(&usb_control_context.parser);
}
static size_t control_reply_queue(const char *reply, size_t bytes)
{
    uint8_t wire[SXNU_CONTROL_REPLY_BYTES + 1u];
    uint64_t flags;
    int queued = 0;
    if (!reply || !bytes || bytes > SXNU_CONTROL_REPLY_BYTES || !acquire(&flags)) return 0;
    if (native.status.phase == QCOM_USB_DEBUG_ACTIVE) {
        backlog_drain_owned();
        if (DWC3_TX_BYTES - dwc3_queued(native.core) >= bytes + 1u) {
            wire[0] = (uint8_t)'\n';
            bytes_copy(wire + 1u, reply, bytes);
            if (dwc3_queue(native.core, wire, bytes + 1u) == bytes + 1u) queued = 1;
        }
    }
    release(flags);
    if (queued) (void)__atomic_add_fetch(&control_reply_count, 1u, __ATOMIC_RELAXED);
    return queued ? bytes : 0;
}
static size_t control_reply_hello(char *reply, size_t capacity,
                                  const struct sxnu_control_frame *frame)
{
    struct sxnu_control_writer writer;
    sxnu_control_writer_init(&writer, reply, capacity);
    if (!sxnu_control_reply_start(&writer, usb_control_context.state.session, frame->seq,
                                  frame->verb, "OK") ||
        !sxnu_control_writer_text(&writer, " session=") ||
        !sxnu_control_writer_hex(&writer, usb_control_context.state.session,
                                 SXNU_CONTROL_SESSION_HEX) ||
        !sxnu_control_writer_text(&writer, " last_seq=") ||
        !sxnu_control_writer_hex(&writer, usb_control_context.state.last_seq,
                                 SXNU_CONTROL_SEQ_HEX) ||
        !sxnu_control_writer_text(&writer, " control=1")) return 0;
    return sxnu_control_reply_finish(&writer);
}
static size_t control_reply_ping(char *reply, size_t capacity,
                                 const struct sxnu_control_frame *frame,
                                 const struct qcom_usb_debug_status *status)
{
    struct sxnu_control_writer writer;
    sxnu_control_writer_init(&writer, reply, capacity);
    if (!sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK") ||
        !sxnu_control_writer_text(&writer, " rx=") ||
        !sxnu_control_writer_decimal(&writer, status->rx_pending) ||
        !sxnu_control_writer_text(&writer, " rxo=") ||
        !sxnu_control_writer_decimal(&writer, status->rx_overflow) ||
        !sxnu_control_writer_text(&writer, " tx=") ||
        !sxnu_control_writer_decimal(&writer, status->transport_pending) ||
        !sxnu_control_writer_text(&writer, " poll=") ||
        !sxnu_control_writer_decimal(&writer, status->poll_calls)) return 0;
    return sxnu_control_reply_finish(&writer);
}
static size_t control_reply_status(char *reply, size_t capacity,
                                   const struct sxnu_control_frame *frame,
                                   const struct qcom_usb_debug_status *status)
{
    struct sxnu_control_writer writer;
    sxnu_control_writer_init(&writer, reply, capacity);
    if (!sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK") ||
        !sxnu_control_writer_text(&writer, " usb=") ||
        !sxnu_control_writer_decimal(&writer, status->phase) ||
        !sxnu_control_writer_text(&writer, " er=") ||
        !sxnu_control_writer_decimal(&writer, status->error) ||
        !sxnu_control_writer_text(&writer, " rx=") ||
        !sxnu_control_writer_decimal(&writer, status->rx_pending) ||
        !sxnu_control_writer_text(&writer, " rxo=") ||
        !sxnu_control_writer_decimal(&writer, status->rx_overflow) ||
        !sxnu_control_writer_text(&writer, " tx=") ||
        !sxnu_control_writer_decimal(&writer, status->transport_pending) ||
        !sxnu_control_writer_text(&writer, " bp=") ||
        !sxnu_control_writer_decimal(&writer, status->buffer_pending) ||
        !sxnu_control_writer_text(&writer, " bo=") ||
        !sxnu_control_writer_decimal(&writer, status->buffer_overflow) ||
        !sxnu_control_writer_text(&writer, " poll=") ||
        !sxnu_control_writer_decimal(&writer, status->poll_calls) ||
        !sxnu_control_writer_text(&writer, " ev=") ||
        !sxnu_control_writer_decimal(&writer, status->last_poll_events) ||
        !sxnu_control_writer_text(&writer, " gen=") ||
        !sxnu_control_writer_decimal(&writer, status->rx_generation) ||
        !sxnu_control_writer_text(&writer, " rej=") ||
        !sxnu_control_writer_decimal(&writer, status->control_rejected)) return 0;
    return sxnu_control_reply_finish(&writer);
}
static size_t control_reply_reboot(char *reply, size_t capacity,
                                   const struct sxnu_control_frame *frame,
                                   const char *status, const char *reason)
{
    struct sxnu_control_writer writer;
    sxnu_control_writer_init(&writer, reply, capacity);
    if (!sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, status) ||
        !sxnu_control_writer_text(&writer, " state=") ||
        !sxnu_control_writer_text(&writer, reason)) return 0;
    return sxnu_control_reply_finish(&writer);
}
static void control_handle_frame(const struct sxnu_control_frame *frame,
                                 const struct qcom_usb_debug_status *status,
                                 uint32_t generation)
{
    enum sxnu_control_verb accepted = sxnu_control_accept(&usb_control_context.state, frame);
    char reply[SXNU_CONTROL_REPLY_BYTES];
    size_t reply_bytes = 0;
    int queued = 0;
    int reboot_accepted = 0;
    size_t shot_bytes = 0;

    if (accepted == SXNU_CONTROL_VERB_INVALID) {
        (void)__atomic_add_fetch(&control_rejected, 1u, __ATOMIC_RELAXED);
        return;
    }
    switch (accepted) {
    case SXNU_CONTROL_VERB_HELLO:
        reply_bytes = control_reply_hello(reply, sizeof(reply), frame);
        break;
    case SXNU_CONTROL_VERB_PING:
        reply_bytes = control_reply_ping(reply, sizeof(reply), frame, status);
        break;
    case SXNU_CONTROL_VERB_STATUS:
        reply_bytes = control_reply_status(reply, sizeof(reply), frame, status);
        break;
    case SXNU_CONTROL_VERB_SCREENSHOT: {
        struct qcom_usb_screenshot_info info;
        struct sxnu_control_writer writer;
        int result = __atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE) ?
                     -3 : qcom_usb_shot_capture(frame->seq);
        if (result != 1 || !qcom_usb_shot_info(&info)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR",
                result == -1 ? "unavailable" : result == -3 ? "busy" : "capture-failed");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=ready width=");
        (void)sxnu_control_writer_decimal(&writer, info.width);
        (void)sxnu_control_writer_text(&writer, " height=");
        (void)sxnu_control_writer_decimal(&writer, info.height);
        (void)sxnu_control_writer_text(&writer, " bytes=");
        (void)sxnu_control_writer_decimal(&writer, info.bytes);
        (void)sxnu_control_writer_text(&writer, " encoded=");
        (void)sxnu_control_writer_decimal(&writer, info.encoded_bytes);
        (void)sxnu_control_writer_text(&writer, " crc32=");
        (void)sxnu_control_writer_hex(&writer, info.crc32, 8);
        (void)sxnu_control_writer_text(&writer, " frame=");
        (void)sxnu_control_writer_decimal(&writer, info.frame);
        (void)sxnu_control_writer_text(&writer, info.rle ? " encoding=rle" : " encoding=rgb");
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_SHOTNEXT: {
        struct qcom_usb_screenshot_info info;
        struct sxnu_control_writer writer;
        uint8_t chunk[QUD_SCREENSHOT_CHUNK];
        uint32_t offset;
        if (!qcom_usb_shot_info(&info)) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "noimage");
            break;
        }
        shot_bytes = qcom_usb_shot_peek(chunk, sizeof(chunk), &offset);
        if (!shot_bytes) {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "done");
            break;
        }
        sxnu_control_writer_init(&writer, reply, sizeof(reply));
        (void)sxnu_control_reply_start(&writer, frame->session, frame->seq, frame->verb, "OK");
        (void)sxnu_control_writer_text(&writer, " state=data frame=");
        (void)sxnu_control_writer_decimal(&writer, info.frame);
        (void)sxnu_control_writer_text(&writer, " offset=");
        (void)sxnu_control_writer_decimal(&writer, offset);
        (void)sxnu_control_writer_text(&writer, " data=");
        for (size_t i = 0; i < shot_bytes; ++i)
            (void)sxnu_control_writer_hex(&writer, chunk[i], 2);
        reply_bytes = sxnu_control_reply_finish(&writer);
        break;
    }
    case SXNU_CONTROL_VERB_REBOOT:
        if (!__atomic_load_n(&control_hook_owned, __ATOMIC_ACQUIRE))
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "nohook");
        else if (__atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE) ||
                 __atomic_load_n(&control_reboot_taken, __ATOMIC_ACQUIRE))
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "ERR", "busy");
        else {
            reply_bytes = control_reply_reboot(reply, sizeof(reply), frame, "OK", "queued");
            reboot_accepted = 1;
        }
        break;
    default:
        return;
    }
    if (!reply_bytes) {
        (void)__atomic_add_fetch(&control_rejected, 1u, __ATOMIC_RELAXED);
        return;
    }
    queued = control_reply_queue(reply, reply_bytes) == reply_bytes;
    if (!queued || !sxnu_control_commit(&usb_control_context.state, frame, accepted)) {
        (void)__atomic_add_fetch(&control_rejected, 1u, __ATOMIC_RELAXED);
        return;
    }
    if (shot_bytes) (void)qcom_usb_shot_commit(shot_bytes);
    if (accepted == SXNU_CONTROL_VERB_REBOOT && reboot_accepted) {
        qcom_usb_shot_cancel();
        __atomic_store_n(&control_reboot_seq, frame->seq, __ATOMIC_RELAXED);
        __atomic_store_n(&control_reboot_session, frame->session, __ATOMIC_RELAXED);
        __atomic_store_n(&control_reboot_generation, generation, __ATOMIC_RELAXED);
        __atomic_store_n(&control_reboot_taken, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&control_reboot_armed, 0u, __ATOMIC_RELAXED);
        __atomic_store_n(&control_reboot_requested, 1u, __ATOMIC_RELEASE);
    }
}
static void control_service(const uint8_t *bytes, size_t count, uint32_t generation,
                            const struct qcom_usb_debug_status *status)
{
    size_t i;
    if (!__atomic_load_n(&control_enabled, __ATOMIC_ACQUIRE)) return;
    if (generation != usb_control_context.state.generation) {
        usb_control_context.state.generation = generation;
        control_rotate_session_owned();
    }
    for (i = 0; i < count; i++) {
        char line[SXNU_CONTROL_FRAME_BYTES];
        size_t line_bytes = 0;
        int complete = sxnu_control_parser_feed(&usb_control_context.parser, bytes[i], line,
                                                 sizeof(line), &line_bytes);
        if (complete > 0) {
            struct sxnu_control_frame frame;
            if (sxnu_control_parse(line, line_bytes, &frame))
                control_handle_frame(&frame, status, generation);
            else
                (void)__atomic_add_fetch(&control_rejected, 1u, __ATOMIC_RELAXED);
        } else if (complete < 0) {
            (void)__atomic_add_fetch(&control_rejected, 1u, __ATOMIC_RELAXED);
        }
    }
}
int qcom_usb_debug_control_start(void)
{
    uint64_t flags;
    unsigned int expected = 0;
    int (*halt_expected)(unsigned int) = 0;
    int installed;
    if (!__atomic_compare_exchange_n(&control_started, &expected, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return __atomic_load_n(&control_started, __ATOMIC_ACQUIRE) == 2u;
    if (!acquire(&flags)) {
        __atomic_store_n(&control_started, 0u, __ATOMIC_RELEASE);
        return 0;
    }
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE) {
        release(flags);
        __atomic_store_n(&control_started, 0u, __ATOMIC_RELEASE);
        return 0;
    }
    usb_control_context.seed = counter() ^
                               ((uint64_t)native.status.counter_frequency << 32) ^
                               UINT64_C(0x53584e554354524c);
    usb_control_context.state.generation = dwc3_rx_generation(native.core);
    control_rotate_session_owned();
    release(flags);
    installed = QUD_PSCI_SMC_RESET &&
                __atomic_compare_exchange_n(&PE_halt_restart, &halt_expected,
                  qcom_usb_control_halt_restart, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    __atomic_store_n(&control_hook_owned, installed ? 1u : 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&control_enabled, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&control_started, 2u, __ATOMIC_RELEASE);
    return 1;
}
int qcom_usb_debug_control_take_reboot(void)
{
    unsigned int expected = 0;
    unsigned int claimed;
    if (!__atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE)) return 0;
    if (!__atomic_compare_exchange_n(&control_reboot_taken, &expected, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    if (__atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE)) return 1;
    /* A concurrent failure cleared the request after the initial load. Release
     * our now-empty claim so a later acknowledged REBOOT is not left busy. */
    claimed = 1u;
    (void)__atomic_compare_exchange_n(&control_reboot_taken, &claimed, 0u, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return 0;
}
int qcom_usb_debug_control_arm_reboot(void)
{
    uint64_t flags;
    uint32_t generation;
    int armed = 0;
    if (!__atomic_load_n(&control_hook_owned, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&control_reboot_requested, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&control_reboot_taken, __ATOMIC_ACQUIRE) || !acquire(&flags)) return 0;
    generation = __atomic_load_n(&control_reboot_generation, __ATOMIC_ACQUIRE);
    if (native.status.phase == QCOM_USB_DEBUG_ACTIVE && native.core_initialized &&
        dwc3_ready(native.core) && dwc3_rx_generation(native.core) == generation) {
        __atomic_store_n(&control_reboot_armed, 1u, __ATOMIC_RELEASE);
        armed = 1;
    }
    release(flags);
    return armed;
}
void qcom_usb_debug_control_reboot_failed(int result)
{
    struct sxnu_control_frame frame;
    struct sxnu_control_writer writer;
    char reply[SXNU_CONTROL_REPLY_BYTES];
    unsigned int requested;
    requested = __atomic_exchange_n(&control_reboot_requested, 0u, __ATOMIC_ACQ_REL);
    __atomic_store_n(&control_reboot_taken, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&control_reboot_armed, 0u, __ATOMIC_RELEASE);
    if (!requested) return;
    (void)__atomic_add_fetch(&control_failures, 1u, __ATOMIC_RELAXED);
    frame.session = __atomic_load_n(&control_reboot_session, __ATOMIC_RELAXED);
    frame.seq = __atomic_load_n(&control_reboot_seq, __ATOMIC_RELAXED);
    frame.verb = SXNU_CONTROL_VERB_REBOOT;
    frame.crc = 0;
    sxnu_control_writer_init(&writer, reply, sizeof(reply));
    if (sxnu_control_reply_start(&writer, frame.session, frame.seq, frame.verb, "ERR") &&
        sxnu_control_writer_text(&writer, " state=reboot result=") &&
        sxnu_control_writer_signed(&writer, result)) {
        size_t bytes = sxnu_control_reply_finish(&writer);
        if (bytes) (void)control_reply_queue(reply, bytes);
    }
}
int qcom_usb_debug_wait_drained(unsigned int timeout_ms)
{
    uint64_t start, ticks;
    if (timeout_ms > 10000u) timeout_ms = 10000u;
    if (!timeout_ms || !native.status.counter_frequency) return 0;
    start = counter();
    ticks = native.status.counter_frequency * timeout_ms / 1000u;
    for (;;) {
        uint64_t flags;
        int pending;
        if (!acquire(&flags)) return 0;
        if (native.status.phase != QCOM_USB_DEBUG_ACTIVE || !poll_owned()) {
            release(flags);
            return 0;
        }
        buffer_status_owned();
        pending = native.status.buffer_pending != 0 || native.status.transport_pending != 0;
        release(flags);
        if (!pending) return 1;
        if (counter() - start >= ticks) return 0;
        delay_us(0, 1000);
    }
}
size_t qcom_usb_debug_write(const void *bytes, size_t count)
{
    uint64_t flags;
    size_t accepted = 0;
    if (!bytes || !count || native.status.phase == QCOM_USB_DEBUG_DISABLED ||
        native.status.phase == QCOM_USB_DEBUG_STOPPED) return 0;
    /* Recovery. poll_owned() latches IO_FAILED and nothing else clears it, so a single failed
     * DWC3 poll silences the console for the rest of the boot while the machine keeps running:
     * measured 2026-10-28, silence at t=35 s in one boot and t=110 s in another, with the home
     * screen still appearing after both. That turned every later diagnosis into guesswork.
     * Retry one poll at a bounded interval; poll_owned() puts the phase back to ACTIVE itself
     * when the controller answers again. */
    if (native.status.phase == QCOM_USB_DEBUG_IO_FAILED) {
        static uint32_t recover_budget;
        if (++recover_budget >= 256u) {
            recover_budget = 0;
            if (acquire(&flags)) {
                native.status.io_failed = 0;
                native.status.phase = QCOM_USB_DEBUG_ACTIVE;
                (void)poll_owned();
                release(flags);
            }
        }
    }
    accepted = backlog_write(bytes, count);
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE || !acquire(&flags)) return accepted;
    if (native.status.phase == QCOM_USB_DEBUG_ACTIVE) {
        (void)poll_owned();
        /* Explicit diagnostic mode uses a 1ms flush window for a ready host so
         * the last character remains observable if a later kernel path
         * stops servicing USB. One bounded core operation may extend it.
         * No-host and normal mode remain nonblocking. */
        if (usb_diag_enabled && native.status.phase == QCOM_USB_DEBUG_ACTIVE && dwc3_ready(native.core)) {
            uint64_t start = counter();
            unsigned int attempts = 128u;
            while (attempts-- && (dwc3_queued(native.core) || soc_usb_buffer_pending(&usb_log_buffer)) &&
                   counter() - start < native.status.counter_frequency / 1000u) {
                if (!poll_owned()) break;
                delay_us(0, 10);
            }
        }
    }
    release(flags); return accepted;
}
void qcom_usb_debug_putc(char c) { (void)qcom_usb_debug_write(&c, 1); }
/* Backlog only: no controller poll, no delay, no lock beyond the local IRQ mask that
 * backlog_write already takes. A kernel path that holds the name cache or vnode locks
 * cannot use qcom_usb_debug_write, because in the ACTIVE phase that also polls the
 * controller and can spin for a millisecond, which stalls every lookup behind it. */
size_t qcom_usb_debug_enqueue(const void *bytes, size_t count)
{
    if (!bytes || !count) return 0;
    if (native.status.phase == QCOM_USB_DEBUG_DISABLED ||
        native.status.phase == QCOM_USB_DEBUG_STOPPED) return 0;
    return backlog_write(bytes, count);
}
void qcom_usb_debug_poll(void)
{
    uint64_t flags;
    uint8_t control_bytes[SXNU_CONTROL_WORK_BYTES];
    struct qcom_usb_debug_status control_status = {0};
    uint32_t generation = 0;
    size_t control_bytes_count = 0;
    int observed = 0;
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE || !acquire(&flags)) return;
    (void)poll_owned();
    if (__atomic_load_n(&control_enabled, __ATOMIC_ACQUIRE) &&
        native.status.phase == QCOM_USB_DEBUG_ACTIVE) {
        buffer_status_owned();
        bytes_copy(&control_status, &native.status, sizeof(control_status));
        generation = dwc3_rx_generation(native.core);
        control_bytes_count = dwc3_read(native.core, control_bytes,
                                            sizeof(control_bytes));
        observed = 1;
    }
    release(flags);
    if (observed)
        control_service(control_bytes, control_bytes_count, generation, &control_status);
}
void qcom_usb_debug_panic_prepare(void)
{
    /* An early panic can stall in platform/debugger handling before the final
     * spin hook. Diagnose PREPARING before entering those paths. */
    if (usb_diag_enabled && (native.status.phase == QCOM_USB_DEBUG_PREPARING ||
        (native.status.phase == QCOM_USB_DEBUG_NOT_INITIALIZED && usb_diag_checkpoint)))
        diag_reset(100u + 2u * usb_diag_checkpoint);
}
void qcom_usb_debug_panic_drain(void)
{
    static const char end[] = "\nQCOM PANIC END\n";
    uint64_t flags, now;
    unsigned int iterations = 200000u, marker_queued = 0, announced = 0;
    char announcement[192];
    size_t announcement_bytes;
    if (usb_diag_enabled && (native.status.phase == QCOM_USB_DEBUG_PREPARING ||
        (native.status.phase == QCOM_USB_DEBUG_NOT_INITIALIZED && usb_diag_checkpoint)))
        diag_reset(100u + 2u * usb_diag_checkpoint);
    /* panic_spin_forever can precede CPU-data initialization. Inactive USB
     * must not call cpu_number(), touch MMIO or acquire a guard in that case. */
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE || !acquire(&flags)) return;
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE) goto out;
    {
        char *p = append_text(announcement, "\nQCOM USB PANIC pending_at_start=");
        p = append_decimal(p, soc_usb_buffer_pending(&usb_log_buffer) + dwc3_queued(native.core));
        p = append_text(p, " dropped_at_start=");
        p = append_decimal(p, soc_usb_buffer_dropped(&usb_log_buffer) + dwc3_dropped(native.core));
        p = append_text(p, " buffer_capacity=");
        p = append_decimal(p, sizeof(usb_log_bytes));
        *p++ = '\n';
        announcement_bytes = (size_t)(p - announcement);
    }
    now = counter();
    native.deadline = now + native.status.counter_frequency * 2u;
    if (native.deadline < now) goto out;
    native.deadline_expired = 0; native.deadline_active = 1;
    while (iterations-- && !deadline_expired()) {
        if (!announced && DWC3_TX_BYTES - dwc3_queued(native.core) >= announcement_bytes)
            announced = dwc3_queue(native.core, announcement, announcement_bytes) == announcement_bytes;
        /* End marker follows all queued bytes, and appears only after there
         * is room for the entire marker. Never partially queue it. */
        if (announced && !marker_queued && dwc3_queued(native.core) == 0 &&
            soc_usb_buffer_pending(&usb_log_buffer) == 0) {
            char text[160];
            size_t bytes = loss_record(text, 1);
            (void)dwc3_queue(native.core, text, bytes);
            marker_queued = dwc3_queue(native.core, end, sizeof(end) - 1) == sizeof(end) - 1;
        }
        if (!poll_owned()) break;
        if (marker_queued && dwc3_queued(native.core) == 0 &&
            soc_usb_buffer_pending(&usb_log_buffer) == 0) break;
        delay_us(0, 10);
    }
    native.deadline_active = 0;
    if (native.deadline_expired) {
        native.status.error = QCOM_USB_DEBUG_DEADLINE_EXPIRED;
        if (native.status.io_failed) native.status.phase = QCOM_USB_DEBUG_IO_FAILED;
    }
out:
    buffer_status_owned();
    release(flags);
    if (usb_diag_enabled) diag_reset(10);
}
int qcom_usb_debug_stop(void)
{
    uint64_t flags;
    int ret = 0;
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE && native.status.phase != QCOM_USB_DEBUG_IO_FAILED &&
        !native.lease.active && !native.status.vbus_owned && !native.status.phy_owned) return 0;
    if (native.status.stop_failed) return 0;
    if (!acquire(&flags)) return 0;
    if (native.status.phase != QCOM_USB_DEBUG_ACTIVE && native.status.phase != QCOM_USB_DEBUG_IO_FAILED &&
        !native.lease.active && !native.status.vbus_owned && !native.status.phy_owned)
        goto out;
    native.deadline_active = 0;
    if ((native.core_initialized && dwc3_stop(native.core)) ||
        (!native.core_initialized && !halt_inherited())) {
        native.status.stop_failed = 1;
        fail(QCOM_USB_DEBUG_HALT_TIMEOUT, 0); goto out;
    }
    if (!restore_vbus() || !restore_lease() || !restore_phy()) { fail(QCOM_USB_DEBUG_RESTORE_FAILED, 0); goto out; }
    native.status.phase = QCOM_USB_DEBUG_STOPPED; ret = 1;
out:
    release(flags); return ret;
}
void qcom_usb_debug_get_status(struct qcom_usb_debug_status *status)
{
    uint64_t flags;
    if (!status || !acquire(&flags)) return;
    buffer_status_owned();
    bytes_copy(status, &native.status, sizeof(*status)); release(flags);
}

/* gts7xlwifi: a status copy for the BootLog page, from any CPU. qcom_usb_debug_get_status() answers only on master_cpu (acquire()),
 * and the BootLog writer seldom runs there, so the page showed an all-zero status. This copy takes no lock: the words are read as they
 * stand, which is enough for a diagnostic row (phase, error and counters are single aligned words). */
void qcom_usb_debug_peek_status(struct qcom_usb_debug_status *status)
{
    if (!status) return;
    const volatile uint32_t *from = (const volatile uint32_t *)&native.status;
    uint32_t *to = (uint32_t *)status;
    for (size_t i = 0; i < sizeof(*status) / 4; i++) to[i] = from[i];
}

/* gts7xlwifi: read-only fault picture of the event path, from any CPU: [0] GEVNTADRLO [1] GEVNTADRHI [2] GEVNTSIZ [3] GEVNTCOUNT
 * [4] GSTS [5] sGFSR [6] sGFSYNR0 [7] context bank | stream slot << 8 | lease active << 16 [8] CB FSR [9] CB FSYNR0 [10] CB FAR lo
 * [11] CB FAR hi [12] CB SCTLR [13] lease IOVA lo [14] the slot's SMR [15] its S2CR. Zero where a window is not mapped. */
void qcom_usb_debug_t970_faults(uint32_t out[16])
{
    if (!out) return;
    for (unsigned i = 0; i < 16; i++) out[i] = 0;
    if (native.usb_va) {
        const volatile uint32_t *u = (const volatile uint32_t *)(uintptr_t)native.usb_va;
        out[0] = u[0xc400u / 4]; out[1] = u[0xc404u / 4]; out[2] = u[0xc408u / 4]; out[3] = u[0xc40cu / 4]; out[4] = u[0xc118u / 4];
    }
    out[7] = (uint32_t)native.lease.cb | ((uint32_t)native.lease.index & 0xffu) << 8 | (native.lease.active ? 1u : 0u) << 16;
    out[13] = (uint32_t)native.lease.iova;
    if (native.smmu_va) {
        const volatile uint32_t *m = (const volatile uint32_t *)(uintptr_t)native.smmu_va;
        out[5] = m[0x48u / 4]; out[6] = m[0x50u / 4];
        if (native.lease.index < 128u) {out[14] = m[(0x800u + 4u * native.lease.index) / 4]; out[15] = m[(0xc00u + 4u * native.lease.index) / 4];}
        if (native.lease.cb_base >= QUSB_SMMU_BASE && native.lease.cb_base + 0x100u <= QUSB_SMMU_BASE + QUSB_SMMU_BYTES) {
            const volatile uint32_t *c = (const volatile uint32_t *)(uintptr_t)(native.smmu_va + (native.lease.cb_base - QUSB_SMMU_BASE));
            out[12] = c[0]; out[8] = c[0x58u / 4]; out[9] = c[0x68u / 4]; out[10] = c[0x60u / 4]; out[11] = c[0x64u / 4];
        }
    }
}

/* gts7xlwifi: a second initialization from a late board thread that is already bound to master_cpu, after it has voted
 * the PHY's supplies (ldoa5, ldoa12, ldoa2). The early attempt has failed by then; this one also runs t970_hsphy_init. */
int qcom_usb_debug_t970_retry(void)
{
    int ok;
    t970_qud_phy[9] = (int32_t)((uint32_t)t970_qud_phy[9] + 1u);
    if (native.status.phase == QCOM_USB_DEBUG_ACTIVE) return 1;
    native.status.phase = QCOM_USB_DEBUG_NOT_INITIALIZED;
    native.status.error = 0;
    native.status.io_failed = 0;
    native.status.core_result = 0;
    /* Every window init needs is mapped here, with interrupts on: init maps them with interrupts masked, which hangs on a running
     * system (periph4 and periph6: the retry never returned once nothing had been mapped before it). */
    if (!t970_phy_va) t970_phy_va = ml_io_map((vm_offset_t)0x088e3000ULL, 0x1000);
    if (!native.gcc_va) native.gcc_va = ml_io_map((vm_offset_t)QUSB_GCC_BASE, GCC_BYTES);
    if (!native.usb_va) native.usb_va = ml_io_map((vm_offset_t)QUSB_DWC3_BASE, USB_BYTES);
    if (!native.smmu_va) native.smmu_va = ml_io_map((vm_offset_t)QUSB_SMMU_BASE, QUSB_SMMU_BYTES);
    t970_qud_phy_wanted = 1;
    ok = qcom_usb_debug_init();
    t970_qud_phy[10] = (int32_t)(native.status.phase << 8 | native.status.error);
    t970_qud_phy[11] = native.status.core_result;
    return ok;
}
