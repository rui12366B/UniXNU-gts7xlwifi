// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
// Stage-1 translation for the display's apps_smmu context bank, so the DPU pipelines fetch the
// compositor's surface pages directly (direct scanout, "zero copy") instead of a copy of them.
//
// Ported from ShengUfs (tmp/ShengUfs-parity.cpp, `dpuEnable` / `native_ufs_dpu_map`). Three
// things differ on this board and are parameters here, not constants:
//
//   * The display streams. Sheng's device tree routes them as 0x1c00 / 0x1c02; SM8250 uses
//     0x820 / 0x821 (reference/k40-kernel/.../kona-sde.dtsi:276,284,
//     `iommus = <&apps_smmu 0x820 0x402>` and `<&apps_smmu 0x821 0x400>`).
//   * The boot loader's scanout address. Sheng's is 0xb8000000; this board's GOP mapping is at
//     0x9c000000 (`GOP actual mapping ... physical=9c000000`). It has to keep working unchanged
//     after the table is installed, because a failure has to be able to fall back to it.
//   * Nothing else: the window, the slot geometry and the TCR2/T0SZ/MAIR values are SMMUv2
//     facts, not board facts.
//
// Everything here is off unless the caller passes a non-zero `scanout`. A refusal at any step
// puts a reason in `native_ufs_dpu_state` and leaves the bank exactly as it was found.
//
// Include inside the anonymous namespace, after read32/write32 exist. The three globals this header reads and writes are declared
// and defined at file scope in NativeUfs.cpp, outside every namespace: a declaration inside the anonymous namespace would give them
// internal linkage and the linker would not find the symbols the display driver imports.

extern "C" volatile uint32_t native_ufs_dpu_smmu[24];
extern "C" volatile uint32_t native_ufs_dpu_state;
extern "C" volatile uint32_t native_ufs_dpu_check[8];

// L1, then kDpuSlots * kDpuSlotTables L3 tables (one per 2 MiB of a slot). The window sits above
// 0x10000000 so it cannot collide with the identity blocks that keep the boot scanout alive.
constexpr uint32_t kDpuWindow = 0x10000000U;
constexpr uint32_t kDpuSlots = 4;
constexpr uint32_t kDpuSlotBytes = 32U << 20;
constexpr uint32_t kDpuSlotTables = kDpuSlotBytes >> 21;
// The fault bits, exactly as arm-smmu.h:214-222 defines ARM_SMMU_CB_FSR_FAULT:
//   MULTI BIT(31) | SS BIT(30) | UUT BIT(8) | EF BIT(4) | PF BIT(3) | TF BIT(1) | IGN
//   IGN = AFF BIT(2) | ASF BIT(7) | TLBMCF BIT(5) | TLBLKF BIT(6)
// FORMAT (GENMASK(10,9)) is not a fault and is not in this mask, which is why 0xc00001fe leaves bits 9 and 10 clear.
constexpr uint32_t kDpuFaults = 0xc00001feU;

static uint64_t *gDpuTables;
static uintptr_t gDpuCb;
static uintptr_t gDpuLiveCb;
static IOLock *gDpuLock;

static void dpuClean (const void *start, size_t length)
{
  for (size_t off = 0; off < length; off += 64)
    __asm__ volatile ("dc civac, %0" :: "r" (static_cast<const uint8_t *> (start) + off) : "memory");
  __asm__ volatile ("dsb sy" ::: "memory");
}

static bool dpuTlbFlush (uintptr_t cb)
{
  write32 (cb, 0x618, 0); write32 (cb, 0x7f0, 0);   // TLBIALL, TLBSYNC
  __asm__ volatile ("dsb sy" ::: "memory");
  for (unsigned spin = 0; spin < 100000; ++spin) if (!(read32 (cb, 0x7f4) & 1)) return true;
  return false;
}

// Installs the table on one context bank. `bootIova` is the address the boot loader is scanning
// out of right now; the table must leave it translating to itself.
static void dpuEnable (uintptr_t cb, uint32_t bank, uint32_t oldSctlr, uint32_t bootIova)
{
  const size_t bytes = (2 + kDpuSlots * kDpuSlotTables) * 4096;
  IOBufferMemoryDescriptor *tables = IOBufferMemoryDescriptor::inTaskWithOptions (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone), bytes, 4096);
  if (!gDpuLock) gDpuLock = IOLockAlloc ();
  if (!tables || !gDpuLock) { native_ufs_dpu_state = 0xdead0002U; return; }
  uint64_t *l1 = static_cast<uint64_t *> (tables->getBytesNoCopy ());
  const uint64_t pa = tables->getPhysicalSegment (0, nullptr, kIOMemoryMapperNone);
  bzero (l1, bytes);
  // Normal non-cacheable (attribute 0 = 0x44), not shareable, access flag, readable and writable at both privilege levels.
  const uint64_t attrs = (1ULL << 10) | (1ULL << 6);
  // i564 (Sheng): the first fetch faulted at once with FSYNR0 level 0, so the walk may start at level 0 although T0SZ 32 means
  // level 1. One table serves every level: [0] points back at itself (level 0 -> level 1, level 1 -> level 2 for the window),
  // [2] and [3] are the 1 GiB identity blocks, [128 + t] the window's level 3 tables. The second page stays unused.
  l1[0] = pa | 3;
  l1[2] = 0x80000000ULL | attrs | 1;
  l1[3] = 0xc0000000ULL | attrs | 1;
  for (uint32_t t = 0; t < kDpuSlots * kDpuSlotTables; ++t) l1[(kDpuWindow >> 21) + t] = (pa + (2 + t) * 4096ULL) | 3;
  dpuClean (l1, bytes);
  // The identity blocks only cover 0x80000000..0xffffffff. A boot scanout outside that range would fault on the first fetch,
  // so refuse before touching the bank rather than after the display stops.
  if (bootIova && !(bootIova >= 0x80000000U && bootIova < 0xffffffffU)) { native_ufs_dpu_state = 0xdead0006U; return; }
  const uint32_t old[5] = { read32 (cb, 0x20), read32 (cb, 0x24), read32 (cb, 0x10), read32 (cb, 0x30), read32 (cb, 0x38) };
  gDpuLiveCb = cb;
  write32 (cb, 0x20, static_cast<uint32_t> (pa)); write32 (cb, 0x24, static_cast<uint32_t> (pa >> 32));
  // TCR2 (PASIZE bits 2:0, SEP bits 17:15): 48-bit output and SEP 7 (the sign extension comes from the upstream address, as
  // arm-smmu's AArch64 contexts set it). SEP 0 takes IA[31] as the sign bit, so every address with bit 31 set but zero above it
  // (the display's own framebuffer, and the first fetch of every earlier try) is out of TTBR0's range and faults at level 0.
  write32 (cb, 0x10, 5U | (7U << 15));
  write32 (cb, 0x30, 32U | (1U << 23));               // T0SZ 32, non-cacheable walks, 4 KiB granule, no TTBR1
  write32 (cb, 0x38, 0x44);                           // MAIR attribute 0: normal non-cacheable
  __asm__ volatile ("dsb sy" ::: "memory");
  volatile uint32_t *c = native_ufs_dpu_check;
  c[0] = read32 (cb, 0x20); c[1] = read32 (cb, 0x30); c[2] = read32 (cb, 0x58);
  const auto undo = [&] (uint32_t why) {
    write32 (cb, 0x0, oldSctlr);
    __asm__ volatile ("dsb sy; isb" ::: "memory");
    write32 (cb, 0x20, old[0]); write32 (cb, 0x24, old[1]); write32 (cb, 0x10, old[2]); write32 (cb, 0x30, old[3]); write32 (cb, 0x38, old[4]);
    const uint32_t fsr = read32 (cb, 0x58);
    if (fsr & (1U << 30)) write32 (cb, 0x8, 1);        // RESUME: terminate a stalled transaction
    if (fsr & kDpuFaults) write32 (cb, 0x58, fsr);     // write one to clear
    dpuTlbFlush (cb);
    native_ufs_dpu_state = why;
  };
  if (c[0] != static_cast<uint32_t> (pa) || c[1] != (32U | (1U << 23))) { undo (0xdead0003U); return; }   // the bank ignores our writes
  if (!dpuTlbFlush (cb)) { undo (0xdead0004U); return; }
  write32 (cb, 0x0, 1U | (1U << 5) | (1U << 6));      // SCTLR: M, CFRE, CFIE; a fault terminates instead of stalling the display
  __asm__ volatile ("dsb sy; isb" ::: "memory");
  // No check translation here (Sheng i563: IDR0 bit 26 ATOSNS, the SMMU has no ATOS), so watch the display's own fetches for
  // 50 ms (6 frames at 120 Hz) and turn translation off again at the first fault.
  uint32_t fsr = 0, us = 0;
  for (; us < 50000; us += 500) {
    fsr = read32 (cb, 0x58);
    if (fsr & kDpuFaults) break;
    IODelay (500);
  }
  c[3] = fsr; c[7] = us;
  if (fsr & kDpuFaults) {
    c[4] = read32 (cb, 0x60); c[5] = read32 (cb, 0x64); c[6] = read32 (cb, 0x68);
    undo (0xdead0005U); return;
  }
  gDpuTables = l1; gDpuCb = cb;
  native_ufs_dpu_state = 0x80000000U | bank;
}

// Reads the display's streams out of the SMR table (never writes), then, when `scanout` is set, gives the bank they route to a
// stage 1 table. `streams` are the app SMMU stream ids the device tree gives the display context banks.
static void dpuWindowSetup (uintptr_t smmu, const uint16_t *streams, unsigned streamCount, uint32_t scanout, uint32_t bootIova)
{
  const uint32_t idr0 = read32 (smmu, 0x20), idr1 = read32 (smmu, 0x24);
  unsigned groups = idr0 & 255;
  if (groups > 128) groups = 128;
  const unsigned banks = idr1 & 255;
  const unsigned pages = 1U << (((idr1 >> 28) & 7) + 1);
  native_ufs_dpu_smmu[1] = read32 (smmu, 0x0);
  native_ufs_dpu_smmu[2] = idr0;
  native_ufs_dpu_smmu[3] = idr1;
  unsigned valid = 0;
  for (unsigned k = 0; k < streamCount && k < 2; ++k) native_ufs_dpu_smmu[4 + 9 * k] = 0xffffffffU;
  for (unsigned i = 0; i < groups; ++i) {
    const uint32_t smr = read32 (smmu, 0x800 + 4 * i);
    if (!(smr & 0x80000000U)) continue;
    ++valid;
    const uint32_t mask = (smr >> 16) & 0x7fff;
    for (unsigned k = 0; k < streamCount && k < 2; ++k) {
      volatile uint32_t *r = &native_ufs_dpu_smmu[4 + 9 * k];
      if (r[0] != 0xffffffffU || ((smr ^ streams[k]) & 0x7fff & ~mask)) continue;
      const uint32_t s2cr = read32 (smmu, 0xc00 + 4 * i), bank = s2cr & 255;
      r[0] = i | (bank << 16); r[1] = smr; r[2] = s2cr;
      if (((s2cr >> 16) & 3) == 0 && bank < banks && (pages + banks) * 4096 <= 0xd0000) {
        const uintptr_t cb = smmu + (pages + bank) * 4096;
        r[3] = read32 (smmu, 0x1000 + 4 * bank); r[4] = read32 (smmu, 0x1800 + 4 * bank);
        r[5] = read32 (cb, 0x0); r[6] = read32 (cb, 0x20); r[7] = read32 (cb, 0x24); r[8] = read32 (cb, 0x30);
        native_ufs_dpu_smmu[22 + k] = read32 (cb, 0x58);
      }
    }
  }
  native_ufs_dpu_smmu[0] = 0x80000000U | groups | (valid << 8) | (banks << 16);
  // The three conditions Sheng checks before writing: the bank is stage-1 translating (CBAR type 1), stage 1 is currently off
  // (SCTLR bit 0 clear), and the bank's TTBR0 was left in the firmware's shape.
  const uint32_t found = native_ufs_dpu_smmu[4], bank = found >> 16;
  if (scanout && found != 0xffffffffU && bank < banks && ((native_ufs_dpu_smmu[6] >> 16) & 3) == 0 &&
      !(native_ufs_dpu_smmu[9] & 1) && ((native_ufs_dpu_smmu[7] >> 16) & 3) == 1) {
    dpuEnable (smmu + (pages + bank) * 4096, bank, native_ufs_dpu_smmu[9], bootIova);
  } else if (scanout) {
    native_ufs_dpu_state = 0xdead0001U;
  }
}

// The display bank's SCTLR (reg 0) or FSR (reg 1) now, for the log; all ones before the bank was touched.
extern "C" uint32_t native_ufs_dpu_live (unsigned reg)
{
  return gDpuLiveCb ? read32 (gDpuLiveCb, reg ? 0x58 : 0x0) : 0xffffffffU;
}

// Points slot (0 .. kDpuSlots-1) of the display's window at count 4 KiB pages and returns the slot's address for the display, 0
// when translation is off or the pages do not fit. The caller makes sure the display is not fetching from the slot while it changes.
extern "C" uint32_t native_ufs_dpu_map (unsigned slot, const uint64_t *pages, unsigned count)
{
  if (!gDpuTables || slot >= kDpuSlots || count > kDpuSlotBytes / 4096) return 0;
  uint64_t *l3 = gDpuTables + 1024 + static_cast<size_t> (slot) * kDpuSlotTables * 512;
  const uint64_t attrs = (1ULL << 10) | (1ULL << 6);
  for (unsigned i = 0; i < kDpuSlotTables * 512; ++i) {
    if (i < count && (pages[i] & 0xfff)) return 0;
    l3[i] = i < count ? (pages[i] | attrs | 3) : 0;
  }
  dpuClean (l3, kDpuSlotTables * 4096);
  IOLockLock (gDpuLock);
  const bool synced = dpuTlbFlush (gDpuCb);
  IOLockUnlock (gDpuLock);
  return synced ? kDpuWindow + slot * kDpuSlotBytes : 0;
}


// The window build itself. Called through the forwarder in NativeUfs.cpp, which the exported entry point below the namespace uses.
static int dpuWindowBuildImpl (uintptr_t smmu)
{
  if (!smmu) return 0;
  if (native_ufs_dpu_state >> 31) return 1;                       // already built
  uint32_t dpu = 0, scanout = 0;
  PE_parse_boot_argn ("dpu", &dpu, sizeof (dpu));
  PE_parse_boot_argn ("dpu-scanout", &scanout, sizeof (scanout));
  if (dpu < 2 && !scanout) return 0;                              // the board did not ask for it
  uint32_t s1 = 0x820, s2 = 0x821;
  PE_parse_boot_argn ("native-dpu-stream", &s1, sizeof (s1));
  PE_parse_boot_argn ("native-dpu-stream2", &s2, sizeof (s2));
  uint32_t bootIova = 0x9c000000U;
  PE_parse_boot_argn ("native-dpu-boot-iova", &bootIova, sizeof (bootIova));
  const uint16_t streams[2] = { static_cast<uint16_t> (s1), static_cast<uint16_t> (s2) };
  dpuWindowSetup (smmu, streams, 2, scanout, bootIova);
  return (native_ufs_dpu_state >> 31) ? 1 : 0;
}
