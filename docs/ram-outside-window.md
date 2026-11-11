# DRAM outside the kernel's window (UniXNU 8f5a4e9 on iPadOS 17.7.11)

Status 2026-11-11: prepared, compiled (5 changed xnu files test-compiled with the main1 tree's own flags), **not run on hardware**.

## Does upstream's mechanism apply to the 21H461 profile?

Not as shipped. Upstream added `window-outside-memory-adoption.patch` only to the macOS profile
(`profiles/os/macos-27.3-26C5035r/patches/xnu-12377/`). The iPadOS 17.7.11 profile builds xnu-10063.141.1 (BCM2837 config:
no PPL, no KTRR/CTRR, no ARM_LARGE_MEMORY), so the patch has to be ported. Differences that mattered:

| upstream (xnu-12377) | here (xnu-10063) |
| --- | --- |
| `vm_page_create_canonical()` + page radix tree | no radix tree; `vm_page_create(pn, pn + 1)` is what `ml_static_mfree` already uses: a zone vm_page that keeps its own page number (`VM_PAGE_GET_PHYS_PAGE`) |
| aperture VA reserved because the kernel map reserves VM_MIN..first dynamic address | 10063's `kmem_init` reserves only `pmap_virtual_region()`s; region 3 is `VM_MIN_KERNEL_ADDRESS..LOW_GLOBAL_BASE_ADDRESS` (0xffffffe0_00000000..0xfffffff0_00000000), so the VA is placed there: `LOW_GLOBAL_BASE_ADDRESS - 4 GiB - ROUND_TWIG(span)` |
| `ROUND_L1(span)` | `ROUND_TWIG(span)`: an L1 entry is 64 GiB with 16 KiB pages and would leave region 3 |
| `arm_vm_excluded_range_t` | own `arm_vm_outside_range_t` (10063 has no excluded-range table) |
| default on when the property exists | **opt-in**: `memory-outside=1`; `memory-outside-floor=ADDR` adopts only what lies at or above ADDR |

The rest (pa_valid/pa_index/pai_to_pa, per-page tables grown by the span, adopted-page bitmap, pmap_object offsets by
pa_index, `pmap_adopt_outside_page` with one probe load, the sleh hooks, `STARTUP(EARLY_BOOT)` adoption, memory size
accounting) is upstream's, re-anchored on 10063.

DarwinBoot: firmware-t970's own DarwinBoot (`LocalPackages/DarwinBootPkg`, the one `loader = "firmware"` runs) has no
`DarwinOutsideRanges`, and the firmware map keeps all of this DRAM reserved anyway (ProtectedLow2/3, ProtectedMid), so
even upstream's DarwinBoot would list nothing (it lists only EfiConventionalMemory). The runs are therefore written
statically into the ADT by board.toml. No firmware or boot-partition change.

## The DRAM map below the window

Window: firmware-t970 `PlatformMemoryMap.c` "XNU window" 0x10AD00000..0x200000000 (+ t970-dram-extend to 0x280000000),
i.e. gPhysBase = 0x10AD00000, MEM totalMiB = 5969. Below it (sources: `evidence/device-preflight.json` iomem from TWRP,
`evidence/recovery-live.dtb` /reserved-memory, PlatformMemoryMap.c):

| range | owner | adopt? |
| --- | --- | --- |
| 0x80000000-0x80600000 | hyp_region (QHEE), no-map | no |
| 0x80603000-0x80894000 | xbl_aop, cmd DB 0x80860000, uefi log, no-map | no |
| 0x80900000-0x80B00000 | SMEM (ADSP/sensors/Wi-Fi use it) | no |
| 0x80B00000-0x88400000 | removed_region (TZ/modem), no-map | no |
| **0x88400000-0x89200000** | Android System RAM | **yes, 14 MiB** |
| 0x89200000-0x90A00000 | pil_slpi/adsp/spss/npu/video/cvp/cdsp/camera/wlan_fw/ipa, GPU zap 0x9031A000, cdsp_secure_heap; no-map | no (the 0x9031C000 0.9 MiB scrap skipped) |
| **0x90A00000-0x9C000000** | Android System RAM | **yes, 182 MiB** |
| 0x9C000000-0x9E400000 | cont_splash/disp_rdump/dfps = GOP scanout | no |
| **0x9E400000-0x9F000000** | Android System RAM | **yes, 12 MiB** |
| 0x9F000000-0x9FA00000 | sec_debug_region (no-map), ss_plog | no |
| 0x9FA00000-0xA0400000 | Linux-reserved scraps; 0xA0000000+4 MiB = Wi-Fi SMMU arena IOVA (MSI sink 0xA03FF000) | no |
| **0xA0400000-0xB0100000** | Android System RAM (the Android kernel's own image lived here; irrelevant to us) | **yes, 253 MiB** |
| 0xB0100000-0xB1C00000 | gap + Samsung uh_heap_region | no |
| **0xB1C00000-0xBBB00000** | Android System RAM | **yes, 159 MiB** |
| 0xBBB00000-0xC0000000 | not in Android's memory node | no |
| 0xC0000000-0xD0000000 | UEFI DXE heap / stack / FD / runtime (DarwinBoot's own allocations) | no |
| **0xD0000000-0xFFC00000** | Android System RAM (Linux dynamic CMA/secure pools at the top are Linux's own choice); ran as the kernel window in v26 | **yes, 764 MiB** |
| 0xFFC00000-0x10AD00000 | Linux gap + sec_debug_rdx_bootdev | no |

Only memory Android itself treats as System RAM is listed. The modem/ADSP/CDSP/TZ carve-outs are the no-map gaps and stay
untouched. On SM8250 a refused access (XPU / QHEE stage 2) tends to end in a TrustZone `TZBSP_ERR_FATAL_NOC_ERROR` warm
reset (seen in v63/v64), not a synchronous abort, so upstream's probe load cannot be relied on to stop at a secure region.

## Driver fixed addresses (`darwinboot-memory-outside-kept`, = upstream's `owned_memory_outside_reserved`)

None overlaps a listed run; they are listed so a later, wider table cannot take them.

| base + size | user |
| --- | --- |
| 0x80860000 + 0x20000 | RPMh command DB (Adsp, Sensors, Wi-Fi, touch, GpuPowerBoard) |
| 0x80900000 + 0x200000 | SMEM (T970Adsp, T970Sensors) |
| 0x89200000 + 0x1500000 | pil_slpi (T970Sensors) |
| 0x8A700000 + 0x2C00000 | pil_adsp (T970Adsp) |
| 0x8DE00000 + 0x500000 | pil_cvp: T970Audio's DSP buffer (kDefaultPa) |
| 0x8E300000 + 0x1A00000 | pil_cdsp: T970Camera frame (kDefaultFramePa) |
| 0x9031A000 + 0x2000 | GPU zap (GpuProbe kZapPhys) |
| 0x9C000000 + 0x2400000 | GOP scanout (DpuWindow bootIova, NativeLegacyDisplay) |
| 0x9F000000 + 0xA00000 | sec_debug_region, ss_plog |
| 0xA0000000 + 0x400000 | Wi-Fi SMMU arena IOVA (T970WifiProbe/Stack/Ath11k), PA-equal if the stream were ever in bypass |
| 0xB0100000 + 0x1B00000 | uh_heap and the gap below it |
| 0xC0000000 + 0x10000000 | UEFI DXE heap / FD |
| 0xFFC00000 + 0xB100000 | Linux gap + sec_debug_rdx (same as t970-dram-holes) |

## Changes (device repo, nothing committed)

- `patches/xnu/t970-memory-outside.patch` (new, last in `patches/xnu/series`): the 10063 port. Generated against the
  main1 tree's `.cache/xnu-work` (all board patches applied); `patch -p1 --dry-run` clean there.
  Files: osfmk/arm/pmap/{pmap.c,pmap_data.c,pmap_data.h}, osfmk/arm64/{arm_vm_init.c,machine_routines.c,sleh.c}, osfmk/vm/pmap.h.
- `board.toml [adt.chosen.bytes]`: `darwinboot-memory-outside` (6 runs, 1384 MiB) and `darwinboot-memory-outside-kept`
  (13 ranges). Harmless without `memory-outside=1`. Do **not** also set `payload.adt owned_memory_outside_ranges`: it would
  emit a second, zeroed `darwinboot-memory-outside` that this firmware's DarwinBoot never fills.
- Unchanged: `t970-dram-extend.patch`, `t970-dram-holes.patch`, `t970-dram-end=0x280000000`, `/chosen/t970-dram-holes`,
  firmware-t970, boot partition.

## Boot args and expected totals

| step | args | adds | expected MEM totalMiB |
| --- | --- | --- | --- |
| A | `memory-outside=1 memory-outside-floor=0xd0000000` | 0xD0000000+764 MiB | ~6730 (6.57 GiB) |
| B | `memory-outside=1` | all six runs, 1384 MiB | ~7350 (7.18 GiB) |

Minus ~5 MiB of per-page pmap tables for the 1.9 GiB span (taken from the window). Kernel log line:
`memory outside the span: N pages adopted, M refused` (M should be 0). The line costs 16 / 48 characters of the
1000-byte command line (main1 is at ~927).

## Risks

1. **Secure memory**: if any listed page is XPU/QHEE-protected in our boot, the expected result is a NoC-error reset
   (check `/proc/reset_history` in TWRP), not a refused probe. Step A only touches memory our kernel already ran in (v26).
2. **First touch of 0x88400000..0xBBB00000**: never used by any of our kernels; Android uses it as plain RAM. Step B.
3. **Upstream's own caveat** (sheng): with adoption on, "contiguous kernel allocations failed in some boots and the GPU did
   not start"; sheng keeps it off by default. On 10063 `vm_page_find_contiguous` scans only the linear array, so adopted
   pages never serve contiguous allocations; watch GPU, Wi-Fi, BT and ADSP bring-up rows anyway.
4. Aperture VA in region 3 is new on this kernel (upstream used a different reserve); a fault there panics with
   "Unexpected fault in kernel physical aperture (memory outside)".
5. Hang before console: the patch does nothing unless `memory-outside=1`, so dropping the argument is the rollback.
