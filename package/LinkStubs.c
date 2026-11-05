// SPDX-License-Identifier: BSD-2-Clause-Patent
// No-op definitions of the probe hooks the patched XNU references while gts7xlwifi_defconfig leaves their component out.
// From UniXNU-alioth 520581af package/LinkStubs.c.
// Each hook runs only on its boot argument, which board.toml does not pass; the kernel link needs the symbol anyway.

// offline-component-load-probes.patch, bsd_init: called on offline-legacy-register=1.
// CONFIG_XNU_NATIVE_LEGACY_DISPLAY is now on, so libNativeLegacyDisplay.late.a defines this
// entry and the kernel calls the real one. The stub is gone: keeping it was a duplicate symbol
// at the link. Set the displayed switch back to n and this definition has to come back with it.
#include <stdint.h>

/* From UniXNU-alioth 520581af, now that GopFramebuffer is linked: the compositor's IOSurface stride
 * field can hold pixels while native_legacy_surface_bounds compares it with width*4. Take the field as
 * pixels when the allocation and the row count prove it, and leave a getter that returns bytes alone. */
extern unsigned long long native_legacy_surface_stride_raw(void *);
extern unsigned long long native_legacy_surface_allocation(void *);
extern unsigned long long native_legacy_surface_height(void *);
unsigned long long native_legacy_surface_stride(void *surface);
unsigned long long native_legacy_surface_stride(void *surface) {
    unsigned long long stride = native_legacy_surface_stride_raw(surface);
    unsigned long long allocation = native_legacy_surface_allocation(surface);
    unsigned long long height = native_legacy_surface_height(surface);
    if (stride && allocation && height && stride <= allocation / height / 4) return stride * 4;
    return stride;
}

// ShengUfs imports this as a weak reference, which a kernel link still has to resolve.
// alioth leaves CONFIG_XNU_BOOT_PROGRESS unset, so libBootProgress.a is not linked.
// Weak here, so enabling that option later keeps the real definition.
__attribute__((weak)) void boot_progress_poll(void);
__attribute__((weak)) void boot_progress_poll(void)
{
}

// offline-owner-systemos-mount.patch, kern_exec: native_owner_data_prepare() calls
// native_gpu_addon_prepare(), which tests native_ram_block_gpu_addon before calling it. The patch
// declares that symbol weak_import, and ld64 still refuses to leave a weak_import undefined in a
// static kernel link, so CONFIG_XNU_OWNER_DATA_MOUNT does not link until this name resolves.
// libRamBlock.a owns the real definition, but alioth leaves CONFIG_XNU_NATIVE_RAM_BLOCK unset: the
// Data mount reads the DarwinBoot RAMDisk through mdevlookup(0), not a virtio provider.
// The hook runs only on sheng-gpu-addon=1, which board.toml does not pass. Weak here, so
// selecting the RamBlock transport later keeps the real definition.
__attribute__((weak)) int native_ram_block_gpu_addon(const unsigned char **, unsigned long long *);
__attribute__((weak)) int native_ram_block_gpu_addon(const unsigned char **addon, unsigned long long *bytes)
{
	(void)addon;
	(void)bytes;
	return 0;
}

// GopFramebuffer's compositor-scanout path imports the UFS DPU window as weak_import, and a
// static kernel link refuses to leave a weak_import undefined. NativeUfs defines both; this board
// selects ShengUfs, so neither exists here. NativeLegacyDisplay.cpp:136 tests `&native_ufs_dpu_map`
// and `!(native_legacy_dpu_state >> 31)` before using either, so a zero state and a reporter that
// maps nothing make the path decline itself, which is the correct answer on a board whose DPU is
// not published. Weak, so a component that later defines the real symbols wins.
__attribute__((weak)) volatile uint32_t native_ufs_dpu_state;
__attribute__((weak)) uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages,
    unsigned count);
__attribute__((weak)) uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages,
    unsigned count)
{
	(void)slot;
	(void)pages;
	(void)count;
	return 0;
}

// Diagnostics of components this board does not select yet (GOP display, GPU, HID, touch, USB debug, UFS log).
// BootProgress.c imports each as weak_import and tests its address before use, but a static kernel link still
// needs a definition. Zero-valued and weak: the real component's definition replaces each one when it moves here.
#define T970_WEAK __attribute__((weak))
T970_WEAK volatile uint32_t native_gop_failures, native_gop_ready, native_hid_stage, native_hid_touch_status;
T970_WEAK volatile uint64_t native_gop_transfers;
T970_WEAK char native_gpu_rows[1][120];
T970_WEAK volatile uint32_t native_hid_class_sizes[5];
T970_WEAK volatile int native_hid_input_meta_status, native_hid_load_status, native_hid_matching_load_status;
T970_WEAK volatile uint32_t native_legacy_gop_height, native_legacy_gop_width, native_legacy_presented_generation;
T970_WEAK volatile uint32_t native_legacy_retired_frames, native_legacy_surfaces[14];
T970_WEAK volatile uint32_t native_touch_fail_step, native_touch_frames, native_touch_hid_reports, native_touch_valid;
T970_WEAK volatile unsigned native_touch_frame_raw[8], native_touch_irq_level, native_touch_irq_low;
T970_WEAK volatile int native_touch_last_x, native_touch_last_y;
// The touch core's interface as ShengHidInput and LegacyDisplayProbe import it (weak_import, which a static link
// still has to resolve). Zero and no-op until the STM FTS core on QUP SE5 defines them; a strong definition wins.
T970_WEAK volatile int native_touch_stage_result;
T970_WEAK volatile unsigned native_touch_stage_value, native_touch_stage_total, native_touch_ready_ms;
T970_WEAK volatile unsigned native_touch_tlmm_probe[3];
T970_WEAK char native_touch_stage_name[32];
// The finger set ShengHidInput reads under the native_touch_seq seqlock, and the protocol diagnostics the display
// probe prints. The STM FTS core will own all of these.
T970_WEAK volatile uint32_t native_touch_seq, native_touch_count;
T970_WEAK volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
T970_WEAK volatile unsigned native_touch_app_id, native_touch_app_rc[3], native_touch_ready;
T970_WEAK volatile unsigned native_touch_crc_expected, native_touch_crc_received;
T970_WEAK volatile unsigned native_touch_hdr0, native_touch_hdr1, native_touch_hdr2, native_touch_hdr3;
T970_WEAK volatile unsigned native_touch_data0, native_touch_data1, native_touch_data2;
T970_WEAK volatile unsigned native_touch_pin_cfg[3], native_touch_pin_want[3];
T970_WEAK void native_touch_set_sink(void (*sink)(void *), void *context);
T970_WEAK void native_touch_set_sink(void (*sink)(void *), void *context) { (void)sink; (void)context; }
typedef unsigned long t970_size_t;
T970_WEAK int native_ufs_log_read(uint64_t lba, uint32_t blocks, void *out);
T970_WEAK int native_ufs_log_read(uint64_t lba, uint32_t blocks, void *out) { (void)lba; (void)blocks; (void)out; return 0; }
T970_WEAK int native_ufs_log_read_bytes(uint64_t at, uint32_t bytes, void *out);
T970_WEAK int native_ufs_log_read_bytes(uint64_t at, uint32_t bytes, void *out) { (void)at; (void)bytes; (void)out; return 0; }
T970_WEAK t970_size_t qcom_usb_debug_write(const void *data, t970_size_t bytes);
T970_WEAK t970_size_t qcom_usb_debug_write(const void *data, t970_size_t bytes) { (void)data; (void)bytes; return 0; }
// offline-owner-root-crypto-probe.patch: bsd_init calls it only on offline-owner-root-check=1, which this board does not pass.
T970_WEAK void native_verified_root_probe(void);
T970_WEAK void native_verified_root_probe(void) {}
// LegacyDisplayProbe offers the framebuffer to the USB console for screenshots; this board has no USB console.
T970_WEAK int qcom_usb_debug_register_framebuffer(const void *framebuffer);
T970_WEAK int qcom_usb_debug_register_framebuffer(const void *framebuffer) { (void)framebuffer; return -1; }
// LegacyDisplayProbe's A650 (pipa-gpu) and Wi-Fi PCIe entries; this board ports neither yet.
T970_WEAK void native_gpu_probe(void);
T970_WEAK void native_gpu_probe(void) {}
T970_WEAK void alioth_pcie_init(void);
T970_WEAK void alioth_pcie_init(void) {}
// LegacyDisplayProbe calls the touch core's C++ entry touchProbeStart() (mangled __Z15touchProbeStartv) on
// native-touch=1. Not started until the STM FTS core defines it; this stub reports "not started".
T970_WEAK _Bool t970_touch_probe_start(void) __asm__("__Z15touchProbeStartv");
T970_WEAK _Bool t970_touch_probe_start(void) { return 0; }

// The kernel's GPU addon fallback (patches/xnu/t970-gpu-addon-embedded.patch) when no package/T970GpuAddon.S is built in:
// size 0 makes the install return as it did without an addon.
T970_WEAK const uint8_t t970_gpu_addon_data[8];
T970_WEAK const uint64_t t970_gpu_addon_size = 0;

// The per-process-VM GPU driver (FFT-infra/Adreno650 a6xx-on-7fa13d4) weak-imports a board GPU level table for its job records and an
// XNU helper that names a physical page's user mappings for its release diagnostics; the static link still needs both names.
// BootLog's [gpu badpa] row read the previous driver's DRAM guard. Weak, so a real definition wins.
__attribute__((weak)) volatile uint32_t native_gpu_dcvs[15];
__attribute__((weak)) unsigned int native_pmap_user_mappings(unsigned int, void **, uint64_t *, char *);
__attribute__((weak)) unsigned int native_pmap_user_mappings(unsigned int page, void **pmap, uint64_t *va, char *name)
{
	(void)page;
	(void)pmap;
	(void)va;
	(void)name;
	return 0;
}
__attribute__((weak)) volatile uint32_t native_adreno_bad_physical[4];
