/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* gts7xlwifi: start the original cfg80211/mac80211 stack and Linux ath11k (QCA6390 hw2.0, PCI/MHI/QMI) on the UniXNU Ath12kWifi shared
 * ABI. This is the ath11k counterpart of the module's src/nw_wifi.c (the ath12k transport it replaces in scripts/build-t970-wifi.py): the
 * same start order, worker step, interrupt and wait hooks, with ath11k's own PCI driver and ath11k structures in the diagnostics.
 * QCA6390 hw2.0 parameters (hw_params, qca6390_regs, the QCA6390 MHI channel table, CE and ring maps) are ath11k's own: pci.c picks them
 * from device 17cb:1101 and TCSR_SOC_HW_VERSION, as on Linux. Only what differs on this board is here. */
#include "lx_pci.h"
#include "lx_plat.h"
#include <net/mac80211.h>
#include "core.h"
#include "debug.h"
#include "peer.h"
#include "pci.h"
#include "nw_mhi.h"
#include "nw_qrtr.h"
#include "nw_mainline.h"
#include "nw_wifi.h"
#include "nw_board.h"
#include "nw_events.h"
#include "nw_control_endpoint.h"
#include <linux/mhi.h>

int t970_ath11k_pci_init(void);
void t970_ath11k_pci_exit(void);
int mhi_init_hook(void);
int lx_init_cfg80211_init(void);
int lx_init_ieee80211_init(void);
void lx_exit_cfg80211_exit(void);
void lx_exit_ieee80211_exit(void);
extern const struct bus_type mhi_bus_type;

struct nw_wifi_stats nw_wifi_stats;
/* Set by the generated MHI boot.c right before the BHI boot loader buffer is allocated; the board's arena consumes it and puts
 * that buffer at the arena base (IOVA 0xa0000000), where stage 5's boot loader was accepted. */
volatile int nw_wifi_bhi_next;
volatile unsigned int nw_wifi_t970_dbg;   /* ath11k debug mask bits printed (board: t970-wifi-dbg, default BOOT|QMI) */
/* t970-wifi-quirks, set by the board before the arena and nw_wifi_start: 1 no ath11k sw_reset at power-up, 2 no MHI pre-load hook
 * (its mhi_init_mmio writes the context registers before BHI), 16 no RDDM (generated pci.c, boot.c, mhi.c; build-t970-wifi.py);
 * the board's: 4 keep MRRS, 8 at most 4 MSI vectors, 32 keep stage 4's MSI capability as it is (4 vectors at 0xa03ff000). */
volatile int nw_wifi_t970_quirks;
/* Register write trace (int9): every BAR0 write of the stack (ath11k, and MHI through ath11k's register callback, via the shim's
 * lx_mmio_trace hook) and every config write of the board, in order, the first 512: {offset, value}; config writes carry bit 31 and
 * the access size in bits 27:24. Stage 5 (T970WifiProbe.cpp) records its own BAR writes here too, for the same diff. BootLog prints
 * them as [wifi-wr] rows. Offsets are BAR offsets as written, so a write through the 0x310c window shows as the window write
 * followed by 0x80000 + offset. */
volatile uint32_t nw_wifi_wr_n;
volatile uint32_t nw_wifi_wr[512][3];   /* offset, value, microseconds since the first entry */
uint64_t (*volatile nw_wifi_wr_clock)(void);   /* microseconds; T970WifiProbe.cpp sets it with its first write */
void nw_wifi_wr_note(uint32_t off, uint32_t v) {
  static uint64_t first;
  uint32_t i = nw_wifi_wr_n;
  if (i >= 512) return;
  const uint64_t now = nw_wifi_wr_clock ? nw_wifi_wr_clock() : 0;
  if (!i) first = now;
  nw_wifi_wr[i][0] = off; nw_wifi_wr[i][1] = v; nw_wifi_wr[i][2] = (uint32_t)(now - first); nw_wifi_wr_n = i + 1;
}
/* Called (board, T970WifiStack.cpp) just before BHI_IMGADDR_LOW is written, with the image address: int16b found the boot
   loader's page 1 different in DRAM from the CPU's write-combine alias, so the board writes it again through the cache and cleans
   it, as stage 5 does. */
void (*volatile nw_wifi_pre_bhi)(uint32_t off, uint32_t v);   /* every write below BAR 0x1000 (MHI, BHI, BHIe, doorbells) */
static void mmio_trace(volatile void *a, u32 v) {
  if (!nw_board || !nw_board->bar) return;
  uintptr_t o = (uintptr_t)a - (uintptr_t)nw_board->bar;
  if (o < nw_board->bar_len) nw_wifi_wr_note((uint32_t)o, v);
  if (nw_wifi_pre_bhi && o < 0x1000) nw_wifi_pre_bhi((uint32_t)o, v);
}
static struct ieee80211_hw *g_hw;
static bool cfg_started, mac_started, pci_started, mhi_started, qrtr_started;
void nw_wifi_hw_ready(struct ieee80211_hw *hw) { g_hw = hw; }
static struct ath11k *g_ar(void) { return g_hw ? g_hw->priv : NULL; }

static int has_text(const char *s, const char *w) {
  for (; *s; s++) { const char *a = s, *b = w; while (*a && *b && *a == *b) { a++; b++; } if (!*b) return 1; }
  return 0; }

/* The endpoint's view of the link and of errors (as nw_wifi.c): command/status, PCIe device and link registers, AER. */
static void pci_state(void) {
  u32 v, v2, v3, v4;
  if (!nw_board->cfg_read(0x04, 4, &v)) lx_log(3, "stuck: pci command/status %08x", v);
  if (!nw_board->cfg_read(0x80000008, 4, &v) && !nw_board->cfg_read(0x8000000c, 4, &v2) && !nw_board->cfg_read(0x80000010, 4, &v3))
    lx_log(3, "stuck: pcie devctl/sta %08x linkcap %08x linkctl/sta %08x", v, v2, v3);
  for (u32 at = 0x100, guard = 0; at >= 0x100 && at < 0x1000 && guard < 32; guard++) {
    if (nw_board->cfg_read(at, 4, &v)) break;
    if ((v & 0xffff) == 1) {
      u32 ue = 0, uem = 0, ces = 0, cap = 0;
      nw_board->cfg_read(at + 4, 4, &ue); nw_board->cfg_read(at + 8, 4, &uem); nw_board->cfg_read(at + 0x10, 4, &ces); nw_board->cfg_read(at + 0x18, 4, &cap);
      lx_log(3, "stuck: aer at %x ue %08x mask %08x ce %08x cap %08x", at, ue, uem, ces, cap);
      nw_board->cfg_read(at + 0x1c, 4, &v); nw_board->cfg_read(at + 0x20, 4, &v2); nw_board->cfg_read(at + 0x24, 4, &v3); nw_board->cfg_read(at + 0x28, 4, &v4);
      lx_log(3, "stuck: aer header log %08x %08x %08x %08x", v, v2, v3, v4);
      return;
    }
    at = v >> 20;
  }
  lx_log(3, "stuck: no aer capability");
}
/* The first WMI command that times out: copy engine positions, WMI credits and MHI state are logged, then the RAM dump is asked for. */
static int g_wmi_stuck;
static void ce_state(struct ath11k_base *ab, int p) {
  struct ath11k_ce_pipe *pipe = &ab->ce.ce_pipe[p];
  if (pipe->src_ring) {
    struct hal_srng *s = &ab->hal.srng_list[pipe->src_ring->hal_ring_id];
    lx_log(3, "stuck: ce%d src sw %u write %u hp %u reap %u cached-tp %u", p, pipe->src_ring->sw_index, pipe->src_ring->write_index,
           s->u.src_ring.hp, s->u.src_ring.reap_hp, s->u.src_ring.cached_tp);
  }
  if (pipe->status_ring) {
    struct hal_srng *s = &ab->hal.srng_list[pipe->status_ring->hal_ring_id];
    lx_log(3, "stuck: ce%d status sw %u tp %u cached-hp %u", p, pipe->status_ring->sw_index, s->u.dst_ring.tp, s->u.dst_ring.cached_hp);
  }
}
static void wmi_stuck(void) {
  struct ath11k *ar = g_ar();
  if (g_wmi_stuck != 1 || !ar) return;
  g_wmi_stuck = 2;
  struct ath11k_base *ab = ar->ab; struct ath11k_pci *ab_pci = ath11k_pci_priv(ab);
  for (int p = 0; p < ab->hw_params.ce_count && p < 9; p++) ce_state(ab, p);
  lx_log(3, "stuck: wmi credits %d, mhi ee %d state %d", ab->htc.endpoint[ATH11K_HTC_EP_2].tx_credits,
         ab_pci->mhi_ctrl ? (int)ab_pci->mhi_ctrl->ee : -1, ab_pci->mhi_ctrl ? (int)ab_pci->mhi_ctrl->dev_state : -1);
  pci_state();
  if (ab_pci->mhi_ctrl) lx_log(3, "stuck: asking for the RAM dump: %d", mhi_force_rddm_mode(ab_pci->mhi_ctrl));
}
/* The first RAM dump state report: where the boot interface says the device stopped. Offsets above the BAR's first 512 KiB are not read. */
static void log_tap(int level, const char *line) {
  static int done; (void)level;
  if (!g_wmi_stuck && has_text(line, "wmi command") && has_text(line, "timeout")) g_wmi_stuck = 1;
  if (done || !has_text(line, "MHI_CB_EE_RDDM") || !nw_board || !nw_board->bar) return;
  done = 1;
  const volatile u8 *m = nw_board->bar; u32 bhi = readl(m + 0x28);
  if (bhi >= nw_board->bar_len || bhi > 0x1000) bhi = 0x100;
  lx_log(3, "rddm: mhistatus %08x mhictrl %08x ee %08x bhi-status %08x err %08x dbg %08x %08x %08x",
         readl(m + 0x48), readl(m + 0x38), readl(m + bhi + 0x28), readl(m + bhi + 0x2c), readl(m + bhi + 0x30), readl(m + bhi + 0x34),
         readl(m + bhi + 0x38), readl(m + bhi + 0x3c)); }

/* The station's peer: rate and signal as ath11k keeps them from the firmware's peer statistics events (no request is made). */
void nw_wifi_link_state(char *text, int size) {
  struct nw_wifi_status state; nw_wifi_status_get(&state);
  text[0] = 0;
  struct ath11k *ar = g_ar();
  if (!ar || state.state != NW_STA_UP) return;
  struct ath11k_base *ab = ar->ab;
  struct rate_info rate = {}; int rssi = 0;
  spin_lock_bh(&ab->base_lock);
  struct ath11k_peer *peer = ath11k_peer_find_by_addr(ab, state.bssid);
  if (peer && peer->sta) {
    struct ath11k_sta *arsta = (struct ath11k_sta *)peer->sta->drv_priv;
    rate = arsta->txrate; rssi = arsta->rssi_comb;
  }
  spin_unlock_bh(&ab->base_lock);
  u32 bitrate = cfg80211_calculate_bitrate(&rate);
  nw_mainline_rate_update(state.bssid, bitrate);
  if (rssi) {
    int signal = test_bit(WMI_TLV_SERVICE_HW_DB2DBM_CONVERSION_SUPPORT, ab->wmi_ab.svc_map) ? (s8)rssi : rssi + ATH11K_DEFAULT_NOISE_FLOOR;
    nw_mainline_signal_update(state.bssid, signal);
  }
  struct nw_wifi_status updated; nw_wifi_status_get(&updated);
  snprintf(text, size, "freq %u rssi %d tx %u.%u Mbps flags %x mcs %u nss %u bw-code %u gi %u", state.freq_mhz, updated.signal_dbm,
           bitrate / 10, bitrate % 10, rate.flags, rate.mcs, rate.nss, rate.bw, rate.he_gi);
}
/* ath11k keeps no per-module release counters like ath12k's device_stats: report the soc-level counters it has. */
void nw_wifi_tx_reasons(char *b, int n) {
  b[0] = 0; struct ath11k *ar = g_ar(); if (!ar) return;
  struct ath11k_soc_dp_stats *st = &ar->ab->soc_stats; int o = 0;
  o += snprintf(b + o, n - o, "tx err"); for (int i = 0; i < DP_TCL_NUM_RING_MAX && o < n; i++) o += snprintf(b + o, n - o, " %u", st->tx_err.desc_na[i]);
  if (o < n) snprintf(b + o, n - o, " misc %u", atomic_read(&st->tx_err.misc_fail)); }
void nw_wifi_ring_state(char *b, int n) {
  b[0] = 0; struct ath11k *ar = g_ar(); if (!ar) return;
  struct ath11k_soc_dp_stats *st = &ar->ab->soc_stats;
  snprintf(b, n, "err ring pkts %u invalid rbm %u hal reo err %u %u %u %u", st->err_ring_pkts, st->invalid_rbm,
           st->hal_reo_error[0], st->hal_reo_error[1], st->hal_reo_error[2], st->hal_reo_error[3]); }
void nw_wifi_rx_state(char *b, int n) {
  b[0] = 0; if (!g_ar()) return;
  snprintf(b, n, "frames %u; queued %u, dropped %u", nw_wifi_stats.frames, nw_rx_backlog(), nw_rx_dropped()); }
/* Firmware HTT statistics need debugfs in ath11k (not built): the request is logged as not made. */
void nw_wifi_htt_stats(int type) { lx_log(6, "wifi: firmware statistics %d not available (ath11k without debugfs)", type); }

/* The address of a radio whose firmware reports none (or a random default): the board's wlan_mac.bin (text, twelve hex digits after the
 * last '='; package/T970WifiStack.cpp derives it from /chosen/unique-chip-id), else a locally administered address made up once. */
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
void nw_board_mac(u8 *a) {
  static u8 made[6]; static int have;
  const u8 *d; size_t n;
  if (!have && lx_plat->fw_get && !lx_plat->fw_get("wlan_mac.bin", &d, &n)) {
    size_t s = 0; for (size_t i = 0; i < n; i++) if (d[i] == '=') s = i + 1;
    u8 m[6]; size_t k = 0;
    for (size_t i = s; i < n && k < 12; i++) { int v = hexval((char)d[i]); if (v < 0) { if (k) break; continue; } m[k / 2] = (u8)(k & 1 ? m[k / 2] << 4 | v : v); k++; }
    if (k == 12 && is_valid_ether_addr(m)) { memcpy(made, m, 6); have = 1; lx_log(6, "wifi: address from wlan_mac.bin"); }
  }
  if (!have) {
    for (int i = 0; i < 6; i++) made[i] = (u8)(lx_random_u32() ^ lx_now_ms() >> (i * 3));
    made[0] = (made[0] & 0xfe) | 0x02; have = 1; lx_log(4, "wifi: the firmware and the board give no address: a made-up one is used");
  }
  memcpy(a, made, 6); }

void nw_wifi_irq_mode(int delivered) { lx_irq_polling(!delivered); }
void nw_wifi_poll(void) { nw_mainline_poll(); nw_wifi_control_poll(); }
void nw_wifi_irq_pending(uint32_t vectors) {
  if (!nw_board) return;
  while (vectors) { unsigned bit = __builtin_ctz(vectors); vectors &= vectors - 1; lx_irq_raise(nw_board->irq_base + bit); }
}
unsigned nw_wifi_wait_ms(unsigned maximum) { return nw_mainline_pending() || nw_wifi_control_pending() ? 0 : lx_next_wait_ms(maximum); }
void nw_wifi_set_platform(const struct lx_platform *p) { lx_plat = p; }

int nw_wifi_start(const struct lx_platform *p, const struct nw_board *b) {
  if (!p || !b || b->device_id != 0x1101 || !b->bar || b->bar_len < 0x100000 || !b->cfg_read || !b->cfg_write || !b->msi_target) return -EINVAL;
  lx_plat = p; nw_board = b;
  /* the ath11k_dbg() macro tests ath11k_debug_mask itself (combo2 printed none): the board's t970-wifi-dbg mask (default
     BOOT|QMI) goes there; no HTT/WMI debug formatting in normal traffic unless asked for */
  ath11k_debug_mask = nw_wifi_t970_dbg;
  /* ath11k_frame_mode stays native Wi-Fi, as sheng runs ath12k: mac80211 does the 802.3/802.11 conversion. */
  nw_wifi_buffer_pool_configure(1024 * 1024);
  lx_root_init(); lx_log_tap = log_tap; lx_mmio_trace = mmio_trace; nw_qrtr_init();
  int r = nw_mainline_init(); if (r) goto fail;
  r = lx_init_cfg80211_init(); if (r) goto fail;
  cfg_started = true;
  r = lx_init_ieee80211_init(); if (r) goto fail;
  mac_started = true;
  r = mhi_init_hook(); if (r) goto fail;
  mhi_started = true;
  r = nw_qrtr_mhi_register(); if (r) goto fail;
  qrtr_started = true;
  nw_wifi_stats.stage = 1;
  r = t970_ath11k_pci_init();
  if (r) goto fail;
  pci_started = true; nw_wifi_stats.stage = 2; return 0;
fail:
  nw_wifi_stats.last_err = r; nw_wifi_stop(); lx_log(3, "wifi: driver start failed: %d", r); return r;
}
/* Nonzero while traffic moves (frames arrived or wait to be sent): the board calls again at once instead of sleeping. */
int nw_wifi_step(void) {
  static unsigned last_frames; nw_wifi_control_poll(); lx_pump(); nw_mainline_run(); wmi_stuck();
  static u64 next_signal;
  if (g_hw && lx_now_ms() >= next_signal) { next_signal = lx_now_ms() + 1000; char link[128]; nw_wifi_link_state(link, sizeof(link)); }
  const unsigned f = nw_wifi_stats.frames; const int moved = f != last_frames; last_frames = f;
  return moved || nw_mainline_pending() || nw_wifi_control_pending() || lx_next_wait_ms(1000) == 0; }
void nw_wifi_stop(void) {
  nw_mainline_shutdown(); nw_wifi_control_shutdown();
  if (pci_started) { t970_ath11k_pci_exit(); pci_started = false; }
  if (qrtr_started) { nw_qrtr_mhi_unregister(); qrtr_started = false; }
  if (mhi_started) { bus_unregister(&mhi_bus_type); mhi_started = false; }
  if (mac_started) { lx_exit_ieee80211_exit(); mac_started = false; }
  if (cfg_started) { lx_exit_cfg80211_exit(); cfg_started = false; }
  g_hw = NULL; nw_wifi_buffer_pool_configure(0);
}
