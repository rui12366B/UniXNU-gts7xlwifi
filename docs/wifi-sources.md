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
