// SPDX-License-Identifier: MIT
// Diagnostic log on UFS (boot argument sheng-log=1): every 2 s the text the board page would show (the diagnostic page, the live
// banner, the GPU rows, a caught panic message) and the kernel message buffer are written to the last 32 MiB of userdata through
// NativeUfs's bounded log window, so a run can be read back afterwards (scripts/read-sheng-log.py over the Linux log reader).
//
// Window layout (4096-byte blocks): 0 header, 1..32 the kernel message buffer (oldest first), 33.. text slots of 8 blocks in a ring,
// then the export (version 3, int-150): the newest complete blob that user space handed over (boot_log_export_put: the Metal plugin's
// pipeline caches, so the next image can start with them) lies in NativeUfs's export region, the 256 MiB below the window, which the
// reader fetches on its own. Its two halves alternate, so the header always names a blob that is wholly on the medium. (Version 2 kept a
// 16 MiB area at the window's tail; the UI's pipeline caches outgrew it in int-148 and int-149.)
// The header is written last in each round, so a reader never trusts a slot or an export the header does not count yet.
#include <kern/thread.h>
#include <kern/clock.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include <kern/misc_protos.h>
#include <IOKit/IOLib.h>

extern int native_ufs_log_open(uint64_t *, uint64_t *, const char **) __attribute__((weak_import));
extern int native_ufs_log_write(uint64_t, uint32_t, const void *) __attribute__((weak_import));
extern int native_ufs_log_read(uint64_t, uint32_t, void *) __attribute__((weak_import));
// Same window, byte granular. native_ufs_log_read writes blocks*4096 bytes, so it needs a
// buffer that large; this one reads through a bounce block and fills only what is asked.
extern int native_ufs_log_read_bytes(uint64_t, uint32_t, void *) __attribute__((weak_import));
extern size_t qcom_usb_debug_write(const void *, size_t) __attribute__((weak_import));
extern uint64_t native_ufs_export_blocks(void) __attribute__((weak_import));
extern int native_ufs_export_write(uint64_t, uint32_t, const void *) __attribute__((weak_import));
extern int native_ufs_log_sync(void) __attribute__((weak_import));
extern unsigned native_ufs_store_gpt_check(void) __attribute__((weak_import));
extern uint32_t cs_text_fixup_parsed __attribute__((weak_import));
extern uint32_t cs_text_fixup_applied __attribute__((weak_import));
extern char boot_progress_page_text[48][120] __attribute__((weak_import));
extern char boot_progress_banner_text[49][120] __attribute__((weak_import));
extern volatile unsigned boot_progress_banner_rows __attribute__((weak_import));
extern char boot_progress_ext[8][48] __attribute__((weak_import));
extern char boot_progress_ext_wide[2][112] __attribute__((weak_import));
extern char boot_progress_panic_text[2048] __attribute__((weak_import));
extern uint64_t t970_memtest_result[6] __attribute__((weak_import));
extern volatile uint32_t native_adreno_bad_physical[4] __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_validation_calls __attribute__((weak_import)), native_legacy_swap_accepted_count __attribute__((weak_import)),
    native_legacy_swap_rejected_count __attribute__((weak_import)), native_legacy_rej_reason __attribute__((weak_import)),
    native_legacy_retirement_calls __attribute__((weak_import)), native_legacy_swap_drops __attribute__((weak_import));
extern volatile uint32_t native_legacy_rej_state[3] __attribute__((weak_import)), native_legacy_rej_size[2] __attribute__((weak_import));
extern volatile uint64_t native_legacy_rej_surfaces[4] __attribute__((weak_import)), native_legacy_rej_surface[7] __attribute__((weak_import));
extern volatile uint64_t native_gop_gpu_offers __attribute__((weak_import)), native_gop_gpu_completions __attribute__((weak_import)),
    native_gop_gpu_failures __attribute__((weak_import)), native_gop_gpu_abandoned __attribute__((weak_import));
extern volatile uint32_t native_data_persist_state __attribute__((weak_import));
extern volatile uint32_t native_data_persist_header __attribute__((weak_import));
extern volatile uint32_t native_data_persist_gpt __attribute__((weak_import));
extern volatile uint32_t native_data_persist_gpt_after __attribute__((weak_import));
extern volatile uint64_t native_data_persist_pages __attribute__((weak_import));
extern volatile uint64_t native_data_persist_loaded __attribute__((weak_import));
extern volatile uint64_t native_data_persist_writes __attribute__((weak_import));
extern volatile uint64_t native_data_persist_syncs __attribute__((weak_import));
extern volatile uint64_t native_data_persist_errors __attribute__((weak_import));
extern volatile uint64_t native_data_persist_first_lba __attribute__((weak_import));
extern volatile int native_gpu_addon_files __attribute__((weak_import));
extern volatile int native_gpu_addon_written __attribute__((weak_import));
extern volatile int native_gpu_addon_same __attribute__((weak_import));
extern volatile int native_gpu_addon_failed __attribute__((weak_import));
extern volatile int native_gpu_addon_error __attribute__((weak_import));
extern volatile int native_gpu_addon_stage __attribute__((weak_import));
extern volatile int native_gpu_addon_status __attribute__((weak_import));
extern volatile int native_gpu_addon2_stage __attribute__((weak_import));
extern volatile int native_gpu_addon2_status __attribute__((weak_import));
extern volatile int native_gpu_addon3_stage __attribute__((weak_import));
extern volatile int native_gpu_addon3_status __attribute__((weak_import));
extern volatile int native_cryptex_os_stage __attribute__((weak_import));
extern volatile int native_cryptex_os_status __attribute__((weak_import));
extern volatile int native_gestalt_cache_status __attribute__((weak_import));
extern volatile int native_ls_store_status __attribute__((weak_import));
extern volatile uint64_t native_data_persist_present __attribute__((weak_import));
extern volatile int native_staged_apps_stage __attribute__((weak_import));
// gts7xlwifi t970-migrate-once.patch: stage (1 moved, 2 done before, -1 no complete copy, -2 marker failed), files moved, last error.
extern volatile int native_t970_migrate[3] __attribute__((weak_import));
// gts7xlwifi t970-mgcache-once.patch: stage (1 moved, 2 done before, -2 marker failed), MobileGestalt caches moved, last error.
extern volatile int native_t970_mgcache[3] __attribute__((weak_import));
extern volatile int32_t native_buttons_stats[10] __attribute__((weak_import));
extern volatile int32_t native_backlight_stats[16] __attribute__((weak_import));
extern volatile int native_staged_apps_dirs __attribute__((weak_import));
extern volatile int native_staged_apps_files __attribute__((weak_import));
extern volatile int native_staged_apps_mib __attribute__((weak_import));
extern volatile int native_staged_apps_failed __attribute__((weak_import));
extern volatile int native_staged_apps_error __attribute__((weak_import));
extern char native_staged_apps_path[96] __attribute__((weak_import));
extern const char *boot_progress_tag(void) __attribute__((weak_import));
extern unsigned boot_ui_trace_role_rows(unsigned, char [][120], unsigned, unsigned) __attribute__((weak_import));
extern unsigned boot_ui_trace_hot_rows(char [][120], unsigned) __attribute__((weak_import));
extern unsigned boot_ui_trace_mutex_owner_rows(const void *, char [][120], unsigned) __attribute__((weak_import));
extern int armcpu_stall_row(char *, size_t) __attribute__((weak_import));
extern int t970_dpu_timing_row(char *, size_t) __attribute__((weak_import));
// gts7xlwifi direct scanout: NativeLegacyDisplay's discovery (native_t970_direct) and counters (native_legacy_direct), NativeUfs's
// display window (native_ufs_dpu_state: bit 31 on, 0xdead000N the refusal; native_ufs_dpu_smmu[4] slot|bank<<16 of stream 0x820).
extern volatile uint32_t native_t970_direct[11] __attribute__((weak_import));
extern volatile uint32_t native_t970_swapfmt[5] __attribute__((weak_import));
extern volatile uint32_t native_legacy_direct[14] __attribute__((weak_import));
extern volatile uint32_t native_ufs_dpu_smmu[24] __attribute__((weak_import));
extern volatile uint32_t native_ufs_dpu_check[8] __attribute__((weak_import));
extern const void *native_adreno_global_lock(void) __attribute__((weak_import));
extern unsigned long vm_kernel_slide;
extern volatile uint64_t native_gop_wait_stats[7] __attribute__((weak_import));
extern volatile uint32_t native_adreno_fence_wait[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_stall[24] __attribute__((weak_import));
extern volatile uint32_t native_adreno_stall_mem[59] __attribute__((weak_import));
// Adreno650 NativeAdrenoVm.cpp (periph13): the last 256 maps and unmaps {IOVA >> 12, pages, ASID, seq} and the VM failures.
extern volatile uint32_t native_adreno_map_ring[256][4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_unmap_ring[256][4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_map_ring_count __attribute__((weak_import));
extern volatile uint32_t native_adreno_unmap_ring_count __attribute__((weak_import));
extern volatile uint32_t native_adreno_vm_fail[7] __attribute__((weak_import));
extern volatile uint32_t native_adreno_unstick[15] __attribute__((weak_import));
extern volatile uint64_t native_adreno_wait_outer[4] __attribute__((weak_import));
/* Timer deadlines from ArmCpu: how far ahead of the counter each core's virtual timer sits after
 * rtclock_intr, and how often that lead was under 1000 ticks. A deadline in the past makes XNU
 * program its 10-tick minimum, so the timer fires far more often than any thread asked, which is
 * what the wakeups monitor reports on this board. */
extern volatile int32_t armcpu_timer_ahead[8] __attribute__((weak_import));
extern volatile uint32_t armcpu_timer_short[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_export_stats[4] __attribute__((weak_import));
extern volatile int32_t native_touch_strokes[16][7] __attribute__((weak_import));
extern volatile uint32_t native_touch_stroke_count __attribute__((weak_import));
extern volatile uint32_t native_wifi_stats[29] __attribute__((weak_import));
extern volatile uint32_t native_wifi_dump[108] __attribute__((weak_import));
extern volatile uint32_t native_wifi_pmic[472] __attribute__((weak_import));
extern volatile uint32_t native_wifi_ep[16] __attribute__((weak_import));
extern volatile uint32_t native_wifi_pon[12] __attribute__((weak_import));
extern volatile uint32_t native_wifi_cfg[32] __attribute__((weak_import));
extern volatile uint32_t native_wifi_chip[16] __attribute__((weak_import));
extern volatile uint32_t native_wifi_ev[48] __attribute__((weak_import));
extern volatile uint32_t native_wifi_ldo[24] __attribute__((weak_import));
extern volatile uint32_t native_wifi_mhi[16] __attribute__((weak_import));
extern volatile uint32_t native_wifi_mhi_ms[8] __attribute__((weak_import));
extern char native_wifi_text[512][112] __attribute__((weak_import));
extern volatile uint32_t native_wifi_text_n __attribute__((weak_import));
extern char native_wifi_note[128][112] __attribute__((weak_import));
extern volatile uint32_t native_wifi_note_n __attribute__((weak_import));
extern int native_ufs_extra_fault(uint32_t *) __attribute__((weak_import));
extern struct {uint32_t stage, scans, frames, beacons, bss_seen; int last_err;} nw_wifi_stats __attribute__((weak_import));
extern volatile uint64_t nw_wifi_where[4] __attribute__((weak_import));
extern volatile uint32_t native_dpu_stats[20] __attribute__((weak_import));
extern volatile uint32_t native_touch_jitter[3] __attribute__((weak_import));
extern volatile uint32_t native_touch_hold[14] __attribute__((weak_import));
extern char native_lookup_log[160][112] __attribute__((weak_import));
extern volatile unsigned native_lookup_count __attribute__((weak_import));
extern volatile uint32_t native_dpu_pipes[54] __attribute__((weak_import));
extern volatile int32_t native_adsp_stats[16] __attribute__((weak_import));
extern volatile int32_t native_audio_stats[16] __attribute__((weak_import));
extern char native_adsp_text[96] __attribute__((weak_import));
extern volatile int32_t t970_audio_stats[16] __attribute__((weak_import));
extern volatile int32_t t970_audio_cmd[16] __attribute__((weak_import));
extern volatile uint32_t t970_audio_dma[8] __attribute__((weak_import));
extern volatile uint32_t t970_audio_amp[4][4] __attribute__((weak_import));
extern volatile uint32_t t970_audio_i2c[3] __attribute__((weak_import));
extern volatile uint32_t t970_audio_trace[25] __attribute__((weak_import));
extern volatile uint32_t t970_audio_clk[4][4] __attribute__((weak_import));
extern volatile uint32_t t970_audio_stream[16] __attribute__((weak_import));
extern volatile int32_t t970_audio_timing[8] __attribute__((weak_import));
extern volatile int32_t t970_audio_vol[16] __attribute__((weak_import));
extern volatile uint32_t t970_audio_bytes[12] __attribute__((weak_import));
extern volatile uint32_t t970_audio_bst[12] __attribute__((weak_import));
extern volatile uint32_t t970_audio_cap[8] __attribute__((weak_import));
extern volatile uint32_t t970_audio_ab[4] __attribute__((weak_import));
extern volatile int16_t t970_audio_cap_p[512] __attribute__((weak_import)), t970_audio_cap_k[512] __attribute__((weak_import));
extern volatile uint32_t t970_audio_hal[12] __attribute__((weak_import));
extern volatile int32_t t970_audio_service[8] __attribute__((weak_import));
extern volatile uint64_t t970_audio_clock[4] __attribute__((weak_import));
extern volatile int native_gpu_addon4_stage __attribute__((weak_import));
extern volatile int native_gpu_addon4_status __attribute__((weak_import));
extern volatile int32_t native_battery_stats[16] __attribute__((weak_import));
// T970Battery.cpp (periph14): stage, permille, mV, mA, deci-C, flags, full/design mAh, cycles, samples, failures, charger regs.
extern volatile int32_t t970_battery[13] __attribute__((weak_import));
// T970Haptics.cpp: stage, last vote rc, pulses, last ms, failed votes, boot test rc, refused requests, rail-off rc.
extern volatile int32_t t970_haptics[8] __attribute__((weak_import));
extern volatile int32_t t970_sensors[16] __attribute__((weak_import));
extern volatile int32_t t970_accel[8] __attribute__((weak_import));
extern volatile uint32_t t970_sensors_srv[8] __attribute__((weak_import));
extern char t970_sensors_text[96] __attribute__((weak_import));
extern volatile uint32_t native_accel_hid[5] __attribute__((weak_import));
extern volatile uint32_t native_orient_hid[5] __attribute__((weak_import));
extern volatile uint32_t native_gyro_hid[5] __attribute__((weak_import));
extern volatile uint32_t native_hid_gyro_status __attribute__((weak_import));
extern volatile int32_t t970_orient[4] __attribute__((weak_import));
extern volatile uint32_t native_hid_accel_status __attribute__((weak_import));
extern volatile uint32_t native_hid_orient_status __attribute__((weak_import));
extern volatile int32_t t970_camera[40] __attribute__((weak_import));
extern volatile uint8_t t970_camera_thumb[64 * 48] __attribute__((weak_import));
// T970Bluetooth.cpp: stage/error/ids (see the file's header), and its start-up lines, '|' separated.
extern volatile int32_t t970_bt[32] __attribute__((weak_import));
extern char t970_bt_text[1024] __attribute__((weak_import));
extern volatile uint32_t t970_bt_cp[3] __attribute__((weak_import));
extern volatile uint32_t native_swipe_stats[16] __attribute__((weak_import));
extern volatile int32_t native_touch_extent[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_method_stats[20][4] __attribute__((weak_import));
extern volatile uint64_t native_adreno_vm_phase_us[2][8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_map_fail[7][2] __attribute__((weak_import));
extern volatile uint32_t native_adreno_pool_stats[8] __attribute__((weak_import));
extern volatile uint32_t native_t970_bo_kernel[4] __attribute__((weak_import));
extern volatile uint32_t native_t970_bo_owner[2] __attribute__((weak_import));
extern volatile uint32_t native_t970_bo_budget[6] __attribute__((weak_import));
extern volatile uint32_t native_adreno_client_bytes[5][2] __attribute__((weak_import));
extern volatile uint32_t native_adreno_bo_memory[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_wc_audit[6] __attribute__((weak_import));
extern volatile uint32_t native_adreno_submit_ibs[4] __attribute__((weak_import));
extern volatile uint32_t native_touch_edge_stats[4] __attribute__((weak_import));
// T970FtsTouch state (package/T970FtsTouch.cpp), under local names so the weak references do not clash with other declarations.
#define T970_TOUCH(type, name, sym) extern type name __asm__("_" #sym) __attribute__((weak_import));
T970_TOUCH(volatile uint32_t, t970_touch_frames, native_touch_frames)
T970_TOUCH(volatile uint32_t, t970_touch_valid, native_touch_valid)
T970_TOUCH(volatile uint32_t, t970_touch_seq, native_touch_seq)
T970_TOUCH(volatile uint32_t, t970_touch_fail_step, native_touch_fail_step)
T970_TOUCH(volatile int, t970_touch_stage_result, native_touch_stage_result)
T970_TOUCH(volatile unsigned, t970_touch_stage_value, native_touch_stage_value)
T970_TOUCH(volatile unsigned, t970_touch_stage_total, native_touch_stage_total)
T970_TOUCH(volatile unsigned, t970_touch_ready, native_touch_ready)
T970_TOUCH(volatile unsigned, t970_touch_ready_ms, native_touch_ready_ms)
T970_TOUCH(volatile unsigned, t970_touch_irq_low, native_touch_irq_low)
T970_TOUCH(volatile int, t970_touch_last_x, native_touch_last_x)
T970_TOUCH(volatile int, t970_touch_last_y, native_touch_last_y)
T970_TOUCH(char, t970_touch_stage_name[32], native_touch_stage_name)
T970_TOUCH(volatile uint32_t, t970_hid_reports, native_touch_hid_reports)
T970_TOUCH(volatile int, t970_hid_results[8], native_hid_input_results)
T970_TOUCH(volatile uint32_t, t970_hid_stage, native_hid_stage)
T970_TOUCH(volatile uint32_t, t970_hid_class_sizes[5], native_hid_class_sizes)
T970_TOUCH(volatile uint32_t, t970_hid_touch_status, native_hid_touch_status)
T970_TOUCH(volatile uintptr_t, t970_hid_props_driver, native_hid_props_driver)
T970_TOUCH(char, t970_gpu_rows[31][120], native_gpu_rows)
T970_TOUCH(volatile uint32_t, t970_gpu_step, native_gpu_step)
T970_TOUCH(volatile uint32_t, t970_core_timer[8], armcpu_core_timer)
T970_TOUCH(volatile uint32_t, t970_core_sgi[8], armcpu_core_sgi)
T970_TOUCH(volatile uint32_t, t970_core_phase[8], armcpu_core_phase)
T970_TOUCH(volatile uint32_t, t970_core_last_id[8], armcpu_core_last_id)
T970_TOUCH(volatile uint32_t, t970_core_entries[8], armcpu_core_entries)
T970_TOUCH(volatile uint32_t, t970_qud_diag_phase, qcom_usb_diag_phase)
T970_TOUCH(volatile uint32_t, t970_qud_diag_failed, qcom_usb_diag_failed)
#undef T970_TOUCH
// QcomXnuUsbDebug's status (include/QcomXnuUsbDebug.h struct qcom_usb_debug_status): phase, error, core_result, qcom_result,
// restore_result, dma_mode first, as 32-bit words; the buffer is larger than the struct.
extern void qcom_usb_debug_get_status(void *status) __attribute__((weak_import));
extern void qcom_usb_debug_peek_status(void *status) __attribute__((weak_import));
// gts7xlwifi Wi-Fi bring-up (package/T970WifiProbe.cpp, t970-wifi=<stage>).
extern volatile uint32_t t970_wifi_stats[28] __attribute__((weak_import));
extern char t970_wifi_cmddb[640] __attribute__((weak_import));
extern volatile int32_t t970_wifi_rail_rc[12] __attribute__((weak_import));
extern volatile uint32_t t970_qud_inherited[24] __attribute__((weak_import));
extern volatile uint32_t t970_wifi_smmu[10] __attribute__((weak_import));
extern volatile uint32_t t970_wifi_mhi[13] __attribute__((weak_import));
extern volatile int32_t t970_qud_retry[8] __attribute__((weak_import));
// T970WifiProbe.cpp MAX77705 MUIC USB switch: step, hw/fw rev, status1/2/bc, CONTROL1 before/after, nacks, cc status, uic int.
extern volatile int32_t t970_muic[8] __attribute__((weak_import));
extern volatile uint32_t t970_wifi_dom[14] __attribute__((weak_import));
extern volatile uint32_t t970_wifi_trace[27] __attribute__((weak_import));
extern volatile uint32_t t970_wifi_rst[9] __attribute__((weak_import));
extern volatile uint32_t t970_wifi_chk[18] __attribute__((weak_import));
extern volatile uint32_t t970_wifi_pbl[26] __attribute__((weak_import));
extern volatile uint32_t t970_wifi_msi[10] __attribute__((weak_import));
extern volatile uint32_t t970_wifi_stack[16] __attribute__((weak_import));   // T970WifiStack.cpp, stage 6
extern volatile uint32_t nw_wifi_wr_n __attribute__((weak_import));          // T970Ath11k.c write trace (stage 5 and 6)
extern volatile uint32_t nw_wifi_wr[512][3] __attribute__((weak_import));
extern volatile uint32_t t970_qud_faults[16] __attribute__((weak_import));
extern volatile int32_t t970_qud_phy[15] __attribute__((weak_import));
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern volatile uint32_t native_touch_hid_multi[3] __attribute__((weak_import));
extern volatile int32_t native_backlight_request[6] __attribute__((weak_import));
extern volatile uint64_t native_adreno_reassert_ticks __attribute__((weak_import));
extern volatile uint64_t native_adreno_cache_stats[5] __attribute__((weak_import));
// NativeAdrenoPublish.cpp: per process name that opened the GPU, and the processes that asked the Metal discovery for its plugin.
struct native_adreno_client_record {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;};
extern struct native_adreno_client_record native_adreno_client_log[8] __attribute__((weak_import));
struct native_adreno_client_text {char name[20]; char text[2][64]; unsigned lines;};
extern struct native_adreno_client_text native_adreno_client_text[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_mount_fail[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_window_overflow[4][3] __attribute__((weak_import));
extern volatile uint32_t native_adreno_client_stats[5][6] __attribute__((weak_import));
extern volatile uint32_t native_adreno_table_switches[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_cp_aperture[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_table_pages[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_close_step[8] __attribute__((weak_import));
extern volatile uint32_t native_t970_ttbr[12] __attribute__((weak_import));
extern volatile uint32_t native_adreno_cp_image[22] __attribute__((weak_import));
struct native_adreno_asker_record {char name[20]; uint32_t reads;};
extern struct native_adreno_asker_record native_adreno_metal_askers[12] __attribute__((weak_import));
extern char native_gpu_coherence_row[120] __attribute__((weak_import));
extern size_t boot_progress_profile_rows(char *, size_t) __attribute__((weak_import));
extern size_t boot_progress_spawn_rows(char *, size_t) __attribute__((weak_import));
extern volatile uint64_t native_gpu_idle_sleeps __attribute__((weak_import));
extern volatile uint64_t native_gpu_idle_spins __attribute__((weak_import));
extern volatile uint32_t native_adreno_coherent_skips[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_io_coherent __attribute__((weak_import));
extern char native_cpufreq_row[120] __attribute__((weak_import));
extern uint32_t native_gpu_perf_vote[2] __attribute__((weak_import));
extern volatile uint32_t native_adreno_pool_status __attribute__((weak_import));
extern volatile uint32_t native_adreno_pool_wc __attribute__((weak_import));
extern volatile uint64_t native_adreno_fence_phys __attribute__((weak_import));
extern int (*native_adreno_dram_read32)(uint64_t, uint32_t *) __attribute__((weak_import));
extern int (*native_adreno_cached_read32)(uint64_t, uint32_t *) __attribute__((weak_import));
extern volatile uint32_t native_adreno_ring_log[4][4 + 112] __attribute__((weak_import));
extern volatile uint32_t native_adreno_ring_log_count __attribute__((weak_import));
extern volatile uint32_t native_adreno_ib_log[4][6 + 28] __attribute__((weak_import));
extern volatile uint32_t native_adreno_ib_log_count __attribute__((weak_import));
extern volatile uint32_t native_t970_wc_attr[3] __attribute__((weak_import));
extern volatile uint32_t native_t970_wc_reassert __attribute__((weak_import));
extern unsigned native_legacy_properties_now(char *, unsigned) __attribute__((weak_import));
extern unsigned native_legacy_clients_dump(char *, unsigned) __attribute__((weak_import));
extern unsigned native_legacy_swap_timeline(char *, unsigned) __attribute__((weak_import));
extern unsigned native_legacy_prepare_report(char *, unsigned) __attribute__((weak_import));
extern char native_adreno_metal_rows[48][96] __attribute__((weak_import));
extern unsigned native_legacy_state_words(char *, unsigned) __attribute__((weak_import));
extern volatile uint32_t native_adreno_view_diag[6] __attribute__((weak_import));
extern int boot_reset_probe(char *, unsigned) __attribute__((weak_import));
extern int boot_reset_bootloader(char *, unsigned) __attribute__((weak_import));
extern int boot_reset_mark_bootloader(char *, unsigned) __attribute__((weak_import));
extern int boot_reset_watchdog(uint32_t, char *, unsigned) __attribute__((weak_import));
extern volatile uint64_t native_adreno_import_diag[10] __attribute__((weak_import));
extern volatile uint32_t native_adreno_import_stage __attribute__((weak_import));
extern volatile uint64_t native_adreno_import_live[3 + 8 * 4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_import_persistent[2] __attribute__((weak_import));
extern volatile uint64_t native_adreno_import_fail_pa __attribute__((weak_import)), native_adreno_import_fail_offset __attribute__((weak_import));
// What the reset probe and the autoreset attempt saw, for the [reset] section of the log.
// The mark line is "mark: %08x -> %08x rc=%d | <pon line> pon_rc=%d", about 100 bytes with the
// PON text at 50, so 64 truncated it before pon_rc and left the PON verdict unreadable even when
// the write happened. 160 leaves room for a wider status word and a longer PON line.
static char boot_log_reset_probe_text[240], boot_log_reset_result[200], boot_log_reset_mark[160], boot_log_reset_watchdog[120];
// The kernel message buffer (bsd/sys/msgbuf.h); only these leading fields are read.
struct boot_log_msgbuf {int msg_magic, msg_size, msg_bufx, msg_bufr; char *msg_bufc;};
extern struct boot_log_msgbuf *msgbufp;

enum {LogBlock = 4096, LogMsgFirst = 1, LogMsgBlocks = 32, LogSlotFirst = 33, LogSlotBlocks = 16, LogPeriodMs = 2000, LogMaxErrors = 3,
      LogExportStage = 64};   // export bytes staged per write: 256 KiB
struct boot_log_header {
  char magic[8];                 // "SHLOG001"
  uint32_t version, header_bytes;
  uint64_t boot_id;              // CNTPCT when the writer started: tells one run from the next
  uint64_t first_lba;            // absolute 4096-byte LBA of block 0 on LUN 0
  uint32_t window_blocks, msg_first, msg_blocks, msg_bytes, slot_first, slot_blocks, slot_count, slots_written;
  uint32_t uptime_s, rounds, write_errors, gpt_open, gpt_last, state;
  char tag[32];
  // version 3: the newest complete export at block export_first of the export region (export_blocks long, ending export_below blocks
  // below block 0 of this window); export_bytes 0 = nothing exported yet
  uint32_t export_first, export_blocks, export_bytes, export_generation;
  uint32_t export_below;
  // version 4: the diagnostic file export (BootDiagExport.c) at block diag_first of the export region, diag_bytes long (0 = none)
  uint32_t diag_first, diag_bytes;
};
// Writer state for the board page: 0 off, 1 waiting for the window, 2 writing, 3 stopped after errors, 4 stopped (GPT changed).
volatile uint32_t boot_log_state, boot_log_rounds, boot_log_errors;
volatile uint64_t boot_log_first_lba;
static int boot_log_started;
// The timed return is the board's, not the writer's. The writer can block in a UFS write --
// measured on boot 43ba9162: rounds 3, uptime 4 s, write_errors 0, state 2 -- and while the
// deadline lived in its loop nothing restarted that board, so an unattended round never came
// back. This runs from the board monitor's once-a-second tick instead, which does not share a
// thread with the writer.
static unsigned tick_first, tick_seen_seq, tick_seen_at;
static int tick_reset_tried;
static uint32_t tick_autoreset;
static volatile unsigned log_sync_seq;
/* Which step of a round the writer is in, for a round that never finishes. Every wait on that path
 * is supposed to be bounded (the SCSI commands carry deadlines and poison the controller on
 * timeout, which shows up as write_errors), so a writer that stops with write_errors still 0 is
 * stuck somewhere that is not a command. This says which call it entered and never left; the
 * board monitor prints it, because the writer cannot report on itself. */
static volatile unsigned log_phase;
enum {ResetSyncGrace = 10};
static uint8_t *boot_log_buffer;   // LogMsgBlocks blocks: the message buffer, then reused for a slot and the header

// The export: boot_log_export_put takes the blob's pieces in order (any thread) and writes them through a staging buffer straight into
// the half of the export region that does not hold the last complete blob; once the last piece is on the medium that blob becomes the
// complete one, and the writer's next header names it. The lock serialises pieces and guards the counters.
static IOLock *export_lock;
static uint8_t *export_stage;
static uint32_t export_region;   // blocks of the export region, 0 = none
static uint32_t boot_log_previous_copied;   // gts7xlwifi: blocks of the previous run copied to the export region
static uint32_t export_total, export_filled, export_generation, export_target, export_ready, export_ready_bytes, export_ready_first;
// The diagnostic file export takes the half the ready blob does not hold; from then on the plugin's exports are refused (-7), so neither
// overwrites the other. diag_status: 0 not asked, 1 running, 2 done, negative failed.
static uint32_t diag_first, diag_bytes;
static int diag_owned;
volatile int boot_diag_status;
extern volatile uint32_t boot_diag_files __attribute__((weak_import));
extern volatile uint32_t boot_diag_skipped __attribute__((weak_import));
extern volatile uint32_t boot_diag_errors __attribute__((weak_import));
extern uint64_t boot_diag_scan(uint64_t) __attribute__((weak_import));
extern int boot_diag_stream(int (*)(const void *, uint32_t, void *), void *) __attribute__((weak_import));
int boot_log_export_put(uint32_t generation, uint32_t total, uint32_t offset, const void *data, uint32_t size) {
  if (!export_lock || !generation || !total || !data || !size || offset > total || size > total - offset) return -1;
  IOLockLock(export_lock);
  int status = 0;
  const uint32_t half = export_region / 2, stage = LogExportStage * LogBlock;
  if (!half || !&native_ufs_export_write) status = -4;
  else if (total > (uint64_t)half * LogBlock) status = -5;
  else if (diag_owned) status = -7;
  else if (export_ready & 0x80000000u) status = -10;   // a generation with the top bit set (a device's RAM dump) is kept
  if (!status && !export_stage) export_stage = (uint8_t *)IOMallocAligned(stage, LogBlock);
  if (!status && !export_stage) status = -2;
  if (!status && offset == 0) {
    export_generation = generation; export_total = total; export_filled = 0;
    export_target = export_ready && export_ready_first == 0 ? half : 0;
  }
  if (!status && (generation != export_generation || total != export_total || offset != export_filled)) status = -3;   // only in order
  const uint8_t *from = (const uint8_t *)data;
  while (!status && size) {
    const uint32_t at = export_filled % stage, n = size < stage - at ? size : stage - at;
    memcpy(export_stage + at, from, n);
    export_filled += n; from += n; size -= n;
    const int full = at + n == stage, done = export_filled == export_total;
    if (full || done) {
      const uint32_t used = at + n, blocks = (used + LogBlock - 1) / LogBlock;
      if (used % LogBlock) memset(export_stage + used, 0, blocks * LogBlock - used);
      if (!native_ufs_export_write(export_target + (export_filled - used) / LogBlock, blocks, export_stage)) {
        status = -6; export_generation = 0;   // the next generation starts over
      }
    }
    if (!status && done) {export_ready = export_generation; export_ready_bytes = export_total; export_ready_first = export_target;}
  }
  IOLockUnlock(export_lock);
  return status;
}

// The diagnostic export's writer: the same staging as boot_log_export_put, into its own half.
struct diag_sink {uint8_t *stage; uint32_t target, filled;};
static int diag_flush(struct diag_sink *k) {
  const uint32_t stage = LogExportStage * LogBlock, used = k->filled % stage ? k->filled % stage : stage;
  const uint32_t blocks = (used + LogBlock - 1) / LogBlock;
  if (used % LogBlock) memset(k->stage + used, 0, blocks * LogBlock - used);
  return native_ufs_export_write(k->target + (k->filled - used) / LogBlock, blocks, k->stage) ? 0 : -6;
}
static int diag_emit(const void *data, uint32_t size, void *arg) {
  struct diag_sink *k = arg;
  const uint32_t stage = LogExportStage * LogBlock;
  const uint8_t *from = data;
  while (size) {
    const uint32_t at = k->filled % stage, n = size < stage - at ? size : stage - at;
    memcpy(k->stage + at, from, n);
    k->filled += n; from += n; size -= n;
    if (at + n == stage && diag_flush(k)) return -6;
  }
  return 0;
}
static void diag_thread(void *arg, wait_result_t wait) {
  (void)arg; (void)wait;
  IOLockLock(export_lock);
  const uint32_t half = export_region / 2;
  struct diag_sink k = {NULL, export_ready && export_ready_first == 0 ? half : 0, 0};
  diag_owned = half != 0;
  IOLockUnlock(export_lock);
  int status = -4;
  if (half && &native_ufs_export_write && (k.stage = (uint8_t *)IOMallocAligned(LogExportStage * LogBlock, LogBlock))) {
    const uint64_t stream = boot_diag_scan((uint64_t)half * LogBlock - 16);
    const uint32_t head[4] = {0, 0, (uint32_t)stream, 0};
    if (!stream) status = -8;
    else {
      status = diag_emit("SHEXR001", 8, &k);
      if (!status) status = diag_emit(&head[2], 8, &k);
      if (!status) status = boot_diag_stream(diag_emit, &k);
      if (!status && k.filled % (LogExportStage * LogBlock)) status = diag_flush(&k);
      if (!status && k.filled != stream + 16) status = -9;
    }
    IOFreeAligned(k.stage, LogExportStage * LogBlock);
  }
  IOLockLock(export_lock);
  if (!status) {diag_first = k.target; diag_bytes = k.filled;}
  IOLockUnlock(export_lock);
  boot_diag_status = status ? status : 2;
}

static uint64_t log_ticks(void) {uint64_t v; __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v)); return v;}

// Appends a text row without its trailing blanks; returns the new length (never past cap - 1).
static size_t put(char *out, size_t at, size_t cap, const char *text, size_t max) {
  size_t n = strnlen(text, max);
  while (n && (text[n - 1] == ' ' || text[n - 1] == 0)) --n;
  for (size_t i = 0; i < n && at + 2 < cap; ++i) {char c = text[i]; out[at++] = (c >= 32 && c < 127) ? c : '?';}
  if (at + 1 < cap) out[at++] = '\n';
  return at;
}

// The ring entries closest to a GPU fault address (pages): distance 0 inside a range, else to its nearest end. Picks up to `want`.
static size_t gpu_far_near(char *row, size_t cap, const char *label, volatile uint32_t (*ring)[4], uint32_t count, uint64_t far_page, unsigned want) {
  size_t n = (size_t)snprintf(row, cap, " %s(%u)", label, count);
  unsigned used[8] = {0};
  unsigned have = 0;
  const unsigned total = count < 256 ? count : 256;
  for (unsigned pick = 0; pick < want && pick < 8; ++pick) {
    uint64_t best = ~0ull; unsigned at = 256;
    for (unsigned i = 0; i < total; ++i) {
      int taken = 0;
      for (unsigned k = 0; k < have; ++k) if (used[k] == i) taken = 1;
      if (taken) continue;
      const uint64_t start = ring[i][0], end = start + ring[i][1];
      const uint64_t d = far_page < start ? start - far_page : far_page >= end ? far_page - end + 1 : 0;
      if (d < best) {best = d; at = i;}
    }
    if (at == 256) break;
    used[have++] = at;
    if (n < cap) n += (size_t)snprintf(row + n, cap - n, " %x+%x a%u #%u d%llx", ring[at][0], ring[at][1], ring[at][2], ring[at][3], (unsigned long long)best);
  }
  return n;
}

static size_t compose(char *out, size_t cap, uint32_t slot, unsigned now) {
  size_t at = 0;
  char head[96];
  snprintf(head, sizeof head, "#slot %u t=%us tag=%s", slot, now, &boot_progress_tag ? boot_progress_tag() : "");
  at = put(out, at, cap, head, sizeof head);
  if (&boot_progress_page_text) {
    at = put(out, at, cap, "[page]", 8);
    for (unsigned i = 0; i < 48; ++i) if (boot_progress_page_text[i][0]) at = put(out, at, cap, boot_progress_page_text[i], 120);
  }
  if (&boot_progress_banner_text && &boot_progress_banner_rows) {
    unsigned rows = boot_progress_banner_rows; if (rows > 49) rows = 49;
    at = put(out, at, cap, "[banner]", 10);
    for (unsigned i = 0; i < rows; ++i) at = put(out, at, cap, boot_progress_banner_text[i], 120);
  }
  if (&boot_progress_ext) {
    at = put(out, at, cap, "[ext]", 8);
    for (unsigned i = 0; i < 8; ++i) {char row[56]; snprintf(row, sizeof row, "X%u %.47s", i, boot_progress_ext[i]); at = put(out, at, cap, row, sizeof row);}
  }
  // The Data store's counters (NativeRamBlock), a row of their own: the ext slots they once shared are taken by other kits.
  if (&native_data_persist_state && native_data_persist_state) {
    char row[400];
    snprintf(row, sizeof row, "D st=%u hdr=%u gpt=%x/%x lba=%llx pres=%llu ld=%llu wr=%llu cm=%llu er=%llu pages=%llu", native_data_persist_state,
             native_data_persist_header, native_data_persist_gpt, native_data_persist_gpt_after, (unsigned long long)native_data_persist_first_lba,
             (unsigned long long)native_data_persist_present, (unsigned long long)native_data_persist_loaded, (unsigned long long)native_data_persist_writes,
             (unsigned long long)native_data_persist_syncs, (unsigned long long)native_data_persist_errors, (unsigned long long)native_data_persist_pages);
    at = put(out, at, cap, "[data]", 8);
    at = put(out, at, cap, row, sizeof row);
  }
  // The payload's GPU addon (kern_exec.c native_gpu_addon_install): files in it (-1 none), written, already equal, failed, last error; then
  // the stage/status of the boot-time addon mounts (stage 5 = mounted and both entries visible): plugin, launchd job, wallpaper.
  if (&native_gpu_addon_files && &native_gpu_addon_stage) {
    char row[120];
    snprintf(row, sizeof row, "A files=%d wr=%d same=%d fail=%d err=%d mount=%d/%d mount2=%d/%d mount3=%d/%d", native_gpu_addon_files,
             native_gpu_addon_written, native_gpu_addon_same, native_gpu_addon_failed, native_gpu_addon_error, native_gpu_addon_stage,
             native_gpu_addon_status, native_gpu_addon2_stage, native_gpu_addon2_status, &native_gpu_addon3_stage ? native_gpu_addon3_stage : 0,
             &native_gpu_addon3_status ? native_gpu_addon3_status : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  // The OS cryptex path (kern_exec.c native_cryptex_os_view; stage 5 = a WebKit extension found through /System/Cryptexes/OS) and the
  // removed MobileGestalt cache (0 removed, 2 none there).
  if (&native_cryptex_os_stage && &native_gestalt_cache_status) {
    char row[80];
    snprintf(row, sizeof row, "[cryptex os] stage=%d status=%d gestalt-cache=%d ls-store=%d", native_cryptex_os_stage, native_cryptex_os_status,
             native_gestalt_cache_status, &native_ls_store_status ? native_ls_store_status : -9);
    at = put(out, at, cap, row, sizeof row);
  }
  // The one-time copy of the OS volume's staged system apps into Data (kern_exec.c native_staged_apps_prepare): stage (2 copied, 3 copied
  // before, -1 no source), directories, files, MiB, failures and the last error.
  if (&native_staged_apps_stage) {
    char row[160];
    snprintf(row, sizeof row, "S st=%d dirs=%d files=%d mib=%d fail=%d err=%d | migrate st=%d moved=%d err=%d | mg st=%d n=%d e=%d",
             native_staged_apps_stage, native_staged_apps_dirs, native_staged_apps_files, native_staged_apps_mib, native_staged_apps_failed,
             native_staged_apps_error, &native_t970_migrate ? native_t970_migrate[0] : 0, &native_t970_migrate ? native_t970_migrate[1] : 0,
             &native_t970_migrate ? native_t970_migrate[2] : 0, &native_t970_mgcache ? native_t970_mgcache[0] : 0,
             &native_t970_mgcache ? native_t970_mgcache[1] : 0, &native_t970_mgcache ? native_t970_mgcache[2] : 0);
    at = put(out, at, cap, row, sizeof row);
    if (native_staged_apps_stage == 1 && &native_staged_apps_path) {
      snprintf(row, sizeof row, "S at %.100s", native_staged_apps_path);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // Power and volume buttons (ButtonProbe.cpp): stage (3 armed, negative the failed step), interrupts, reports, pressed mask, the power and
  // GPIO APIDs, interrupts of other APIDs cleared, PDC version, last error, spurious interrupts.
  if (&native_buttons_stats) {
    char row[120];
    snprintf(row, sizeof row, "B st=%d irq=%d rep=%d mask=%x apid=%d/%d off=%d pdc=%x err=%d spur=%d", native_buttons_stats[0], native_buttons_stats[1],
             native_buttons_stats[2], native_buttons_stats[3], native_buttons_stats[4], native_buttons_stats[5], native_buttons_stats[6],
             native_buttons_stats[7], native_buttons_stats[8], native_buttons_stats[9]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Panel brightness (AppleARMBacklight.cpp): stage (3 published, negative the failed step), requests, last level, the levels read back from
  // both drivers at start, transfer errors, last failed M_IRQ status, both engines' protocol and clock configuration, last error.
  if (&native_backlight_stats) {
    char row[150];
    snprintf(row, sizeof row, "L st=%d req=%d lvl=%d a=%d b=%d xerr=%d irq=%x ea=%x eb=%x err=%d goal=%d fade=%d", native_backlight_stats[0],
             native_backlight_stats[1], native_backlight_stats[2], native_backlight_stats[3], native_backlight_stats[4], native_backlight_stats[5],
             native_backlight_stats[6], native_backlight_stats[7], native_backlight_stats[8], native_backlight_stats[9],
             &native_backlight_request ? native_backlight_request[0] : -1, &native_backlight_request ? native_backlight_request[1] : -1);
    at = put(out, at, cap, row, sizeof row);
    // The request path and the engines: request timer runs, fades begun, interrupts from a and b, interrupt mode, timeouts, transfers.
    snprintf(row, sizeof row, "L path runs=%d fades=%d irq=%d/%d mode=%d tmo=%d done=%d empty=%d last=%x", &native_backlight_request ? native_backlight_request[2] : -1,
             &native_backlight_request ? native_backlight_request[3] : -1, native_backlight_stats[10], native_backlight_stats[11],
             native_backlight_stats[12], native_backlight_stats[13], native_backlight_stats[15],
             &native_backlight_request ? native_backlight_request[4] : -1, &native_backlight_request ? (unsigned)native_backlight_request[5] : 0u);
    at = put(out, at, cap, row, sizeof row);
    // How transfers complete: interrupts from each engine, interrupt mode (1 attached, 0 polling, else the attach result), timeouts,
    // attach tries and transfers done.
    snprintf(row, sizeof row, "L2 ia=%d ib=%d mode=%d to=%d tries=%d done=%d", native_backlight_stats[10], native_backlight_stats[11],
             native_backlight_stats[12], native_backlight_stats[13], native_backlight_stats[14], native_backlight_stats[15]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&boot_progress_ext_wide)
    for (unsigned i = 0; i < 2; ++i) {char row[120]; snprintf(row, sizeof row, "W%u %.111s", i, boot_progress_ext_wide[i]); at = put(out, at, cap, row, sizeof row);}
  // Every traced process's stacks (the banner shows only what fits the panel): role 0..3 are the boot-ui-focus slots, main thread in full
  // (symbolicate with unslid = address - cache slide), then its thread groups.
  if (&boot_ui_trace_role_rows) {
    static char rows[64][120];
    at = put(out, at, cap, "[stacks]", 10);
    for (unsigned role = 0; role < 4; ++role) {
      unsigned n = boot_ui_trace_role_rows(role, rows, 64, 2);   // 2: with each thread group's deeper frames
      for (unsigned i = 0; i < n; ++i) at = put(out, at, cap, rows[i], 120);
    }
    // gts7xlwifi: the display pipeline's timing (pingpong frame counters, refresh rate, interfaces).
    if (&t970_dpu_timing_row) {
      char row[400];
      t970_dpu_timing_row(row, sizeof row);
      at = put(out, at, cap, row, sizeof row);
    }
    if (&native_t970_direct) {
      char row[320];
      snprintf(row, sizeof row, "[direct] st %u pipes %x ctl %u flush %x boot %x stride %x src %x %x %x %x win %x | smmu s0 %x sctlr %x "
               "fsr %x us %u | flips %u mapped %u mapfail %u copy %u boot %u waits %u slow %u",
               native_t970_direct[0], native_t970_direct[1], native_t970_direct[2], native_t970_direct[3], native_t970_direct[4],
               native_t970_direct[5], native_t970_direct[6], native_t970_direct[7], native_t970_direct[8], native_t970_direct[9],
               native_t970_direct[10], &native_ufs_dpu_smmu ? native_ufs_dpu_smmu[4] : 0, &native_ufs_dpu_smmu ? native_ufs_dpu_smmu[9] : 0,
               &native_ufs_dpu_check ? native_ufs_dpu_check[3] : 0, &native_ufs_dpu_check ? native_ufs_dpu_check[7] : 0,
               &native_legacy_direct ? native_legacy_direct[0] : 0, &native_legacy_direct ? native_legacy_direct[1] : 0,
               &native_legacy_direct ? native_legacy_direct[2] : 0, &native_legacy_direct ? native_legacy_direct[7] : 0,
               &native_legacy_direct ? native_legacy_direct[8] : 0, &native_legacy_direct ? native_legacy_direct[9] : 0,
               &native_legacy_direct ? native_legacy_direct[13] : 0);
      at = put(out, at, cap, row, sizeof row);
      if (&native_t970_swapfmt) {
        const uint32_t f = native_t970_swapfmt[0];
        snprintf(row, sizeof row, "[swapfmt] last %c%c%c%c xr10 %u half %u 4byte %u offer-refused %u", (char)(f >> 24), (char)(f >> 16),
                 (char)(f >> 8), (char)f, native_t970_swapfmt[1], native_t970_swapfmt[2], native_t970_swapfmt[3], native_t970_swapfmt[4]);
        at = put(out, at, cap, row, sizeof row);
      }
    }
    // gts7xlwifi: a core that stopped taking interrupts (every freeze from v80 to v90 had one).
    if (&armcpu_stall_row) {
      char row[220];
      armcpu_stall_row(row, sizeof row);
      at = put(out, at, cap, row, sizeof row);
    }
    // gts7xlwifi: the holder of the GPU driver's global lock (v89 froze with backboardd waiting for it).
    if (&boot_ui_trace_mutex_owner_rows && &native_adreno_global_lock) {
      unsigned n = boot_ui_trace_mutex_owner_rows(native_adreno_global_lock(), rows, 64);
      for (unsigned i = 0; i < n; ++i) at = put(out, at, cap, rows[i], 120);
    }
  }
  if (&native_legacy_state_words) {
    static char words[240];
    if (native_legacy_state_words(words, sizeof words)) {at = put(out, at, cap, "[display state]", 16); at = put(out, at, cap, words, sizeof words);}
  }
  if (&native_legacy_prepare_report) {
    char row[220];
    native_legacy_prepare_report(row, sizeof row);
    at = put(out, at, cap, "[scanout prepare]", 18); at = put(out, at, cap, row, sizeof row);
  }
  if (&native_legacy_swap_timeline) {
    static char timeline[4096];
    if (native_legacy_swap_timeline(timeline, sizeof timeline)) {
      at = put(out, at, cap, "[swap timeline]", 16);
      for (char *line = timeline; *line && at + 130 < cap;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        at = put(out, at, cap, line, 120);
        if (!end) break;
        line = end + 1;
      }
    }
  }
  if (&native_adreno_metal_rows && native_adreno_metal_rows[0][0]) {
    at = put(out, at, cap, "[metal table]", 14);
    for (unsigned r = 0; r < 40 && at + 130 < cap; ++r)
      if (native_adreno_metal_rows[r][0]) at = put(out, at, cap, (const char *)native_adreno_metal_rows[r], 96);
  }
  // gts7xlwifi: rows 40..47 are the plugin's XR10 probe (NativeAdrenoMetalPlugin.mm t970Xr10Probe), printed on their own: the table above
  // only prints when its row 0 has text, which the probe does not write.
  if (&native_adreno_metal_rows) {
    for (unsigned r = 40; r < 48; ++r)
      if (native_adreno_metal_rows[r][0]) {
        char row[112];
        snprintf(row, sizeof row, "[xr10 probe %u] %.96s", r, (const char *)native_adreno_metal_rows[r]);
        at = put(out, at, cap, row, sizeof row);
      }
  }
  // The framebuffer user clients' notification slots (is the render server's vsync registered?).
  if (&native_legacy_clients_dump) {
    static char dump[4096];
    unsigned n = native_legacy_clients_dump(dump, sizeof dump);
    if (n) {
      at = put(out, at, cap, "[mfb clients]", 14);
      for (char *line = dump; *line && at + 130 < cap;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        at = put(out, at, cap, line, 120);
        if (!end) break;
        line = end + 1;
      }
    }
  }
  // The display object's properties now (refresh intervals, timing, scale): the lines the original driver published.
  if (&native_legacy_properties_now) {
    static char props[7168];
    unsigned n = native_legacy_properties_now(props, sizeof props);
    if (n) {
      at = put(out, at, cap, "[display]", 10);
      // The XML is one long line; 116 characters per row with a prefix, and a tag per key on its own row so refresh/timing keys stand out.
      for (unsigned i = 0; i < n && at + 130 < cap; i += 116) {
        char row[124];
        snprintf(row, sizeof row, "D%03u %.116s", i / 116, props + i);
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  // The traced processes' threads with the highest context-switch rate (they are idle in every stack sample but woken hundreds of times a second).
  if (&boot_ui_trace_hot_rows) {
    static char hot[96][120];
    unsigned n = boot_ui_trace_hot_rows(hot, 96);
    if (n) {char head[64]; snprintf(head, sizeof head, "[hot] kernel slide=%lx", (unsigned long)vm_kernel_slide); at = put(out, at, cap, head, sizeof head);}
    for (unsigned i = 0; i < n; ++i) at = put(out, at, cap, hot[i], 120);
  }
  // The four newest ring submissions (fence, ring range, then the words the CP reads) and the first words of their IBs: all that the
  // logs keep, so a stalled fence behind newer submissions is still shown.
  if (&native_adreno_ring_log_count && native_adreno_ring_log_count) {
    const uint32_t newest = native_adreno_ring_log_count - 1;
    for (unsigned back = 0; back < 4 && back < native_adreno_ring_log_count; ++back) {
      const volatile uint32_t *r = native_adreno_ring_log[(newest - back) & 3];
      char row[130];
      snprintf(row, sizeof row, "[ring] fence=%u ring %x..%x words=%u", r[0], r[1], r[2], r[3]);
      at = put(out, at, cap, row, sizeof row);
      const uint32_t words = r[3] < 112 ? r[3] : 112;
      for (uint32_t i = 0; i < words; i += 8) {
        int n = snprintf(row, sizeof row, "R%03x", i);
        for (uint32_t k = i; k < i + 8 && k < words && n > 0 && n < 110; ++k) n += snprintf(row + n, sizeof row - n, " %08x", r[6 + k]);
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  if (&native_adreno_ib_log_count && native_adreno_ib_log_count) {
    const uint32_t newest = native_adreno_ib_log_count - 1;
    for (unsigned back = 0; back < 4 && back < native_adreno_ib_log_count; ++back) {
      const volatile uint32_t *r = native_adreno_ib_log[(newest - back) & 3];
      char row[130];
      snprintf(row, sizeof row, "[ib] fence=%u iova=%x%08x bytes=%u", r[0], r[2], r[1], r[3]);
      at = put(out, at, cap, row, sizeof row);
      for (uint32_t i = 0; i < 28; i += 8) {
        int n = snprintf(row, sizeof row, "I%02x", i);
        for (uint32_t k = i; k < i + 8 && k < 28 && n > 0 && n < 110; ++k) n += snprintf(row + n, sizeof row - n, " %08x", r[6 + k]);
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  // The fence page, word by word as DRAM holds it and as the CPU's cacheable alias reads it (the CP writes the timestamps with a GPU-side write).
  if (&native_adreno_fence_phys && native_adreno_fence_phys && &native_adreno_dram_read32 && native_adreno_dram_read32 && &native_adreno_cached_read32 &&
      native_adreno_cached_read32) {
    char row[130];
    int n = snprintf(row, sizeof row, "[fence page] pa=%llx dram:", (unsigned long long)native_adreno_fence_phys);
    for (unsigned w = 0; w < 8 && n > 0 && n < 110; ++w) {uint32_t v = 0xdead; native_adreno_dram_read32(native_adreno_fence_phys + 4 * w, &v); n += snprintf(row + n, sizeof row - n, " %x", v);}
    at = put(out, at, cap, row, sizeof row);
    n = snprintf(row, sizeof row, "[fence page] cached:");
    for (unsigned w = 0; w < 8 && n > 0 && n < 110; ++w) {uint32_t v = 0xdead; native_adreno_cached_read32(native_adreno_fence_phys + 4 * w, &v); n += snprintf(row + n, sizeof row - n, " %x", v);}
    at = put(out, at, cap, row, sizeof row);
  }
  // Kernel GPU methods by number: calls, failures, last status and total time (the calls of the last 2 s are the difference of two slots).
  if (&native_adreno_method_stats) {
    char row[130];
    int n = snprintf(row, sizeof row, "[gpu methods] n:calls/fail/us");
    for (unsigned m = 1; m < 20; ++m) {
      if (!native_adreno_method_stats[m][0]) continue;
      if (n > 100) {at = put(out, at, cap, row, sizeof row); n = snprintf(row, sizeof row, "[gpu methods+]");}
      n += snprintf(row + n, sizeof row - n, " %u:%u/%u/%u", m, native_adreno_method_stats[m][0], native_adreno_method_stats[m][1], native_adreno_method_stats[m][3]);
    }
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_vm_phase_us && (native_adreno_vm_phase_us[0][0] || native_adreno_vm_phase_us[1][0])) {
    char row[170];
    for (unsigned k = 0; k < 2; ++k) {
      const volatile uint64_t *p = native_adreno_vm_phase_us[k];
      if (!p[0]) continue;
      snprintf(row, sizeof row, "[vm %s] n=%llu us: observe1=%llu %s=%llu batch=%llu observe2=%llu %s=%llu max=%llu MiB=%llu", k ? "unmap" : "map",
               (unsigned long long)p[0], (unsigned long long)p[1], k ? "acquire" : "publish", (unsigned long long)p[2], (unsigned long long)p[3],
               (unsigned long long)p[4], k ? "retire" : "-", (unsigned long long)p[5], (unsigned long long)p[6], (unsigned long long)(p[7] >> 20));
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&native_adreno_pool_stats) {
    char row[200];
    snprintf(row, sizeof row, "[bo pool] %u MiB (last reserve status %x, wc %x): %u BOs from it, %u fell back, in use %u MiB (peak %u) | scattered: %u BOs %u spans | import spans %u",
             native_adreno_pool_stats[0], native_adreno_pool_status, &native_adreno_pool_wc ? native_adreno_pool_wc : 0, native_adreno_pool_stats[1], native_adreno_pool_stats[2], native_adreno_pool_stats[3],
             native_adreno_pool_stats[4], native_adreno_pool_stats[5], native_adreno_pool_stats[6], native_adreno_pool_stats[7]);
    at = put(out, at, cap, row, sizeof row);
  }
  // gts7xlwifi: kernel-mapped (wired) BOs live/peak MiB, live count, frees; then system BOs live/peak MiB.
  if (&native_t970_bo_kernel) {
    char row[200];
    snprintf(row, sizeof row, "[bo kernel] live %u MiB peak %u BOs %u freed %u | system live %u peak %u owner %u refused %u", native_t970_bo_kernel[0],
             native_t970_bo_kernel[1], native_t970_bo_kernel[2], native_t970_bo_kernel[3], &native_adreno_bo_memory ? native_adreno_bo_memory[4] : 0,
             &native_adreno_bo_memory ? native_adreno_bo_memory[5] : 0, &native_t970_bo_owner ? native_t970_bo_owner[0] : 0,
             &native_t970_bo_owner ? native_t970_bo_owner[1] : 0);
    at = put(out, at, cap, row, sizeof row);
    if (&native_t970_bo_budget && &native_adreno_client_bytes) {
      // per client slot (0 the render server): owned MiB now/peak; then the budget's refusals, last slot and request.
      snprintf(row, sizeof row, "[bo budget] s0 %u/%u s1 %u/%u s2 %u/%u s3 %u/%u o %u/%u | caps %u %u %u refused %u last s%u %uM",
               native_adreno_client_bytes[0][0], native_adreno_client_bytes[0][1], native_adreno_client_bytes[1][0], native_adreno_client_bytes[1][1],
               native_adreno_client_bytes[2][0], native_adreno_client_bytes[2][1], native_adreno_client_bytes[3][0], native_adreno_client_bytes[3][1],
               native_adreno_client_bytes[4][0], native_adreno_client_bytes[4][1], native_t970_bo_budget[0], native_t970_bo_budget[1],
               native_t970_bo_budget[2], native_t970_bo_budget[3], native_t970_bo_budget[4], native_t970_bo_budget[5]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // GPU clients: name opens/refused/closes, live slot (- none), highest GPU address its mappings reached (MiB), last refusal.
  if (&native_adreno_client_log) {
    char row[400];
    size_t used = (size_t)snprintf(row, sizeof row, "[gpu clients]");
    for (unsigned i = 0; i < 8 && native_adreno_client_log[i].name[0] && used < sizeof row; ++i) {
      const struct native_adreno_client_record *r = &native_adreno_client_log[i];
      char slot[8];
      if (r->slot < 8) snprintf(slot, sizeof slot, "%u", r->slot); else snprintf(slot, sizeof slot, "-");
      used += (size_t)snprintf(row + used, sizeof row - used, " %.19s %u/%u/%u s%s %uM r%x", r->name, r->opens, r->refused, r->closes, slot,
                               r->peak_mib, r->last_refusal);
    }
    at = put(out, at, cap, row, sizeof row);
  }
  // [gpu windows]: per client window the mappings that fell outside it (count, last size KiB, last end MiB), the latest failed mount (stage 1 no free
  // slot, 2 core open, 3 window scope; status; slot; count), then one line per process other than the render server: why its Metal device failed.
  if (&native_adreno_window_overflow && &native_adreno_mount_fail) {
    char row[300];
    snprintf(row, sizeof row, "[gpu windows] ovf %u/%uK/%uM %u/%uK/%uM %u/%uK/%uM %u/%uK/%uM | mount fail stage %u st %x slot %u n %u",
             native_adreno_window_overflow[0][0], native_adreno_window_overflow[0][1], native_adreno_window_overflow[0][2],
             native_adreno_window_overflow[1][0], native_adreno_window_overflow[1][1], native_adreno_window_overflow[1][2],
             native_adreno_window_overflow[2][0], native_adreno_window_overflow[2][1], native_adreno_window_overflow[2][2],
             native_adreno_window_overflow[3][0], native_adreno_window_overflow[3][1], native_adreno_window_overflow[3][2],
             native_adreno_mount_fail[0], native_adreno_mount_fail[1], native_adreno_mount_fail[2], native_adreno_mount_fail[3]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_adreno_client_text)
      for (unsigned i = 0; i < 4 && native_adreno_client_text[i].name[0]; ++i) {
        for (unsigned k = 0; k < 2; ++k) {
          if (!native_adreno_client_text[i].text[k][0]) continue;
          snprintf(row, sizeof row, "[gpu client text] %.19s: %.63s", native_adreno_client_text[i].name, native_adreno_client_text[i].text[k]);
          at = put(out, at, cap, row, sizeof row);
        }
      }
  }
  if (&native_adreno_metal_askers) {
    char row[400];
    size_t used = (size_t)snprintf(row, sizeof row, "[metal askers]");
    for (unsigned i = 0; i < 12 && native_adreno_metal_askers[i].name[0] && used < sizeof row; ++i)
      used += (size_t)snprintf(row + used, sizeof row - used, " %.19s:%u", native_adreno_metal_askers[i].name, native_adreno_metal_askers[i].reads);
    at = put(out, at, cap, row, sizeof row);
  }
  // gts7xlwifi: pages a write-combined BO held without the attribute at submission (NativeAdrenoCpu.cpp).
  if (&native_t970_wc_attr) {
    char row[100];
    snprintf(row, sizeof row, "[wc attr] mismatch %u last %x after %x reassert %u", native_t970_wc_attr[0], native_t970_wc_attr[1], native_t970_wc_attr[2],
             &native_t970_wc_reassert ? native_t970_wc_reassert : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  // The IO-coherence proof (GpuPowerProbe coherenceProbe) and the maintenance passes it saved: map, unmap, submission, completion.
  if (&native_gpu_coherence_row && native_gpu_coherence_row[0]) {
    char row[260];
    snprintf(row, sizeof row, "[coherence] %.110s | on=%u skipped map=%u unmap=%u submit=%u complete=%u | fence pauses slept=%llu spun=%llu",
             native_gpu_coherence_row,
             &native_adreno_io_coherent ? native_adreno_io_coherent : 0, &native_adreno_coherent_skips ? native_adreno_coherent_skips[0] : 0,
             &native_adreno_coherent_skips ? native_adreno_coherent_skips[1] : 0, &native_adreno_coherent_skips ? native_adreno_coherent_skips[2] : 0,
             &native_adreno_coherent_skips ? native_adreno_coherent_skips[3] : 0,
             (unsigned long long)(&native_gpu_idle_sleeps ? native_gpu_idle_sleeps : 0), (unsigned long long)(&native_gpu_idle_spins ? native_gpu_idle_spins : 0));
    at = put(out, at, cap, row, sizeof row);
  }
  // Edge contacts (drivers/ShengHidInput edge::): wrapper installed, contacts begun, of them from an edge, events skipped.
  if (&native_touch_edge_stats) {
    char row[100];
    snprintf(row, sizeof row, "[touch edge] installed=%u contacts=%u from_edge=%u skipped=%u", native_touch_edge_stats[0],
             native_touch_edge_stats[1], native_touch_edge_stats[2], native_touch_edge_stats[3]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Multi-finger reports (drivers/ShengHidInput): reports with two or more fingers down, the most at once, lifts of vanished fingers.
  if (&native_touch_hid_multi) {
    char row[80];
    snprintf(row, sizeof row, "[touch multi] reports=%u max=%u lifts=%u", native_touch_hid_multi[0], native_touch_hid_multi[1],
             native_touch_hid_multi[2]);
    at = put(out, at, cap, row, sizeof row);
  }
  // gts7xlwifi T970FtsTouch: last bring-up stage, failure step, READY, event frames read / valid touches, attention lows, last point.
  if (&t970_touch_frames) {
    char row[160];
    snprintf(row, sizeof row, "[t970 touch] stage=%.24s v=%x r=%d n=%u fail=%u ready=%u@%ums frames=%u valid=%u irqlow=%u seq=%u xy=%d,%d",
             t970_touch_stage_name, t970_touch_stage_value, t970_touch_stage_result, t970_touch_stage_total, t970_touch_fail_step,
             t970_touch_ready, t970_touch_ready_ms, t970_touch_frames, t970_touch_valid, t970_touch_irq_low, t970_touch_seq,
             t970_touch_last_x, t970_touch_last_y);
    at = put(out, at, cap, row, sizeof row);
  }
  // gts7xlwifi Wi-Fi: stage, rails, GDSC, halt before/after, pins, PHY status, link, endpoint id, BAR/MHI and BHI.
  if (&t970_wifi_stats && t970_wifi_stats[0]) {
    const volatile uint32_t *w = t970_wifi_stats;
    char row[260];
    snprintf(row, sizeof row, "[t970 wifi] st %d rails %x gdsc %x halt %x pins %x pcs %x rp %x ltssm %x/%x dbg %x/%x %ums ep %x cls %x", (int)w[0], w[5], w[2],
             w[3], w[4], w[6], w[7], w[10], w[11], w[8], w[9], w[12], w[13], w[14]);
    at = put(out, at, cap, row, sizeof row);
    if (&t970_wifi_rail_rc) {
      snprintf(row, sizeof row, "[t970 wifi rails] smpa4 %d smpa6 %d smpa5 %d smpc8 %d ldoc5 %d | rfclka1 %d rfclka3 %d lnbclka2 %d lnbclka3 %d",
               (int)t970_wifi_rail_rc[0], (int)t970_wifi_rail_rc[1], (int)t970_wifi_rail_rc[2], (int)t970_wifi_rail_rc[3], (int)t970_wifi_rail_rc[4],
               (int)t970_wifi_rail_rc[8], (int)t970_wifi_rail_rc[9], (int)t970_wifi_rail_rc[10], (int)t970_wifi_rail_rc[11]);
      at = put(out, at, cap, row, sizeof row);
    }
    if (&t970_wifi_cmddb && t970_wifi_cmddb[0]) {
      for (unsigned part = 0; part < 4; ++part) {
        const char *from = t970_wifi_cmddb + part * 160;
        if (!from[0]) break;
        snprintf(row, sizeof row, "[t970 cmddb %u] %.160s", part, from);
        at = put(out, at, cap, row, sizeof row);
        if (strnlen(from, 160) < 160) break;
      }
    }
    if ((int)w[0] >= 18 || (int)w[0] <= -18) {
      snprintf(row, sizeof row, "[t970 wifi ep] bar %x/%x cmd %x mhi len %x ver %x cfg %x bhi %x ctrl %x status %x | ee %x st %x err %x sid %x",
               w[15], w[16], w[17], w[18], w[19], w[20], w[21], w[22], w[23], w[24], w[25], w[26], w[27]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&t970_wifi_smmu && t970_wifi_smmu[9]) {
    const volatile uint32_t *m = t970_wifi_smmu;
    char row[180];
    snprintf(row, sizeof row, "[t970 wifi smmu] scr0 %x idr0 %x valid %u | 1c01 smr#%x %x s2cr %x | 1c00 smr#%x %x s2cr %x", m[0], m[1], m[2], m[3], m[4],
             m[5], m[6], m[7], m[8]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&t970_wifi_dom && t970_wifi_dom[0]) {
    const volatile uint32_t *w = t970_wifi_dom;
    char row[260];
    snprintf(row, sizeof row, "[t970 wifi dom] rc %d cr0 %x idr1 %x had %x cb/smr/asid %x tables %x:%08x fsr %x -> %x far %x:%08x fsynr %x | bank %u at %x",
             (int)w[0], w[1], w[2], w[3], w[4], w[6], w[5], w[7], w[8], w[10], w[9], w[11], w[12], w[13]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&t970_wifi_rst && t970_wifi_rst[8]) {
    const volatile uint32_t *r = t970_wifi_rst;
    char row[200];
    snprintf(row, sizeof row, "[t970 wifi rst] ltssm %x hotrst %x l1ss %x cookie %x warm %x cause %x greset %x mhistatus %x", r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&t970_wifi_chk && (t970_wifi_chk[3] || t970_wifi_chk[4])) {
    const volatile uint32_t *c = t970_wifi_chk;
    char row[320];
    snprintf(row, sizeof row, "[t970 wifi chk] differ %u first %u still %u of %u | bhi %x err %x dbg %x %x %x at %ums | gfsr %x>%x syn %x %x %x | soc %x smr %x fsr %x", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15], c[16], c[17]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&t970_wifi_msi && (t970_wifi_msi[3] || t970_wifi_msi[0])) {
    const volatile uint32_t *q = t970_wifi_msi;
    char row[200];
    snprintf(row, sizeof row, "[t970 wifi msi] lnkctl %x msi %x addr %x set/mair %x | sink %x %x %x %x nonzero %u first %u", q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], q[9]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&t970_wifi_stack && t970_wifi_stack[0]) {   // stage 6 (T970WifiStack.cpp): [0] checkpoint, layout there
    const volatile uint32_t *q = t970_wifi_stack;
    char row[220];
    snprintf(row, sizeof row, "[t970 wifi stack] cp %x nw %x cb/smr/asid %x mib/mair %x dma %uK peak %uK fail %u msi %u vec %x att %d frames %u err %d fsr %x far %x wait %u rounds %u",
             q[0], q[1], q[2], q[3], q[4], q[5], q[6], q[7], q[8], (int)q[9], q[10], (int)q[11], q[12], q[13], q[14], q[15]);
    at = put(out, at, cap, row, sizeof row);
    // the driver's own messages (ath11k, MHI, QMI via T970WifiStack's pLog): sheng's block below prints them only under its
    // native_wifi_stats, which stays 0 here. Per slot: the stack's counters, then up to 40 new notices (level <= 6) and up to 20
    // new lines of the full ring.
    if (&nw_wifi_stats && &native_wifi_text_n) {
      snprintf(row, sizeof row, "[wifi-nw] stage=%u scans=%u frames=%u beacons=%u networks=%u err=%d lines=%u notes=%u", nw_wifi_stats.stage,
               nw_wifi_stats.scans, nw_wifi_stats.frames, nw_wifi_stats.beacons, nw_wifi_stats.bss_seen, nw_wifi_stats.last_err,
               native_wifi_text_n, &native_wifi_note_n ? native_wifi_note_n : 0);
      at = put(out, at, cap, row, sizeof row);
      if (&nw_wifi_where) {
        snprintf(row, sizeof row, "[wifi-where] kind=%llu fn=%llx depth=%llu ms=%llu", (unsigned long long)nw_wifi_where[0],
                 (unsigned long long)nw_wifi_where[1], (unsigned long long)nw_wifi_where[2], (unsigned long long)nw_wifi_where[3]);
        at = put(out, at, cap, row, sizeof row);
      }
    }
    if (&native_wifi_note && &native_wifi_note_n) {
      static uint32_t noted6;
      const uint32_t m = native_wifi_note_n;
      uint32_t j = noted6;
      if (m - j > 128) j = m - 128;
      for (uint32_t k = 0; j < m && k < 40; ++j, ++k) {
        char line[124];
        snprintf(line, sizeof line, "[wifi-msg] %.110s", native_wifi_note[j % 128]);
        at = put(out, at, cap, line, sizeof line);
      }
      noted6 = j;
    }
    if (&native_wifi_text && &native_wifi_text_n) {
      static uint32_t shown6;
      const uint32_t n = native_wifi_text_n;
      uint32_t i = shown6;
      if (n - i > 20) i = n - 20;   // only the newest 20 of the full ring: the notices above carry the rest
      for (; i < n; ++i) {
        char line[124];
        snprintf(line, sizeof line, "[wifi-log] %.110s", native_wifi_text[i % 512]);
        at = put(out, at, cap, line, sizeof line);
      }
      shown6 = i;
    }
  }
  // [wifi-wr] first: index, then offset=value@microseconds, 6 per row (c<size>:<offset> for config writes); new ones only, 16 rows per slot
  if (&nw_wifi_wr_n && &nw_wifi_wr && nw_wifi_wr_n) {
    static uint32_t wrShown;
    const uint32_t n = nw_wifi_wr_n < 512 ? nw_wifi_wr_n : 512;
    for (unsigned rows = 0; wrShown < n && rows < 16; ++rows) {
      char row[240];
      int o = snprintf(row, sizeof row, "[wifi-wr] %u:", wrShown);
      for (unsigned k = 0; k < 6 && wrShown < n && o < (int)sizeof row - 38; ++k, ++wrShown) {
        const uint32_t off = nw_wifi_wr[wrShown][0], v = nw_wifi_wr[wrShown][1], us = nw_wifi_wr[wrShown][2];
        o += (off & 0x80000000u) ? snprintf(row + o, sizeof row - o, " c%u:%x=%x@%u", (off >> 24) & 15, off & 0xffffff, v, us)
                                 : snprintf(row + o, sizeof row - o, " %x=%x@%u", off, v, us);
      }
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&t970_wifi_pbl && t970_wifi_pbl[25]) {
    const volatile uint32_t *q = t970_wifi_pbl;
    char row[300];
    int n = snprintf(row, sizeof row, "[t970 wifi pbl] log %x cfg %x boot %x dbg2 %x qfprom %x | sram", q[0], q[1], q[2], q[3], q[4]);
    for (unsigned k = 5; k < 25 && n > 0 && (size_t)n < sizeof row; ++k) n += snprintf(row + n, sizeof row - n, " %x", q[k]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&t970_wifi_trace && t970_wifi_trace[26]) {
    char row[300];
    int n = snprintf(row, sizeof row, "[t970 wifi trace]");
    for (unsigned k = 0; k < t970_wifi_trace[26] && k < 26 && n > 0 && (size_t)n < sizeof row; ++k) n += snprintf(row + n, sizeof row - n, " %x", t970_wifi_trace[k]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&t970_wifi_mhi && t970_wifi_mhi[0]) {
    const volatile uint32_t *h = t970_wifi_mhi;
    char row[200];
    snprintf(row, sizeof row, "[t970 wifi mhi] step %d smr %x fw %u wait %us pa %x | boot %x ee %u state %u bhi %x err %x %ums status %x fwerr %x",
             (int)h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[11], h[12]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&t970_qud_retry && t970_qud_retry[0]) {
    const volatile int32_t *r = t970_qud_retry;
    char row[260];
    snprintf(row, sizeof row, "[qud retry] step %d rails %x result %d polls %d cpu %d | dsts %x dctl %x devten %x", (int)r[0], (unsigned)r[1], (int)r[2],
             (int)r[3], (int)r[4], (unsigned)r[5], (unsigned)r[6], (unsigned)r[7]);
    at = put(out, at, cap, row, sizeof row);
    if (&t970_qud_faults && (t970_qud_faults[0] | t970_qud_faults[7] | t970_qud_faults[5])) {
      const volatile uint32_t *f = t970_qud_faults;
      snprintf(row, sizeof row, "[qud ev] adr %x:%08x siz %x count %x gsts %x | gfsr %x gfsynr %x cb %x fsr %x fsynr %x far %x:%08x sctlr %x iova %x smr %x s2cr %x",
               f[1], f[0], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[11], f[10], f[12], f[13], f[14], f[15]);
      at = put(out, at, cap, row, sizeof row);
    }
    if (&t970_muic && t970_muic[0]) {
      const volatile int32_t *u = t970_muic;
      snprintf(row, sizeof row, "[t970 muic] step %d rev %x status %06x ctrl1 %x -> %x nacks %d cc %x int %x", (int)u[0], (unsigned)u[1],
               (unsigned)u[2], (unsigned)u[3], (unsigned)u[4], (int)u[5], (unsigned)u[6], (unsigned)u[7]);
      at = put(out, at, cap, row, sizeof row);
    }
    if (&t970_qud_phy && t970_qud_phy[0]) {
      const volatile int32_t *h = t970_qud_phy;
      snprintf(row, sizeof row, "[qud phy] step %x cfg0 %x ctrl0 %x ctrl5 %x com0 %x com1 %x com2 %x ctrl1 %x ctrl2 %x | calls %d res %x core %d qs %x hs %x ss %x", (unsigned)h[0],
               (unsigned)h[1], (unsigned)h[2], (unsigned)h[3], (unsigned)h[4], (unsigned)h[5], (unsigned)h[6], (unsigned)h[7], (unsigned)h[8], (int)h[9],
               (unsigned)h[10], (int)h[11], (unsigned)h[12], (unsigned)h[13], (unsigned)h[14]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&t970_qud_inherited && t970_qud_inherited[0]) {
    const volatile uint32_t *q = t970_qud_inherited;
    char row[200];
    for (unsigned r = 0; r < 2 && (q[r * 12] >> 16); ++r) {
      snprintf(row, sizeof row, "[qud in%u] res %u resumed %x cbcr %x %x %x %x %x bcr %x gdsc %x cfg %x gctl %x dctl %x dsts %x", r, q[r * 12] & 0xff,
               (q[r * 12] >> 8) & 0xff, q[r * 12 + 1], q[r * 12 + 2], q[r * 12 + 3], q[r * 12 + 4], q[r * 12 + 5], q[r * 12 + 6], q[r * 12 + 7],
               q[r * 12 + 8], q[r * 12 + 9], q[r * 12 + 10], q[r * 12 + 11]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&qcom_usb_debug_get_status) {
    static uint32_t st[128];
    char row[180];
    memset(st, 0, sizeof st);
    // get_status answers only on the boot CPU; peek reads the words from any CPU (the old row was all zeros for that reason)
    if (&qcom_usb_debug_peek_status) qcom_usb_debug_peek_status(st); else qcom_usb_debug_get_status(st);
    snprintf(row, sizeof row, "[qud] phase %u error %u core %d qcom %u restore %u dma %u hw %x trace %u/%x polls %u ev %u iofail %u | diag %u failed %u",
             st[0], st[1], (int)st[2], st[3], st[4], st[5], st[15], st[16], st[17], st[20], st[21], st[22],
             &t970_qud_diag_phase ? t970_qud_diag_phase : 0, &t970_qud_diag_failed ? t970_qud_diag_failed : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  // gts7xlwifi: the boot-time CPU write/read test of DRAM at or above 0x200000000 (t970-dram-holes.patch, t970-memtest=1).
  if (&t970_memtest_result && t970_memtest_result[5]) {
    char row[120];
    snprintf(row, sizeof row, "[memtest] %llx..%llx pages %llu bad %llu first %llx", t970_memtest_result[0], t970_memtest_result[1],
             t970_memtest_result[2], t970_memtest_result[3], t970_memtest_result[4]);
    at = put(out, at, cap, row, sizeof row);
  }
  // gts7xlwifi: GPU mappings refused because a page lay outside DRAM (NativeAdrenoVm::map).
  if (&native_adreno_bad_physical) {
    char row[120];
    snprintf(row, sizeof row, "[gpu badpa] refused %u imported %u pa %x000 pages %u", native_adreno_bad_physical[0],
             native_adreno_bad_physical[1], native_adreno_bad_physical[2], native_adreno_bad_physical[3]);
    at = put(out, at, cap, row, sizeof row);
  }
  // gts7xlwifi: the display driver's swap queue (NativeLegacyDisplay): validations, rejections and their last reason, retirements, and
  // rejected swaps dropped from the head of the queue so later frames are not held behind them. rej: the last rejected request's layer
  // masks, its four surface slots (nonzero = present) and surface 0's width, height, pixel format and planes.
  if (&native_legacy_swap_validation_calls) {
    char row[120];
    snprintf(row, sizeof row, "[swap] calls %u ok %u rej %u reason %u retired %u dropped %u", native_legacy_swap_validation_calls,
             &native_legacy_swap_accepted_count ? native_legacy_swap_accepted_count : 0, &native_legacy_swap_rejected_count ? native_legacy_swap_rejected_count : 0,
             &native_legacy_rej_reason ? native_legacy_rej_reason : 0, &native_legacy_retirement_calls ? native_legacy_retirement_calls : 0,
             &native_legacy_swap_drops ? native_legacy_swap_drops : 0);
    at = put(out, at, cap, row, sizeof row);
    if (&native_legacy_rej_state && &native_legacy_rej_surfaces && &native_legacy_rej_surface) {
      snprintf(row, sizeof row, "[swap rej] st %x/%x/%x s %d%d%d%d surf %llux%llu fmt %llx pl %llu row %llu alloc %llu mem %llu",
               native_legacy_rej_state[0], native_legacy_rej_state[1], native_legacy_rej_state[2], !!native_legacy_rej_surfaces[0],
               !!native_legacy_rej_surfaces[1], !!native_legacy_rej_surfaces[2], !!native_legacy_rej_surfaces[3], native_legacy_rej_surface[0],
               native_legacy_rej_surface[1], native_legacy_rej_surface[2], native_legacy_rej_surface[3], native_legacy_rej_surface[4],
               native_legacy_rej_surface[5], native_legacy_rej_surface[6]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // gts7xlwifi: frames handed to the compositor's GPU copy (NativeGOPPresenter): abandoned counts the ones that timed out uncompleted.
  if (&native_gop_gpu_offers) {
    char row[120];
    snprintf(row, sizeof row, "[gpu frame] offered %llu done %llu fail %llu abandon %llu", native_gop_gpu_offers,
             &native_gop_gpu_completions ? native_gop_gpu_completions : 0, &native_gop_gpu_failures ? native_gop_gpu_failures : 0,
             &native_gop_gpu_abandoned ? native_gop_gpu_abandoned : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  // gts7xlwifi: the APSS watchdog (live FDT qcom,msm-watchdog 0x17c10000; msm_watchdog.c: RST 0x4, EN 0x8, STS 0xc, BARK 0x10,
  // BITE 0x14). firmware-t970 clears EN early; this shows whether anything turned it back on before an unexplained reset.
  {
    static volatile uint8_t *wdt;
    char row[120];
    if (!wdt) wdt = (volatile uint8_t *)ml_io_map(0x17c10000, 0x1000);
    if (wdt) {
      snprintf(row, sizeof row, "[wdt] en %x sts %08x bark %08x bite %08x", *(volatile uint32_t *)(wdt + 0x8),
               *(volatile uint32_t *)(wdt + 0xc), *(volatile uint32_t *)(wdt + 0x10), *(volatile uint32_t *)(wdt + 0x14));
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // gts7xlwifi: what the USB controller looks like when XNU runs (for QcomXnuUsbDebug, which needs a powered DEVICE-mode DWC3).
  // GCC (Linux gcc-sm8250.c): usb30_prim_gdsc gdscr 0xf004 (bit 31 power on), usb30_prim_master_clk 0xf010, sleep 0xf018,
  // mock_utmi 0xf01c, usb3_prim_phy_aux 0xf054, com_aux 0xf058, pipe 0xf05c (bit 31 clock off). The DWC3 core (0xa600000) is read
  // only when the GDSC and the master clock are on, since an unclocked read can hang the bus: GCTL 0xc110, GSTS 0xc118,
  // DCFG 0xc700, DCTL 0xc704, DSTS 0xc70c.
  {
    static volatile uint8_t *gcc, *dwc;
    char row[200];
    if (!gcc) gcc = (volatile uint8_t *)ml_io_map(0x10f000, 0x1000);
    if (gcc) {
      uint32_t g[7];
      static const unsigned off[7] = {0x004, 0x010, 0x018, 0x01c, 0x054, 0x058, 0x05c};
      for (unsigned i = 0; i < 7; ++i) g[i] = *(volatile uint32_t *)(gcc + off[i]);
      size_t n = (size_t)snprintf(row, sizeof row, "[usb] gdsc %08x master %08x sleep %08x utmi %08x aux %08x com %08x pipe %08x",
                                  g[0], g[1], g[2], g[3], g[4], g[5], g[6]);
      if ((g[0] & (1u << 31)) && !(g[1] & (1u << 31))) {
        if (!dwc) dwc = (volatile uint8_t *)ml_io_map(0xa600000, 0x10000);
        if (dwc && n < sizeof row)
          snprintf(row + n, sizeof row - n, " | gctl %08x gsts %08x dcfg %08x dctl %08x dsts %08x", *(volatile uint32_t *)(dwc + 0xc110),
                   *(volatile uint32_t *)(dwc + 0xc118), *(volatile uint32_t *)(dwc + 0xc700), *(volatile uint32_t *)(dwc + 0xc704),
                   *(volatile uint32_t *)(dwc + 0xc70c));
      }
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // gts7xlwifi: SoC temperatures, TSENS v2 (live FDT tsens0 tm 0xc263000, tsens1 tm 0xc265000; qcom,tsens24xx): sensor n's last
  // reading at tm + 0xa0 + 4n, bits 11:0 signed tenths of a degree, bit 21 valid. Whole degrees per sensor and the hottest,
  // to tell a thermal reset from the others (nothing throttles the CPU or the GPU on this board).
  {
    static volatile uint8_t *tm[2];
    static const uint64_t tmPhys[2] = {0xc263000, 0xc265000};
    char row[200];
    int hottest = -1000;
    size_t n = (size_t)snprintf(row, sizeof row, "[temp]");
    for (unsigned b = 0; b < 2; ++b) {
      if (!tm[b]) tm[b] = (volatile uint8_t *)ml_io_map(tmPhys[b], 0x1000);
      if (!tm[b]) continue;
      for (unsigned i = 0; i < 16 && n < sizeof row; ++i) {
        const uint32_t v = *(volatile uint32_t *)(tm[b] + 0xa0 + 4 * i);
        if (!(v & (1u << 21))) continue;
        int t = (int)(v & 0xfff); if (t & 0x800) t -= 0x1000;
        if (t / 10 > hottest) hottest = t / 10;
        n += (size_t)snprintf(row + n, sizeof row - n, " %u.%u:%d", b, i, t / 10);
      }
    }
    if (n < sizeof row) snprintf(row + n, sizeof row - n, " max %d", hottest);
    at = put(out, at, cap, row, sizeof row);
  }
  // gts7xlwifi: why the P cluster takes no thread. Per logical core the timer interrupts and IPIs taken (ArmCpu counters), and the GIC
  // distributor's view of the IPI SPIs 1008..1015 (armcpu-spi-ipi=1): IGROUPR/ISENABLER/ISPENDR word 31 (bits 16..23 are
  // 1008..1015; a group bit that reads 0 after ArmCpu set it means the secure side owns the interrupt) and each IROUTER's Aff1.
  if (&t970_core_timer && &t970_core_sgi) {
    char row[200];
    size_t n = (size_t)snprintf(row, sizeof row, "[cpu ipi]");
    for (unsigned c = 0; c < 8 && n < sizeof row; ++c)
      n += (size_t)snprintf(row + n, sizeof row - n, " c%u:t%u/i%u", c, t970_core_timer[c], t970_core_sgi[c]);
    at = put(out, at, cap, row, sizeof row);
    if (&t970_core_phase) {
      n = (size_t)snprintf(row, sizeof row, "[cpu irq] prevlog %u |", boot_log_previous_copied);
      for (unsigned c = 0; c < 8 && n < sizeof row; ++c)
        n += (size_t)snprintf(row + n, sizeof row - n, " c%u:ph%u id%u n%u", c, t970_core_phase[c], t970_core_last_id[c], t970_core_entries[c]);
      at = put(out, at, cap, row, sizeof row);
    }
    static volatile uint8_t *gicd;
    if (!gicd) gicd = (volatile uint8_t *)ml_io_map(0x17a00000, 0x10000);   // live FDT intc: interrupt-controller@17a00000
    if (gicd) {
      // The IPI range ArmCpu uses (armcpu-spi-ipi-base, default 1008): one bit per core 0..7 of group, enable, pending, group modifier.
      static uint32_t base = 0;
      if (!base) {unsigned v = 1008; PE_parse_boot_argn("armcpu-spi-ipi-base", &v, sizeof v); base = v >= 32 && v + 8 <= 1020 ? v : 1008;}
      unsigned bits[4] = {0, 0, 0, 0};
      static const unsigned regs[4] = {0x80, 0x100, 0x200, 0xd00};
      for (unsigned r = 0; r < 4; ++r)
        for (unsigned c = 0; c < 8; ++c) {
          const unsigned id = base + c;
          if ((*(volatile uint32_t *)(gicd + regs[r] + (id / 32) * 4) >> (id % 32)) & 1) bits[r] |= 1u << c;
        }
      n = (size_t)snprintf(row, sizeof row, "[gicd ipi] base %u typer %08x grp %02x en %02x pend %02x grpmod %02x route", base,
                           *(volatile uint32_t *)(gicd + 4), bits[0], bits[1], bits[2], bits[3]);
      for (unsigned c = 0; c < 8 && n < sizeof row; ++c)
        n += (size_t)snprintf(row + n, sizeof row - n, " %llx",
                              (unsigned long long)(*(volatile uint64_t *)(gicd + 0x6000 + 8 * (base + c)) & 0xff00ffffffULL));
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // gts7xlwifi: the A650 probe's rows (package/GpuProbe.cpp, pipa-gpu=<n>), which otherwise only reach the USB console.
  if (&t970_gpu_rows) {
    char row[136];
    snprintf(row, sizeof row, "[gpu step] %u", &t970_gpu_step ? t970_gpu_step : 0);
    at = put(out, at, cap, row, sizeof row);
    for (unsigned r = 0; r < 31; ++r) {   // kRows in GpuProbe.cpp
      if (!t970_gpu_rows[r][0]) continue;
      snprintf(row, sizeof row, "[gpu %u] %.119s", r, t970_gpu_rows[r]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // The HID half: reports handed to handleReport, its last result, the HID stage, event driver count and the split device.
  if (&t970_hid_reports) {
    char row[120];
    snprintf(row, sizeof row, "[t970 hid] reports=%u hres=%d hstage=%u hevdrv=%u tsplit=%u props=%u",
             t970_hid_reports, &t970_hid_results ? t970_hid_results[6] : 99, &t970_hid_stage ? t970_hid_stage : 99,
             &t970_hid_class_sizes ? t970_hid_class_sizes[1] : 0, &t970_hid_touch_status ? t970_hid_touch_status : 99,
             (&t970_hid_props_driver && t970_hid_props_driver) ? 1u : 0u);
    at = put(out, at, cap, row, sizeof row);
  }
  // IBs per GPU submission: submissions, IBs, the most in one, and those with more than 16 (a frame used to be split at 16).
  if (&native_adreno_submit_ibs) {
    char row[120];
    snprintf(row, sizeof row, "[gpu ibs] submissions=%u ibs=%u max=%u over16=%u", native_adreno_submit_ibs[0], native_adreno_submit_ibs[1],
             native_adreno_submit_ibs[2], native_adreno_submit_ibs[3]);
    at = put(out, at, cap, row, sizeof row);
  }
  // The submissions' write-combine work: audits, IB lines compared and found dirty, pages set back to write-combined, the re-assertion's
  // total time, and the CPU cache maintenance passes (count, total ms, MiB).
  if (&native_adreno_wc_audit && &native_adreno_reassert_ticks && &native_adreno_cache_stats) {
    uint64_t f = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (!f) f = 19200000;
    char row[200];
    snprintf(row, sizeof row, "[wc] audited=%u lines=%u dirty=%u reset=%u reassert_ms=%llu | maintenance passes=%llu ms=%llu MiB=%llu",
             native_adreno_wc_audit[0], native_adreno_wc_audit[1], native_adreno_wc_audit[2], native_adreno_wc_audit[5],
             (unsigned long long)(native_adreno_reassert_ticks * 1000 / f), (unsigned long long)native_adreno_cache_stats[3],
             (unsigned long long)(native_adreno_cache_stats[0] * 1000 / f), (unsigned long long)(native_adreno_cache_stats[1] >> 20));
    at = put(out, at, cap, row, sizeof row);
  }
  // CPU clusters and L3 as voted (CpuFreqProbe, refreshed every round) and the GPU's initial HFI perf/bandwidth vote.
  if (&native_cpufreq_row && native_cpufreq_row[0]) {
    char row[200];
    snprintf(row, sizeof row, "[clocks] %.118s | gpu perf %u bw %u", native_cpufreq_row, &native_gpu_perf_vote ? native_gpu_perf_vote[0] : 0,
             &native_gpu_perf_vote ? native_gpu_perf_vote[1] : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_export_stats && (native_adreno_export_stats[0] || native_adreno_export_stats[1])) {
    char row[120];
    snprintf(row, sizeof row, "[export] pieces=%u refused=%u generation=%u bytes=%u | complete generation=%u bytes=%u", native_adreno_export_stats[0],
             native_adreno_export_stats[1], native_adreno_export_stats[2], native_adreno_export_stats[3], export_ready, export_ready_bytes);
    at = put(out, at, cap, row, sizeof row);
  }
  // WiFi bring-up (WifiProbe.cpp): stage, GCC votes, PCIe0 GDSC, halted branches, WLAN_EN/SW_CTRL/PERST# pins, LTSSM, root port ID, link.
  if (&native_wifi_stats && native_wifi_stats[0]) {
    char row[400];
    snprintf(row, sizeof row, "[wifi] st=%d v8=%x v0=%x gdsc=%x off=%x gpio=%06x ltssm=%x id=%08x lnk=%08x phy=%x", (int)native_wifi_stats[0],
             native_wifi_stats[1], native_wifi_stats[2], native_wifi_stats[3], native_wifi_stats[4], native_wifi_stats[5], native_wifi_stats[6],
             native_wifi_stats[7], native_wifi_stats[8], native_wifi_stats[9]);
    if (native_wifi_stats[0] >= 10 || (int)native_wifi_stats[0] <= -10)
      snprintf(row + strlen(row), sizeof row - strlen(row), " pins=%06x pwr=%x pcs=%x dbg0=%x dbg1=%x ep=%08x ms=%u wlan=%08x perst=%08x clkreq=%08x rails=%x rsc=%08x tcs=%x/%x/%x/%x rsctest=%x/%x/%x", native_wifi_stats[10],
               native_wifi_stats[11], native_wifi_stats[12], native_wifi_stats[13], native_wifi_stats[14], native_wifi_stats[15], native_wifi_stats[16],
               native_wifi_stats[17], native_wifi_stats[18], native_wifi_stats[19], native_wifi_stats[20], native_wifi_stats[21], native_wifi_stats[22], native_wifi_stats[23], native_wifi_stats[24], native_wifi_stats[25],
               native_wifi_stats[26], native_wifi_stats[27], native_wifi_stats[28]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_wifi_dump && native_wifi_stats[20])
      for (unsigned s = 0; s < 6; ++s) {
        char dump[200];
        int len = snprintf(dump, sizeof dump, "[wifi-tcs%u]", s);
        for (unsigned w = 0; w < 18; ++w) len += snprintf(dump + len, sizeof dump - (unsigned)len, " %x", native_wifi_dump[s * 18 + w]);
        at = put(out, at, cap, dump, sizeof dump);
      }
    if (&native_wifi_pmic && native_wifi_pmic[0]) {
      char row[260];
      snprintf(row, sizeof row, "[wifi-pmic] arb=%x apids=%u regs=%u/%u gpio3=%x before=%08x.%08x.%08x write=%x after=%08x.%08x.%08x", native_wifi_pmic[0],
               native_wifi_pmic[1], native_wifi_pmic[2] & 0xffff, native_wifi_pmic[2] >> 16, native_wifi_pmic[3], native_wifi_pmic[4], native_wifi_pmic[5], native_wifi_pmic[6],
               native_wifi_pmic[10], native_wifi_pmic[7], native_wifi_pmic[8], native_wifi_pmic[9]);
      at = put(out, at, cap, row, sizeof row);
      snprintf(row, sizeof row, "[wifi-rails] cmd-db addresses ldob15 smpg6 smpg4 smpe4 smpg5 smpg2: %x %x %x %x %x %x", native_wifi_pmic[11], native_wifi_pmic[12],
               native_wifi_pmic[13], native_wifi_pmic[14], native_wifi_pmic[15], native_wifi_pmic[16]);
      at = put(out, at, cap, row, sizeof row);
      snprintf(row, sizeof row, "[wifi-rails] enabled-by-pmic-read=%x looked-at=%x last-wait-ms=%u total-ms=%u", native_wifi_pmic[22] & 0xff, native_wifi_pmic[22] >> 8 & 0xff,
               native_wifi_pmic[22] >> 16, native_wifi_pmic[23]);
      at = put(out, at, cap, row, sizeof row);
      if (&native_wifi_ldo && (native_wifi_ldo[0] || native_wifi_ldo[12])) {
        snprintf(row, sizeof row, "[wifi-1v8] before: %08x/%08x %08x/%08x %08x/%08x %08x/%08x %08x/%08x %08x/%08x", native_wifi_ldo[0], native_wifi_ldo[1], native_wifi_ldo[2], native_wifi_ldo[3],
                 native_wifi_ldo[4], native_wifi_ldo[5], native_wifi_ldo[6], native_wifi_ldo[7], native_wifi_ldo[8], native_wifi_ldo[9], native_wifi_ldo[10], native_wifi_ldo[11]);
        at = put(out, at, cap, row, sizeof row);
        snprintf(row, sizeof row, "[wifi-1v8] after: %08x/%08x %08x/%08x %08x/%08x %08x/%08x %08x/%08x %08x/%08x", native_wifi_ldo[12], native_wifi_ldo[13], native_wifi_ldo[14], native_wifi_ldo[15],
                 native_wifi_ldo[16], native_wifi_ldo[17], native_wifi_ldo[18], native_wifi_ldo[19], native_wifi_ldo[20], native_wifi_ldo[21], native_wifi_ldo[22], native_wifi_ldo[23]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_cfg && (native_wifi_cfg[0] || native_wifi_cfg[8])) {
        snprintf(row, sizeof row, "[wifi-cfg] first(at %u): ep=%08x cmd=%08x bar0=%08x bar1=%08x lnk=%08x dbg0=%08x mem=%08x rcmd=%08x | end: ep=%08x cmd=%08x bar0=%08x bar1=%08x lnk=%08x dbg0=%08x mem=%08x rcmd=%08x restored=%u",
                 native_wifi_cfg[16], native_wifi_cfg[0], native_wifi_cfg[1], native_wifi_cfg[2], native_wifi_cfg[3], native_wifi_cfg[4], native_wifi_cfg[5], native_wifi_cfg[6], native_wifi_cfg[7],
                 native_wifi_cfg[8], native_wifi_cfg[9], native_wifi_cfg[10], native_wifi_cfg[11], native_wifi_cfg[12], native_wifi_cfg[13], native_wifi_cfg[14], native_wifi_cfg[15], native_wifi_cfg[17]);
        at = put(out, at, cap, row, sizeof row);
        snprintf(row, sizeof row, "[wifi-smmu] gfsr=%x gfsynr0=%x cb-fsr=%x far=%x.%x fsynr0=%x sctlr=%x tcr=%x ttbr0=%x cbar=%x", native_wifi_cfg[18], native_wifi_cfg[19], native_wifi_cfg[20], native_wifi_cfg[22], native_wifi_cfg[21], native_wifi_cfg[23], native_wifi_cfg[24], native_wifi_cfg[25], native_wifi_cfg[26], native_wifi_cfg[27]);
        at = put(out, at, cap, row, sizeof row);
        snprintf(row, sizeof row, "[wifi-img] sum32(first 512K)=%u after=%u first-word=%08x size=%u", native_wifi_cfg[28], native_wifi_cfg[30], native_wifi_cfg[29], native_wifi_cfg[31]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_ev) for (unsigned k = 0; k < 2; ++k) {
        volatile uint32_t *e = native_wifi_ev + 24 * k;
        if (!e[8] && !e[0] && !e[12] && !e[13] && !e[18]) continue;
        snprintf(row, sizeof row, "[wifi-ev%u] er0: %08x %08x %08x %08x | %08x %08x %08x %08x er1: %08x %08x %08x %08x ctx rp/wp: %x/%x %x/%x sink: %08x %08x bhie: addrlo=%x hi=%x size=%x db=%x status=%x", k,
                 e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[10], e[11], e[12], e[13], e[14], e[15], e[16], e[17], e[18], e[19], e[20], e[22], e[23]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_chip && native_wifi_chip[0]) {
        snprintf(row, sizeof row, "[wifi-chip] ltssm=%x hotrst=%x cookie=%x warm=%x warm-after=%x cause=%x global-before=%x after=%x mhistatus=%x->%x ee=%x aspm-ctl=%x msi-ctl=%x",
                 native_wifi_chip[0], native_wifi_chip[1], native_wifi_chip[2], native_wifi_chip[3], native_wifi_chip[4], native_wifi_chip[5], native_wifi_chip[6], native_wifi_chip[7],
                 native_wifi_chip[8], native_wifi_chip[9], native_wifi_chip[10], native_wifi_chip[11], native_wifi_chip[12]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_pon && native_wifi_pon[0]) {
        snprintf(row, sizeof row, "[wifi-pon] ppid/owner=%x bytes08..27=%08x %08x %08x %08x %08x %08x %08x %08x sn7=%08x sid=%08x", native_wifi_pon[0], native_wifi_pon[1], native_wifi_pon[2],
                 native_wifi_pon[3], native_wifi_pon[4], native_wifi_pon[5], native_wifi_pon[6], native_wifi_pon[7], native_wifi_pon[8], native_wifi_pon[9], native_wifi_pon[10]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_ep && native_wifi_ep[0]) {
        snprintf(row, sizeof row, "[wifi-ep] st=%u bar0-mask=%08x bar0=%08x cmd=%08x rp-mem=%08x mhi: len=%x ver=%x cfg=%x bhioff=%x bhieoff=%x ctrl=%x status=%x bhi: ver=%x exec=%x st=%x err=%x",
                 native_wifi_ep[0], native_wifi_ep[1], native_wifi_ep[2], native_wifi_ep[3], native_wifi_ep[4], native_wifi_ep[5], native_wifi_ep[6], native_wifi_ep[7],
                 native_wifi_ep[8], native_wifi_ep[9], native_wifi_ep[10], native_wifi_ep[11], native_wifi_ep[12], native_wifi_ep[13], native_wifi_ep[14], native_wifi_ep[15]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_mhi && native_wifi_mhi[0]) {
        snprintf(row, sizeof row, "[wifi-mhi] step=%d smmu=%x part=%x amss=%u segs=%u boot=%x ee=%x state=%x bhi-status=%x bhi-err=%x dbg=%x.%x.%x mhistatus=%x ms=%u bhie=%x",
                 (int)native_wifi_mhi[0], native_wifi_mhi[1], native_wifi_mhi[2], native_wifi_mhi[3], native_wifi_mhi[4], native_wifi_mhi[5], native_wifi_mhi[6], native_wifi_mhi[7],
                 native_wifi_mhi[8], native_wifi_mhi[9], native_wifi_mhi[10], native_wifi_mhi[11], native_wifi_mhi[12], native_wifi_mhi[13], native_wifi_mhi[14], native_wifi_mhi[15]);
        at = put(out, at, cap, row, sizeof row);
        if (&native_wifi_mhi_ms) {
          snprintf(row, sizeof row, "[wifi-mhi-ms] smmu-substep=%u last-smmu-access=%x buffers=%u found=%u read=%u smmu=%u sbl=%u ctx=%u end=%u", native_wifi_mhi[12], native_wifi_mhi_ms[7], native_wifi_mhi_ms[0], native_wifi_mhi_ms[1],
                   native_wifi_mhi_ms[2], native_wifi_mhi_ms[3], native_wifi_mhi_ms[4], native_wifi_mhi_ms[5], native_wifi_mhi_ms[6]);
          at = put(out, at, cap, row, sizeof row);
        }
      }
      if (&native_wifi_text && native_wifi_text_n) {
        if (&nw_wifi_stats) {
          snprintf(row, sizeof row, "[wifi-nw] stage=%u scans=%u frames=%u beacons=%u networks=%u err=%d lines=%u", nw_wifi_stats.stage, nw_wifi_stats.scans, nw_wifi_stats.frames,
                   nw_wifi_stats.beacons, nw_wifi_stats.bss_seen, nw_wifi_stats.last_err, native_wifi_text_n);
          at = put(out, at, cap, row, sizeof row);
          if (&nw_wifi_where) {
            snprintf(row, sizeof row, "[wifi-where] kind=%llu fn=%llx depth=%llu ms=%llu", (unsigned long long)nw_wifi_where[0], (unsigned long long)nw_wifi_where[1],
                     (unsigned long long)nw_wifi_where[2], (unsigned long long)nw_wifi_where[3]);
            at = put(out, at, cap, row, sizeof row);
          }
          uint32_t f[10] = {0};
          if (&native_ufs_extra_fault && native_ufs_extra_fault(f)) {
            snprintf(row, sizeof row, "[wifi-smmu] gfsr=%08x cb-fsr=%08x far=%08x%08x fsynr0=%08x", f[0], f[2], f[4], f[3], f[5]);
            at = put(out, at, cap, row, sizeof row);
          }
        }
        // the lines added since the last slot, at most 100 per slot; the rest wait for the next one while the ring still holds them
        static uint32_t shown;
        const uint32_t n = native_wifi_text_n;
        uint32_t i = shown;
        if (n - i > 512) i = n - 512;
        for (uint32_t k = 0; i < n && k < 100; ++i, ++k) {
          char line[124];
          snprintf(line, sizeof line, "[wifi-log] %.110s", native_wifi_text[i % 512]);
          at = put(out, at, cap, line, sizeof line);
        }
        shown = i;
        // the notices among them, which the debug lines cannot push out of their own ring: at most 40 new ones per slot
        if (&native_wifi_note) {
          static uint32_t noted;
          const uint32_t m = native_wifi_note_n;
          uint32_t j = noted;
          if (m - j > 128) j = m - 128;
          for (uint32_t k = 0; j < m && k < 40; ++j, ++k) {
            char line[124];
            snprintf(line, sizeof line, "[wifi-msg] %.110s", native_wifi_note[j % 128]);
            at = put(out, at, cap, line, sizeof line);
          }
          noted = j;
        }
      }
      snprintf(row, sizeof row, "[wifi-pcie] pipe-halt=%08x perst=%08x dbg0@20ms=%x dbg1@20ms=%x pipe-mux=%x", native_wifi_pmic[17], native_wifi_pmic[18], native_wifi_pmic[19],
               native_wifi_pmic[21], native_wifi_pmic[20]);
      at = put(out, at, cap, row, sizeof row);
      // every candidate regulator peripheral that exists, before and after the votes; v is the 0x41 0x40 pair
      const unsigned nb = native_wifi_pmic[2] & 0xffff, na = native_wifi_pmic[2] >> 16;
      for (unsigned i = 0; i < nb && i < 112; ++i) {
        const uint32_t k0 = native_wifi_pmic[24 + 2 * i], v0 = native_wifi_pmic[25 + 2 * i];
        uint32_t v1 = v0;
        if (i < na && native_wifi_pmic[248 + 2 * i] == k0) v1 = native_wifi_pmic[249 + 2 * i];
        char line[120];
        snprintf(line, sizeof line, "[wifi-reg] ppid=%03x type=%x/%x before st=%02x en=%02x v=%02x%02x after st=%02x en=%02x", k0 >> 16, k0 >> 8 & 0xff, k0 & 0xff,
                 v0 >> 24, v0 >> 16 & 0xff, v0 & 0xff, v0 >> 8 & 0xff, v1 >> 24, v1 >> 16 & 0xff);
        at = put(out, at, cap, line, sizeof line);
      }
    }
  }
  // Display controller state (DisplayTimingProbe.cpp): per interface timing enable, config, counter enable, frame count then one second
  // later, line count, vsync period, status; then every pipe's size/address/stride/format and the CTLs.
  if (&native_dpu_stats && &native_dpu_pipes && native_dpu_stats[0]) {
    char row[200];
    const volatile uint32_t *d = native_dpu_stats, *p = native_dpu_pipes;
    snprintf(row, sizeof row, "[dpu] st=%d hw=%x us=%u i1 en=%x cfg=%x cnt=%x f=%u..%u ln=%u vp=%u s=%x i2 en=%x cfg=%x cnt=%x f=%u..%u ln=%u vp=%u s=%x",
             (int)d[0], d[1], d[18], d[2], d[3], d[4], d[5], d[6], d[7], d[8], d[9], d[10], d[11], d[12], d[13], d[14], d[15], d[16], d[17]);
    at = put(out, at, cap, row, sizeof row);
    for (int half = 0; half < 2; ++half) {
      int n = snprintf(row, sizeof row, "[dpu pipes%d]", half);
      for (int i = half * 5; i < half * 5 + 5 && n < (int)sizeof row; ++i)
        n += snprintf(row + n, sizeof row - n, " %x/%x/%x/%x", p[4 * i], p[4 * i + 1], p[4 * i + 2], p[4 * i + 3]);
      at = put(out, at, cap, row, sizeof row);
    }
    snprintf(row, sizeof row, "[dpu ctl] fb=%x%08x %x/%x/%x %x/%x/%x %x/%x/%x %x/%x/%x", p[41], p[40], p[42], p[43], p[44], p[45], p[46], p[47],
             p[48], p[49], p[50], p[51], p[52], p[53]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Touch noise before the fuzz filter: resting or slow frames, mean step (x + y) and largest single-axis step, in 0.1 px.
  if (&native_touch_hold && native_touch_hold[7]) {
    char row[200];
    snprintf(row, sizeof row, "[touch hold] bridged=%u lifts none=%u far=%u weak=%u palm=%u taken=%u frames t2=%u t4=%u merges=%u peaks=%u full=%u ref=%u",
             native_touch_hold[0], native_touch_hold[1], native_touch_hold[2], native_touch_hold[3], native_touch_hold[4], native_touch_hold[5],
             native_touch_hold[6], native_touch_hold[7], native_touch_hold[10], native_touch_hold[11], native_touch_hold[12], native_touch_hold[13]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_touch_jitter && native_touch_jitter[0]) {
    char row[128];
    snprintf(row, sizeof row, "[touch jitter] frames=%u mean=%u max=%u smoothed mean=%u max=%u (0.1 px)", native_touch_jitter[0],
             native_touch_jitter[1] / native_touch_jitter[0], native_touch_jitter[2], &native_touch_hold ? native_touch_hold[8] / native_touch_jitter[0] : 0,
             &native_touch_hold ? native_touch_hold[9] : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  // Distinct path lookups that failed for MobileSafari (S) and lsd (L): errno and path, in the order they first failed.
  if (&native_lookup_count && native_lookup_count) {
    char row[128];
    const unsigned n = native_lookup_count < 160 ? native_lookup_count : 160;
    snprintf(row, sizeof row, "[safari lookups] failed=%u", native_lookup_count);
    at = put(out, at, cap, row, sizeof row);
    for (unsigned i = 0; i < n; ++i) {
      snprintf(row, sizeof row, "SL%u %s", i, native_lookup_log[i]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // Audio DSP bring-up (AdspProbe.cpp): stage and error, IPCC interrupts, the DSP's slave-kernel bits, SMEM items, AOSS mailbox, RSC id,
  // adsp.mbn KiB, ms to ready, GLINK messages in/out, channels up, opens nobody serves, last SCM result, rail votes; then its note.
  if (&native_adsp_stats && native_adsp_stats[0]) {
    char row[200];
    snprintf(row, sizeof row, "[adsp] st=%d err=%d irq=%d slave=%x items=%x qmp=%x rsc=%x fw=%dK rdy=%dms rx=%d tx=%d ch=%x unk=%d scm=%d/%x votes=%x",
             native_adsp_stats[0], native_adsp_stats[1], native_adsp_stats[2], native_adsp_stats[3], native_adsp_stats[4], native_adsp_stats[5],
             native_adsp_stats[6], native_adsp_stats[7], native_adsp_stats[8], native_adsp_stats[9], native_adsp_stats[10], native_adsp_stats[11],
             native_adsp_stats[12], native_adsp_stats[13], native_adsp_stats[14], native_adsp_stats[15]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_adsp_text && native_adsp_text[0]) {
      snprintf(row, sizeof row, "[adsp] %.95s", native_adsp_text);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // Audio stage A2 (T970Audio.cpp, t970-audio=1): stage, error, APR in/out, last opcode, core state, framework version, ms to
  // channel up / to the tone, COPP, map handle, periods sent/returned/failed, stray answers; then the DSP command statuses (99 not
  // sent), the SMMU stream route, the four amplifiers (DEVID, REVID|OTPID<<8|step<<16, IRQ1_STATUS1 after power-up, IRQ1_STATUS3
  // while playing) and the I2C counters.
  if (&t970_audio_stats && t970_audio_stats[0]) {
    const volatile int32_t *s = t970_audio_stats;
    char row[240];
    snprintf(row, sizeof row, "[audio] st=%d err=%d rx=%d tx=%d op=%x core=%d fwk=%x/%x up=%dms tone=%dms copp=%d map=%x per=%d/%d bad=%d stray=%d",
             s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], s[8], s[9], s[10], s[11], s[12], s[13], s[14], s[15]);
    at = put(out, at, cap, row, sizeof row);
    if (&t970_audio_cmd) {
      const volatile int32_t *c = t970_audio_cmd;
      snprintf(row, sizeof row, "[audio cmd] vote=%d/%d clk=%d grp=%d/%d tdm=%d slot=%d start=%d adm=%d mtx=%d asm=%d fmt=%d map=%d run=%d close=%d stop=%d",
               c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8], c[9], c[10], c[11], c[12], c[13], c[14], c[15]);
      at = put(out, at, cap, row, sizeof row);
    }
    if (&t970_audio_dma && &t970_audio_amp && &t970_audio_i2c) {
      const volatile uint32_t *d = t970_audio_dma;
      snprintf(row, sizeof row, "[audio dma] route=%x cr0=%x id1=%x smr=%x s2cr=%x cbar=%x sctlr=%x fsr=%x i2c nack=%u scl=%u fail=%u",
               d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], t970_audio_i2c[0], t970_audio_i2c[1], t970_audio_i2c[2]);
      at = put(out, at, cap, row, sizeof row);
      int n = snprintf(row, sizeof row, "[audio amp]");
      for (int i = 0; i < 4 && n < (int)sizeof row; ++i)
        n += snprintf(row + n, sizeof row - n, " %x/%x/%x/%x", t970_audio_amp[i][0], t970_audio_amp[i][1], t970_audio_amp[i][2],
                      t970_audio_amp[i][3]);
      at = put(out, at, cap, row, sizeof row);
    }
    // Every DSP command in order as opcode:status (status byte; c4 no answer, c3 not sent), two rows of 12; then the amplifiers'
    // IRQ1_STATUS1/IRQ1_STATUS3 right after the AFE port start and 100 ms later.
    if (&t970_audio_trace && t970_audio_trace[0]) {
      const unsigned count = t970_audio_trace[0] < 24 ? t970_audio_trace[0] : 24;
      for (unsigned half = 0; half * 12 < count; ++half) {
        int n = snprintf(row, sizeof row, "[audio trace%u]", half);
        for (unsigned i = half * 12; i < count && i < half * 12 + 12 && n < (int)sizeof row; ++i)
          n += snprintf(row + n, sizeof row - n, " %x:%x", t970_audio_trace[1 + i] >> 8, t970_audio_trace[1 + i] & 0xff);
        at = put(out, at, cap, row, sizeof row);
      }
    }
    // Device mode (t970-audio bit 32, st=20 serving): the service (published, opens, closes, requests, refused, memory maps), the
    // stream (starts/stops/failed starts, periods sent/returned/failed, late, lead, peak, resyncs, settled lead, last failed start)
    // and the plug-in (loads, unknown properties, volume x1000, volume sets, mix peak, writes, frames, gaps, overlaps, host clock Hz
    // and result, IO thread priority; host clock step/value from the service).
    if (&t970_audio_stream && &t970_audio_hal && &t970_audio_service && (t970_audio_service[0] || t970_audio_stream[0])) {
      const volatile uint32_t *m = t970_audio_stream, *h = t970_audio_hal;
      const volatile int32_t *v = t970_audio_service;
      snprintf(row, sizeof row, "[audio svc] pub=%d open=%d close=%d req=%d refused=%d maps=%d start=%u/%u/%u per=%u/%u/%u late=%u lead=%u peak=%u sync=%u base=%d fail=%x",
               v[0], v[1], v[2], v[3], v[4], v[5], m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], (int)m[10], m[11]);
      at = put(out, at, cap, row, sizeof row);
      snprintf(row, sizeof row, "[audio lvl] near-clip=%u run-peak=%u status-reads=%u drift-max=%u (amp IRQ1_STATUS1..4 in [audio clk])", m[12], m[13], m[14], m[15]);
      at = put(out, at, cap, row, sizeof row);
      if (&t970_audio_timing) {
        const volatile int32_t *t = t970_audio_timing;
        snprintf(row, sizeof row, "[audio timing] ahead=%d..%d notyet=%d lost-starts=%d runs=%d settled=%d test=%d resyncs=%u holes=%d",
                 t[0], t[1], t[2], t[3], t[4], t[5], t[6], m[9], t[7]);
        at = put(out, at, cap, row, sizeof row);
        // The plug-in's volume: last set kind (1 scalar, 2 dB) / raw x1000 / element, applied gain x10000 now, least..most during IO,
        // sets, then the last 8 sets as kind:raw x1000 (oldest slot first in ring order).
        if (&t970_audio_vol) {
          const volatile int32_t *q = t970_audio_vol;
          int n = snprintf(row, sizeof row, "[audio vol] last=%d:%d el=%d gain=%d (%d..%d) sets=%d amp=%x hist", q[0], q[1], q[2], q[3], q[4], q[5], q[6], (unsigned)q[7]);
          for (int k = 0; k < 8 && n < (int)sizeof row; ++k)
            if (q[8 + k]) n += snprintf(row + n, sizeof row - n, " %u:%d", (unsigned)q[8 + k] >> 30, (int)((unsigned)q[8 + k] & 0x3fffffff) - (1 << 29));
          at = put(out, at, cap, row, sizeof row);
        }
        // Every sample copied to the DSP: peak per slot FL/FR/RL/RR, big neighbour steps per slot, XOR of the words, frames, nonzero frames.
        if (&t970_audio_bytes) {
          const volatile uint32_t *b = t970_audio_bytes;
          snprintf(row, sizeof row, "[audio bytes] peak=%u/%u/%u/%u jumps=%u/%u/%u/%u xor=%08x frames=%u nonzero=%u tw=%08x",
                   b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11]);
          at = put(out, at, cap, row, sizeof row);
        }
        // Amplifier 0x41's boost registers as found (VCTRL1 VCTRL2 COEFF SLOPE PEAK CLASSH WKFET NG), then VCTRL1 VCTRL2 PWR_CTRL2 as set.
        if (&t970_audio_bst) {
          const volatile uint32_t *s = t970_audio_bst;
          snprintf(row, sizeof row, "[audio bst] %x %x %x %x %x %x %x %x set %x %x %x", s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
                   s[8], s[9], s[10]);
          at = put(out, at, cap, row, sizeof row);
        }
        // The A/B cycle (t970-audio bit 524288; 1048576 the format cycle, seg 0x11..0x16): segment now, changes, ms since the cycle
        // began at the last change, failures.
        if (&t970_audio_ab && t970_audio_ab[1]) {
          snprintf(row, sizeof row, "[audio ab] seg=%x changes=%u at=%ums fail=%u", t970_audio_ab[0], t970_audio_ab[1], t970_audio_ab[2], t970_audio_ab[3]);
          at = put(out, at, cap, row, sizeof row);
        }
        // The capture (t970-audio bit 262144): 256 frames L,R as signed 16-bit hex, 16 frames per row; P = the plug-in's raw mix,
        // K = the same timeline frames as the service took them from the ring.
        if (&t970_audio_cap && t970_audio_cap[2]) {
          const volatile uint32_t *c = t970_audio_cap;
          snprintf(row, sizeof row, "[audio cap] t=%u write=%u at=%u peak=%u k=%u found=%d wrote-ahead=%d hal-ahead=%d..%d", c[0], c[1] >> 16,
                   c[1] & 0xffff, c[2], c[3], (int)c[4], (int)c[5], (int)c[6], (int)c[7]);
          at = put(out, at, cap, row, sizeof row);
          for (unsigned side = 0; side < 2; ++side) {
            const volatile int16_t *v = side ? t970_audio_cap_k : t970_audio_cap_p;
            for (unsigned r = 0; r < 16; ++r) {
              int n = snprintf(row, sizeof row, "%c%02u", side ? 'K' : 'P', r);
              for (unsigned k = 0; k < 32 && n < (int)sizeof row; ++k) n += snprintf(row + n, sizeof row - n, " %04x", (unsigned)(uint16_t)v[32 * r + k]);
              at = put(out, at, cap, row, sizeof row);
            }
          }
        }
      }
      snprintf(row, sizeof row, "[audio hal] mount=%d/%d loads=%u unk=%u vol=%u sets=%u peak=%u writes=%u frames=%u gaps=%u overlap=%u clock=%u/%d pri=%d clkstep=%llu",
               &native_gpu_addon4_stage ? native_gpu_addon4_stage : -99, &native_gpu_addon4_status ? native_gpu_addon4_status : -99,
               h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], (int)h[10], (int)h[11],
               &t970_audio_clock ? (unsigned long long)t970_audio_clock[0] : 99ULL);
      at = put(out, at, cap, row, sizeof row);
    }
    if (&t970_audio_clk) {
      int n = snprintf(row, sizeof row, "[audio clk]");
      for (int i = 0; i < 4 && n < (int)sizeof row; ++i)
        n += snprintf(row + n, sizeof row - n, " %x/%x/%x/%x", t970_audio_clk[i][0], t970_audio_clk[i][1], t970_audio_clk[i][2],
                      t970_audio_clk[i][3]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // Battery power source (ShengBattery.cpp): stage, percent, mV, mA, temperature (raw), flags (present, external, charging, full,
  // notifications on), refresh cycles, answers, timeouts|errors<<16, notifications, USB type, full/design mAh, cycles, dropped|ignored<<16,
  // send refusals.
  if (&t970_battery && t970_battery[0]) {
    const volatile int32_t *b = t970_battery;
    char row[180];
    snprintf(row, sizeof row, "[t970 battery] st %d soc %d.%d%% %dmV %dmA %d.%dC fl %x full %dmAh design %dmAh cyc %d n %d fail %d chg %x kick %d",
             (int)b[0], (int)b[1] / 10, (int)b[1] % 10, (int)b[2], (int)b[3], (int)b[4] / 10, (int)(b[4] < 0 ? -b[4] : b[4]) % 10, (unsigned)b[5],
             (int)b[6], (int)b[7], (int)b[8], (int)b[9], (int)b[10], (unsigned)b[11], (int)b[12]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&t970_bt && t970_bt[0]) {
    const volatile int32_t *b = t970_bt;
    char row[400];
    snprintf(row, sizeof row, "[t970 bt] st %d err %d fwrev %x qup %x cbcr %x pins %x smmu %x pa %x loop %x baud %d prod %x soc %x rom/patch %x "
             "tx %d rx %d cmd %d to %d tlv %x hw %x dma %x lerr %x irq %x/%x/%x en %d hci %x lmp %x mfr %x rails %x reset %x rcg %x t %d",
             (int)b[0], (int)b[1], (unsigned)b[2], (unsigned)b[3], (unsigned)b[4], (unsigned)b[5], (unsigned)b[6], (unsigned)b[7],
             (unsigned)b[8], (int)b[9], (unsigned)b[10], (unsigned)b[11], (unsigned)b[12], (int)b[13], (int)b[14], (int)b[15], (int)b[16],
             (unsigned)b[17], (unsigned)b[18], (unsigned)b[19], (unsigned)b[20], (unsigned)b[21], (unsigned)b[22], (unsigned)b[23],
             (int)b[24], (unsigned)b[25], (unsigned)b[26], (unsigned)b[27], (unsigned)b[28], (unsigned)b[29], (unsigned)b[30], (int)b[31]);
    at = put(out, at, cap, row, sizeof row);
    if (&t970_bt_cp) {
      // last MMIO checkpoint: block (1 SE6, 2 SMMU, 3 GCC, 4 TLMM gpio<<12|reg, 5 QUP0, 6 rail, 7 phase) << 28 | offset
      char cprow[96];
      snprintf(cprow, sizeof cprow, "[t970 bt cp] %x n %u t %u", (unsigned)t970_bt_cp[0], (unsigned)t970_bt_cp[1], (unsigned)t970_bt_cp[2]);
      at = put(out, at, cap, cprow, sizeof cprow);
    }
    if (&t970_bt_text && t970_bt_text[0]) {
      // the start-up lines in 200-byte rows
      char text[204];
      for (unsigned o = 0; o < sizeof t970_bt_text && t970_bt_text[o]; o += 200) {
        unsigned n = 0;
        while (n < 200 && o + n < sizeof t970_bt_text && t970_bt_text[o + n]) n++;
        snprintf(text, sizeof text, "[t970 bt log] %.*s", (int)n, t970_bt_text + o);
        at = put(out, at, cap, text, sizeof text);
      }
    }
  }
  // Sensors (package/T970Sensors.cpp, t970-sensors): SLPI bring-up and QRTR state, the services the hub announced, the latest
  // accelerometer sample (mg in iPadOS axes, then mm/s^2 raw) and the HID device's counters (started, sent, refused, last result).
  if (&t970_sensors && (t970_sensors[0] || (&native_hid_accel_status && native_hid_accel_status))) {
    const volatile int32_t *s = t970_sensors;
    char row[220];
    snprintf(row, sizeof row, "[sensors] st=%d err=%d irq=%d slave=%x items=%x qmp=%d fw=%dK rdy=%dms rx=%d tx=%d ch=%x unk=%d qrtr=%d node=%x sns=%x req=%x",
             (int)s[0], (int)s[1], (int)s[2], (unsigned)s[3], (unsigned)s[4], (int)s[5], (int)s[6], (int)s[7], (int)s[8], (int)s[9],
             (unsigned)s[10], (int)s[11], (int)s[12], (unsigned)s[13], (unsigned)s[14], (unsigned)s[15]);
    at = put(out, at, cap, row, sizeof row);
    const volatile int32_t *a = t970_accel;
    const volatile uint32_t *v = t970_sensors_srv;
    snprintf(row, sizeof row, "[sensors accel] n=%d mg=%d,%d,%d raw=%d,%d,%d st=%x srv=%x %x %x %x %x %x %x %x",
             (int)a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], (int)a[5], (int)a[6], (unsigned)a[7],
             v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    at = put(out, at, cap, row, sizeof row);
    // HID devices: status (1 started, 2 refused) / started / sent / refused / dropped (no event driver yet) / last result, for the
    // accelerometer and the device orientation device; then the orientation state (bit index, changes, sink calls, ms of last change).
    const volatile uint32_t *ha = &native_accel_hid ? native_accel_hid : NULL, *ho = &native_orient_hid ? native_orient_hid : NULL;
    snprintf(row, sizeof row, "[sensors hid] accel=%u/%u/%u/%u/%u/%x orient=%u/%u/%u/%u/%u/%x now=%d changes=%d sent=%d at=%dms",
             &native_hid_accel_status ? native_hid_accel_status : 0, ha ? ha[0] : 0, ha ? ha[1] : 0, ha ? ha[2] : 0, ha ? ha[3] : 0,
             ha ? ha[4] : 0, &native_hid_orient_status ? native_hid_orient_status : 0, ho ? ho[0] : 0, ho ? ho[1] : 0, ho ? ho[2] : 0,
             ho ? ho[3] : 0, ho ? ho[4] : 0, &t970_orient ? (int)t970_orient[0] : 0, &t970_orient ? (int)t970_orient[1] : 0,
             &t970_orient ? (int)t970_orient[2] : 0, &t970_orient ? (int)t970_orient[3] : 0);
    at = put(out, at, cap, row, sizeof row);
    const volatile uint32_t *hg = &native_gyro_hid ? native_gyro_hid : NULL;
    snprintf(row, sizeof row, "[sensors hid] gyro=%u/%u/%u/%u/%u/%x", &native_hid_gyro_status ? native_hid_gyro_status : 0,
             hg ? hg[0] : 0, hg ? hg[1] : 0, hg ? hg[2] : 0, hg ? hg[3] : 0, hg ? hg[4] : 0);
    at = put(out, at, cap, row, sizeof row);
    if (&t970_sensors_text && t970_sensors_text[0]) {
      snprintf(row, sizeof row, "[sensors] %.95s", t970_sensors_text);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&t970_haptics && t970_haptics[0]) {
    const volatile int32_t *h = t970_haptics;
    char row[120];
    snprintf(row, sizeof row, "[t970 haptics] st %d rc %d n %d ms %d fail %d test %d refused %d off %d",
             (int)h[0], (int)h[1], (int)h[2], (int)h[3], (int)h[4], (int)h[5], (int)h[6], (int)h[7]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Camera stage C1 (package/T970Camera.cpp): stage, detail, GDSCR, CCI version, chip ID words front/rear/wide, last CCI IRQ, ms;
  // C2a stream test words front/rear/wide (count1 | count2 << 8 | mode << 16 | rc << 24).
  if (&t970_camera && t970_camera[0]) {
    const volatile int32_t *c = t970_camera;
    char row[240];
    snprintf(row, sizeof row, "[t970 camera] st %d det %x gdsc %x cci %x front %x rear %x wide %x irq %x ms %d sf %x sr %x sw %x",
             (int)c[0], (unsigned)c[1], (unsigned)c[2], (unsigned)c[3], (unsigned)c[4], (unsigned)c[5], (unsigned)c[6],
             (unsigned)c[7], (int)c[8], (unsigned)c[9], (unsigned)c[10], (unsigned)c[11]);
    at = put(out, at, cap, row, sizeof row);
    if (c[16]) {   // C3a: CSIPHY 3 + CSID-lite0 receive test on the front sensor (t970-camera bit 32)
      snprintf(row, sizeof row, "[t970 camera c3] st %d fmt %x out %x len %x rate %d pll %x rx %x pkts %d ecc %x crc %d long %x short %x csid %x cfg %x ref %x phy %x",
               (int)c[16], (unsigned)c[12], (unsigned)c[13], (unsigned)c[14], (int)c[15], (unsigned)c[17], (unsigned)c[18], (int)c[19],
               (unsigned)c[20], (int)c[21], (unsigned)c[22], (unsigned)c[23], (unsigned)c[24], (unsigned)c[25], (unsigned)c[26],
               (unsigned)c[27]);
      at = put(out, at, cap, row, sizeof row);
    }
    if (c[28]) {   // C4: one frame through CSID-lite0 RDI0 and IFE-lite0 into the carve-out (t970-camera bit 64)
      snprintf(row, sizeof row, "[t970 camera c4] st %d route %x ife %x top|bus %x rdi %x measure %x written %d stats %x ms %d pa %x000 stride %d fault %x",
               (int)c[28], (unsigned)c[29], (unsigned)c[30], (unsigned)c[31], (unsigned)c[32], (unsigned)c[33], (int)c[34],
               (unsigned)c[35], (int)c[36], (unsigned)c[37], (int)c[38], (unsigned)c[39]);
      at = put(out, at, cap, row, sizeof row);
    }
    if (c[28] >= 6 && &t970_camera_thumb) {   // the 64x48 thumbnail, one row of 128 hex digits per line (tools/camera-thumb.py)
      static const char hex[] = "0123456789abcdef";
      for (unsigned ty = 0; ty < 48; ++ty) {
        int n = snprintf(row, sizeof row, "[t970 camera thumb %02u] ", ty);
        for (unsigned tx = 0; tx < 64 && n + 2 < (int)sizeof row; ++tx) {
          const uint8_t v = t970_camera_thumb[ty * 64 + tx];
          row[n++] = hex[v >> 4]; row[n++] = hex[v & 15];
        }
        row[n] = 0;
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  if (&native_battery_stats && native_battery_stats[0]) {
    char row[200];
    snprintf(row, sizeof row, "[battery] st=%d pct=%d mv=%d ma=%d temp=%d fl=%x cyc=%d ans=%d to=%x ntf=%d usb=%d full=%d design=%d cycles=%d drop=%x refuse=%d",
             native_battery_stats[0], native_battery_stats[1], native_battery_stats[2], native_battery_stats[3], native_battery_stats[4],
             native_battery_stats[5], native_battery_stats[6], native_battery_stats[7], native_battery_stats[8], native_battery_stats[9],
             native_battery_stats[10], native_battery_stats[11], native_battery_stats[12], native_battery_stats[13], native_battery_stats[14],
             native_battery_stats[15]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Audio framework readiness over the packet router channel (AudioProbe.cpp): stage (5 ready), queries, answers, last status, refusals,
  // timeouts, ignored packets, ms from channel up to ready, channel ups/downs, last opcode, ring drops, send refusals, mode.
  if (&native_audio_stats && native_audio_stats[0]) {
    char row[200];
    snprintf(row, sizeof row, "[audio] st=%d q=%d ans=%d status=%d refused=%d to=%d ign=%d rdy=%dms up=%d down=%d op=%x drop=%d sendfail=%d mode=%d",
             native_audio_stats[0], native_audio_stats[1], native_audio_stats[2], native_audio_stats[3], native_audio_stats[4], native_audio_stats[5],
             native_audio_stats[6], native_audio_stats[7], native_audio_stats[8], native_audio_stats[9], native_audio_stats[10], native_audio_stats[11],
             native_audio_stats[12], native_audio_stats[13]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Touch strokes in panel pixels as the digitizer published them (y mirrored, 0 = the edge the UI calls top): where each finger came
  // down and lifted, how long and over how many frames, and the extent of every contact so far (edge gestures need the outer pixels).
  if (&native_touch_stroke_count && &native_touch_strokes && &native_touch_extent && native_touch_stroke_count) {
    char row[160];
    const uint32_t n = native_touch_stroke_count;
    snprintf(row, sizeof row, "[touch] strokes=%u extent x %d..%d y %d..%d (px of 3048x2032)", n, native_touch_extent[0] / 10,
             native_touch_extent[1] / 10, native_touch_extent[2] / 10, native_touch_extent[3] / 10);
    at = put(out, at, cap, row, sizeof row);
    for (uint32_t k = n > 16 ? n - 16 : 0; k < n; ++k) {
      const volatile int32_t *r = native_touch_strokes[k & 15];
      snprintf(row, sizeof row, "TS%u t=%u.%03us %ums (%d,%d)->(%d,%d) f%d", k, (uint32_t)r[0] / 1000, (uint32_t)r[0] % 1000, (uint32_t)r[1],
               r[2] / 10, r[3] / 10, r[4] / 10, r[5] / 10, r[6]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&native_swipe_stats && native_swipe_stats[3]) {
    char row[200];
    snprintf(row, sizeof row, "[swipe] done=%u first=%u.%03us last=%u.%03us frames=%u tap=%u.%03us home=%u.%03us library=%u.%03us..%u.%03us", native_swipe_stats[0],
             native_swipe_stats[1] / 1000, native_swipe_stats[1] % 1000, native_swipe_stats[2] / 1000, native_swipe_stats[2] % 1000,
             native_swipe_stats[3], native_swipe_stats[4] / 1000, native_swipe_stats[4] % 1000, native_swipe_stats[5] / 1000,
             native_swipe_stats[5] % 1000, native_swipe_stats[6] / 1000, native_swipe_stats[6] % 1000, native_swipe_stats[7] / 1000,
             native_swipe_stats[7] % 1000);
    at = put(out, at, cap, row, sizeof row);
    snprintf(row, sizeof row, "[swipe2] heavy=%u.%03us controlcenter..%u.%03us notification..%u.%03us apps..%u.%03us", native_swipe_stats[8] / 1000,
             native_swipe_stats[8] % 1000, native_swipe_stats[9] / 1000, native_swipe_stats[9] % 1000, native_swipe_stats[10] / 1000,
             native_swipe_stats[10] % 1000, native_swipe_stats[11] / 1000, native_swipe_stats[11] % 1000);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_map_fail && native_adreno_map_fail[0][1]) {
    char row[170];
    int n = snprintf(row, sizeof row, "[map fails]");
    for (unsigned k = 0; k < 5 && native_adreno_map_fail[k][1]; ++k) n += snprintf(row + n, sizeof row - n, " %x x%u", native_adreno_map_fail[k][0], native_adreno_map_fail[k][1]);
    snprintf(row + n, sizeof row - n, " | at last: spans=%u objects=%u iova_pfn=%x handle=%x", native_adreno_map_fail[5][0], native_adreno_map_fail[5][1], native_adreno_map_fail[6][0], native_adreno_map_fail[6][1]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_client_stats) {
    // per client slot: live objects/peak, refused BoAllocs, last refusal status and bytes, highest mapped end in the window (MiB)
    char row[200];
    int n = snprintf(row, sizeof row, "[gpu objects]");
    for (unsigned k = 0; k < 5; ++k)
      if (native_adreno_client_stats[k][1] || native_adreno_client_stats[k][2])
        n += snprintf(row + n, sizeof row - n, " s%u %u/%u r%u %x/%u w%uM", k, native_adreno_client_stats[k][0], native_adreno_client_stats[k][1],
                      native_adreno_client_stats[k][2], native_adreno_client_stats[k][3], native_adreno_client_stats[k][4], native_adreno_client_stats[k][5]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_table_switches && &native_adreno_cp_aperture && &native_adreno_table_pages && &native_adreno_close_step) {
    // Per-process tables: switches by CP/CPU, VMs made/freed/live/peak, last error and createVm step; aperture, CP check, table mode;
    // table pages live/taken/freed/refused; the last clientClose step and its pid.
    char row[200];
    snprintf(row, sizeof row, "[gpu tables] sw %u/%u vm %u/%u/%u/%u err %x at %x | ap %x/%x chk %x mode %u | pages %u/%u/%u/%u | close %u p%u",
             native_adreno_table_switches[0], native_adreno_table_switches[1], native_adreno_table_switches[2], native_adreno_table_switches[3],
             native_adreno_table_switches[4], native_adreno_table_switches[5], native_adreno_table_switches[6], native_adreno_table_switches[7],
             native_adreno_cp_aperture[0], native_adreno_cp_aperture[1], native_adreno_cp_aperture[2], native_adreno_cp_aperture[3],
             native_adreno_table_pages[0], native_adreno_table_pages[1], native_adreno_table_pages[2], native_adreno_table_pages[3],
             native_adreno_close_step[0], native_adreno_close_step[1]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_t970_ttbr && native_t970_ttbr[10]) {
    // The bank after the last CPU table switch: TTBR0 asked/read, TTBR1, TCR, TCR2, SCTLR, FSR, switch count, kit status.
    char row[200];
    snprintf(row, sizeof row, "[gpu ttbr] ask %x%08x read %x%08x t1 %x%08x tcr %x tcr2 %x sctlr %x fsr %x n %u st %x",
             native_t970_ttbr[1], native_t970_ttbr[0], native_t970_ttbr[3], native_t970_ttbr[2], native_t970_ttbr[5], native_t970_ttbr[4],
             native_t970_ttbr[6], native_t970_ttbr[7], native_t970_ttbr[8], native_t970_ttbr[9], native_t970_ttbr[10], native_t970_ttbr[11]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_cp_image && (native_adreno_cp_image[16] || native_adreno_cp_image[17] || native_adreno_cp_image[18])) {
    char row[200];
    snprintf(row, sizeof row, "[gpu probe] kit %x/%x before tcr %x ttbr1 %x%08x fsr %x after tcr %x fsr %x far %x%08x syn %x clr %x",
             native_adreno_cp_image[16], native_adreno_cp_image[17], native_adreno_cp_image[1], native_adreno_cp_image[5], native_adreno_cp_image[4],
             native_adreno_cp_image[7], native_adreno_cp_image[9], native_adreno_cp_image[15], native_adreno_cp_image[19], native_adreno_cp_image[18],
             native_adreno_cp_image[20], native_adreno_cp_image[21]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_fence_wait && native_adreno_fence_wait[0]) {
    char row[160];
    snprintf(row, sizeof row, "[fence wait] calls=%u timeouts=%u last timed-out fence=%u gpu wrote=%u submitted=%u completed=%u no-job=%u refused fence=%u",
             native_adreno_fence_wait[0], native_adreno_fence_wait[1], native_adreno_fence_wait[2], native_adreno_fence_wait[3],
             native_adreno_fence_wait[4], native_adreno_fence_wait[5], native_adreno_fence_wait[6], native_adreno_fence_wait[7]);
    at = put(out, at, cap, row, sizeof row);
    // The callers' waits (int-147): the polls above run under the connection lock, the pauses between them outside it.
    if (&native_adreno_wait_outer && native_adreno_wait_outer[0]) {
      snprintf(row, sizeof row, "[fence wait] outer waits=%llu us=%llu pauses outside the lock=%llu longest=%lluus",
               (unsigned long long)native_adreno_wait_outer[0], (unsigned long long)native_adreno_wait_outer[1],
               (unsigned long long)native_adreno_wait_outer[2], (unsigned long long)native_adreno_wait_outer[3]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // The CP's registers once a fence stopped advancing: RBBM status 0-3, interrupt, CP fault, status 1, protect, ring rptr/wptr,
  // IB1 base/rem, IB2 base/rem, BV rptr, BV IB1 base/rem, BV IB2 rem, BV IB2 base.
  if (&native_adreno_unstick && native_adreno_unstick[0]) {
    char row[200];
    // [0] write-pointer retries, [1] fence of the last, [2] translation drops, [4] bus drains with ack [5]/[6], [9..11] stalls that ended after
    // the write pointer / translations / drain stage, [12] the most timeouts one stall took, [13] a wait's timeout in us.
    snprintf(row, sizeof row, "[gpu unstick] kick=%u last=%u tlb=%u drain=%u ack=%x/%x ended: kick=%u tlb=%u drain=%u longest=%u unit=%uus",
             native_adreno_unstick[0], native_adreno_unstick[1], native_adreno_unstick[2], native_adreno_unstick[4],
             native_adreno_unstick[5], native_adreno_unstick[6], native_adreno_unstick[9], native_adreno_unstick[10],
             native_adreno_unstick[11], native_adreno_unstick[12], native_adreno_unstick[13]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_vm_fail && native_adreno_vm_fail[0]) {
    char row[160];
    snprintf(row, sizeof row, "[gpu vmfail] n %u first st %u stage %u asid %u | last st %u stage %u asid %u", native_adreno_vm_fail[0],
             native_adreno_vm_fail[1], native_adreno_vm_fail[2], native_adreno_vm_fail[3], native_adreno_vm_fail[4], native_adreno_vm_fail[5],
             native_adreno_vm_fail[6]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_stall_mem && &native_adreno_map_ring && &native_adreno_unmap_ring && (native_adreno_stall_mem[1] | native_adreno_stall_mem[2])) {
    char row[400];
    const uint64_t far = (uint64_t)native_adreno_stall_mem[2] << 32 | native_adreno_stall_mem[1];
    size_t n = (size_t)snprintf(row, sizeof row, "[gpu far] %llx", (unsigned long long)far);
    if (n < sizeof row) n += gpu_far_near(row + n, sizeof row - n, "map", native_adreno_map_ring, native_adreno_map_ring_count, far >> 12, 4);
    if (n < sizeof row) (void)gpu_far_near(row + n, sizeof row - n, "unmap", native_adreno_unmap_ring, native_adreno_unmap_ring_count, far >> 12, 3);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_stall && native_adreno_stall[0]) {
    char row[240];
    int n = snprintf(row, sizeof row, "[gpu stall] fence=%u n=%u", native_adreno_stall[0], native_adreno_stall[1]);
    for (unsigned k = 2; k < 24 && n > 0 && (size_t)n < sizeof row; ++k) n += snprintf(row + n, sizeof row - n, " %x", native_adreno_stall[k]);
    at = put(out, at, cap, row, sizeof row);
    // SMMU FSR, FAR lo/hi, FSYNR0, then for the BR IB1, BV IB1 and BV IB2: the dword index in the IB where the 16 words start
    // (ffff00NN: NN words before the fetch address, the IB start being unknown), then 16 words; deadbeef = not readable.
    if (&native_adreno_stall_mem)
      for (unsigned part = 0; part < 4; ++part) {
        static const char *const names[4] = {"smmu", "br ib1", "bv ib1", "bv ib2"};
        n = snprintf(row, sizeof row, "[gpu stall] %s", names[part]);
        if (part) n += snprintf(row + n, sizeof row - n, " @%x:", native_adreno_stall_mem[4 + 17 * (part - 1)]);
        const unsigned first = part ? 5 + 17 * (part - 1) : 0, count = part ? 16 : 4;
        for (unsigned k = first; k < first + count && n > 0 && (size_t)n < sizeof row; ++k) n += snprintf(row + n, sizeof row - n, " %x", native_adreno_stall_mem[k]);
        at = put(out, at, cap, row, sizeof row);
      }
    // Map handles: next at the stalled job's submission, then under the BR IB1, BV IB1, BV IB2 (>= the first: mapped after it).
    if (&native_adreno_stall_mem) {
      snprintf(row, sizeof row, "[gpu stall] handles job>=%u br=%u bv=%u bv2=%u", native_adreno_stall_mem[55], native_adreno_stall_mem[56],
               native_adreno_stall_mem[57], native_adreno_stall_mem[58]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&native_gop_wait_stats && native_gop_wait_stats[0]) {
    char row[160];
    snprintf(row, sizeof row, "[scanout wait] calls=%llu timeouts=%llu asked=%llums last=%lluus min=%lluus max=%lluus mean=%lluus", native_gop_wait_stats[0],
             native_gop_wait_stats[1], native_gop_wait_stats[2], native_gop_wait_stats[3] / 1000, native_gop_wait_stats[4] / 1000,
             native_gop_wait_stats[5] / 1000, native_gop_wait_stats[1] ? native_gop_wait_stats[6] / native_gop_wait_stats[1] / 1000 : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_view_diag && native_adreno_view_diag[0]) {
    char row[120];
    snprintf(row, sizeof row, "[bo views] calls=%u new=%u cached=%u refused=%u last=%x inside=%u", native_adreno_view_diag[0], native_adreno_view_diag[1],
             native_adreno_view_diag[2], native_adreno_view_diag[3], native_adreno_view_diag[4], native_adreno_view_diag[5]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_import_diag && native_adreno_import_diag[0]) {
    char row[160];
    at = put(out, at, cap, "[import]", 9);
    snprintf(row, sizeof row, "calls=%llu refused=%llu last: st=%llx stage=%llu bytes=%llx addr=%llx | first: st=%llx stage=%llu bytes=%llx addr=%llx",
             native_adreno_import_diag[0], native_adreno_import_diag[1], native_adreno_import_diag[2], native_adreno_import_diag[3],
             native_adreno_import_diag[4], native_adreno_import_diag[5], native_adreno_import_diag[6], native_adreno_import_diag[7],
             native_adreno_import_diag[8], native_adreno_import_diag[9]);
    at = put(out, at, cap, row, sizeof row);
    snprintf(row, sizeof row, "stage_now=%u refused_page pa=%llx offset=%llx", native_adreno_import_stage, native_adreno_import_fail_pa,
             native_adreno_import_fail_offset);
    at = put(out, at, cap, row, sizeof row);
    if (&native_adreno_import_live) {
      snprintf(row, sizeof row, "alive=%llu free-refused=%llu last=%llx persistent=%u plain=%u", native_adreno_import_live[0],
               native_adreno_import_live[1], native_adreno_import_live[2], &native_adreno_import_persistent ? native_adreno_import_persistent[0] : 0,
               &native_adreno_import_persistent ? native_adreno_import_persistent[1] : 0);
      at = put(out, at, cap, row, sizeof row);
      for (unsigned i = 0; i < 8; i += 2) {
        const volatile uint64_t *a = &native_adreno_import_live[3 + i * 4], *b = a + 4;
        snprintf(row, sizeof row, "I p%llu %llx+%llx h%llu s%llu | p%llu %llx+%llx h%llu s%llu", a[1] >> 40, a[0], a[1] & 0xffffffffffull, a[2],
                 a[3], b[1] >> 40, b[0], b[1] & 0xffffffffffull, b[2], b[3]);
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  if (boot_log_reset_probe_text[0]) {
    at = put(out, at, cap, "[reset]", 8);
    at = put(out, at, cap, boot_log_reset_probe_text, sizeof boot_log_reset_probe_text);
    if (boot_log_reset_mark[0]) at = put(out, at, cap, boot_log_reset_mark, sizeof boot_log_reset_mark);
    if (boot_log_reset_watchdog[0]) at = put(out, at, cap, boot_log_reset_watchdog, sizeof boot_log_reset_watchdog);
    if (boot_log_reset_result[0]) at = put(out, at, cap, boot_log_reset_result, sizeof boot_log_reset_result);
  }
  if (boot_diag_status) {
    char row[120];
    snprintf(row, sizeof row, "[diag] status=%d files=%u skipped=%u errors=%u bytes=%u first=%u", boot_diag_status, &boot_diag_files ? boot_diag_files : 0,
             &boot_diag_skipped ? boot_diag_skipped : 0, &boot_diag_errors ? boot_diag_errors : 0, diag_bytes, diag_first);
    at = put(out, at, cap, row, sizeof row);
  }
  // The signed-text fixups given as text-fixupN boot arguments: how many parsed and how many page writes they made.
  if (&cs_text_fixup_parsed && cs_text_fixup_parsed) {
    char row[64];
    snprintf(row, sizeof row, "[text fixup] parsed=%u applied=%u", cs_text_fixup_parsed, cs_text_fixup_applied);
    at = put(out, at, cap, row, sizeof row);
  }
  // Which processes were started and ended (BootProgress.c census) and the throw site of the last uncaught exception (kits BootFaultProbe.c).
  if (&boot_progress_spawn_rows) {
    static char spawn[16384];
    const size_t n = boot_progress_spawn_rows(spawn, sizeof spawn);
    if (n) {
      at = put(out, at, cap, "[spawn]", 8);
      for (char *line = spawn; *line;) {
        char *end = strchr(line, '\n');
        const size_t len = end ? (size_t)(end - line) : strlen(line);
        at = put(out, at, cap, line, len);
        if (!end) break;
        line = end + 1;
      }
    }
  }
  // The sampling profile (BootProgress.c): which processes the interrupts landed in and where, at most 6 KiB of the slot.
  if (&boot_progress_profile_rows) {
    static char profile[6144];
    const size_t n = boot_progress_profile_rows(profile, sizeof profile);
    if (n) {
      at = put(out, at, cap, "[profile]", 10);
      for (char *line = profile; *line;) {
        char *end = strchr(line, '\n');
        const size_t len = end ? (size_t)(end - line) : strlen(line);
        at = put(out, at, cap, line, len);
        if (!end) break;
        line = end + 1;
      }
    }
  }
  if (&boot_progress_panic_text && boot_progress_panic_text[0]) {
    at = put(out, at, cap, "[panic]", 8);
    at = put(out, at, cap, boot_progress_panic_text, 2048);
  }
  return at;
}

/* The message buffer, NUL-padded to LogMsgBlocks blocks; returns the bytes copied.
 *
 * The newest bytes, not the oldest. The region holds 128 KiB while the kernel's msgbuf is larger, so
 * every read-back stored exactly 131072 bytes and it was always the start of the ring: the copy ran
 * from msg_bufx, the oldest byte, and stopped at the limit. A crash in the last seconds of a boot
 * therefore left nothing behind, which is precisely the window an abort needs (u250 to u255 all
 * reported msg_bytes 131072). Starting at msg_bufx + size - cap keeps the tail instead. */
static uint32_t copy_msgbuf(uint8_t *out) {
  memset(out, 0, LogMsgBlocks * LogBlock);
  struct boot_log_msgbuf *m = msgbufp;
  if (!m || !m->msg_bufc || m->msg_size <= 0) return 0;
  int size = m->msg_size, x = m->msg_bufx;
  if (x < 0 || x >= size) x = 0;
  const uint32_t cap = LogMsgBlocks * LogBlock;
  const uint32_t keep = (uint32_t)size < cap ? (uint32_t)size : cap;
  const int start = (int)(((uint32_t)x + (uint32_t)size - keep) % (uint32_t)size);
  uint32_t n = 0;
  for (uint32_t i = 0; i < keep; ++i) {
    char c = m->msg_bufc[(start + (int)i) % size];
    if (c) out[n++] = (uint8_t)c;
  }
  return n;
}

static void writer(void *arg, wait_result_t result) {
  (void)arg; (void)result;
  thread_set_thread_name(current_thread(), "boot_log");
  uint64_t first = 0, blocks = 0;
  const char *why = "";
  boot_log_state = 1;
  // The window exists once NativeUfs has read the root layout; until then the open is refused ("no root layout").
  while (!native_ufs_log_open(&first, &blocks, &why)) {
    if (strcmp(why, "no root layout")) {boot_log_state = 3; return;}
    IOSleep(1000);
  }
  boot_log_first_lba = first;
  /* Put the previous run's log back on the console before the new header replaces it. An
   * unattended round ends with the USB link gone, so the medium is the only record, and the next
   * boot is the only chance to show it. The tail is what matters: that is the state the reset
   * left behind.
   * Every read of this window fills whole 4096-byte blocks, so each destination must hold
   * blocks*4096 bytes. The header goes through the byte-granular call, which stages the block
   * itself. Passing &previous to the block call is what faulted boot-usb99: 4096 bytes landed in
   * a 152-byte stack object and ran on into the guard page (pc in memcpy, lr in hostRead4k,
   * far = sp + 0x3b0, x20 = 0x1000). */
  if (&native_ufs_log_read && &native_ufs_log_read_bytes &&
      blocks > (uint64_t)LogSlotFirst + LogSlotBlocks) {
    _Static_assert(sizeof(struct boot_log_header) <= LogBlock, "the header must fit in one block");
    struct boot_log_header previous;
    if (native_ufs_log_read_bytes(0, sizeof previous, &previous) && memcmp(previous.magic, "SHLOG001", 8) == 0 &&
        previous.slot_count && previous.slots_written && previous.slot_blocks == LogSlotBlocks) {
      uint32_t slot = (previous.slots_written - 1u) % previous.slot_count;
      /* Sized to exactly what the block read writes: LogSlotBlocks * LogBlock bytes. */
      static char previous_slot[LogSlotBlocks * LogBlock];
      if (native_ufs_log_read(LogSlotFirst + (uint64_t)slot * LogSlotBlocks, LogSlotBlocks, previous_slot)) {
        size_t end = sizeof previous_slot;
        while (end && previous_slot[end - 1] == 0) --end;
        size_t begin = end > 1536 ? end - 1536 : 0;
        if (&qcom_usb_debug_write) {
          char head[96];
          int n = snprintf(head, sizeof head, "PREVLOG run=%llu slots=%u err=%u tail=%zu bytes\n",
                           (unsigned long long)previous.boot_id, previous.slots_written,
                           previous.write_errors, end);
          if (n > 0) qcom_usb_debug_write(head, (size_t)n);
          if (end > begin) qcom_usb_debug_write(previous_slot + begin, end - begin);
        }
      }
    }
  }
  const uint32_t slot_count = (uint32_t)((blocks - LogSlotFirst) / LogSlotBlocks);
  if (&native_ufs_export_blocks && &native_ufs_export_write) {
    IOLockLock(export_lock);
    export_region = (uint32_t)native_ufs_export_blocks();
    IOLockUnlock(export_lock);
  }
  /* gts7xlwifi: keep the previous run's whole window. A run that resets the tablet is followed by the menu's default entry
   * three seconds later, whose new header and slots replaced the evidence (v43: the reset after two swipes was lost). Before
   * anything is written, the previous window is copied block for block to the start of the export region (unused on this
   * board), so it is read back from TWRP at window_lba - export_below with the same reader. */
  // boot_log_previous_copied: blocks copied, or 0xe0000000 | why not (1 no read call, 2 export region too small, 3 header read
  // failed, 4 no previous log).
  boot_log_previous_copied = !&native_ufs_log_read || !&native_ufs_log_read_bytes ? 0xe0000001u :
                             export_region < blocks ? 0xe0000002u | (export_region << 4) : 0;
  if (!boot_log_previous_copied) {
    struct boot_log_header previous;
    if (!native_ufs_log_read_bytes(0, sizeof previous, &previous)) boot_log_previous_copied = 0xe0000003u;
    else if (memcmp(previous.magic, "SHLOG001", 8) != 0 || !previous.slots_written) boot_log_previous_copied = 0xe0000004u;
    else {
      static char copy[64 * LogBlock];
      uint32_t copied = 0;
      for (uint64_t at = 0; at < blocks; at += 64) {
        const uint32_t n = (uint32_t)(blocks - at < 64 ? blocks - at : 64);
        if (!native_ufs_log_read(at, n, copy) || !native_ufs_export_write(at, n, copy)) break;
        copied += n;
      }
      boot_log_previous_copied = copied;
    }
  }
  struct boot_log_header h;
  memset(&h, 0, sizeof h);
  memcpy(h.magic, "SHLOG001", 8);
  h.version = 4; h.header_bytes = sizeof h;
  h.export_blocks = export_region; h.export_below = export_region; h.boot_id = log_ticks(); h.first_lba = first; h.window_blocks = (uint32_t)blocks;
  h.msg_first = LogMsgFirst; h.msg_blocks = LogMsgBlocks; h.slot_first = LogSlotFirst; h.slot_blocks = LogSlotBlocks; h.slot_count = slot_count;
  h.gpt_open = h.gpt_last = &native_ufs_store_gpt_check ? native_ufs_store_gpt_check() : 0xff;
  if (&boot_progress_tag) strlcpy(h.tag, boot_progress_tag(), sizeof h.tag);
  uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 19200000;
  const uint64_t start = log_ticks();
  // The reboot-into-fastboot path is not here any more: boot_log_tick owns it, because this loop
  // can stop (state 3 on three failed rounds, state 4 on a GPT change, or blocked in a UFS write,
  // measured on boot 43ba9162) and the board still has to come back. This loop only reports that a
  // round landed, through log_sync_seq.
  // sheng-diag-export=<seconds of log time>: export the diagnostic files once then (set it shortly before sheng-autoreset).
  uint32_t diag_at = 0;
  if (&boot_diag_scan && &boot_diag_stream) PE_parse_boot_argn("sheng-diag-export", &diag_at, sizeof diag_at);
  uint32_t errors = 0;
  boot_log_state = 2;
  for (;;) {
    const unsigned now = (unsigned)((log_ticks() - start) / f);
    int ok = 1;
    // Text slot, then the message buffer, then the header that makes both visible.
    memset(boot_log_buffer, 0, LogSlotBlocks * LogBlock);
    compose((char *)boot_log_buffer, LogSlotBlocks * LogBlock, h.slots_written, now);
    log_phase = 1;
    ok &= native_ufs_log_write(LogSlotFirst + (uint64_t)(h.slots_written % slot_count) * LogSlotBlocks, LogSlotBlocks, boot_log_buffer);
    log_phase = 2;
    h.msg_bytes = copy_msgbuf(boot_log_buffer);
    log_phase = 3;
    ok &= native_ufs_log_write(LogMsgFirst, LogMsgBlocks, boot_log_buffer);
    log_phase = 4;
    if (ok) ++h.slots_written;
    else ++h.write_errors;
    // The newest complete export is already on the medium; the header names it.
    IOLockLock(export_lock);
    h.export_generation = export_ready; h.export_bytes = export_ready_bytes; h.export_first = export_ready_first;
    h.diag_first = diag_first; h.diag_bytes = diag_bytes;
    IOLockUnlock(export_lock);
    if (diag_at && now >= diag_at && !boot_diag_status) {
      thread_t diag;
      boot_diag_status = 1;
      if (kernel_thread_start_priority(diag_thread, NULL, MINPRI_KERNEL, &diag) == KERN_SUCCESS) thread_deallocate(diag);
      else boot_diag_status = -10;
    }
    h.uptime_s = now; h.rounds = ++boot_log_rounds; h.state = boot_log_state;
    // Every 30 rounds the GPT is read back; any change stops the writer for good.
    if (&native_ufs_store_gpt_check && !(h.rounds % 30)) h.gpt_last = native_ufs_store_gpt_check();
    memset(boot_log_buffer, 0, LogBlock);
    memcpy(boot_log_buffer, &h, sizeof h);
    log_phase = 5;
    ok &= native_ufs_log_write(0, 1, boot_log_buffer);
    log_phase = 6;
    log_phase = 7;
    ok &= native_ufs_log_sync();
    log_phase = 8;
    if (ok) __atomic_fetch_add(&log_sync_seq, 1, __ATOMIC_RELAXED);
    if (!ok) {boot_log_errors = ++errors;} else errors = 0;
    if (errors >= LogMaxErrors) {boot_log_state = 3; return;}
    if (h.gpt_last != h.gpt_open) {boot_log_state = 4; return;}
    log_phase = 9;
    IOSleep(LogPeriodMs);
    log_phase = 10;   // woke; anything still at 9 is inside the sleep, anything at 10 is compose
  }
}

// Called by the board page's monitor once a second; starts the writer the first time when sheng-log=1.
void boot_log_tick(unsigned now) {
  if (!boot_log_started) {
    boot_log_started = 1;
    tick_first = now; tick_seen_at = now;
    // An unattended run must come back however it ends: on the desktop past its deadline (int-229 first suspected) or in a panic seconds
    // after the start, before UFS and this log exist (int-229: a duplicate class at PE_lockdown_iokit). So the first tick, independent of
    // sheng-log, marks the cell (any restart is fastboot) and arms the hypervisor's watchdog, which bites a minute after the pings stop:
    // after the autoreset deadline, or when a panic parks the cores.
    uint32_t autoreset = 0;
    // The probe is called for its text, not for a verdict: what the cell held at boot is worth
    // recording, but it must not decide whether the cell may be rewritten. It used to be the gate
    // and refused every run: it returns 0 only when the cell already holds a reason br_known()
    // accepts, this handset leaves 0x4ab2b0e2 in it, so the mark, the PON write and the reset were
    // skipped on every boot (boot-usb101, 273 rounds, known=0). Whether the cell held an old reason
    // says nothing about whether it can be overwritten.
    if (PE_parse_boot_argn("sheng-autoreset", &autoreset, sizeof autoreset) && autoreset) {
      tick_autoreset = autoreset;
      if (&boot_reset_probe) (void)boot_reset_probe(boot_log_reset_probe_text, sizeof boot_log_reset_probe_text);
      if (&boot_reset_mark_bootloader) boot_reset_mark_bootloader(boot_log_reset_mark, sizeof boot_log_reset_mark);
      if (&boot_reset_watchdog) boot_reset_watchdog(autoreset + 60, boot_log_reset_watchdog, sizeof boot_log_reset_watchdog);
    }
    uint32_t mode = 0;
    if (PE_parse_boot_argn("sheng-log", &mode, sizeof mode) && mode == 1 &&
        &native_ufs_log_open && &native_ufs_log_write && &native_ufs_log_sync) {
      boot_log_buffer = (uint8_t *)IOMallocAligned(LogMsgBlocks * LogBlock, LogBlock);
      if (boot_log_buffer) {
        export_lock = IOLockAlloc();
        thread_t thread;
        if (kernel_thread_start_priority(writer, 0, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
      }
    }
  }
  /* Phase heartbeat: once a second is too much, once on every change plus every tenth second
   * otherwise. A writer that stops leaves its last phase on the console and nothing else. */
  static unsigned last_phase = ~0U, last_at;
  {
    const unsigned phase = __atomic_load_n(&log_phase, __ATOMIC_RELAXED);
    if (phase != last_phase || now - last_at >= 10) {
      last_phase = phase; last_at = now;
      char row[64];
      const int n = snprintf(row, sizeof row, "BLPHASE t=%us phase=%u\n", now, phase);
      if (n > 0 && &qcom_usb_debug_write) qcom_usb_debug_write(row, (size_t)n < sizeof row ? (size_t)n : sizeof row - 1);
    }
  }
  if (!tick_autoreset || tick_reset_tried) return;
  // A round that landed recently means the medium is keeping up; one that did not means the writer
  // stopped, and it will not start again (boot_log_started is never cleared, and the loop has
  // returned on every exit path). Either way the board still has to come back.
  const unsigned seq = __atomic_load_n(&log_sync_seq, __ATOMIC_RELAXED);
  if (seq != tick_seen_seq) {tick_seen_seq = seq; tick_seen_at = now;}
  const unsigned elapsed = now - tick_first;
  if (elapsed < tick_autoreset) return;
  // log-capture section 6: the timeout covers the measured checkpoint and the log sync. Restart at
  // the deadline when a round has landed within the grace, and at the deadline plus the grace when
  // none has, so an in-flight round gets its moment and a stopped writer costs only the grace. The
  // header still names the last complete round either way.
  if (now - tick_seen_at > ResetSyncGrace && elapsed < tick_autoreset + ResetSyncGrace) return;
  tick_reset_tried = 1;
  if (!&boot_reset_bootloader) {
    strlcpy(boot_log_reset_result, "autoreset skipped: no reset entry point", sizeof boot_log_reset_result);
    return;
  }
  // The text is only readable if the reset is refused; on success the tablet restarts into fastboot.
  const int rc = boot_reset_bootloader(boot_log_reset_result, sizeof boot_log_reset_result);
  char tail[24]; snprintf(tail, sizeof tail, " (returned %d)", rc);
  strlcat(boot_log_reset_result, tail, sizeof boot_log_reset_result);
}
