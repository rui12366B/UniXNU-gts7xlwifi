# Wi-Fi sources (QCA6390 on SM8250 PCIe0)

Inputs for the full Wi-Fi stack on the SM-T970. None of these is committed here;
each is fetched or cloned locally, and firmware stays in `.cache/firmware/wifi`.

## Fetched

| Source | Where | Version | Used for |
|---|---|---|---|
| Linux ath11k, MHI host, QRTR, QMI, mac80211, cfg80211 | `sources/periph/Ath12kWifi/deps/linux` | v7.0.14 (`458c6079`), verified by `tools/fetch-sources.sh` | the driver itself: `drivers/net/wireless/ath/ath11k` (71 files), `drivers/bus/mhi/host`, `net/qrtr`, `net/mac80211`, `net/wireless` |
| wpa_supplicant | `sources/periph/Ath12kWifi/deps/wpa_supplicant-2.12` | 2.12, sha256 `08e23937…` | WPA2-PSK authentication, through Ath12kWifi `userspace/driver_sheng.c` |
| FreeBSD linuxkpi and net80211 headers | `sources/periph/Ath12kWifi/deps/freebsd` | releng/15.1 | Ath12kWifi shim headers |
| UniXNU Ath12kWifi | `sources/periph/Ath12kWifi` | local checkout | shared station ABI, Linux runtime shim (`shim/`), cfg80211 control endpoint `org.unixnu.wifi.control`, crypto |
| pong-wifi (Chengdong-Yu) | `sources/wifi-ref/pong-wifi` | `d2e33ca6` (the commit UniXNU-pong `e0ee6df1` pins), BSD-2-Clause-Patent | the working ath11k port to XNU: `ath11k/PongAth11k.c` (ath11k PCI/MHI/QMI on the station ABI), `PongWifiPlatform.cpp` (power, PCIe, SMMU context, MSI), `PongWifiNet.cpp` (ifnet, Apple80211 ioctls) |
| UniXNU-pong | `sources/wifi-ref/UniXNU-pong` | `e0ee6df1` | `scripts/build-pong-ath11k.py` (builds ath11k against Ath12kWifi), `configs/pong-wifi-sm8475.json`, the `migration/ipados18/services*-wifi*.md` records |
| cnss2 and msm MHI (Qualcomm, as shipped for SM8250 with QCA6390) | `sources/wifi-ref/cnss2-xiaomi-sm8250`, from LineageOS `android_kernel_xiaomi_sm8250` lineage-20 | `drivers/net/wireless/cnss2/{pci,power,reg}.c/h`, `drivers/bus/mhi/core` | the vendor boot sequence this board's Android uses: 512 KiB SBL by BHI, `set_wlaon_pwr_ctrl`, PBL debug registers |

Firmware (from this device's `vendor/firmware/qca6390`, listed in `proprietary-files.txt`):
`amss20.bin` (hash-only MBN; the same file Android boots), `m3.bin`, `bdwlan.elf*`, `regdb.bin`.

## How the pieces fit

1. **Board** (ours): `package/T970WifiProbe.cpp` stages 1–5 already do the rails, WLAN_EN, PCIe0 link,
   BAR, the private SMMU context for streams 0x1c00/0x7f and the BHI transfer. They become the T970
   counterpart of `PongWifiPlatform.cpp`: the `lx_platform` callbacks (`dmaAlloc`, `dmaMap`, MSI, `fwGet`)
   that `PongAth11k.c` calls.
2. **Driver**: `PongAth11k.c` with ath11k's QCA6390 hw2.0 parameters (`ath11k_hw_params`, `qca6390_regs`,
   `ath11k_mhi_config_qca6390`) in place of pong's QCA6490 ones; built by a copy of `build-pong-ath11k.py`.
3. **Network**: `PongWifiNet.cpp` targets Darwin 24 (iPadOS 18). This port is iPadOS 17.7.11 (Darwin 23),
   so its Apple80211 ioctl structures and the wifid handshake (`InterfaceNamer`) have to be checked
   against 21H461 before reuse.

## Blocker before any of this runs

The QCA6390 PBL rejects the SBL with BHI ERRCODE `0xef0e0003` about 300 ms after reading it (PBL log
`ef0e0200`, SBL log start 0). periph22 proved the device's DMA reaches the SMMU bank, and the image is the
one Android uses. periph24 votes the PMIC RF/LN_BB clock outputs, in case the chip's XO is the missing piece.

## Stage 6: the full stack (wired 2026-11-05, not run on the tablet yet)

`t970-wifi=6` runs stages 1–4 (rails, WLAN_EN, PCIe0 link, BAR0, ASPM off), skips stage 5, and hands the
endpoint to the Linux stack on the same thread. Without the argument nothing of it runs.

| Piece | File | What it is |
|---|---|---|
| Recipe | `scripts/build-t970-wifi.py` (board.toml archive step) | builds `build/t970-wifi/libT970Wifi.a` from the Ath12kWifi checkout (`sources/periph/Ath12kWifi` at `28de7b9`, or `T970_ATH12K_WIFI_DIR`) |
| Driver | Linux v7.0.14 `ath11k` (core … p2p, debug, mhi, pci: 22 objects), generated under `build/t970-wifi/ath11k-src` | QCA6390 hw2.0 parameters are ath11k's own (`pci.c` picks them from 17cb:1101 and TCSR_SOC_HW_VERSION). Patches: `core.h`/`dp_rx.c` jiffies member, `core.c` board file = `bdwlan.elf` / `bdwlan.eNN` by QMI board ID (pong's rule), `mac.c` board address fallback, `pci.c` callable init/exit |
| Runtime | Ath12kWifi `tools/sources.txt` minus `ath12k:*`, `nw_wifi.c`, `nw_debug.c` (88 objects) | mainline cfg80211/mac80211, MHI host (with the module's pre-load settle hook), QMI, QRTR, crypto, control endpoint |
| Glue | `package/T970Ath11k.c`, `package/ath11k-compat/` | ath11k counterpart of `nw_wifi.c`; compat from pong's `PongCompat.h` |
| Board | `package/T970WifiStack.cpp` | `lx_platform`/`nw_board`: its own apps-SMMU stage-1 bank for streams 0x1c00/0x7f (as stage 5's `wdom`, ASIDs from 0xfb00 down), 64 MiB arena at IOVA 0xa0000000 with an uncached CPU alias, power-of-two-aligned blocks, DW iMSI on SPI 141, config space via the iATU CFG0 window, firmware from `/private/var/ngpu/firmware/qca6390` (`amss.bin` → `amss20.bin`), `wlan_mac.bin` from `/chosen/unique-chip-id`, `org.unixnu.wifi.control`, worker loop. Row `t970_wifi_stack[16]` |
| Network | `package/T970WifiNet.cpp` | UniXNU-sheng `WifiNet.cpp` (`5d51e87`) with T970 names; initialiser runs only at stage 6 |
| Link | `configs/gts7xlwifi_defconfig` | `-Wl,-force_load,{tree}/build/t970-wifi/libT970Wifi.a` in `CONFIG_XNU_EXTRA_LD_ARCHIVES` (re-run `unixnu defconfig`, or the resolved `.config` keeps the old line) |
| DTS | `dts/gts7xlwifi.dts` `wlan` | `status = "disabled"` so the default ADT is unchanged; set `okay` for a Wi-Fi payload |

Boot arguments: `t970-wifi=6`, `t970-wifi-delay=<s>` (start the stack that long after stage 4), `t970-wifi-msi6=1`
(take MSIs on SPI 141; default off: polling), `t970-wifi-bank=<n>` / `t970-wifi-asid=<a>` (bank and ASID; by default the first free bank, ASID 0xfb00), `t970-wifi-sbl-base=0` (do not hold the arena's first 512 KiB for the BHI boot loader), `t970-wifi-step-ms=<ms>` (pause after each checkpoint so the 2 s boot log records it), `t970-wifi-dma-mib=<16..256>` (64), `t970-wifi-mair6=<attr>` (0x44, Normal-NC),
`t970-wifi-aj=1` (join `join.txt` from the firmware directory). BootLog row `[t970 wifi stack]`: `cp` is the step about to run
(layout at `t970_wifi_stack` in `T970WifiStack.cpp`).

First device run (wifi6, MSI line on by default then): the whole kernel stopped 19–24 s in, when stage 6 started, with no
checkpoint. Suspects: SPI 141 being a different or stuck level line (now opt-in), a blocking lock taken under the shim's
preemption-disabled spinlocks (the arena now uses a spin), a BAR mapping past BAR0 (now sized from the BAR mask).

Checked on the host: all 110 module objects compile, the module imports only 14 libc names XNU has, every
other defined name is renamed `_t970w*` except the 40 `nw_wifi_*` boundary symbols, the board objects build
with the kernel's `-Werror` recipe, and the kernel links with the archive force-loaded.

Untested, in the order they will matter:
1. The PBL rejection above was solved on periph27/27b: the PBL needs MSI enabled in the endpoint's capability (a target inside
   the mapped IOVA window) before BHI; MAIR Normal-NC is fine. Stage 6 has that: stage 4 enables MSI to 0xa03ff000 (inside the
   64 MiB arena), and ath11k's probe re-programs 32 vectors to the arena's first page (the iMSI address) before MHI power-up,
   with ASPM off during the load (ath11k_pci_aspm_disable). Whether BHIe, QMI and mission mode follow is untested.
2. The DMA domain at 64 MiB with an uncached CPU alias (stage 5 used 4 MiB, cached plus explicit cleaning).
3. iMSI delivery on SPI 141 (sheng's mechanism, never measured on SM8250); without it the worker polls.
4. QMI board ID → bdwlan file, m3.bin, QMI host memory size.
5. **Scan and join need userspace**: the mainline runtime sends scan/join requests to `unixnu-wpa_supplicant`
   over `org.unixnu.wifi.control` (Ath12kWifi `userspace/driver_sheng.c`). It is not built or packaged for this
   board yet (Ath12kWifi `tools/build-supplicant.py --target ios`, signed, plus a launchd job and its trust hash).
6. The GPU addon must carry `m3.bin`, `bdwlan.elf`, `bdwlan.e05` (and `regdb.bin`) next to `amss20.bin`.
7. wifid on 21H461 with the `wlan` node enabled and `T970WifiNet.cpp`'s Apple80211 answers.

int4 (bank 7 chosen automatically, no freeze): BHI failed with ERRCODE `ef0a0104` (PBL segment load), ERRDBG1 `0xa0500000`.
Since then: the CPU alias is a write-combine mapping made after `dc civac` (UniXNU-sheng NativeUfs), not `map(kIOMapInhibitCache)`;
the boot loader goes to IOVA 0xa0000000 like stage 5, and the MSI sink moved to the arena's last page; QFPROM bit 0 is cleared
with VDD4BLOW (cnss2); on a BHI failure the stack logs the bank FSR/FAR/FSYNR0, the BHI error registers, the PTE of ERRDBG1 and
how many SBL pages differ between DRAM and the alias and between DRAM and amss20.bin (`bhi fail:` lines).
int5: the write-combine `createMappingInTask` on the arena's buffer gave no separate mapping (stack stopped at sdom -12). The
alias is now a physical-range descriptor over the arena's page runs mapped write-combine, checked by translating its first and
last page; if that fails the stack keeps the cached mapping and sets the device MAIR to write-back (coherent PCIe0). The row's
mib/mair field carries the path in bit 24 (alias) or 25 (cached), and a log line names it. The bank is re-checked just before
it is programmed (SCTLR.M clear, no valid SMR routed to it, SMR slot still free), since audio and USB share the apps SMMU.
int6: alias and image verified (DRAM equals the alias, no SMMU fault, 0 unmapped pages), PBL error moved to `ef128100` with the
boot loader at 0xa0500000: ath11k's RDDM table (nine 512 KiB segments, allocated by mhi_prepare_for_power_up) had taken the held
first 512 KiB. A generated copy of MHI boot.c now sets `nw_wifi_bhi_next` right before the BHI buffer is allocated, and only that
allocation gets the arena base (0xa0000000). The BHI chunk itself is the first 0x80000 bytes of amss20.bin, as in stage 5.
int8b: boot loader at 0xa0000000, DRAM equals amss20.bin, no SMMU fault, still `ef128100`: the sequence differs, not the data.
`t970-wifi-quirks=<bits>` bisects it: 1 no ath11k sw_reset at power-up, 2 no MHI pre-load hook (200 ms + context registers before
BHI), 4 keep the endpoint's MRRS, 8 at most 4 MSI vectors. Handing ath11k a chip already in SBL is not possible with Linux MHI as
is: `MHI_POWER_UP_CAPABLE` admits PBL/PTHRU/EDL/AMSS only, and BHIe is set up only on the PBL path.
int9: quirks 15 change nothing (`ef128100`). Write trace: `nw_wifi_wr[512]` (T970Ath11k.c) records every BAR0 write of stage 6
(the shim's `lx_mmio_trace` hook covers ath11k and MHI) and every board config write, and stage 5's BAR writes in
T970WifiProbe.cpp; BootLog prints them as `[wifi-wr]` rows, 8 per row, for a diff of one boot of each stage.
int10/int11 write-trace diff: stage 6 additionally rings the BHIe RX vector doorbell for RDDM (BAR 0x284..0x294) and writes the MHI
context base registers (0x58..0xa4, MHICFG) before BHI, and reprograms MSI to 32 vectors at the arena's last page. New quirk bits:
16 no RDDM (`rddm_size = 0`), 32 keep stage 4's MSI capability (its sink page 0xa03ff000 reserved in the arena). The context
writes before BHI are the module's pre-load hook: bit 2.
int13 (quirks 18): BAR0 writes before BHI now match stage 5's; still `ef128100`. The write trace carries microseconds since its
first entry and stage 4's endpoint config writes (shared by stages 5 and 6), for the timing and config comparison.
int14 (quirks 62): same `-110`, then the tablet reset at ~55 s (no panic text). Since then a failed start quiesces the endpoint
(MSI enable and bus mastering off by direct config writes) and T970WifiProbe.cpp holds PERST# then WLAN_EN low once the stack
returns; the SMMU bank and arena are never released.
int15 (quirks 30): `ef128100` again, no SMMU fault, DRAM equals amss20.bin, clean shutdown (no reset). Against stage 5 (int10,
`ee 1` at 321 ms) the BAR writes, config writes (int14 kept stage 4's MSI with bit 32) and delays match: hot reset → 5 ms →
clears → 11 ms → SOC_GLOBAL_RESET 11 ms each way → MHICTRL reset → 11.5 ms → BHI. The bank registers are wdom's. What still
differs: stage 6 starts at ~43 s, after the USB console's SMMU lease (~25 s); stage 5 ran before it. Both used CBAR VMID 0, shared
with the other stage-1 banks, and only TLBIASID: a global TLB entry for 0xa0000000 from another bank on VMID 0 would send the
PBL's reads elsewhere with no fault in this bank. Since then: the bank has VMID 0xfb (`t970-wifi-vmid=<v>`, 0 = as stage 5) and is
also TLBIALL'd; `t970-wifi-hole=<n>` leaves arena page n-1 unmapped (hole=1: does the PBL's header read fault here?); the log
names every SMR matching 0x1c00 and sGFSR before the start, and at the failure BHI status/EE, MHISTATUS, sGFSR/GFSYNR0/GFAR, the
matching SMRs and the PBL log registers and SRAM words of stage 5's `[t970 wifi pbl]` row (`bhi fail:` lines).
int16b (VMID 0xfb, quirks 30): `ef128100` again; one SMR matches 0x1c00 (ours), no global fault. The PBL's own log agrees:
`pbl log ef128100`, `cfg 201 boot 42 qfprom 2` are the values stage 5 had when it passed, so the fuses and image are not it
(`amss20.bin` is the tablet's `/vendor/firmware/qca6390/amss20.bin`, the file stage 5 booted to SBL). BHI_STATUS reads
ffffffff, which is error state 3 in bits 31:30 and not a dead link, since ERRCODE in the same BAR reads correctly. New this run: DRAM
page 1 of the boot loader (0xa0001000, which normally holds the MBN hash segment) differed from both the write-combine
alias and the file; in int15 no page differed. The CPU's write-combine path to DRAM is not reliable while the pages also have
cacheable mappings. Since then, right before BHI_IMGADDR_LOW is written (T970Ath11k.c MMIO hook `nw_wifi_pre_bhi`), the board
writes the 512 KiB again through the arena's cached mapping and runs `dc civac` (stage 5's method). It then compares DRAM with the
file through an uncached view and logs a `wifi: before BHI` line. Quirk bit 64 skips the rewrite (check only). The failure check
now logs the first differing word.
int17a (quirks 30, the boot loader rewritten through the cache and cleaned before BHI): **past the PBL**. "Device in READY State" at
562 ms, MHI registers initialised (context bases 0x58..0xa4, event-ring doorbells 0x700/0x708, M0 `38=200`), then "Wait for
device to enter SBL or Mission mode" until the 20 s timeout. So the cache hazard was the PBL blocker. The next step depends on the
same hazard in both directions: the device reads the contexts and rings the CPU wrote through the alias, and the CPU reads events
the device wrote. Since then (default; quirk bit 256 turns it off) every used arena block of at most 64 KiB is cleaned and
invalidated by VA (`dc civac`) before every BAR write below 0x1000 (MHI registers and doorbells) and in every shim poll (`pPoll`),
and all used blocks before the BHIe TX vector doorbell. That doorbell also gets a check of each vector segment in DRAM against
amss20.bin. Quirk bit 128 drops the alias: the CPU uses the cached mapping and the device MAIR is write-back (fully coherent,
if PCIe0 really snoops, as Samsung's DT says). Log lines are shorter: `wifi: pre-BHI rw 1 diff 0/128`, `wifi: maint small N all N`,
`wifi: BHIe segs N diff N`.
combo1b (int18, quirks 30): **MISSION MODE**. The pre-BHI check shows 0 of 128 pages differing, the BHIe check 0 of 9 segments,
and EE events SBL then MISSION MODE at 669 ms. QRTR traffic follows, and QMI gets through host-cap, respond-mem and the board
file (`bdwlan.elf` read at 697 ms) and asks for `m3.bin` (711 ms). Then nothing: warnings at 1.7 and 12.8 s and an error at 2.7 s,
which printed as `0x…V` because the shim's vsnprintf has no `%pV` (every ath11k_info/err/warn/dbg line was lost). No MSI ever
arrives (polling, as configured). Since then: the recipe patches `debug.c` to format the message first; debug lines print for
the bits in `t970-wifi-dbg=<mask>` (default 0x60 = BOOT|QMI); a block of any size is also cleaned for 2 s after it is handed out
(the M3 image, 266 KiB, and the QMI host-memory regions are single large blocks the firmware reads after the CPU filled them,
and only blocks of 64 KiB or less were cleaned); `pDmaMap` and `pDmaSync` (dma_map/unmap/sync_single) clean and invalidate their range.
combo2 (int19, quirks 30): MISSION MODE again (BHIe 9 segments, 0 differing), but no QRTR message at all this time (combo1b had
five). ath11k info lines now print as text ("MSI vectors: 32", "qca6390 hw2.0"), so the `%pV` fix works. The debug lines never
reached `__ath11k_dbg`: the `ath11k_dbg()` macro tests `ath11k_debug_mask` first, and `nw_wifi_start` had set it to 0. It now takes
`t970-wifi-dbg` (default 0x60). Defect found: `bhieCheck` read the 4.3 MB of AMSS segments through the cached mapping and invalidated
only the vector table, leaving clean lines over pages the arena can hand out again, where the alias's non-cacheable accesses hit
them. Since then every block is cleaned and invalidated when it is allocated (before it is zeroed), and each checked segment is
invalidated after its check. Quirk 512 skips the per-poll clean (doorbell cleaning stays), for an A/B against combo1b.

### combo4 (int20), 2026-11-06

QMI works: ath11k got the target capabilities (fw WLAN.HST.1.0.1.r1-01571-QCAHST, board id 0xff, `bdwlan.elf` 57900 bytes) and
began the board-file download. Chunks 1–4 (6144 bytes each) were acknowledged. After chunk 5 the MHI transfer event ring returned
code 0x11 (MHI_EV_CC_BAD_TRE, logged as "Unknown event 0x11"). ath11k then timed out after 10 s ("failed to wait board file download
request: -110"). The MMIO trace has the IPCR uplink (ch 20, doorbell 0x3a0, ring a0087000) at TRE index 11 when it stopped. int21
generates its own MHI main.c, in which the BAD_TRE path logs the channel, the event's words, the ring's offsets and the TRE the event
names.

### combo5 (int21), 2026-11-08: QRTR HELLO race

combo5, like combo2b, reached MISSION MODE and then started no QMI. The MMIO traces of all three runs are identical up to the
first uplink packet: the IPCR inbound ring was filled (63 TREs), then the host's broadcast HELLO went out on ch 20 (TRE 0).
- combo2b/combo5: the device sent one packet back (one ch 21 refill, one event) and then nothing.
- combo4: the device sent two packets back in the same poll, and the host's next uplink packet followed 13 µs later.

The module's name service (src/nw_qrtr.c) never answers a remote HELLO once it has sent its own. Linux's qrtr_ns answers each one,
and the firmware announces its servers only after that answer. int22 generates nw_qrtr.c so that it answers the firmware's first
HELLO once and logs each control packet (`qrtr: ctrl <cmd> from <node>:<port> svc <id>`).

### combo6 (int22), 2026-11-08

The HELLO answer works: `qrtr: ctrl 2 from 7:4294967294` (the firmware's HELLO from its control port), then `HELLO from node 7
answered`, sent as ch 20 TRE 1 (doorbell 0x3a0=a0087020). After that the device sent nothing more: no NEW_SERVER, and no MMIO write
from the host for 700 s, even though lx_pump keeps raising the polled MHI interrupts every round. int23 adds `wifi: rings …` and
`wifi: er1[0..3] …` to the board's 30 s report. They show the read and write pointers that the device writes into the event and
channel contexts, plus event ring 1's first entries, so one log tells "device silent" apart from "host not consuming".

### combo7 (int23), 2026-11-08

int22's HELLO answer works: the firmware sent NEW_SERVER svc 0x45 at 692 ms, and QMI and the board-file download then ran as in
combo4. BAD_TRE came back at chunk 5, decoded:
- Event: dword0 11000000 (code 0x11, len 0), dword1 14220000 (ch 20, type 0x22 TX event).
- Rejected TRE a00870b0 (index 11): ptr a0080ec0, len 0x34, dword1 00020600 (type 2 TRANSFER, IEOT|BEI). That is well formed,
  and the CPU reads it back that way through the alias.
- 52 bytes is a QRTR v1 control packet (32-byte header + 20). At this point it can only be the RESUME_TX that nw_qrtr.c sends when
  the firmware's 5th packet asks for confirm_rx, which makes it the first RESUME_TX of the boot (combo4 failed at the same spot).
- Host ring rp b0, wp d0 (TREs 11 and 12, chunk 5, outstanding).
- Event ring 1: EOT events, len 0x34, for ch 21 and ch 20 alternately.
- The ch 20 context rp read 0x1_00000001. The host sets rp = rbase and never writes it again, so this value comes from the device.
int24 adds to the BAD_TRE path the channel context's 11 words and TREs i-3..i+1, and splits `wifi: rings` into two rows.

### combo8 (int24), 2026-11-08: past BDF, stuck at HTC

No BAD_TRE this boot. BDF, M3, firmware init/start, wlan cfg and "wlan mode req mode 0" all completed (722–732 ms). The
"htc service 'Control' … ready" line is ath11k_htc_init setting up endpoint 0 locally; the wait that failed is ath11k_htc_wait_target,
the target's HTC READY message on CE pipe 2. It never came, not even after ath11k's own poll of every CE. Then "qmi wlan mode req mode 4"
also went unanswered for 10 s, so the firmware stopped answering QMI too. That points to a firmware hang or crash after mode 0, not
to a host that misses one message. No SMMU fault (fsr 400). MHI event rings fully consumed (er1 rp a00a0300, wp a00a02f0).
int25 logs, at that timeout, the MHI state and EE plus every CE ring's host and hardware pointers (t970_htc_dump). Running with
t970-wifi-dbg=0x8864 (BOOT|QMI|HTC|HAL|CE) also shows the shadow-register ring set-up and any CE traffic.

### combo8b (int24, quirks 158), 2026-11-08: interface up, no scan

On the coherent path (quirk 128: no write-combine alias, device write-back) the firmware got past HTC/WMI, the regulatory set-up ran
(6 GHz disabled by the custom regd, as expected), en0 came up, the cfg80211 ready callback ran ([wifi-nw] stage 4) and wifid
opened the interface. Over the whole boot wifid issued only state/capability gets, `kCurrentNetwork` (103) polls and a few set types
the bridge refuses (383 len 4104, 419 len 136, 470, 473). It sent no scan request (type 10/86) and never set power (19). The station
reports IDLE, so the power get answers "on".

Two separate gaps:
1. wifid never asks for a scan. int25 logs the first 48 bytes of each refused set type once (`wifi-net: refused <type> …`); if
   383 is 21H461's scan request, the bridge needs it.
2. Even when asked, nw_wifi_scan only queues NW_CONTROL_SCAN_REQUEST for the userspace supplicant (`unixnu-wpa_supplicant` over
   `org.unixnu.wifi.control`, Ath12kWifi `userspace/driver_sheng.c`). That is still not built or packaged for this board (item 5 of
   the list above), so a scan would just time out with no results.

int25 also makes quirks 158 the default when `t970-wifi-quirks` is absent.

### Supplicant service (int26), 2026-11-08

Open item 5 is now scripted, untested on the device:
- `scripts/build-t970-wifi-service.py --os-root <extracted 21H461 OS> [--addon IN --addon-out OUT --trust IN --trust-out OUT]`
  builds Ath12kWifi's wpa_supplicant 2.12 with the native backend for arm64e iOS 17.0 (`out/wpa-ios17`) and signs it ad hoc as
  `org.unixnu.wifi.supplicant` (no-container entitlement).
- The launchd job `org.unixnu.wifi.supplicant` (`/usr/libexec/unixnu-wpa_supplicant -Dsheng -ien0 -c/private/var/ngpu/etc/wifi/wpa_supplicant.conf -q`,
  root, KeepAlive, log /var/log/unixnu-wifi.log) is merged into the OS's `System/Library/xpc/launchd.plist`, keeping all 561 original
  jobs, and the result gets its own detached signature (UniXNU `build-boot-runtime.py` merge/sign).
- The addon gains `wifi-bin/unixnu-wpa_supplicant` (0755), `etc/wifi/wpa_supplicant.conf` and `xpc/launchd.plist{,.sig}`. Both
  CDHashes go into one new trust module.
- `patches/xnu/t970-wifi-service-mount.patch` (applied after t970-audio-addon-mount.patch) union-mounts /private/var/ngpu/xpc over
  /System/Library/xpc and /private/var/ngpu/wifi-bin over /usr/libexec, only with `t970-wifi-svc=1`. 21H461's
  launchd_cache_loader checks launchd.plist.sig and looks the CDHash up in the trust cache ("cdhash: %s is not in trust cache"). A
  cache that fails that check stops every job, so without the boot argument the stock cache stays in place.
The merged cache is built from Apple's file at staging time and is never committed.

combo10b (int26, t970-wifi-svc=1), 2026-11-08: launchd_cache_loader accepted the merged cache (`status=0`, so the signature and trust
are good), the boot reached SpringBoard and launchd started org.unixnu.wifi.supplicant once. xpcproxy then exited with 78
(EX_CONFIG, `XPC org.unixnu.wifi.supplicant n=1 st=4e00`) before the supplicant ran. That build was arm64e, with ptrauth ABI subtype
USR00 from the iOS 27 SDK, while every binary this board's addon already runs (adrenoprobed, T970Audio) is plain arm64. int27 builds the
supplicant as arm64-apple-ios17.0 and cleans out/wpa-ios17 first (make would keep the arm64e objects). The board's 30 s Wi-Fi
report adds `wifi: service mounts xpc S/E bin S/E` (stage 5 / status 0 = mounted), because the kernel printf reaches only the USB
console.

combo11 (int27 on the GPU session's bosafe addon/trust, t970-wifi-svc=1), 2026-11-09: both mounts reported stage 5, then PID 1 spun in
the kernel and launchd spawned nothing, not even launchd_cache_loader. The bosafe files check out: the launchd cache's detached signature
verifies, its CDHash and the executable's are in trust module 4, and the addon's binary matches. So the cache check was not the
failure. combo10b's xpcproxy exit 78 fits a missing /usr/libexec executable as well as the arm64e ABI. int28 (patch revised):
t970-wifi-svc is a bit mask, 1 = the launchd cache over /System/Library/xpc (combo10b showed it works), 2 = the /usr/libexec mount
(off unless asked). The job runs /private/var/ngpu/wifi-bin/unixnu-wpa_supplicant directly, as adrenoprobed runs from
/private/var/ngpu/bin. `--drop-cdhash` replaces an earlier Wi-Fi trust module instead of stacking another one.

combo12 (int28, t970-wifi-svc=1), 2026-11-09: the boot is fine and launchd runs the job, but every ~3 s "System Policy: xpcproxy deny(1)
process-exec* /private/var/ngpu/wifi-bin/unixnu-wpa_supplicant": the platform policy lets xpcproxy exec launchd jobs only from system
paths (the reason sheng mounted over /usr/libexec). int29: t970-wifi-svc bit 1 (2) now union-mounts wifi-bin/ over /usr/libexec/dtrace
(21H461: four DTrace scripts, referenced by no launchd job) rather than all of /usr/libexec, which hung PID 1 in combo11. The job runs
/usr/libexec/dtrace/unixnu-wpa_supplicant; boot with t970-wifi-svc=3.
Also seen: "Protobox: configd deny(1) iokit-open-service IO80211Interface" (configd's sandbox and our interface's class name), which
matters for IP configuration once the supplicant joins a network.

combo13 (int29, t970-wifi-svc=3), 2026-11-09: hung with colour stripes after both mounts reported stage 5. The same build also carried
other sessions' changes; the bisect boot is t970-wifi-svc=1. int30 avoids a second mount: the signed executable also goes into the
addon's xpc/, and the job runs /System/Library/xpc/unixnu-wpa_supplicant, so the launchd-cache mount alone (t970-wifi-svc=1, fine in
combo10b and combo12) puts it at a system path.

combo13a/combo14, 2026-11-09:
- combo13a: launchd aborted with "No service cache" (21H461 /sbin/launchd at base 0x100ee6000: offset 0x184f8 loads that string
  and 0x3dc34 calls abort_with_reason). launchd_cache_loader passed it no cache.
- combo14 (wifi5, the executable inside the xpc union): PID 1 stayed in the kernel before its first spawn, as in combo11. Symbolicated
  against mach.development.bcm2837.unstripped, the samples sit in polled UFS writes through the file cache (hostWrite4k / pollUfsTransfer
  / delay_for_interval under memory_object_control_uiomove), after the addon install and the mount had finished.
Both boots that hung this way (combo11, combo14) had an executable inside a union over a system directory. int31 avoids that entirely:
- the addon carries only launchd.plist + .sig, in a fresh `wifi-xpc/` directory (combo14 left an executable in /private/var/ngpu/xpc,
  and the addon install never deletes); the old xpc/ entries are removed from the addon;
- the job runs /private/var/ngpu/wifi-bin/unixnu-wpa_supplicant, and patches/xnu/t970-wifi-service-mount.patch now also skips the
  Sandbox policy's exec check (security/mac_vfs.c) for exactly that path with t970-wifi-svc=1. AMFI and the other policies still check
  it. The /usr/libexec/dtrace option is gone.
The launchd cache is byte-identical to wifi3's (CDHash 5c0f8047…), the one combo12 booted with.

combo15 (int31, wifi-xpc holding only the cache), 2026-11-09: PID 1 hung the same way, so an executable in the union was not the trigger.
In every hang (combo11, 14, 15) and in combo13a's "No service cache", the addon install had rewritten the cache files in the same boot
just before the union mount exposed them. The PID-1 samples are a polled UFS write that never completes, and the slot writer, on the same
path, died with it. int32 (patch revised): the install counts the wifi-xpc/ files it wrote, and when there are any, the mount is
deferred to the next boot. That boot uses the stock cache: stage 6, status = files written, kernel printf "launchd cache mount
deferred". Test-compiled: kern_exec.c and mac_vfs.c, patched and unpatched, -fsyntax-only with the kernel's defines and the
xnu-work/EXPORT_HDRS include paths; no new warnings.

combo15a→15b, 2026-11-09: on the same int31 kernel, a boot that rewrote the addon (15a, svc off) followed by an svc=1 boot that wrote
none of the cache files booted with the mount at stage 5. That confirms the defer of int32. The supplicant then passed the Sandbox exec
check, but AMFI refused it: "Launch Constraint Violation ... c[8]p[1]m[1]e[6] (Constraint not matched)", relaunched every ~3 s. An
owner-trusted binary has no platform constraint category. UniXNU's zzyc-owner-skip-launch-constraints.patch is in the kernel, but only as
the global boot argument sheng-no-lc=1. int33: t970_service_exec_exempt() in mac_vfs.c (exact paths
/private/var/ngpu/wifi-bin/unixnu-wpa_supplicant and /private/var/ngpu/bt-bin/BlueTool, t970-wifi-svc=1) now also returns early from
mac_proc_check_launch_constraints. The patch is rebased on the live xnu-work (it carries the MG marker bump) and test-compiled with
-Wall -Wmissing-prototypes.

int34, 2026-11-09 (asked for by the UFS investigation): after the addon install's loop, if it wrote anything, one F_FULLFSYNC
(VNOP_IOCTL, as fcntl does it) on the last written file commits an APFS checkpoint, logged as "t970 addon: full sync rc=N (W written,
last PATH)" and kept in t970_addon_full_sync (-1 = nothing written). combo17 hung in PID 1's UFS writes even with the mount deferred, so the
deferral does not prevent that hang; whatever causes it, the files the install already wrote must survive the hard reset that follows.

combo19 (int34 + the UFS fix, t970-wifi-svc=1), 2026-11-09: scans work in iPadOS ("[wifi-nw] scans=6 networks=41"). Joining failed with
"unsupported association layout: 856 bytes" (type 20 → EINVAL). Decoded from 21H461's IO80211.framework `__performAssociation`
(SystemOS cryptex dyld cache, extracted with Xcode's dsc_extractor): the request is 0x358 bytes, version 1, request base sp+0x230; u16
mode/auth_lower/auth_upper at 4/6/8; SSID length 0x0c, SSID 0x10; BSSID 0x30 (4+2 bytes); key version 0x38; key length 0x3c; cipher 0x40
(10 for a passphrase, whose bytes are copied to 0x48); PMK at 0x1d8 with its length 32 at 0x1d4. That is the legacy (592-byte) network
prefix with a longer tail. int35: T970WifiNet.cpp accepts it as assoc_request17. It logs mode, auth, SSID, part of the BSSID, key
length and cipher, never the key. Since 856 is not the 24A446 656-byte form, the legacy association-done event (no payload) applies.

combo20 (int35), 2026-11-09: the join works. The 21H461 request carried mode 2, auth lower 1 / upper 8, cipher 6 (a 32-byte PMK
derived by wifid); the supplicant authenticated and associated, DHCP gave an address and frames flow both ways. But there is no Wi-Fi icon
and Safari loads nothing. Seen: "Protobox: configd/wifid deny(1) iokit-open-service IO80211Interface", and every Apple80211 request after
the 300th went unlogged, so wifid's post-join queries were invisible. int36:
- NativeSandboxGrantProcess on the IO80211Interface (configd, wifid, wifianalyticsd, WiFiCloudAssetsClient, locationd, sharingd,
  mDNSResponder) and on the controller (configd, wifid). These use the board's IOUserClient grant. The interface still offers no user
  client (the ioctl path serves everything), so the open now fails with "unsupported" rather than a sandbox denial;
- the 60 requests after a successful join are logged whatever the request cap;
- the traffic line counts DNS (UDP 53) and TCP SYN, SYN-ACK and RST per direction, and logs the first few of each with addresses, so
  "connections answered" and "no route" can be told apart.

combo21 (int36), 2026-11-09: IP works. DNS gets answers and TCP to Apple's servers completes the SYN/SYN-ACK handshake, and the
sandbox denials are gone. Still no Wi-Fi icon, and wifid polled APPLE80211_IOC_AUTH_TYPE (2) every 1-4 s. After the join it read SSID,
BSSID, channel, state 4 (RUN), RSSI, noise and the current network, all answered. The auth answer was the problem: a constant upper
0x10 (from the 24A446 port), while 21H461's own join request said upper 8 (WPA2-PSK; lower 1, open system). int37 answers AUTH_TYPE
with the lower/upper of the last join request (all three layouts, and the unattended join as 1/8). In the 21H461 IO80211 `__eventRead`,
socket events carry an interface-name check at +0x18, the event code at +0x14 and the payload at +0x28, so the 32-byte link event is
delivered. IO80211 also has an IOUserClient event path (Apple80211EventMonitoringInitIOUC); our interface offers no user client, which
leaves wifid on the socket path. Still refused: gets 346 (1032 bytes) and 387 (37 bytes), set 471.

combo22 (int37), 2026-11-09: the unified log was decoded from the diag export with tv3.py, extended to resolve shared-cache formats
(offset + 0x180000000 into the dsc_extractor output) and uuid-relative ones (configd's IPConfiguration bundle).
- wifid: "Completed Apple80211AssociateAsync (0)", "Manually Joined", "Set password", "Updated associated network ... wpa2-personal",
  "{LINK+} link up". wifid does consider the join complete.
- wifid: "{IP-} IPv4 Primary interface is (null)" throughout, and configd's IPMonitor never reported an IPv4 primary ("network changed"
  only for DNS/Proxy). configd set "SetInterfaceRank(en0) = Never" at start, then wifid set Default repeatedly.
- IPConfiguration: DHCP "publish success { IPv4, DNS, DHCP }", MANUAL "publish success { IPv4 }", but also "failed to create
  /var/db/dhcpclient/leases, Read-only file system (30)".
- wifid: "SCPreferencesCommitChanges() Failed!", "_CFPropertyListCreateBinaryData: CFPropertyListWrite ... 3851", "Fetched 0 known
  networks from the plist", WiFiSecurity -25300. The network and its password are not persisted.
- wifid: "Apple80211SetProperty: failed to set IO80211InterfaceIPConfigurationBusy / IPV6Params / AppState / HeartBeat / RoamParams /
  WifidEnumeratedInfra / ExternelPowerState / BSP_CommandParams: errno 102" = our refused set 383 (an XML plist {key: value}).
int38: set 383 accepted, with each key logged once; sets 470/471/473 accepted; SIOCGIFDEVMTU answered (1500, 1280..1500).
The EROFS and SCPreferences failures say parts of /private/var are not writable for configd and wifid; that is a Data-volume or
mount matter outside the Wi-Fi driver.
