// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native UFS bring-up. Default-off: no MMIO is touched unless a probe or an
// explicitly selected root route is present.
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#if !defined(NATIVE_UFS_PROBE_ONLY) || !NATIVE_UFS_PROBE_ONLY
#include <IOKit/storage/IOBlockStorageDevice.h>
#endif
#include <libkern/crypto/sha2.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <sys/param.h>
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
#include "verified_root.h"
#include "VerifiedBlockPolicy.h"
extern "C" {
extern const struct vr_crypto vr_xnu_crypto;
extern const uint8_t verified_root_key[];
}
#endif
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSIterator.h>
#include <IOKit/IOBSD.h>
#include <kern/thread.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <kern/debug.h>
#include "GccUfs.h"
#include "RootDiagnostics.h"
#include "TransferPoll.h"
#include "SmmuTakeover.h"
#include "WholeUserdataRoot.h"
#include "WholeUserdataDisk.h"

static UfsRootDiagnostics gRootDiagnostic;
static bool gRootDiagnosticEnabled;
static void rootDiagnosticFailure (const char *reason);

struct UfsProbeStreamSnapshot {
  uint16_t sid;
  uint32_t smr, s2cr;
};

struct UfsProbeSnapshot {
  uint32_t cap, hcs, hce, utrlba, utrlbau, doorbell, rsr;
  UfsProbeStreamSnapshot stream[2];
  uint32_t gfsr;
};

static UfsProbeSnapshot probeSnapshot (uintptr_t hc, uintptr_t smmu,
                                       uint16_t primary, uint16_t secondary);

struct UfsStreamConfig {
  uint16_t primary;
  uint16_t secondary;
};

// The sheng board routes the UFS host through SMMU stream 0x60. Keep that
// default, and let a board with the generic qcom,ufshc binding select one or
// two streams. SM8250 UFS uses both 0x0e0 and 0x4e0.
static UfsStreamConfig ufsStreams (void)
{
  UfsStreamConfig config = { 0x60, 0 };
  uint32_t stream = 0, stream2 = 0;
  if (PE_parse_boot_argn ("native-ufs-stream", &stream, sizeof (stream)) && stream && stream <= 0xffff) {
    config.primary = static_cast<uint16_t> (stream);
  }
  if (PE_parse_boot_argn ("native-ufs-stream2", &stream2, sizeof (stream2)) &&
      stream2 && stream2 <= 0xffff && stream2 != config.primary) {
    config.secondary = static_cast<uint16_t> (stream2);
  }
  return config;
}

static uint16_t ufsStream (void)
{
  return ufsStreams ().primary;
}

extern "C" {
// Optional passive observer; absent in ordinary builds. It may run while a
// window lock is held, so its contract forbids blocking or transport access.
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
extern "C" void boot_progress_poll (void) __attribute__((weak_import));
#endif
volatile uint32_t native_ufs_probe_calls = 0;
// The display's stage 1 window (DpuWindow.h, from FFT-infra/NativeUfs-alioth d6bd207: same SM8250 display streams 0x820/0x821 and
// the same boot scanout at 0x9c000000 here). Defined at file scope outside every namespace: the display driver imports them by these
// names. native_ufs_dpu_state bit 31 means "translation installed"; NativeLegacyDisplay checks that bit before it takes the direct path.
volatile uint32_t native_ufs_dpu_smmu[24] = {};
volatile uint32_t native_ufs_dpu_state = 0;
volatile uint32_t native_ufs_dpu_check[8] = {};
volatile uint32_t native_ufs_matched = 0;
volatile uint32_t native_ufs_probe_requested = 0;
volatile uint32_t native_ufs_mapped = 0;
volatile uint32_t native_ufs_status = 0;
// Root start: 1 entered, 2 host up, 3 layout found; the last soft-failure reason (a static string), for the boot page.
volatile uint32_t native_ufs_root_step = 0;
const char *volatile native_ufs_root_failure = nullptr;
volatile uint32_t native_ufs_cap = 0;
volatile uint32_t native_ufs_version = 0;
volatile uint32_t native_ufs_hcs = 0;
volatile uint32_t native_ufs_hce = 0;
volatile uint32_t native_ufs_utrl_ready = 0;
volatile uint32_t native_ufs_uic_status = 0;
volatile uint32_t native_ufs_uic_result = 0;
}

class NativeUfs : public IOService {
  OSDeclareDefaultStructors (NativeUfs)
  OSSharedPtr<IOMemoryMap> fMap;
  OSSharedPtr<IOBufferMemoryDescriptor> fCmdDesc;
  void inspect (void);
  void adoptAndRead (unsigned level);
  void dumpFirmwareList (void);
  void inspectSmmu (void);
  void inspectContextBanks (void);
#if !defined(NATIVE_UFS_PROBE_ONLY) || !NATIVE_UFS_PROBE_ONLY
  bool startRoot (IOService *provider);
  bool startWholeUserdataRoot (IOService *provider);
#endif
  static bool waitSet (uintptr_t base, unsigned offset, uint32_t mask, unsigned loops);
public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void stop (IOService *provider) override;
};

OSDefineMetaClassAndStructors (NativeUfs, IOService)

static uint32_t read32 (uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *> (base + offset);
}

static UfsProbeSnapshot probeSnapshot (uintptr_t hc, uintptr_t smmu,
                                       uint16_t primary, uint16_t secondary)
{
  UfsProbeSnapshot snapshot = {};
  const uint16_t configured[2] = { primary, secondary };
  snapshot.cap = read32 (hc, 0x00);
  snapshot.hcs = read32 (hc, 0x30);
  snapshot.hce = read32 (hc, 0x34);
  snapshot.utrlba = read32 (hc, 0x50);
  snapshot.utrlbau = read32 (hc, 0x54);
  snapshot.doorbell = read32 (hc, 0x58);
  snapshot.rsr = read32 (hc, 0x60);
  snapshot.gfsr = read32 (smmu, 0x48);
  unsigned count = read32 (smmu, 0x20) & 0xff;
  if (count > 128) {
    count = 128;
  }
  for (unsigned stream = 0; stream < 2; ++stream) {
    snapshot.stream[stream].sid = configured[stream];
    if (!configured[stream]) {
      continue;
    }
    for (unsigned index = 0; index < count; ++index) {
      uint32_t smr = read32 (smmu, 0x800 + 4 * index);
      if (ufsSmmuStreamMatches (smr, configured[stream])) {
        snapshot.stream[stream].smr = smr;
        snapshot.stream[stream].s2cr = read32 (smmu, 0xc00 + 4 * index);
        break;
      }
    }
  }
  return snapshot;
}

static void write32 (uintptr_t base, unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *> (base + offset) = value;
}

static bool contains (const char *data, size_t length, const char *needle)
{
  size_t wanted = 0;
  while (needle[wanted]) {
    wanted++;
  }
  for (size_t at = 0; at + wanted <= length; at++) {
    size_t index = 0;
    while (index < wanted && data[at + index] == needle[index]) {
      index++;
    }
    if (index == wanted) {
      return true;
    }
  }
  return false;
}

IOService *NativeUfs::probe (IOService *provider, SInt32 *score)
{
  native_ufs_probe_calls = native_ufs_probe_calls + 1;
  if (OSDynamicCast (IOPlatformExpertDevice, provider)) {
    // Second personality (KernelConfigTables: IOMatchCategory NativeUfsRoot): attaches to the
    // root device, which starts before the platform expert publishes IOResources, so the
    // hardware probes run before the original AMFI (whose start panics without a PMGR).
    native_ufs_matched = 1;
    if (score) {
      *score = 500;
    }
    return this;
  }
  OSObject *property = provider->getProperty ("compatible");
  OSData *data = OSDynamicCast (OSData, property);
  OSString *string = OSDynamicCast (OSString, property);
  bool match = false;
  // The board tree names the controller with its own SoC string. sm8250 publishes
  // "qcom,sm8250-ufshc", which is neither "qcom,sm8550-ufshc" nor a superstring of
  // "qcom,ufshc", so listing only the SM8550 identity made probe() return nullptr on
  // every alioth boot and start() never ran. The transport below is addressed by
  // physical windows (GccUfs.h), not by the match, so the same code serves both SiPs.
  if (data) {
    const char *text = static_cast<const char *> (data->getBytesNoCopy ());
    match = text && (contains (text, data->getLength (), "qcom,sm8550-ufshc") ||
                     contains (text, data->getLength (), "qcom,sm8250-ufshc") ||
                     contains (text, data->getLength (), "qcom,ufshc"));
  } else if (string) {
    const char *text = string->getCStringNoCopy ();
    match = text && (contains (text, string->getLength (), "qcom,sm8550-ufshc") ||
                     contains (text, string->getLength (), "qcom,sm8250-ufshc") ||
                     contains (text, string->getLength (), "qcom,ufshc"));
  }
  if (!match) {
    return nullptr;
  }
  native_ufs_matched = 1;
  if (score) {
    *score = 1000;
  }
  return this;
}

bool NativeUfs::waitSet (uintptr_t base, unsigned offset, uint32_t mask, unsigned loops)
{
  for (unsigned index = 0; index < loops; index++) {
    if ((read32 (base, offset) & mask) == mask) {
      return true;
    }
    IODelay (1);
  }
  return false;
}

// Read-only inspection (native-ufs-probe=2): reports what firmware left behind.
// The physical windows are the sheng board DT's own GCC (0x100000) and UFS host
// (0x1d84000, 0x3000) ranges. The controller is read only if the GCC says its
// GDSC is powered and the AXI/AHB branches are running; nothing is written.
static uintptr_t mapPhys (uint64_t phys, uint64_t length, OSSharedPtr<IOMemoryMap> &keep)
{
  IODeviceMemory *memory = IODeviceMemory::withRange (phys, length);
  if (!memory) {
    return 0;
  }
  keep = OSSharedPtr<IOMemoryMap> (memory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  memory->release ();
  return keep ? static_cast<uintptr_t> (keep->getVirtualAddress ()) : 0;
}

static void requireDualStreamReuse (unsigned probe, unsigned rootMode, const char *where)
{
  UfsStreamConfig streams = ufsStreams ();
  if (!streams.secondary) return;
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t smmu = mapPhys (0x15000000ULL, 0xd0000, map);
  if (!smmu) panic ("NATIVE_UFS dual stream: SMMU map failed");
  uint16_t configured[2] = { streams.primary, streams.secondary };
  for (unsigned index = 0; index < 2; ++index) {
    UfsSmmuReuse reuse = reuseUfsSmmuStream (configured[index],
      [smmu](unsigned offset) { return read32 (smmu, offset); });
    if (reuse.error) {
      panic ("NATIVE_UFS dual stream reuse refused: %s sid=%04x slot=%x bank=%x refs=%u smr=%08x s2cr=%08x cbar=%08x cba2r=%08x sctlr=%08x where=%s probe=%u root=%u",
             reuse.error, configured[index], reuse.slot, reuse.bank, reuse.references,
             reuse.smr, reuse.s2cr, reuse.cbar, reuse.cba2r, reuse.sctlr,
             where, probe, rootMode);
    }
    IOLog ("NativeUfs: dual stream reuse sid=%04x slot=%x bank=%x refs=%u smr=%08x s2cr=%08x cbar=%08x cba2r=%08x sctlr=%08x\n",
           configured[index], reuse.slot, reuse.bank, reuse.references,
           reuse.smr, reuse.s2cr, reuse.cbar, reuse.cba2r, reuse.sctlr);
  }
}

void NativeUfs::inspect (void)
{
  OSSharedPtr<IOMemoryMap> gccMap, memPhyMap, hcMap;
  uintptr_t gcc = mapPhys (GCC_BASE + 0x77000, 0x1000, gccMap);
  uintptr_t memPhy = mapPhys (GCC_BASE + 0x9e000, 0x1000, memPhyMap);
  if (!gcc || !memPhy) {
    panic ("NATIVE_UFS inspect: GCC map failed gcc=%p memphy=%p", (void *) gcc, (void *) memPhy);
  }
  uint32_t gdsc = read32 (gcc, GCC_UFS_PHY_GDSCR - 0x77000);
  uint32_t axi = read32 (gcc, GCC_UFS_PHY_AXI_ENABLE - 0x77000);
  uint32_t ahb = read32 (gcc, GCC_UFS_PHY_AHB_ENABLE - 0x77000);
  uint32_t unipro = read32 (gcc, GCC_UFS_PHY_UNIPRO_CORE_ENABLE - 0x77000);
  uint32_t txs = read32 (gcc, GCC_UFS_PHY_TX_SYMBOL0_ENABLE - 0x77000);
  uint32_t rxs = read32 (gcc, GCC_UFS_PHY_RX_SYMBOL0_ENABLE - 0x77000);
  uint32_t aux = read32 (gcc, GCC_UFS_PHY_PHY_AUX_ENABLE - 0x77000);
  uint32_t memGdsc = read32 (memPhy, 0);
  // GDSCR bit31 PWR_ON; CBCR bit31 CLK_OFF.
  bool powered = (gdsc & 0x80000000U) != 0;
  // Hardware 2026-10-17: AXI running, AHB enabled but CLK_OFF (bit31). With bit1 (HW gating)
  // set the branch wakes on access, so enable bits, not CLK_OFF, gate the read-only probe.
  bool busClocks = !(axi & 0x80000000U) && (ahb & 0x1U) && (axi & 0x1U);
  uint32_t cap = 0, ver = 0, hcs = 0, hce = 0, utrlba = 0, is = 0;
  unsigned readHc = 0;
  if (powered && busClocks) {
    uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
    if (hc) {
      cap = read32 (hc, 0x00);
      ver = read32 (hc, 0x08);
      is = read32 (hc, 0x20);
      hcs = read32 (hc, 0x30);
      hce = read32 (hc, 0x34);
      utrlba = read32 (hc, 0x50);
      readHc = 1;
    }
  }
  panic ("NATIVE_UFS inspect\n"
         "gdsc=%08x memphygdsc=%08x\n"
         "axi=%08x ahb=%08x unipro=%08x\n"
         "txsym=%08x rxsym=%08x aux=%08x\n"
         "hc_read=%u cap=%08x ver=%08x\n"
         "is=%08x hcs=%08x hce=%08x utrlba=%08x",
         gdsc, memGdsc, axi, ahb, unipro, txs, rxs, aux, readHc, cap, ver, is, hcs, hce, utrlba);
}

// Adopt the controller the firmware left running (native-ufs-probe=3) and issue
// read-only SCSI commands through one UTP transfer request slot. Nothing is written
// to the storage. The controller is not reset and the link is not restarted; the
// firmware's request list is stopped (refused if a request is outstanding) and replaced
// by ours. Interrupts stay masked and completion is polled.
namespace {
struct UfsResult {
  uint32_t ocs;      // UTRD overall command status; 0xf = never completed
  uint32_t response; // UPIU response byte
  uint32_t scsi;     // SCSI status byte
  uint32_t sense;    // sense key<<16 | ASC<<8 | ASCQ from the response UPIU's autosense data
  uint32_t senseLen; // sense data length reported by the device
  uint32_t seg;      // UPIU data segment length
};

static void be32 (uint8_t *at, uint32_t value)
{
  at[0] = uint8_t (value >> 24); at[1] = uint8_t (value >> 16);
  at[2] = uint8_t (value >> 8); at[3] = uint8_t (value);
}

static uint32_t getBe32 (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}
}

static void cacheClean (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc cvac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void cacheCleanInvalidate (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// The region is mapped as device memory: only aligned element-sized stores are allowed
// (bzero's block zeroing faulted on hardware, 2026-10-17).
static void zero32 (void *start, size_t length)
{
  volatile uint32_t *word = static_cast<volatile uint32_t *> (start);
  for (size_t index = 0; index < length / 4; index++) {
    word[index] = 0;
  }
}


// Give the UFS stream a pass-through mapping the way Linux does on this firmware.
// Hardware 2026-10-17: after hand-off apps_smmu has sCR0 = 0x002f0406 (USFCFG=1, unmatched
// streams fault), no stream-match entry for the UFS host (ID 0x60, `iommus = <&apps_smmu 0x60
// 0x0>` in the board DT), a TYPE=BYPASS S2CR reads back as FAULT, and sCR0 writes are ignored.
// Linux (drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c, qcom_smmu_cfg_probe + qcom_smmu_write_s2cr,
// "bypass_quirk") meets exactly this: it reserves the LAST context bank (num_context_banks-1),
// writes SCTLR=0 (stage 1 disabled) and CBAR.TYPE=S1_TRANS_S2_BYPASS there, and maps a bypass
// stream as S2CR TYPE=TRANSLATE with CBNDX = that bank. S2CR is written before SMR
// (arm_smmu_write_sme). ARM IHI 0062D.c table 3-1 additionally requires VA64=1
// for inputs above 32 bits even with SCTLR.M=0. Storage DMA path only; this
// is physical pass-through, not a page-table or Adreno 4 KiB qualification.
static void smmuBypassStream (uint16_t stream, OSSharedPtr<IOMemoryMap> &keep, uint32_t *report,
                              uint64_t dma, uint64_t bytes)
{
  uintptr_t smmu = mapPhys (0x15000000ULL, 0xd0000, keep);
  if (!smmu) {
    panic ("NATIVE_UFS smmu: map failed");
  }
  UfsSmmuReuse reuse = reuseUfsSmmuStream (stream,
    [smmu](unsigned offset) { return read32 (smmu, offset); });
  if (!reuse.error) {
    report[0] = reuse.slot | (reuse.bank << 16);
    report[1] = reuse.cbar;
    report[2] = reuse.sctlr;
    return;
  }
  UfsSmmuPlan plan = planUfsSmmu (stream, dma, bytes, [smmu](unsigned offset) { return read32 (smmu, offset); });
  gRootDiagnostic.smmuId1 = read32 (smmu, 0x24);
  gRootDiagnostic.smmuId2 = read32 (smmu, 0x28);
  gRootDiagnostic.smmuSlot = plan.slot;
  if (plan.error) {
    gRootDiagnostic.stage = "SMMU planning";
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnNotReady);
    gRootDiagnostic.smr = plan.smr; gRootDiagnostic.s2cr = plan.s2cr;
    rootDiagnosticFailure (plan.error);
    panic ("NATIVE_UFS smmu: %s slot=%u smr=%x s2cr=%x", plan.error, plan.slot, plan.smr, plan.s2cr);
  }
  unsigned bank = plan.bank, index = plan.slot;
  uintptr_t cb = smmu + (plan.pages + bank) * 4096;
  UfsSmmuSnapshot before = captureUfsSmmuSnapshot (plan,
    [smmu](unsigned offset) { return read32 (smmu, offset); });
  UfsSmmuConfigDiagnostic diagnostic;
  const char *failure = configureUfsSmmu (plan, stream,
    [smmu](unsigned offset) { return read32 (smmu, offset); },
    [smmu](unsigned offset, uint32_t value) {
      write32 (smmu, offset, value);
      __asm__ volatile ("dsb sy" ::: "memory");
    }, &diagnostic);
  uint32_t smr = read32 (smmu, 0x800 + 4 * index), s2cr = read32 (smmu, 0xc00 + 4 * index);
  uint32_t cbar = read32 (smmu, 0x1000 + 4 * bank), sctlr = read32 (cb, 0x0);
  gRootDiagnostic.smr = smr; gRootDiagnostic.s2cr = s2cr;
  gRootDiagnostic.cbar = cbar; gRootDiagnostic.sctlr = sctlr;
  gRootDiagnostic.smmuId2 = read32 (smmu, 0x28);
  gRootDiagnostic.cba2r = read32 (smmu, 0x1800 + 4 * bank);
  if (failure) {
    UfsSmmuSnapshot after = captureUfsSmmuSnapshot (plan,
      [smmu](unsigned offset) { return read32 (smmu, offset); });
    gRootDiagnostic.stage = "SMMU readback";
    gRootDiagnostic.result = static_cast<uint32_t> (kIOReturnNotReady);
    rootDiagnosticFailure (failure);
    panic ("NATIVE_UFS smmu: %s idx=%u bank=%u step=%s off=%x exp=%x act=%x\n"
           "before scr0=%08x gfsr=%08x smr=%08x s2cr=%08x cbar=%08x cba2r=%08x sctlr=%08x\n"
           "after  scr0=%08x gfsr=%08x smr=%08x s2cr=%08x cbar=%08x cba2r=%08x sctlr=%08x",
           failure, index, bank, diagnostic.step ? diagnostic.step : "(none)",
           diagnostic.offset, diagnostic.expected, diagnostic.actual,
           before.scr0, before.gfsr, before.smr, before.s2cr,
           before.cbar, before.cba2r, before.sctlr,
           after.scr0, after.gfsr, after.smr, after.s2cr,
           after.cbar, after.cba2r, after.sctlr);
  }
  report[0] = index | (bank << 16);
  report[1] = cbar;
  report[2] = sctlr;
}

void NativeUfs::adoptAndRead (unsigned level)
{
  OSSharedPtr<IOMemoryMap> hcMap;
  uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
  if (!hc) {
    panic ("NATIVE_UFS adopt: controller map failed");
  }
  // The controller's own request list is the one region its DMA is known to reach
  // (hardware 2026-10-17: a kernel-allocated buffer at 0xf080f000 reset the system when the
  // first command was issued, and nothing below 0xc8000000 is allocatable). The firmware
  // zeroed it; use 16 KiB from UTRLBA as ordinary cached memory (the DT marks the host
  // dma-coherent). Test instrument only: the kernel may also own these pages.
  uint64_t fwList = (uint64_t (read32 (hc, 0x54)) << 32) | read32 (hc, 0x50);
  if (!fwList || (fwList & 0xfff)) {
    panic ("NATIVE_UFS adopt: no usable firmware list base=%llx", fwList);
  }
  IOMemoryDescriptor *region = IOMemoryDescriptor::withPhysicalAddress (fwList, 16384, kIODirectionInOut);
  OSSharedPtr<IOMemoryMap> regionMap;
  if (region) {
    regionMap = OSSharedPtr<IOMemoryMap> (region->map (kIOMapAnywhere), OSNoRetain);
    region->release ();
  }
  if (!regionMap) {
    panic ("NATIVE_UFS adopt: firmware list map failed base=%llx", fwList);
  }
  uint8_t *virt = reinterpret_cast<uint8_t *> (regionMap->getVirtualAddress ());
  uint64_t phys = fwList;
  zero32 (virt, 16384);
  cacheClean (virt, 16384);
  // Layout: [0,4096) request list (slot 0 used), [4096,5120) command + response UPIU +
  // PRDT, [8192,16384) data.
  const uint32_t ucdOffset = 4096, rspOffset = 512, prdtOffset = 1024, dataOffset = 8192;
  uint32_t hcsBefore = read32 (hc, 0x30);
  uint32_t doorbellBefore = read32 (hc, 0x58);
  if (doorbellBefore != 0 || read32 (hc, 0x78) != 0) {
    panic ("NATIVE_UFS adopt: firmware request outstanding doorbell=%08x hcs=%08x", doorbellBefore, hcsBefore);
  }
  OSSharedPtr<IOMemoryMap> smmuMap;
  uint32_t smmuReport[3] = { 0, 0, 0 };
  write32 (hc, 0x24, 0);            // mask interrupts
  write32 (hc, 0x60, 0);            // stop the firmware's request list
  __asm__ volatile ("dsb sy" ::: "memory");
  unsigned stopLoops = 10000;
  while ((read32 (hc, 0x60) & 1) && stopLoops--) IODelay (10);
  if (read32 (hc, 0x60) & 1) panic ("NATIVE_UFS adopt: request list did not stop");
  smmuBypassStream (ufsStream (), smmuMap, smmuReport, phys, 16384);
  write32 (hc, 0x50, static_cast<uint32_t> (phys));
  write32 (hc, 0x54, static_cast<uint32_t> (phys >> 32));
  write32 (hc, 0x20, 0xffffffffU);  // clear stale status
  write32 (hc, 0x60, 1);
  if (!waitSet (hc, 0x30, 0x2, 100000)) {
    panic ("NATIVE_UFS adopt: request list not ready hcs=%08x", read32 (hc, 0x30));
  }
  if (level == 3) {
    panic ("NATIVE_UFS adopt: list replaced\n"
           "phys=%llx hcs=%08x->%08x\n"
           "is=%08x db=%08x rsr=%08x ba=%08x bau=%08x ie=%08x",
           phys, hcsBefore, read32 (hc, 0x30), read32 (hc, 0x20), read32 (hc, 0x58),
           read32 (hc, 0x60), read32 (hc, 0x50), read32 (hc, 0x54), read32 (hc, 0x24));
  }

  auto command = [&] (uint8_t lun, const uint8_t *cdb, unsigned cdbLength, bool toHost, uint32_t length) -> UfsResult {
    uint8_t *utrd = virt;
    uint8_t *ucd = virt + ucdOffset;
    zero32 (utrd, 32);
    zero32 (ucd, 1024 + 16);
    zero32 (virt + dataOffset, 8192);
    ucd[0] = 0x01;                  // COMMAND UPIU
    ucd[1] = toHost ? 0x40 : 0x00;  // flags: read
    ucd[2] = lun;
    ucd[3] = 0;                     // task tag
    be32 (ucd + 12, length);        // expected data transfer length
    for (unsigned index = 0; index < cdbLength; index++) {
      ucd[16 + index] = cdb[index];
    }
    uint64_t ucdPhys = phys + ucdOffset;
    uint32_t direction = length ? (toHost ? 2U : 1U) : 0U;
    *reinterpret_cast<uint32_t *> (utrd + 0) = 0x10000000U | (direction << 25);
    *reinterpret_cast<uint32_t *> (utrd + 8) = 0x0fU;     // OCS: not completed
    *reinterpret_cast<uint32_t *> (utrd + 16) = static_cast<uint32_t> (ucdPhys);
    *reinterpret_cast<uint32_t *> (utrd + 20) = static_cast<uint32_t> (ucdPhys >> 32);
    *reinterpret_cast<uint32_t *> (utrd + 24) = (512U >> 2) | ((rspOffset >> 2) << 16);   // response length | offset (dwords)
    uint32_t prdEntries = length ? 1U : 0U;
    *reinterpret_cast<uint32_t *> (utrd + 28) = prdEntries | ((prdtOffset >> 2) << 16);
    if (length) {
      uint64_t dataPhys = phys + dataOffset;
      uint8_t *prd = ucd + prdtOffset;
      *reinterpret_cast<uint32_t *> (prd + 0) = static_cast<uint32_t> (dataPhys);
      *reinterpret_cast<uint32_t *> (prd + 4) = static_cast<uint32_t> (dataPhys >> 32);
      *reinterpret_cast<uint32_t *> (prd + 12) = length - 1;
    }
    cacheClean (virt, 16384);
    write32 (hc, 0x20, 0xffffffffU);
    write32 (hc, 0x58, 1);          // ring slot 0
    UfsResult result = { 0xf, 0xff, 0xff, 0, 0, 0 };
    bool done = false;
    for (unsigned loops = 0; loops < 100000; loops++) {
      if (!(read32 (hc, 0x58) & 1)) {
        done = true;
        break;
      }
      IODelay (1);
    }
    cacheCleanInvalidate (virt, 16384);
    if (!done) {
      return result;
    }
    result.ocs = *reinterpret_cast<volatile uint32_t *> (utrd + 8) & 0xff;
    const uint8_t *rsp = ucd + rspOffset;
    result.response = rsp[6];
    result.scsi = rsp[7];
    // Response UPIU: 12-byte header, residual count, 16 reserved bytes, then the data segment
    // (2-byte sense length + sense data) at offset 32.
    result.seg = (uint32_t (rsp[10]) << 8) | rsp[11];
    result.senseLen = (uint32_t (rsp[32]) << 8) | rsp[33];
    result.sense = ((rsp[36] & 0xfU) << 16) | (uint32_t (rsp[46]) << 8) | rsp[47];
    return result;
  };

  // NOP OUT (kind 0) and Query read requests (kind 1: opcode 0x05 READ FLAG, 0x03 READ ATTRIBUTE).
  // Read-only: no data segment, no state change on the device. Returns the UPIU response byte and
  // the 32-bit value field (attributes) or flag byte via *value; ocs 0xf means no completion.
  auto query = [&] (unsigned kind, uint8_t opcode, uint8_t idn, uint32_t *value, uint32_t *respByte) -> uint32_t {
    uint8_t *utrd = virt;
    uint8_t *ucd = virt + ucdOffset;
    zero32 (utrd, 32);
    zero32 (ucd, 1024 + 16);
    if (kind == 0) {
      ucd[0] = 0x00;                // NOP OUT
    } else {
      ucd[0] = 0x16;                // QUERY REQUEST
      ucd[5] = 0x01;                // query function (header dword 1, byte 1): standard read request
      ucd[12] = opcode;
      ucd[13] = idn;
    }
    ucd[3] = 1;                     // task tag
    uint64_t ucdPhys = phys + ucdOffset;
    *reinterpret_cast<uint32_t *> (utrd + 0) = 0x10000000U;
    *reinterpret_cast<uint32_t *> (utrd + 8) = 0x0fU;
    *reinterpret_cast<uint32_t *> (utrd + 16) = static_cast<uint32_t> (ucdPhys);
    *reinterpret_cast<uint32_t *> (utrd + 20) = static_cast<uint32_t> (ucdPhys >> 32);
    *reinterpret_cast<uint32_t *> (utrd + 24) = (512U >> 2) | ((rspOffset >> 2) << 16);
    *reinterpret_cast<uint32_t *> (utrd + 28) = (prdtOffset >> 2) << 16;
    cacheClean (virt, 16384);
    write32 (hc, 0x20, 0xffffffffU);
    write32 (hc, 0x58, 1);
    bool done = false;
    for (unsigned loops = 0; loops < 100000; loops++) {
      if (!(read32 (hc, 0x58) & 1)) {
        done = true;
        break;
      }
      IODelay (1);
    }
    cacheCleanInvalidate (virt, 16384);
    if (!done) {
      return 0xf;
    }
    const uint8_t *rsp = ucd + rspOffset;
    *respByte = (uint32_t (rsp[0]) << 8) | rsp[6];     // response type << 8 | query/NOP response
    *value = getBe32 (rsp + 20);
    return *reinterpret_cast<volatile uint32_t *> (utrd + 8) & 0xff;
  };
  // Device state before waking it. Hardware 2026-10-17: NOP OUT answers, fDeviceInit = 0 (done),
  // bCurrentPowerMode = 0x33 (UFS-PowerDown) and every LU returns NOT READY. Linux resumes the
  // device with START STOP UNIT (power condition ACTIVE) to the UFS Device well-known LU 0xD0
  // (ufshcd_set_dev_pwr_mode); no user data is touched.
  uint32_t powerBefore = 0, powerAfter = 0, queryResp = 0, ignoredValue = 0;
  uint32_t wakeStatus = 0xee;
  if (level == 5 || level == 9) {
    query (1, 0x03, 0x02, &powerBefore, &queryResp);     // READ ATTRIBUTE bCurrentPowerMode
    uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };          // START STOP UNIT, power condition ACTIVE
    UfsResult w = command (0xd0, ssu, 6, false, 0);
    wakeStatus = (w.ocs << 16) | (w.response << 8) | w.scsi;
    for (unsigned settle = 0; settle < 50; settle++) {
      IODelay (1000);
    }
    query (1, 0x03, 0x02, &powerAfter, &queryResp);
    (void) ignoredValue;
  }
  if (level == 4) {
    uint8_t tur0[6] = { 0, 0, 0, 0, 0, 0 };
    UfsResult t = command (0, tur0, 6, false, 0);
    const uint8_t *rsp0 = virt + ucdOffset + rspOffset;
    panic ("NATIVE_UFS adopt: TEST UNIT READY lun0\n"
           "ocs=%x resp=%02x scsi=%02x\n"
           "phys=%llx hcs=%08x->%08x smmu idx/bank=%08x cbar=%08x\n"
           "is=%08x db=%08x rsr=%08x\n"
           "rsp=%08x %08x",
           t.ocs, t.response, t.scsi, phys, hcsBefore, read32 (hc, 0x30), smmuReport[0], smmuReport[1],
           read32 (hc, 0x20), read32 (hc, 0x58), read32 (hc, 0x60), getBe32 (rsp0), getBe32 (rsp0 + 4));
  }
  uint32_t hcsAfter = 0;
  const unsigned maxLun = 6;
  uint32_t status[maxLun], last[maxLun], blockSize[maxLun], senseOf[maxLun];
  for (unsigned lun = 0; lun < maxLun; lun++) {
    status[lun] = 0xee; last[lun] = 0; blockSize[lun] = 0; senseOf[lun] = 0;
    uint8_t tur[6] = { 0x00, 0, 0, 0, 0, 0 };
    UfsResult r = { 0xf, 0xff, 0xff, 0, 0, 0 };
    for (unsigned attempt = 0; attempt < 3; attempt++) {
      r = command (static_cast<uint8_t> (lun), tur, 6, false, 0);
      if (lun == 0 && attempt == 0 && r.ocs == 0xf) {
        // Never completed: stop here and report instead of waiting out every command.
        const uint8_t *rsp = virt + ucdOffset + rspOffset;
        panic ("NATIVE_UFS adopt: first command did not complete\n"
               "phys=%llx hcs=%08x->%08x\n"
               "is=%08x db=%08x rsr=%08x ba=%08x\n"
               "utrd2=%08x rsp=%08x %08x",
               phys, hcsBefore, read32 (hc, 0x30),
               read32 (hc, 0x20), read32 (hc, 0x58), read32 (hc, 0x60), read32 (hc, 0x50),
               *reinterpret_cast<volatile uint32_t *> (virt + 8), getBe32 (rsp), getBe32 (rsp + 4));
      }
      if (r.ocs == 0 && r.scsi == 0) {
        break;
      }
    }
    uint8_t cap[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    r = command (static_cast<uint8_t> (lun), cap, 10, true, 8);
    status[lun] = (r.ocs << 16) | (r.response << 8) | r.scsi;
    senseOf[lun] = r.sense;
    if (r.ocs == 0 && r.scsi == 0) {
      last[lun] = getBe32 (virt + dataOffset);
      blockSize[lun] = getBe32 (virt + dataOffset + 4);
    }
  }
  if (level == 9) {
    // Read-only GPT survey of LUN 0 (4096-byte blocks): header fields and CRCs for before/after
    // comparison, then every non-empty entry among the first 64. Nothing is written.
    if (blockSize[0] != 4096) {
      panic ("NATIVE_UFS gpt: unexpected LUN0 block size %08x", blockSize[0]);
    }
    uint8_t rd[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    UfsResult h = command (0, rd, 10, true, 4096);
    if (h.ocs != 0 || h.scsi != 0) {
      panic ("NATIVE_UFS gpt: header read failed ocs=%x resp=%x scsi=%x", h.ocs, h.response, h.scsi);
    }
    const uint8_t *hdr = virt + dataOffset;
    auto le32 = [] (const uint8_t *at) -> uint32_t {
      return uint32_t (at[0]) | (uint32_t (at[1]) << 8) | (uint32_t (at[2]) << 16) | (uint32_t (at[3]) << 24);
    };
    auto le64 = [&] (const uint8_t *at) -> uint64_t {
      return uint64_t (le32 (at)) | (uint64_t (le32 (at + 4)) << 32);
    };
    uint32_t sig0 = le32 (hdr), sig1 = le32 (hdr + 4), hdrCrc = le32 (hdr + 16), entCrc = le32 (hdr + 88);
    uint64_t curLba = le64 (hdr + 24), bakLba = le64 (hdr + 32), firstUsable = le64 (hdr + 40), lastUsable = le64 (hdr + 48);
    uint64_t entLba = le64 (hdr + 72);
    uint32_t entCount = le32 (hdr + 80), entSize = le32 (hdr + 84);
    uint32_t guid0 = le32 (hdr + 56), guid1 = le32 (hdr + 60), guid2 = le32 (hdr + 64), guid3 = le32 (hdr + 68);
    if (sig0 != 0x20494645U || sig1 != 0x54524150U || entSize != 128 || entLba > 64) {
      panic ("NATIVE_UFS gpt: unexpected header sig=%08x %08x esz=%u elba=%llx", sig0, sig1, entSize, entLba);
    }
    uint32_t shown = entCount > 64 ? 64 : entCount;
    uint32_t blocks = (shown * 128 + 4095) / 4096;
    uint8_t rd2[10] = { 0x28, 0, 0, 0, 0, uint8_t (entLba), 0, 0, uint8_t (blocks), 0 };
    UfsResult e = command (0, rd2, 10, true, blocks * 4096);
    if (e.ocs != 0 || e.scsi != 0) {
      panic ("NATIVE_UFS gpt: entries read failed ocs=%x scsi=%x", e.ocs, e.scsi);
    }
    const uint8_t *ents = virt + dataOffset;
    static char text[3400];
    int used = snprintf (text, sizeof (text),
                         "NATIVE_UFS gpt LUN0 hdr crc=%08x ents crc=%08x cnt=%u\n"
                         "cur=%llx backup=%llx usable=%llx..%llx elba=%llx\n"
                         "disk guid %08x %08x %08x %08x last=%08x\n",
                         hdrCrc, entCrc, entCount, curLba, bakLba, firstUsable, lastUsable, entLba,
                         guid0, guid1, guid2, guid3, last[0]);
    unsigned column = 0, listed = 0;
    for (uint32_t index = 0; index < shown && used < int (sizeof (text)) - 80; index++) {
      const uint8_t *entry = ents + index * 128;
      bool empty = true;
      for (unsigned b = 0; b < 16; b++) {
        if (entry[b]) { empty = false; break; }
      }
      if (empty) {
        continue;
      }
      char name[14];
      for (unsigned c = 0; c < 13; c++) {
        uint8_t lo = entry[56 + 2 * c], hi = entry[57 + 2 * c];
        name[c] = (hi == 0 && lo >= 32 && lo < 127) ? char (lo) : (lo ? '?' : 0);
        if (!name[c]) { name[c] = 0; break; }
      }
      name[13] = 0;
      used += snprintf (text + used, sizeof (text) - static_cast<size_t> (used), "%u:%s %llx-%llx%s", index, name,
                        le64 (entry + 32), le64 (entry + 40), (++column % 3) ? "  " : "\n");
      listed++;
    }
    snprintf (text + used, sizeof (text) - static_cast<size_t> (used), "\nlisted=%u", listed);
    panic ("%s", text);
  }
  // INQUIRY of LUN 0: vendor/product bytes 8..23.
  uint8_t inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
  UfsResult inq = command (0, inquiry, 6, true, 36);
  uint32_t inqWord[4] = { 0, 0, 0, 0 };
  for (unsigned index = 0; index < 4; index++) {
    inqWord[index] = getBe32 (virt + dataOffset + 8 + index * 4);
  }
  // LBA 1 of LUN 0 holds the GPT header when the block size is 4096 (or 512).
  uint32_t gptStatus = 0xee, sig0 = 0, sig1 = 0;
  if (blockSize[0] >= 512 && blockSize[0] <= 4096) {
    uint8_t read10[10] = { 0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0 };
    UfsResult g = command (0, read10, 10, true, blockSize[0]);
    gptStatus = (g.ocs << 16) | (g.response << 8) | g.scsi;
    if (g.ocs == 0 && g.scsi == 0) {
      sig0 = *reinterpret_cast<uint32_t *> (virt + dataOffset);
      sig1 = *reinterpret_cast<uint32_t *> (virt + dataOffset + 4);
    }
  }
  hcsAfter = read32 (hc, 0x30);
  panic ("NATIVE_UFS adopt+read\n"
         "hcs %08x->%08x pwr %08x->%08x ssu=%06x\n"
         "L0 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L1 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L2 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L3 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L4 st=%06x last=%08x bs=%08x sns=%06x\n"
         "L5 st=%06x last=%08x bs=%08x sns=%06x\n"
         "inq st=%06x %08x %08x %08x %08x\n"
         "inq sns=%06x slen=%x seg=%x\n"
         "rsp0 %08x %08x %08x %08x\n"
         "rsp8 %08x %08x %08x %08x\n"
         "gpt st=%06x sig=%08x %08x",
         hcsBefore, hcsAfter, powerBefore, powerAfter, wakeStatus,
         status[0], last[0], blockSize[0], senseOf[0], status[1], last[1], blockSize[1], senseOf[1],
         status[2], last[2], blockSize[2], senseOf[2], status[3], last[3], blockSize[3], senseOf[3],
         status[4], last[4], blockSize[4], senseOf[4], status[5], last[5], blockSize[5], senseOf[5],
         (inq.ocs << 16) | (inq.response << 8) | inq.scsi, inqWord[0], inqWord[1], inqWord[2], inqWord[3],
         inq.sense, inq.senseLen, inq.seg,
         getBe32 (virt + ucdOffset + rspOffset), getBe32 (virt + ucdOffset + rspOffset + 4),
         getBe32 (virt + ucdOffset + rspOffset + 8), getBe32 (virt + ucdOffset + rspOffset + 12),
         getBe32 (virt + ucdOffset + rspOffset + 32), getBe32 (virt + ucdOffset + rspOffset + 36),
         getBe32 (virt + ucdOffset + rspOffset + 40), getBe32 (virt + ucdOffset + rspOffset + 44),
         gptStatus, sig0, sig1);
}

// Read-only dump of the request list the firmware left (native-ufs-probe=6): shows where
// the firmware placed its descriptors, to compare with an allocation the controller's DMA
// is known to reach. Nothing is written and no register is touched except reads.
static uint32_t dumpWord (const volatile uint32_t *base, unsigned index)
{
  return base[index];
}

void NativeUfs::dumpFirmwareList (void)
{
  OSSharedPtr<IOMemoryMap> hcMap, listMap, ucdMap;
  uintptr_t hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, hcMap);
  if (!hc) {
    panic ("NATIVE_UFS dump: controller map failed");
  }
  uint64_t listPhys = (uint64_t (read32 (hc, 0x54)) << 32) | read32 (hc, 0x50);
  uint32_t db = read32 (hc, 0x58), rsr = read32 (hc, 0x60), hcs = read32 (hc, 0x30);
  IOMemoryDescriptor *listDesc = IOMemoryDescriptor::withPhysicalAddress (listPhys, 4096, kIODirectionIn);
  if (!listDesc) {
    panic ("NATIVE_UFS dump: list descriptor failed phys=%llx", listPhys);
  }
  listMap = OSSharedPtr<IOMemoryMap> (listDesc->map (kIOMapAnywhere), OSNoRetain);
  listDesc->release ();
  if (!listMap) {
    panic ("NATIVE_UFS dump: list map failed phys=%llx", listPhys);
  }
  const volatile uint32_t *utrd = reinterpret_cast<const volatile uint32_t *> (listMap->getVirtualAddress ());
  uint64_t ucdPhys = (uint64_t (dumpWord (utrd, 5)) << 32) | dumpWord (utrd, 4);
  uint32_t ucd0 = 0, ucd1 = 0, ucd2 = 0, ucd3 = 0;
  if (ucdPhys) {
    IOMemoryDescriptor *ucdDesc = IOMemoryDescriptor::withPhysicalAddress (ucdPhys, 4096, kIODirectionIn);
    if (ucdDesc) {
      ucdMap = OSSharedPtr<IOMemoryMap> (ucdDesc->map (kIOMapAnywhere), OSNoRetain);
      ucdDesc->release ();
      if (ucdMap) {
        const volatile uint32_t *ucd = reinterpret_cast<const volatile uint32_t *> (ucdMap->getVirtualAddress ());
        ucd0 = dumpWord (ucd, 0); ucd1 = dumpWord (ucd, 1); ucd2 = dumpWord (ucd, 2); ucd3 = dumpWord (ucd, 3);
      }
    }
  }
  panic ("NATIVE_UFS dump: firmware list\n"
         "list=%llx hcs=%08x db=%08x rsr=%08x\n"
         "utrd0 %08x %08x %08x %08x\n"
         "utrd0 %08x %08x %08x %08x\n"
         "utrd1 %08x %08x %08x %08x\n"
         "ucd=%llx %08x %08x %08x %08x",
         listPhys, hcs, db, rsr,
         dumpWord (utrd, 0), dumpWord (utrd, 1), dumpWord (utrd, 2), dumpWord (utrd, 3),
         dumpWord (utrd, 4), dumpWord (utrd, 5), dumpWord (utrd, 6), dumpWord (utrd, 7),
         dumpWord (utrd, 8), dumpWord (utrd, 9), dumpWord (utrd, 10), dumpWord (utrd, 11),
         ucdPhys, ucd0, ucd1, ucd2, ucd3);
}

// Read-only dump of the apps SMMU (apps_smmu@15000000) global register space,
// both configured UFS streams and their context-bank state (native-ufs-probe=7).
// SMMUv2 layout: sCR0 0x0, IDR0 0x20, sGFSR 0x48, SMR(n) 0x800+4n,
// S2CR(n) 0xc00+4n, CBAR 0x1000+4n, CBA2R 0x1800+4n. Nothing is written.
void NativeUfs::inspectSmmu (void)
{
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t smmu = mapPhys (0x15000000ULL, 0xd0000, map);
  if (!smmu) {
    panic ("NATIVE_UFS smmu: map failed");
  }
  uint32_t scr0 = read32 (smmu, 0x00), idr0 = read32 (smmu, 0x20), idr1 = read32 (smmu, 0x24);
  uint32_t idr2 = read32 (smmu, 0x28), gfsr = read32 (smmu, 0x48);
  unsigned count = idr0 & 0xff;
  if (count > 128) {
    count = 128;
  }
  unsigned numCb = idr1 & 0xff;
  unsigned globalPages = 1U << (((idr1 >> 28) & 7) + 1);
  bool contextMapped = globalPages <= 128 && numCb <= 128 &&
                       (globalPages + numCb) * 0x1000 <= 0xd0000;
  UfsSmmuStreamEntry entries[128] = {};
  unsigned valid = collectUfsSmmuStreams ([smmu](unsigned offset) { return read32 (smmu, offset); },
                                          entries, 128);
  unsigned typeCount[4] = { 0, 0, 0, 0 };
  for (unsigned entry = 0; entry < valid; entry++) {
    typeCount[(entries[entry].s2cr >> 16) & 3]++;
  }

  UfsStreamConfig streams = ufsStreams ();
  uint16_t configured[2] = { streams.primary, streams.secondary };
  char streamText[1400] = {};
  unsigned streamUsed = 0, configuredCount = streams.secondary ? 2 : 1;
  for (unsigned index = 0; index < configuredCount; index++) {
    uint16_t stream = configured[index];
    const UfsSmmuStreamEntry *match = nullptr;
    unsigned matches = 0;
    for (unsigned entry = 0; entry < valid; entry++) {
      if (ufsSmmuStreamMatches (entries[entry].smr, stream)) {
        ++matches;
        if (!match) {
          match = &entries[entry];
        }
      }
    }
    unsigned cb = match ? (match->s2cr & 0xff) : 0xffff;
    unsigned type = match ? ((match->s2cr >> 16) & 3) : 0;
    unsigned references = 0;
    for (unsigned entry = 0; entry < valid; entry++) {
      if (((entries[entry].s2cr >> 16) & 3) == 0 && (entries[entry].s2cr & 0xff) == cb) {
        ++references;
      }
    }
    uint32_t cbar = 0, cba2r = 0, sctlr = 0;
    if (match && contextMapped && cb < numCb) {
      cbar = read32 (smmu, 0x1000 + 4 * cb);
      cba2r = read32 (smmu, 0x1800 + 4 * cb);
      sctlr = read32 (smmu + (globalPages + cb) * 0x1000, 0);
    }
    unsigned mask = match ? ((match->smr >> 16) & 0x7fff) : 0;
    unsigned safe = matches == 1 && match && mask == 0 && type == 0 &&
                    cb < numCb && references == 1 &&
                    ufsSmmuContextIsBypass (cbar, sctlr) && (cba2r & 1);
    int written = snprintf (streamText + streamUsed, sizeof (streamText) - streamUsed,
                            "%ssid%04x matches=%u idx=%x mask=%04x smr=%08x s2cr=%08x type=%u cb=%x refs=%u cbar=%08x cba2r=%08x sctlr=%08x safe=%u",
                            index ? "\n" : "", stream, matches,
                            match ? match->index : 0xffff, mask,
                            match ? match->smr : 0, match ? match->s2cr : 0,
                            type, cb, references, cbar, cba2r, sctlr, safe);
    if (written < 0 || static_cast<unsigned> (written) >= sizeof (streamText) - streamUsed) {
      break;
    }
    streamUsed += static_cast<unsigned> (written);
  }

  char details[1536] = {};
  unsigned used = 0, listed = 0;
  for (unsigned entry = 0; entry < valid; entry++) {
    int written = snprintf (details + used, sizeof (details) - used,
                            "%s[%u] sid=%04x mask=%04x smr=%08x s2cr=%08x",
                            entry ? "\n" : "", entries[entry].index,
                            entries[entry].smr & 0xffffU, (entries[entry].smr >> 16) & 0x7fffU,
                            entries[entry].smr, entries[entry].s2cr);
    if (written < 0 || static_cast<unsigned> (written) >= sizeof (details) - used) {
      break;
    }
    used += static_cast<unsigned> (written);
    listed++;
  }
  if (!listed) {
    snprintf (details, sizeof (details), "(none)");
  } else if (listed < valid) {
    snprintf (details + used, sizeof (details) - used, "\n... %u more", valid - listed);
  }
  panic ("NATIVE_UFS smmu\n"
         "scr0=%08x idr0=%08x idr1=%08x idr2=%08x gfsr=%08x\n"
         "nsmr=%u valid=%u ncb=%u pages=%u context_mapped=%u types t0=%u t1=%u t2=%u\n"
         "%s\n"
         "enabled SMRs:\n%s",
         scr0, idr0, idr1, idr2, gfsr, count, valid, numCb, globalPages, contextMapped,
         typeCount[0], typeCount[1], typeCount[2], streamText, details);
}

// Read-only context-bank survey (native-ufs-probe=8): CBAR/CBA2R in gr1 and SCTLR/TTBR0 in the
// context banks, to find a bank the hypervisor leaves to us. Nothing is written.
void NativeUfs::inspectContextBanks (void)
{
  OSSharedPtr<IOMemoryMap> map;
  uintptr_t smmu = mapPhys (0x15000000ULL, 0xd0000, map);
  if (!smmu) {
    panic ("NATIVE_UFS cb: map failed");
  }
  uint32_t idr1 = read32 (smmu, 0x24);
  unsigned numCb = idr1 & 0xff;
  unsigned globalPages = 1U << (((idr1 >> 28) & 7) + 1);
  if (globalPages > 128 || numCb > 128 || (globalPages + numCb) * 0x1000 > 0xd0000) {
    panic ("NATIVE_UFS cb: unexpected geometry idr1=%08x", idr1);
  }
  unsigned cbarZero = 0, sctlrZero = 0, mSet = 0, bothZero = 0;
  uint32_t firstZero = 0xffff;
  for (unsigned index = 0; index < numCb; index++) {
    uint32_t cbar = read32 (smmu, 0x1000 + 4 * index);
    uint32_t sctlr = read32 (smmu + (globalPages + index) * 0x1000, 0x0);
    cbarZero += cbar == 0;
    sctlrZero += sctlr == 0;
    mSet += (sctlr & 1) != 0;
    if (cbar == 0 && sctlr == 0) {
      bothZero++;
      if (firstZero == 0xffff) {
        firstZero = index;
      }
    }
  }
  uintptr_t cb0 = smmu + globalPages * 0x1000, cb1 = cb0 + 0x1000, cbLast = cb0 + (numCb - 1) * 0x1000;
  panic ("NATIVE_UFS cb survey\n"
         "ncb=%u pages=%u cbar0=%u sctlr0=%u msset=%u both0=%u first0=%x\n"
         "cbar[0]=%08x cbar[1]=%08x cbar[%u]=%08x\n"
         "sctlr[0]=%08x sctlr[1]=%08x sctlr[last]=%08x\n"
         "cba2r[0]=%08x cba2r[1]=%08x",
         numCb, globalPages, cbarZero, sctlrZero, mSet, bothZero, firstZero,
         read32 (smmu, 0x1000), read32 (smmu, 0x1004), numCb - 1, read32 (smmu, 0x1000 + 4 * (numCb - 1)),
         read32 (cb0, 0), read32 (cb1, 0), read32 (cbLast, 0),
         read32 (smmu, 0x1800), read32 (smmu, 0x1804));
}

// ======================================================================================
// Root media: the live controller as a read-only block device.
//
// One shared host (a single UTP request slot, polled; interrupts stay masked) serves every
// block device. The DMA region is ordinary cached, physically contiguous kernel memory with
// explicit cache maintenance around each command (the stream is mapped through the reserved
// apps_smmu bypass context bank by smmuBypassStream, so physical addresses are used as-is).
// Nothing here writes to the storage: only INQUIRY-class and READ(10) commands are built, plus
// the single START STOP UNIT that wakes the device from the power-down the firmware left.
// ======================================================================================
namespace {
#include "RootTransportLayout.h"

struct UfsHost {
  OSSharedPtr<IOMemoryMap> hcMap, smmuMap;
  uintptr_t hc;
  IOBufferMemoryDescriptor *dma;
  uint8_t *virt;
  uint64_t phys;
  IOLock *lock;
  bool ready;
  bool poisoned = false;
  uintptr_t smmu = 0, cb = 0;
};
UfsHost gHost = { OSSharedPtr<IOMemoryMap> (), OSSharedPtr<IOMemoryMap> (), 0, nullptr, nullptr, 0, nullptr, false };
#include "DpuWindow.h"
// Forwarder so the exported entry below (outside every namespace) can reach the window build, which lives in here.
static int dpuWindowBuild (uintptr_t smmu) { return dpuWindowBuildImpl (smmu); }

static IOReturn recordTransport (IOReturn result, uint32_t ocs)
{
  gRootDiagnostic.result = static_cast<uint32_t> (result);
  gRootDiagnostic.ocs = ocs;
  if (result != kIOReturnSuccess && gRootDiagnosticEnabled) {
    // Read only the already mapped HC's status/registers used by this driver.
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
    gRootDiagnostic.doorbell = read32 (gHost.hc, 0x58);
    gRootDiagnostic.interrupt = read32 (gHost.hc, 0x20);
    gRootDiagnostic.list = uint64_t (read32 (gHost.hc, 0x50)) |
                          (uint64_t (read32 (gHost.hc, 0x54)) << 32);
    gRootDiagnostic.runStop = read32 (gHost.hc, 0x60);
    gRootDiagnostic.ahit = read32 (gHost.hc, 0x18);
    for (unsigned i = 0; i < 5; ++i) gRootDiagnostic.uic[i] = read32 (gHost.hc, 0x38 + 4 * i);
    if (gHost.smmu && gHost.cb) {
      gRootDiagnostic.gfsr = read32 (gHost.smmu, 0x48);
      gRootDiagnostic.gsyn = read32 (gHost.smmu, 0x54);
      gRootDiagnostic.fsr = read32 (gHost.cb, 0x58);
      gRootDiagnostic.fsyn = read32 (gHost.cb, 0x68);
      gRootDiagnostic.far = uint64_t (read32 (gHost.cb, 0x60)) |
                            (uint64_t (read32 (gHost.cb, 0x64)) << 32);
    }
  }
  return result;
}

static void cacheCivac (const void *start, size_t length)
{
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~uintptr_t (63);
  uintptr_t end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += 64) {
    __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

// Device-owned cache lines must not be written back over DMA results. All
// callers use complete 64-byte lines from the dedicated page-aligned buffer.
static void cacheInvalidate (const void *start, size_t length)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  uintptr_t address = reinterpret_cast<uintptr_t> (start);
  for (uintptr_t end = address + length; address < end; address += 64) {
    __asm__ volatile ("dc ivac, %0" :: "r" (address) : "memory");
  }
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void hostWrite (unsigned offset, uint32_t value)
{
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (gHost.hc, offset, value);
  __asm__ volatile ("dsb sy" ::: "memory");
}

// Busy-wait `us` microseconds by the counter: no timer, no thread_block (see RootCommands.h,
// poll pacing). Preemption stays enabled; the caller may hold gHost.lock.
static void ufsSpinPause (uint32_t us)
{
  uint64_t until;
  clock_interval_to_deadline (us, kMicrosecondScale, &until);
  while (mach_absolute_time () < until) {
    __asm__ volatile ("yield" ::: "memory");
  }
}

#include "RootQueue.h"

#include "RootCommands.h"

// Bring the live controller up for reading: map, SMMU pass-through for the UFS stream, take over
// the request list, wake the device from UFS-PowerDown and wait for LUN 0 to be ready.
// `why` receives a short reason on failure.
static bool hostBringUp (const char **why)
{
  // A secondary stream is a board-level transport dependency, not an optional
  // diagnostic. Validate both existing mappings before any controller access.
  requireDualStreamReuse (0, 0, "host bring-up");
  if (gHost.poisoned) {
    *why = "transport quarantined";
    return false;
  }
  if (gHost.ready) {
    return true;
  }
  gRootDiagnostic.stage = "controller map";
  gHost.hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, gHost.hcMap);
  if (!gHost.hc) {
    *why = "controller map";
    return false;
  }
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.cap = read32 (gHost.hc, 0x00);
    gRootDiagnostic.version = read32 (gHost.hc, 0x08);
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
  }
  gRootDiagnostic.stage = "DMA allocation";
  gHost.dma = IOBufferMemoryDescriptor::inTaskWithOptions (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone),
    kDmaBytes, 4096);
  gHost.lock = IOLockAlloc ();
  if (!gHost.dma || !gHost.lock) {
    *why = "dma buffer";
    return false;
  }
  gHost.virt = static_cast<uint8_t *> (gHost.dma->getBytesNoCopy ());
  IOByteCount contiguous = 0;
  gHost.phys = gHost.dma->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
  gRootDiagnostic.dma = gHost.phys;
  gRootDiagnostic.contiguous = contiguous;
  if (!gHost.virt || !gHost.phys || (gHost.phys & 4095) || contiguous < kDmaBytes ||
      (!(read32 (gHost.hc, 0) & 0x01000000) && gHost.phys + kDmaBytes > 0x100000000ULL)) {
    *why = "DMA address/contiguity";
    return false;
  }
  bzero (gHost.virt, kDmaBytes);
  cacheCivac (gHost.virt, kDmaBytes);
  uint32_t report[3] = { 0, 0, 0 };
  gRootDiagnostic.stage = "SMMU and request list";
  if (!hostStopQueue (why)) return false;
  smmuBypassStream (ufsStream (), gHost.smmuMap, report, gHost.phys, kDmaBytes);
  gHost.smmu = gHost.smmuMap->getVirtualAddress ();
  uint32_t idr1 = read32 (gHost.smmu, 0x24);
  gHost.cb = gHost.smmu + ((1U << (((idr1 >> 28) & 7) + 1)) + (report[0] >> 16)) * 4096;
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.gfsrBefore = read32 (gHost.smmu, 0x48);
    gRootDiagnostic.fsrBefore = read32 (gHost.cb, 0x58);
    gRootDiagnostic.smmuId2 = read32 (gHost.smmu, 0x28);
    gRootDiagnostic.cba2r = read32 (gHost.smmu, 0x1800 + 4 * (report[0] >> 16));
  }
  if (!hostStartQueue (why)) return false;
  gRootDiagnostic.stage = "NOP OUT";
  // NOP OUT proves the transport before touching the SCSI layer.
  if (hostNop () != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "NOP OUT";
    return false;
  }
  uint32_t power = 0;
  gRootDiagnostic.stage = "power mode query";
  if (hostQueryRead (0x03, 0x02, &power) != kIOReturnSuccess) {
    gHost.ready = false;
    *why = "power mode query";
    return false;
  }
  gRootDiagnostic.power = power;
  if ((power & 0xff) != 0x11) {
    gRootDiagnostic.stage = "device wake";
    // START STOP UNIT, power condition ACTIVE, to the UFS Device well-known LU (as Linux resumes).
    uint8_t ssu[6] = { 0x1b, 0, 0, 0, 0x10, 0 };
    uint32_t scsi = 0;
    if (hostScsi (0xd0, ssu, 6, false, 0, 500000, &scsi) != kIOReturnSuccess) {
      gHost.ready = false;
      *why = "wake (START STOP UNIT)";
      return false;
    }
    IOSleep (50);
  }
  gRootDiagnostic.stage = "LUN ready";
  for (unsigned attempt = 0; attempt < 20; attempt++) {
    uint8_t tur[6] = { 0, 0, 0, 0, 0, 0 };
    uint32_t scsi = 0;
    if (hostScsi (0, tur, 6, false, 0, 200000, &scsi) == kIOReturnSuccess) {
      gHost.ready = true;
      return true;
    }
    if (gHost.poisoned) break;
    IOSleep (10);
  }
  gHost.ready = false;
  *why = "LUN 0 not ready";
  return false;
}
}

namespace {
static uint32_t be32w (const uint8_t *at)
{
  return (uint32_t (at[0]) << 24) | (uint32_t (at[1]) << 16) | (uint32_t (at[2]) << 8) | at[3];
}

#include "RootLayout.h"
#if !defined(NATIVE_UFS_PROBE_ONLY) || !NATIVE_UFS_PROBE_ONLY
#include "RootWindowRead.h"
#include "RootPublication.h"
#endif

}

// ---- Persistent Data store window (sheng-data-persist) ----------------------------------------------------------
// The only part of this driver that ever writes. The consumer (NativeRamBlock's overlay) asks for a window of the given
// size at the END of the userdata partition. Mode 1 (boot argument sheng-data-persist=1) only reads: it proves the window and
// lets the consumer inspect its header. Mode 2 additionally opens the write window. A window is refused unless it lies inside
// the partition, clear of the flashed root image plus a 4 MiB guard, and is at most a quarter of the partition.
namespace {
struct StoreGeometry {
  bool valid;
  uint64_t first, last, imageBytes, entriesLba;
  uint32_t headerCrc, entriesCrc, entryCount;
  uint64_t logFirst, logLast;   // gts7xlwifi: winre
};
StoreGeometry gStoreGeometry;
uint64_t gStoreRegionFirst, gStoreRegionBlocks;
// The diagnostic log (sheng-log=1): always the last 32 MiB of userdata, so the Data store's place does not depend on logging.
constexpr uint64_t kLogReserveBlocks = 8192;
// The log's export region: the 256 MiB just below the log window (the render server's pipeline caches, kits/sheng BootLog.c). It is
// part of the log's write window, so the store ends below both.
constexpr uint64_t kExportBlocks = 65536;
uint64_t gLogRegionFirst, gLogRegionBlocks;
// The reserve at the end of userdata that the Data store stays clear of: the log and its export region sit inside it (sheng-log=1), the rest
// is unused. sheng-log-reserve=<MiB>, default 1024; 0 gives all the room to Data and is only accepted with the log off. The store's place
// depends on it, so an image keeps one value for its whole life (a different value moves the store: the old one is read as blank).
constexpr uint32_t kDefaultReserveMiB = 1024;
static uint64_t reserveBlocks (void)
{
  static int64_t cached = -1;
  if (cached < 0) {
    uint32_t mib = kDefaultReserveMiB;
    if (!PE_parse_boot_argn ("sheng-log-reserve", &mib, sizeof (mib)) || mib > 16384) mib = kDefaultReserveMiB;
    cached = static_cast<int64_t> (mib) * 256;   // 4096-byte blocks
  }
  return static_cast<uint64_t> (cached);
}

static unsigned storeMode (void)
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 0;
    if (!PE_parse_boot_argn ("sheng-data-persist", &value, sizeof (value)) || value > 2) value = 0;
    cached = static_cast<int> (value);
  }
  return static_cast<unsigned> (cached);
}
}

// The board's entry point for the display window (package/LegacyDisplayProbe.cpp, before the display starts). Outside every namespace,
// because an extern "C" definition inside the anonymous namespace would have internal linkage.
extern "C" int native_ufs_dpu_window_build (uintptr_t smmu) { return dpuWindowBuild (smmu); }
// The apps_smmu mapping NativeUfs holds, for the board's late probe. 0 until the host has mapped it.
extern "C" uintptr_t native_ufs_smmu_base (void) { return gHost.smmu; }
extern "C" unsigned native_ufs_store_mode (void) { return storeMode (); }

// Returns 1 and the first absolute 4096-byte LBA of the window, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_store_open (uint64_t blocks, uint64_t *firstLba, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!storeMode ()) reason = "off";
  else if (!gStoreGeometry.valid || !gHost.ready) reason = "no root layout";
  else if (gStoreRegionBlocks) reason = "already open";
  else {
    uint64_t partition = gStoreGeometry.last - gStoreGeometry.first + 1;
    uint64_t imageBlocks = (gStoreGeometry.imageBytes + 4095) / 4096;
    const uint64_t reserve = reserveBlocks ();
    if (!blocks || reserve >= partition || blocks > partition - reserve) reason = "size";
    else if (partition - blocks - reserve < imageBlocks + 1024) reason = "overlaps the root image";
    else {
      // The reserve at the end of the partition belongs to the diagnostic log (its last 32 MiB, with the export region below them);
      // the store ends just below it.
      uint64_t first = gStoreGeometry.last + 1 - reserve - blocks;
      if (storeMode () == 2 && !hostStoreOpen (first, blocks)) reason = "window refused";
      else {
        gStoreRegionFirst = first;
        gStoreRegionBlocks = blocks;
        if (firstLba) *firstLba = first;
        ok = 1;
      }
    }
  }
  if (why) *why = reason;
  return ok;
}

extern "C" int native_ufs_store_read (uint64_t rel, uint32_t blocks, void *dst)
{
  if (!gStoreRegionBlocks || !dst || !blocks || rel >= gStoreRegionBlocks || blocks > gStoreRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostRead4k (0, gStoreRegionFirst + rel, chunk, dst) != kIOReturnSuccess) return 0;
    dst = static_cast<uint8_t *> (dst) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_store_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (storeMode () != 2 || !gStoreRegionBlocks || !src || !blocks || rel >= gStoreRegionBlocks ||
      blocks > gStoreRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    // Forced unit access: the block is on the medium when the command completes, so the overlay needs no cache flush.
    if (hostWrite4k (0, gStoreRegionFirst + rel, chunk, src, true) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_store_sync (void)
{
  return storeMode () == 2 && gStoreRegionBlocks && hostSyncCache () == kIOReturnSuccess;
}

// ---- Diagnostic log window (sheng-log=1) --------------------------------------------------------------------------
// The board's log writer (kits/sheng BootLog.c) gets the last kLogReserveBlocks of userdata and the kExportBlocks below them, refused
// unless the partition holds them clear of the flashed root image plus a 4 MiB guard. Off without the boot argument.
static unsigned logMode (void)
{
  static int cached = -1;
  if (cached < 0) {
    uint32_t value = 0;
    if (!PE_parse_boot_argn ("sheng-log", &value, sizeof (value)) || value > 1) value = 0;
    cached = static_cast<int> (value);
  }
  return static_cast<unsigned> (cached);
}

// Returns 1 with the window's first absolute LBA and size in 4096-byte blocks, 0 on refusal (reason in *why, may be null).
extern "C" int native_ufs_log_open (uint64_t *firstLba, uint64_t *blocks, const char **why)
{
  const char *reason = "";
  int ok = 0;
  if (!logMode ()) reason = "off";
  else if (!gStoreGeometry.valid || !gHost.ready) reason = "no root layout";
  else if (gLogRegionBlocks) reason = "already open";
  else {
    uint64_t partition = gStoreGeometry.last - gStoreGeometry.first + 1;
    uint64_t imageBlocks = (gStoreGeometry.imageBytes + 4095) / 4096;
    if (reserveBlocks () < kLogReserveBlocks + kExportBlocks) reason = "reserve too small for the log";
    else if (partition - reserveBlocks () < imageBlocks + 1024) reason = "overlaps the root image";
    else {
      // gts7xlwifi: the last kLogReserveBlocks of winre, the export region below them, both inside winre.
      uint64_t first = gStoreGeometry.logLast + 1 - kLogReserveBlocks;
      // One write window covers the export region and the log: [first - kExportBlocks, first + kLogReserveBlocks).
      if (!gStoreGeometry.logLast || gStoreGeometry.logLast - gStoreGeometry.logFirst + 1 < kExportBlocks + kLogReserveBlocks)
        reason = "no winre partition for the log";
      else if (!hostLogOpen (first - kExportBlocks, kExportBlocks + kLogReserveBlocks)) reason = "window refused";
      else {
        gLogRegionFirst = first;
        gLogRegionBlocks = kLogReserveBlocks;
        if (firstLba) *firstLba = first;
        if (blocks) *blocks = kLogReserveBlocks;
        ok = 1;
      }
    }
  }
  if (why) *why = reason;
  return ok;
}

extern "C" int native_ufs_log_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogRegionBlocks || !src || !blocks || rel >= gLogRegionBlocks || blocks > gLogRegionBlocks - rel) return 0;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostWrite4k (0, gLogRegionFirst + rel, chunk, src) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

// The export region below the log window: its size in blocks once the log is open, else 0; writes at a block offset inside it.
extern "C" uint64_t native_ufs_export_blocks (void) { return gLogRegionBlocks ? kExportBlocks : 0; }

extern "C" int native_ufs_export_write (uint64_t rel, uint32_t blocks, const void *src)
{
  if (!gLogRegionBlocks || !src || !blocks || rel >= kExportBlocks || blocks > kExportBlocks - rel) return 0;
  const uint64_t base = gLogRegionFirst - kExportBlocks;
  while (blocks) {
    uint32_t chunk = blocks > kDataMax / 4096 ? static_cast<uint32_t> (kDataMax / 4096) : blocks;
    if (hostWrite4k (0, base + rel, chunk, src) != kIOReturnSuccess) return 0;
    src = static_cast<const uint8_t *> (src) + chunk * 4096ULL;
    rel += chunk;
    blocks -= chunk;
  }
  return 1;
}

extern "C" int native_ufs_log_sync (void)
{
  return gLogRegionBlocks && hostSyncCache () == kIOReturnSuccess;
}

// Re-read the GPT header and entry array and compare their CRCs with what was recorded at root start. 0 = unchanged.
extern "C" unsigned native_ufs_store_gpt_check (void)
{
  if (!gStoreGeometry.valid || !gHost.ready) return 1;
  static uint8_t block[4096];
  if (hostRead4k (0, 1, 1, block) != kIOReturnSuccess) return 2;
  uint32_t headerBytes = le32 (block + 12);
  if (headerBytes < 92 || headerBytes > 4096 || rootCrc32 (block, headerBytes, true) != le32 (block + 16) ||
      le32 (block + 16) != gStoreGeometry.headerCrc) return 3;
  uint32_t blocks = (gStoreGeometry.entryCount * 128 + 4095) / 4096;
  static uint8_t entries[4096 * 4];
  if (blocks > 4 || hostRead4k (0, gStoreGeometry.entriesLba, blocks, entries) != kIOReturnSuccess) return 4;
  return rootCrc32 (entries, gStoreGeometry.entryCount * 128) == gStoreGeometry.entriesCrc ? 0 : 5;
}

// One read-only window of the root image as a block device: 512-byte sectors on top of 4096-byte
// UFS blocks, so the unchanged dmg disk images (GPT with 512-byte sectors) parse as they do on
// the virtio path.
#if !defined(NATIVE_UFS_PROBE_ONLY) || !NATIVE_UFS_PROBE_ONLY
extern "C" {
extern volatile uint64_t native_ufs_block_reads;
extern volatile uint64_t native_ufs_block_failures;
}

static IOReturn wholeUserdataIoStatus (alioth::whole_userdata_root::Status status)
{
  using Status = alioth::whole_userdata_root::Status;
  switch (status) {
  case Status::Ok: return kIOReturnSuccess;
  case Status::NullArgument:
  case Status::InvalidGeometry:
  case Status::CapacityMismatch:
  case Status::MediaLbaOutOfRange:
  case Status::ZeroLength:
  case Status::Overflow:
  case Status::OutOfBounds:
  case Status::UnalignedMediaBlock:
  case Status::TransferTooLarge:
  case Status::InvalidAccess: return kIOReturnBadArgument;
  case Status::NotReady: return kIOReturnNotReady;
  case Status::ReadOnly: return kIOReturnNotWritable;
  case Status::IoError:
  case Status::End: return kIOReturnIOError;
  }
  return kIOReturnIOError;
}

// The ios-sweet whole-userdata root contract, backed by NativeUfs's already
// validated UFS transport. It exposes partition-relative 512-byte sectors and
// keeps every physical operation in 4 KiB units strictly inside GPT userdata.
class NativeUfsWholeUserdataBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors (NativeUfsWholeUserdataBlock)
  alioth::whole_userdata_root::Geometry geometry = {};
  uint8_t *bounce = nullptr;
  IOLock *ioLock = nullptr;
  bool online = false;
  bool writable = false;
  bool routeOpen = false;
  char label[16] = {};

  struct Operations {
    IOMemoryDescriptor *buffer;

    alioth::whole_userdata_root::Status readBlock (uint64_t media_lba, uint8_t *out) {
      if (hostRootRead4k (0, media_lba, 1, out) != kIOReturnSuccess) {
        return alioth::whole_userdata_root::Status::IoError;
      }
      return alioth::whole_userdata_root::Status::Ok;
    }
    alioth::whole_userdata_root::Status writeBlock (uint64_t media_lba, const uint8_t *in, bool fua) {
      if (hostRootWrite4k (0, media_lba, 1, in, fua) != kIOReturnSuccess) {
        return alioth::whole_userdata_root::Status::IoError;
      }
      return alioth::whole_userdata_root::Status::Ok;
    }
    alioth::whole_userdata_root::Status copyIn (uint64_t request_offset, uint8_t *out, uint32_t bytes) {
      if (!buffer || buffer->readBytes (request_offset, out, bytes) != bytes) {
        return alioth::whole_userdata_root::Status::IoError;
      }
      return alioth::whole_userdata_root::Status::Ok;
    }
    alioth::whole_userdata_root::Status copyOut (uint64_t request_offset, const uint8_t *in, uint32_t bytes) {
      if (!buffer || buffer->writeBytes (request_offset, in, bytes) != bytes) {
        return alioth::whole_userdata_root::Status::IoError;
      }
      return alioth::whole_userdata_root::Status::Ok;
    }
    alioth::whole_userdata_root::Status synchronize () {
      return hostRootSync () == kIOReturnSuccess
        ? alioth::whole_userdata_root::Status::Ok
        : alioth::whole_userdata_root::Status::IoError;
    }
  };

  IOReturn transfer (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                     IOStorageAttributes *attributes, IOStorageCompletion *completion,
                     bool write) {
    using Disk = alioth::whole_userdata_root::Disk<Operations>;
    using Status = alioth::whole_userdata_root::Status;
    if (!buffer || !completion || !completion->action || !online || !ioLock ||
        count == 0 || block >= geometry.logical_sector_count ||
        count > geometry.logical_sector_count - block ||
        count > UINT64_MAX / alioth::whole_userdata_root::kLogicalSectorBytes ||
        buffer->getLength () < count * alioth::whole_userdata_root::kLogicalSectorBytes) {
      return kIOReturnBadArgument;
    }
    const bool fua = write && attributes &&
      (attributes->options & kIOStorageOptionForceUnitAccess);
    const IODirection direction = buffer->getDirection ();
    if ((!write && direction != kIODirectionIn) ||
        (write && direction != kIODirectionOut)) {
      return kIOReturnBadArgument;
    }
    if (write && !writable) {
      return kIOReturnNotWritable;
    }

    Operations operations = { buffer };
    uint8_t *bounce_pointer = bounce;
    Disk disk;
    Status configured = disk.Configure (geometry, &operations, bounce_pointer, true, writable);
    if (configured != Status::Ok) {
      return wholeUserdataIoStatus (configured);
    }

    IOLockLock (ioLock);
    uint64_t absolute = geometry.first_logical_sector + block;
    alioth::whole_userdata_root::DiskResult result = write
      ? disk.Write (absolute, count, fua)
      : disk.Read (absolute, count);
    IOLockUnlock (ioLock);

    const IOReturn status = wholeUserdataIoStatus (result.status);
    if (status != kIOReturnSuccess) {
      __atomic_fetch_add (&native_ufs_block_failures, 1, __ATOMIC_RELAXED);
    } else if (!write) {
      __atomic_fetch_add (&native_ufs_block_reads, 1, __ATOMIC_RELAXED);
    }
    completion->action (completion->target, completion->parameter, status,
                        result.bytes_done);
    return kIOReturnSuccess;
  }

public:
  bool configure (const alioth::whole_userdata_root::Geometry &value, bool selectedWritable) {
    using Status = alioth::whole_userdata_root::Status;
    if (alioth::whole_userdata_root::ValidateGeometry (value) != Status::Ok ||
        ioLock || bounce) {
      return false;
    }
    geometry = value;
    writable = selectedWritable;
    strlcpy (label, "ufsroot0", sizeof (label));
    ioLock = IOLockAlloc ();
    bounce = static_cast<uint8_t *> (IOMalloc (alioth::whole_userdata_root::kMediaBlockBytes));
    if (!ioLock || !bounce) {
      if (bounce) {
        IOFree (bounce, alioth::whole_userdata_root::kMediaBlockBytes);
        bounce = nullptr;
      }
      if (ioLock) {
        IOLockFree (ioLock);
        ioLock = nullptr;
      }
      return false;
    }
    return true;
  }

  const char *rootLabel () const { return label; }
  uint64_t mediaBlockCount () const { return geometry.media_block_count; }
  uint64_t logicalSectorCount () const { return geometry.logical_sector_count; }
  bool isWritable () const { return writable; }

  bool start (IOService *provider) override {
    if (online || !ioLock || !bounce ||
        !hostRootOpen (geometry.first_media_lba, geometry.media_block_count, writable)) {
      return false;
    }
    routeOpen = true;
    if (!IOBlockStorageDevice::start (provider)) {
      hostRootClose ();
      routeOpen = false;
      return false;
    }
    OSString *rootName = OSString::withCString ("ufsroot0");
    if (rootName) {
      setProperty ("NativeUFSRootName", rootName);
      rootName->release ();
    }
    // gts7xlwifi: transfer() refuses more than kMaxTransferBytes (TransferTooLarge, EINVAL), so advertise that limit and
    // let IOBlockStorageDriver split requests. Without it every 1 MiB APFS pageout on the Data volume failed with 22
    // ("disk3: invalid argument", v19, 2026-10-31) and mapped files faulted with SIGBUS.
    static const char *const kLimits[][2] = {
      {"IOMaximumBlockCountRead", "IOMaximumByteCountRead"}, {"IOMaximumBlockCountWrite", "IOMaximumByteCountWrite"}};
    for (const auto &limit : kLimits) {
      setProperty (limit[0], alioth::whole_userdata_root::kMaxTransferSectors, 64);
      setProperty (limit[1], alioth::whole_userdata_root::kMaxTransferBytes, 64);
    }
    online = true;
    registerService ();
    return true;
  }

  void free () override {
    if (routeOpen) {
      hostRootClose ();
      routeOpen = false;
    }
    if (bounce) {
      IOFree (bounce, alioth::whole_userdata_root::kMediaBlockBytes);
      bounce = nullptr;
    }
    if (ioLock) {
      IOLockFree (ioLock);
      ioLock = nullptr;
    }
    IOBlockStorageDevice::free ();
  }

  void stop (IOService *provider) override {
    online = false;
    if (routeOpen) {
      hostRootClose ();
      routeOpen = false;
    }
    IOBlockStorageDevice::stop (provider);
  }

  IOReturn doEjectMedia () override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia (UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities (UInt64 *, UInt32) const override { return 0; }
  char *getVendorString () override { return const_cast<char *> ("Native"); }
  char *getProductString () override { return label; }
  char *getRevisionString () override { return const_cast<char *> ("1.0"); }
  char *getAdditionalDeviceInfoString () override {
    return const_cast<char *> ("NativeUfs whole GPT userdata, 512-byte logical sectors");
  }
  IOReturn reportBlockSize (UInt64 *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = alioth::whole_userdata_root::kLogicalSectorBytes;
    return kIOReturnSuccess;
  }
  IOReturn reportEjectability (bool *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn reportMaxValidBlock (UInt64 *value) override {
    if (!value || !geometry.logical_sector_count) return kIOReturnBadArgument;
    *value = geometry.logical_sector_count - 1;
    return kIOReturnSuccess;
  }
  IOReturn reportMediaState (bool *present, bool *changed) override {
    if (!present) return kIOReturnBadArgument;
    *present = online;
    if (changed) *changed = false;
    return kIOReturnSuccess;
  }
  IOReturn reportRemovability (bool *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = false;
    return kIOReturnSuccess;
  }
  IOReturn reportWriteProtection (bool *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = !writable;
    return kIOReturnSuccess;
  }
  IOReturn getWriteCacheState (bool *value) override {
    if (!value) return kIOReturnBadArgument;
    *value = writable;
    return kIOReturnSuccess;
  }
  IOReturn setWriteCacheState (bool) override { return kIOReturnUnsupported; }
  IOReturn doSynchronize (UInt64 block, UInt64 count, IOOptionBits) override {
    using Status = alioth::whole_userdata_root::Status;
    if (!online || !ioLock || !writable) {
      return kIOReturnNotWritable;
    }
    // IOBlockStorageDriver uses (0, 0) to request an end-of-media flush. A
    // non-zero request is still checked for 4K alignment and bounded to one
    // policy batch before issuing the whole-LUN cache flush.
    if (count == 0) {
      if (block != 0) return kIOReturnBadArgument;
    } else {
      if (block >= geometry.logical_sector_count ||
          count > geometry.logical_sector_count - block) {
        return kIOReturnBadArgument;
      }
      alioth::whole_userdata_root::SectorRange accepted = {};
      Status policy = alioth::whole_userdata_root::ValidateAccess (
        geometry, alioth::whole_userdata_root::AccessKind::SynchronizeCache,
        { geometry.first_logical_sector + block, count }, &accepted);
      if (policy != Status::Ok) {
        return wholeUserdataIoStatus (policy);
      }
    }
    IOLockLock (ioLock);
    const IOReturn status = hostRootSync ();
    IOLockUnlock (ioLock);
    return status;
  }
  IOReturn doSynchronizeCache () override {
    if (!online || !ioLock || !writable) {
      return online ? kIOReturnNotWritable : kIOReturnNotReady;
    }
    IOLockLock (ioLock);
    const IOReturn status = hostRootSync ();
    IOLockUnlock (ioLock);
    return status;
  }
  IOReturn doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                             IOStorageAttributes *attributes,
                             IOStorageCompletion *completion) override {
    const bool write = buffer && buffer->getDirection () == kIODirectionOut;
    return transfer (buffer, block, count, attributes, completion, write);
  }
};
OSDefineMetaClassAndStructors (NativeUfsWholeUserdataBlock, IOBlockStorageDevice)

// Keeps the selected whole-userdata service alive for the lifetime of the
// controller personality. The route is deliberately exclusive: it is never
// created alongside the SHNGROOT store/log windows.
static NativeUfsWholeUserdataBlock *gWholeUserdataDevice = nullptr;
static bool gWholeUserdataRenameDone;

// IOStorageFamily is prelinked and its IOMedia::metaClass has no recorded original address on gts7xlwifi:
// resolve the class by name at run time instead of linking against it.
static IOMedia *ufsMediaCast (OSObject *object)
{
  return object && object->metaCast ("IOMedia") ? static_cast<IOMedia *> (object) : nullptr;
}

static bool ufsHasAncestor (IORegistryEntry *entry, const IORegistryEntry *ancestor)
{
  IORegistryIterator *parents = IORegistryIterator::iterateOver (
    entry, gIOServicePlane, kIORegistryIterateRecursively | kIORegistryIterateParents);
  if (!parents) return false;
  const bool found = ufsMediaHasAncestor (*parents, ancestor);
  parents->release ();
  return found;
}

// IOBlockStorageDriver publishes a normal IOMedia and IOMediaBSDClient assigns
// diskX after that. Override the final BSD name only after this media is
// reachable from our exact whole-userdata block device.
// ---- gts7xlwifi: persistent Data container after the SHNGROOT image ----------------------------------------------
// The partition holds the SHNGROOT root image (manifest, OS, SystemOS) and, from the image's end to the partition's end,
// one original Apple APFS container whose Data volume is mounted read-write as /private/var. The container is published
// through the whole-userdata block class on the single root route, so every access is confined to that range by
// hostInRoot() and writes need ufsrw=1. Nothing here touches the image windows or any other partition.
static NativeUfsWholeUserdataBlock *gT970DataDevice;

static bool t970PublishData (IOService *owner, const RootLayout *layout)
{
  uint32_t enabled = 0, rw = 0;
  if (!PE_parse_boot_argn ("t970-ufs-data", &enabled, sizeof (enabled)) || enabled != 1) return true;
  PE_parse_boot_argn ("ufsrw", &rw, sizeof (rw));
  if (!layout->imageBytes || (layout->imageBytes & 4095)) {
    IOLog ("NativeUfs: t970 Data refused: image bytes %llx\n", layout->imageBytes);
    return false;
  }
  const uint64_t first = layout->userdataFirst + layout->imageBytes / 4096, last = layout->userdataLast;
  if (first > last) {
    IOLog ("NativeUfs: t970 Data refused: no room after the image (first=%llx last=%llx)\n", first, last);
    return false;
  }
  uint8_t *block = static_cast<uint8_t *> (IOMalloc (4096));
  if (!block) return false;
  const IOReturn read = hostRead4k (0, first, 1, block);
  const bool container = read == kIOReturnSuccess && !memcmp (block + 32, "NXSB", 4);
  IOFree (block, 4096);
  if (!container) {
    IOLog ("NativeUfs: t970 Data refused: no APFS container at %llx (read %x)\n", first, read);
    return false;
  }
  const uint64_t blocks = last - first + 1;
  uint64_t capacity = 0;
  alioth::whole_userdata_root::Geometry geometry = {};
  if (!alioth::whole_userdata_root::CheckedMultiply (blocks, alioth::whole_userdata_root::kMediaBlockBytes, &capacity) ||
      alioth::whole_userdata_root::ProjectGeometry (first, last, capacity, &geometry) !=
        alioth::whole_userdata_root::Status::Ok) {
    IOLog ("NativeUfs: t970 Data geometry rejected first=%llx last=%llx\n", first, last);
    return false;
  }
  NativeUfsWholeUserdataBlock *device = OSTypeAlloc (NativeUfsWholeUserdataBlock);
  if (!device || !device->init (static_cast<OSDictionary *> (nullptr)) || !device->configure (geometry, rw == 1)) {
    if (device) device->release ();
    return false;
  }
  if (!device->attach (owner)) {
    device->release ();
    return false;
  }
  if (!device->start (owner)) {
    device->detach (owner);
    device->release ();
    return false;
  }
  gT970DataDevice = device;
  IOLog ("NativeUfs: t970 Data published first=%llx last=%llx blocks=%llu writable=%u\n",
         first, last, blocks, rw == 1 ? 1U : 0U);
  return true;
}

// The Data volume's BSD device, for the kernel's owner Data mount: the one IOMedia whose UUID is the given volume
// UUID and whose provider chain reaches the published Data container. Waits for APFS to publish it.
extern "C" int t970_ufs_data_volume (const char *uuid, dev_t *device, char *path, size_t size, uint64_t *bytes)
{
  if (!uuid || strlen (uuid) != 36 || !device || !path || size < 8 || !bytes || !gT970DataDevice) return ENXIO;
  *device = NODEV;
  path[0] = 0;
  *bytes = 0;
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity (1);
  OSString *wanted = OSString::withCString (uuid);
  bool ready = matching && properties && wanted && properties->setObject ("UUID", wanted) &&
               matching->setObject ("IOPropertyMatch", properties);
  OSSafeReleaseNULL (wanted);
  OSSafeReleaseNULL (properties);
  if (!ready) {
    OSSafeReleaseNULL (matching);
    return ENOMEM;
  }
  matching->retain ();
  IOService *first = IOService::waitForMatchingService (matching, 30ULL * NSEC_PER_SEC);
  OSSafeReleaseNULL (first);
  OSIterator *iterator = IOService::getMatchingServices (matching);
  matching->release ();
  matching->release ();
  if (!iterator) return ENOENT;
  unsigned matches = 0;
  int result = ENOENT;
  while (IOMedia *media = ufsMediaCast (iterator->getNextObject ())) {
    if (++matches != 1) { result = EEXIST; break; }
    OSString *name = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
    OSNumber *major = OSDynamicCast (OSNumber, media->getProperty (kIOBSDMajorKey));
    OSNumber *minor = OSDynamicCast (OSNumber, media->getProperty (kIOBSDMinorKey));
    if (!ufsHasAncestor (media, gT970DataDevice) || !name || !major || !minor || !name->getLength () ||
        name->getLength () + 6 >= size || major->unsigned64BitValue () > 255 || minor->unsigned64BitValue () > 0xffffff) {
      result = EPERM;
      continue;
    }
    *device = static_cast<dev_t> (makedev (major->unsigned32BitValue (), minor->unsigned32BitValue ()));
    snprintf (path, size, "/dev/%s", name->getCStringNoCopy ());
    *bytes = gT970DataDevice->mediaBlockCount () * alioth::whole_userdata_root::kMediaBlockBytes;
    result = 0;
  }
  iterator->release ();
  if (matches != 1) result = matches ? EEXIST : ENOENT;
  IOLog ("NativeUfs: t970 Data volume %s -> %s (%d, %u matches)\n", uuid, path, result, matches);
  return result;
}

static void renameWholeUserdataThread (void *, wait_result_t)
{
  NativeUfsWholeUserdataBlock *device = gWholeUserdataDevice;
  if (!device) return;
  device->retain ();

  bool observedAssignedName = false;
  for (unsigned tick = 0; tick < 300; ++tick) {
    bool renamedThisTick = false;
    OSDictionary *matching = IOService::serviceMatching ("IOMedia");
    OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
    if (iterator) {
      while (IOMedia *media = ufsMediaCast (iterator->getNextObject ())) {
        if (!media->isWhole () || !ufsHasAncestor (media, device)) continue;
        OSString *oldName = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
        if (!oldName || !oldName->getLength ()) continue;
        observedAssignedName = true;
        if (!strcmp (oldName->getCStringNoCopy (), "ufsroot0")) {
          continue;
        }
        OSString *newName = OSString::withCString ("ufsroot0");
        if (!newName) continue;
        media->setProperty (kIOBSDNameKey, newName);
        newName->release ();
        IOLog ("NativeUfs: whole-userdata IOMedia BSD name %s -> ufsroot0 entry=%llu\n",
               oldName->getCStringNoCopy (), media->getRegistryEntryID ());
        renamedThisTick = true;
      }
      iterator->release ();
    }
    OSSafeReleaseNULL (matching);
    if (observedAssignedName) {
      // Verify on a later tick so a racing IOMediaBSDClient assignment can be
      // detected and corrected before root selection.
      if (OSDictionary *verifyMatch = IOService::serviceMatching ("IOMedia")) {
        OSIterator *verify = IOService::getMatchingServices (verifyMatch);
        bool assigned = false;
        if (verify) {
          while (IOMedia *media = ufsMediaCast (verify->getNextObject ())) {
            if (!media->isWhole () || !ufsHasAncestor (media, device)) continue;
            OSString *name = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
            if (name && name->getLength () && !strcmp (name->getCStringNoCopy (), "ufsroot0")) {
              assigned = true;
              break;
            }
          }
          verify->release ();
        }
        OSSafeReleaseNULL (verifyMatch);
        if (assigned && !renamedThisTick) {
          __atomic_store_n (&gWholeUserdataRenameDone, true, __ATOMIC_RELEASE);
          break;
        }
      }
    }
    IOSleep (100);
  }
  if (!__atomic_load_n (&gWholeUserdataRenameDone, __ATOMIC_ACQUIRE)) {
    IOLog ("NativeUfs: whole-userdata BSD rename timed out\n");
  }
  device->release ();
}

class NativeUfsBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors (NativeUfsBlock)
  uint64_t absolute = 0;      // byte offset of the window on LUN 0
  uint64_t bytes = 0;
  char label[32] = {};
  uint8_t *bounce = nullptr;
  IOLock *windowLock = nullptr;
  bool online = false;
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  struct vr_view verified = {};
  vm_offset_t verifiedStorage = 0;
  vm_size_t verifiedStorageBytes = 0;
  bool configureVerification ();
#endif
public:
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  bool authenticatesVolume (const char *uuid, uint8_t *manifestDigest = nullptr) const;
  bool signedVolumeUuid (char out[37]) const;
#endif
  const char *windowLabel () const { return label; }
  bool configure (const char *name, uint64_t absoluteOffset, uint64_t size);
  bool start (IOService *provider) override;
  void free () override;
  IOReturn doEjectMedia () override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia (UInt64) override { return kIOReturnNotWritable; }
  UInt32 doGetFormatCapacities (UInt64 *, UInt32) const override { return 0; }
  char *getVendorString () override { return const_cast<char *> ("Native"); }
  char *getProductString () override { return label; }
  char *getRevisionString () override { return const_cast<char *> ("1.0"); }
  char *getAdditionalDeviceInfoString () override { return const_cast<char *> ("UFS LUN 0 userdata window, read-only"); }
  IOReturn reportBlockSize (UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock (UInt64 *value) override { *value = bytes / 512 - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState (bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection (bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache () override { return online ? kIOReturnSuccess : kIOReturnNotReady; }
  IOReturn doSynchronize (UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache (); }
  IOReturn getWriteCacheState (bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState (bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite (IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors (NativeUfsBlock, IOBlockStorageDevice)

extern "C" {
volatile uint32_t native_ufs_blocks_published = 0;
volatile uint64_t native_ufs_block_reads = 0;
volatile uint64_t native_ufs_block_failures = 0;
}

static char gRootDiagnosticText[2500];
static char gRootMediaText[1000];
// Passive diagnostic reader: cached software values only. It must remain safe
// when called from an IRQ while the transport is busy: no locks, allocations,
// MMIO reads, queue resets, or changes to the command/trust state. Fields can
// represent adjacent updates; this is explicitly not an atomic HW snapshot.
extern "C" void native_ufs_progress_text (char *out, size_t size)
{
  UfsRootDiagnostics sample = gRootDiagnostic;
  sample.published = native_ufs_blocks_published;
  sample.reads = native_ufs_block_reads;
  sample.failures = native_ufs_block_failures;
  formatUfsRootDiagnostics (out, size, sample, "cached values; not a new HW fault sample", "UFS CACHED SNAPSHOT");
}

// Polled-transport counters, one row for the board log. Same contract as above: cached software
// values only, no lock, no MMIO, safe while a command is in flight. inflight/held are the ages of
// the command now on the wire and of the current gHost.lock hold (0 = none); a value that keeps
// growing from one slot to the next is a transfer that is not coming back.
extern "C" int native_ufs_poll_text (char *out, size_t size)
{
  if (!out || !size) return 0;
  const UfsPollStats s = gUfsPoll;
  const uint64_t now = mach_absolute_time ();
  auto us = [] (uint64_t ticks) -> unsigned long long {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds (ticks, &ns);
    return static_cast<unsigned long long> (ns / 1000);
  };
  const uint64_t inflight = s.inflightSince && now > s.inflightSince ? now - s.inflightSince : 0;
  const uint64_t held = s.lockHeldSince && now > s.lockHeldSince ? now - s.lockHeldSince : 0;
  return snprintf (out, size,
                   "[ufs poll] cmds=%llu slow=%llu sleeps=%llu max=%lluus to=%u fatal=%u | lock wait=%lluus log=%lluus "
                   "hold=%lluus logw=%u | now inflight=%lluus op=%02x held=%lluus poisoned=%d",
                   static_cast<unsigned long long> (s.commands), static_cast<unsigned long long> (s.slowCommands),
                   static_cast<unsigned long long> (s.sleeps), us (s.maxTicks), s.timeouts, s.fatals,
                   us (s.lockWaitMax), us (s.logLockWaitMax), us (s.lockHoldMax), s.logWrites,
                   us (inflight), s.inflightOpcode & 0xff, us (held), gHost.poisoned ? 1 : 0);
}
#else
volatile uint32_t native_ufs_blocks_published = 0;
volatile uint64_t native_ufs_block_reads = 0;
volatile uint64_t native_ufs_block_failures = 0;
static char gRootDiagnosticText[2500];
static char gRootMediaText[1000];
#endif

static void rootDiagnosticFailure (const char *reason)
{
  // Kept even when the panic diagnostic is off, so the boot page can say why nothing was published.
  native_ufs_root_failure = reason;
  if (!gRootDiagnosticEnabled) return;
  gRootDiagnostic.published = native_ufs_blocks_published;
  gRootDiagnostic.reads = native_ufs_block_reads;
  gRootDiagnostic.failures = native_ufs_block_failures;
  formatUfsRootDiagnostics (gRootDiagnosticText, sizeof (gRootDiagnosticText), gRootDiagnostic, reason);
  strlcat (gRootDiagnosticText, gRootMediaText, sizeof (gRootDiagnosticText));
  panic ("%s", gRootDiagnosticText);
}

// UfsRootPhase comes from RootPublication.h, which a probe-only build leaves out, and every caller of
// this mapping is itself inside the block-publication region. Guard it with them so the probe-only
// subset still compiles.
#if !defined(NATIVE_UFS_PROBE_ONLY) || !NATIVE_UFS_PROBE_ONLY
static IOReturn rootPublishResult (UfsRootPhase phase)
{
  switch (phase) {
  case UfsRootPhase::Identity: return kIOReturnNotPrivileged;
  case UfsRootPhase::Schedule: return kIOReturnNoResources;
  case UfsRootPhase::Prepare: return kIOReturnError;
  case UfsRootPhase::Start: return kIOReturnIOError;
  case UfsRootPhase::Media: return kIOReturnTimeout;
  case UfsRootPhase::Complete: return kIOReturnSuccess;
  }
  return kIOReturnError;
}
#endif

#if !defined(NATIVE_UFS_PROBE_ONLY) || !NATIVE_UFS_PROBE_ONLY
bool NativeUfsBlock::configure (const char *name, uint64_t absoluteOffset, uint64_t size)
{
  if (!name || !ufsWindowGeometry (absoluteOffset, size)) {
    return false;
  }
  strlcpy (label, name, sizeof (label));
  absolute = absoluteOffset;
  bytes = size;
  windowLock = IOLockAlloc ();
  if (!windowLock) return false;
  bounce = static_cast<uint8_t *> (IOMalloc (kDataMax));
  if (!bounce) {
    return false;
  }
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  // Reads are served only after each 4096-byte integrity block verifies against the owner-signed
  // policy; without a matching policy the device is never published.
  return configureVerification ();
#else
  return true;
#endif
}

#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
volatile uint64_t native_ufs_verified_blocks = 0;
// Time spent hashing/verifying and reading from the UFS transport, in generic-timer ticks (diagnostic only).
volatile uint64_t native_ufs_verify_ticks = 0, native_ufs_read_ticks = 0;
static inline uint64_t timerTicks (void) {uint64_t v; __asm__ volatile ("isb\n\tmrs %0, cntpct_el0" : "=r" (v)); return v;}
volatile uint32_t native_ufs_integrity_failures = 0;

bool NativeUfsBlock::configureVerification ()
{
  gRootDiagnostic.stage = "policy selection";
  gRootDiagnostic.windowBytes = bytes;
  gRootDiagnostic.policyCount = verified_root_policy_count;
  gRootDiagnostic.verify = 0xffffffff;
  // Select the one policy whose signed image size equals this window; two matches are refused.
  const uint8_t *manifest = nullptr;
  uint64_t manifestBytes = 0;
  if (!verified_root_policy_count || verified_root_policy_count > 16) {
    gRootDiagnostic.stage = "policy count invalid";
    return false;
  }
  for (uint64_t index = 0; index < verified_root_policy_count; ++index) {
    const NativeVerifiedBlockPolicy &policy = verified_root_policies[index];
    if (!policy.manifest || policy.manifestBytes < VR_HEADER_BYTES + VR_SIGNATURE_BYTES) {
      gRootDiagnostic.stage = "policy envelope invalid";
      return false;
    }
    uint64_t imageBytes = 0;
    for (unsigned i = 0; i < 8; ++i) {
      imageBytes |= static_cast<uint64_t> (policy.manifest[24 + i]) << (8 * i);
    }
    if (imageBytes != bytes) {
      continue;
    }
    if (manifest) {
      gRootDiagnostic.stage = "duplicate capacity-matching policy";
      return false;
    }
    manifest = policy.manifest;
    manifestBytes = policy.manifestBytes;
  }
  if (!manifest) {
    gRootDiagnostic.stage = "no capacity-matching policy";
    return false;
  }
  gRootDiagnostic.stage = "policy storage";
  vm_size_t tableBytes = round_page (static_cast<vm_size_t> (manifestBytes));
  if (tableBytes < manifestBytes || tableBytes > UINT64_MAX - 4096) {
    gRootDiagnostic.stage = "policy size overflow";
    return false;
  }
  verifiedStorageBytes = tableBytes + 4096;
  gRootDiagnostic.stage = "policy allocation";
  if (kmem_alloc (kernel_map, &verifiedStorage, verifiedStorageBytes,
                  static_cast<kma_flags_t> (KMA_KOBJECT | KMA_DATA), VM_KERN_MEMORY_SECURITY) != KERN_SUCCESS) {
    return false;
  }
  memcpy (reinterpret_cast<void *> (verifiedStorage), manifest, manifestBytes);
  uint8_t *key = reinterpret_cast<uint8_t *> (verifiedStorage + tableBytes);
  memcpy (key, verified_root_key, VR_KEY_BYTES);
  gRootDiagnostic.stage = "policy read-only protection";
  if (vm_map_protect (kernel_map, verifiedStorage, verifiedStorage + verifiedStorageBytes,
                      VM_PROT_READ, TRUE) != KERN_SUCCESS) {
    return false;
  }
  gRootDiagnostic.stage = "owner signature and hash table";
  int status = vr_open (reinterpret_cast<const uint8_t *> (verifiedStorage), manifestBytes, key, 1,
                        &vr_xnu_crypto, &verified);
  gRootDiagnostic.verify = static_cast<uint32_t> (status);
  return status == VR_OK && verified.image_bytes == bytes;
}

bool NativeUfsBlock::authenticatesVolume (const char *uuid, uint8_t *manifestDigest) const
{
  if (!uuid || !online || !verifiedStorage || !verified.hashes) {
    return false;
  }
  static const char hex[] = "0123456789ABCDEF";
  char expected[37];
  unsigned at = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      expected[at++] = '-';
    }
    expected[at++] = hex[verified.volume_uuid[i] >> 4];
    expected[at++] = hex[verified.volume_uuid[i] & 15];
  }
  expected[at] = 0;
  if (strcmp (uuid, expected)) {
    return false;
  }
  if (manifestDigest) {
    // Hash only the authenticated envelope, excluding allocation padding and the key.
    const uint64_t envelope = VR_HEADER_BYTES + VR_SIGNATURE_BYTES + verified.block_count * 32;
    SHA384_CTX context;
    SHA384_Init (&context);
    SHA384_Update (&context, reinterpret_cast<const void *> (verifiedStorage), envelope);
    SHA384_Final (manifestDigest, &context);
  }
  return true;
}

bool NativeUfsBlock::signedVolumeUuid (char out[37]) const
{
  if (!verifiedStorage || !verified.hashes) {
    return false;
  }
  static const char hex[] = "0123456789ABCDEF";
  unsigned at = 0;
  for (unsigned i = 0; i < 16; ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out[at++] = '-';
    }
    out[at++] = hex[verified.volume_uuid[i] >> 4];
    out[at++] = hex[verified.volume_uuid[i] & 15];
  }
  out[at] = 0;
  return true;
}

// Called from the root authorization query in NativeVirtioBlock: -1 when `provider` is not a
// UFS window, otherwise 1 if its signed volume UUID equals `uuid`, else 0.
extern "C" int native_ufs_block_authenticates (IOService *provider, const char *uuid, uint8_t *digest)
{
  NativeUfsBlock *block = OSDynamicCast (NativeUfsBlock, provider);
  if (!block) {
    return -1;
  }
  return block->authenticatesVolume (uuid, digest) ? 1 : 0;
}
#endif

bool NativeUfsBlock::start (IOService *provider)
{
  if (!bounce || !windowLock || !IOBlockStorageDevice::start (provider)) {
    return false;
  }
  online = true;
  registerService ();
  return true;
}

IOReturn NativeUfsBlock::doAsyncReadWrite (IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                           IOStorageAttributes *, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= bytes / 512 ||
      count > bytes / 512 - block || buffer->getLength () < count * 512) {
    return kIOReturnBadArgument;
  }
  if (buffer->getDirection () != kIODirectionIn) {
    return kIOReturnNotWritable;
  }
  if (!online) {
    return kIOReturnNotReady;
  }
  struct Operations {
    IOLock *mutex;
    IOMemoryDescriptor *buffer;
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
    const vr_view *view;
#endif
    void lock () { IOLockLock (mutex); }
    void unlock () { IOLockUnlock (mutex); }
    IOReturn read (uint64_t lba, uint32_t blocks, void *out) {
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
      uint64_t t0 = timerTicks ();
      IOReturn r = hostRead4k (0, lba, blocks, out);
      __atomic_fetch_add (&native_ufs_read_ticks, timerTicks () - t0, __ATOMIC_RELAXED);
      return r;
#else
      return hostRead4k (0, lba, blocks, out);
#endif
    }
    IOReturn verify (uint64_t index, const void *data, size_t length) {
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
      uint64_t t0 = timerTicks ();
      int status = vr_verify_block (view, index, data, length, &vr_xnu_crypto);
      __atomic_fetch_add (&native_ufs_verify_ticks, timerTicks () - t0, __ATOMIC_RELAXED);
      if (status != VR_OK) {
        gRootDiagnostic.stage = "read block integrity";
        gRootDiagnostic.verify = static_cast<uint32_t> (status);
        __atomic_fetch_add (&native_ufs_integrity_failures, 1, __ATOMIC_RELAXED);
        return kIOReturnNotPrivileged;
      }
      __atomic_fetch_add (&native_ufs_verified_blocks, 1, __ATOMIC_RELAXED);
      if (boot_progress_poll) boot_progress_poll ();
#else
      (void) index; (void) data; (void) length;
#endif
      return kIOReturnSuccess;
    }
    uint64_t copy (uint64_t offset, const void *data, uint64_t length) {
      uint64_t copied = buffer->writeBytes (offset, data, length);
      if (copied == length) __atomic_fetch_add (&native_ufs_block_reads, 1, __ATOMIC_RELAXED);
      return copied;
    }
  } operations = { windowLock, buffer
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
    , &verified
#endif
  };
  UfsWindowResult outcome = ufsReadWindow (operations, absolute, bytes, block, count, bounce);
  IOReturn result = outcome.status;
  uint64_t done = outcome.done;
  if (result != kIOReturnSuccess) {
    __atomic_fetch_add (&native_ufs_block_failures, 1, __ATOMIC_RELAXED);
    gRootDiagnostic.result = static_cast<uint32_t> (result);
    rootDiagnosticFailure ("root window read failed");
  }
  completion->action (completion->target, completion->parameter, result, done);
  return kIOReturnSuccess;
}

void NativeUfsBlock::free ()
{
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
  if (verifiedStorage) {
    kmem_free (kernel_map, verifiedStorage, verifiedStorageBytes);
    verifiedStorage = 0;
  }
#endif
  if (bounce) {
    IOFree (bounce, kDataMax);
    bounce = nullptr;
  }
  if (windowLock) {
    IOLockFree (windowLock);
    windowLock = nullptr;
  }
  IOBlockStorageDevice::free ();
}

// Select the whole GPT userdata partition as a 512-byte-sector block device.
// This is a separate, explicit route from the SHNGROOT window path: it does
// not require a SHNGROOT manifest, but it only runs after the board opts in
// and the first block identifies an HFS+ or HFSX volume. The route stays
// read-only unless ufsrw=1 is present, and hostRootOpen then confines every
// data operation to the exact GPT range.
bool NativeUfs::startWholeUserdataRoot (IOService *provider)
{
  uint32_t writableArg = 0;
  PE_parse_boot_argn ("ufsrw", &writableArg, sizeof (writableArg));
  if (writableArg > 1) {
    rootDiagnosticFailure ("ufsrw must be 0 or 1");
    return false;
  }
  const bool writable = writableArg == 1;

  const char *why = "";
  native_ufs_root_step = 1;
  if (!hostBringUp (&why)) {
    rootDiagnosticFailure (why);
    IOLog ("NativeUfs: whole-userdata bring-up failed: %s\n", why);
    return false;
  }

  native_ufs_root_step = 2;
  RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
  if (!layout || !findRootLayout (layout, &why)) {
    rootDiagnosticFailure (layout ? why : "whole-userdata layout allocation failed");
    IOLog ("NativeUfs: whole-userdata layout failed: %s\n", why);
    if (layout) IOFree (layout, sizeof (RootLayout));
    return false;
  }

  if (layout->lbaBytes != alioth::whole_userdata_root::kMediaBlockBytes) {
    rootDiagnosticFailure ("whole-userdata media block size is not 4 KiB");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  if (layout->hfsMagic != 0x482b && layout->hfsMagic != 0x4858) {
    rootDiagnosticFailure ("whole-userdata first block is not HFS+");
    IOLog ("NativeUfs: whole-userdata refused: HFS magic=%04x first=%llx last=%llx\n",
           layout->hfsMagic, layout->userdataFirst, layout->userdataLast);
    IOFree (layout, sizeof (RootLayout));
    return false;
  }

  const uint64_t mediaBlocks = layout->userdataLast - layout->userdataFirst + 1;
  uint64_t capacity = 0;
  if (!alioth::whole_userdata_root::CheckedMultiply (
        mediaBlocks, alioth::whole_userdata_root::kMediaBlockBytes, &capacity)) {
    rootDiagnosticFailure ("whole-userdata capacity overflow");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  alioth::whole_userdata_root::Geometry geometry = {};
  const alioth::whole_userdata_root::Status geometryStatus =
    alioth::whole_userdata_root::ProjectGeometry (
      layout->userdataFirst, layout->userdataLast, capacity, &geometry);
  if (geometryStatus != alioth::whole_userdata_root::Status::Ok) {
    rootDiagnosticFailure ("whole-userdata GPT geometry rejected");
    IOLog ("NativeUfs: whole-userdata geometry rejected status=%u first=%llx last=%llx\n",
           static_cast<unsigned> (geometryStatus), layout->userdataFirst, layout->userdataLast);
    IOFree (layout, sizeof (RootLayout));
    return false;
  }

  if (!IOService::start (provider)) {
    rootDiagnosticFailure ("whole-userdata IOService start failed");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }

  NativeUfsWholeUserdataBlock *device = OSTypeAlloc (NativeUfsWholeUserdataBlock);
  if (!device || !device->init (static_cast<OSDictionary *> (nullptr)) ||
      !device->configure (geometry, writable)) {
    if (device) device->release ();
    IOService::stop (provider);
    rootDiagnosticFailure ("whole-userdata block configuration failed");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  if (!device->attach (this)) {
    device->release ();
    IOService::stop (provider);
    rootDiagnosticFailure ("whole-userdata block attachment failed");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  if (!device->start (this)) {
    device->detach (this);
    device->release ();
    IOService::stop (provider);
    rootDiagnosticFailure ("whole-userdata block service start failed");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }

  gWholeUserdataDevice = device;
  gWholeUserdataRenameDone = false;
  thread_t renameThread = nullptr;
  if (kernel_thread_start (renameWholeUserdataThread, nullptr, &renameThread) != KERN_SUCCESS) {
    IOLog ("NativeUfs: whole-userdata BSD rename thread creation failed\n");
  } else {
    thread_deallocate (renameThread);
  }
  native_ufs_root_step = 3;
  IOLog ("NativeUfs: whole-userdata root published first=%llx last=%llx blocks=%llu logical=%llu writable=%u\n",
         geometry.first_media_lba, geometry.last_media_lba, geometry.media_block_count,
         geometry.logical_sector_count, writable ? 1U : 0U);
  IOFree (layout, sizeof (RootLayout));
  return true;
}

// Ordered publication of the root windows. The original owner-trust code resolves the SystemOS
// volume by UUID immediately after the root is mounted, so SystemOS must already have published its
// volume media by then; the OS window (whose volume is the root being waited for) goes last.
static NativeUfsBlock *gPendingDevice[4];
static unsigned gPending;
static bool gPendingAttached[4];
static char gPendingUuid[4][37];
static UfsRootPlan gPublicationPlan;
static NativeUfs *gPendingProvider;

static bool mediaWithUuidExists (const char *uuid, NativeUfsBlock *window)
{
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity (1);
  OSString *wanted = OSString::withCString (uuid);
  bool found = false;
  if (matching && properties && wanted && properties->setObject ("UUID", wanted) &&
      matching->setObject ("IOPropertyMatch", properties)) {
    OSIterator *iterator = IOService::getMatchingServices (matching);
    if (iterator) {
      for (unsigned matches = 0; matches < 64 && !found; ++matches) {
        IORegistryEntry *media = OSDynamicCast (IORegistryEntry, iterator->getNextObject ());
        if (!media) break;
        IORegistryIterator *parents = IORegistryIterator::iterateOver (
          media, gIOServicePlane, kIORegistryIterateRecursively | kIORegistryIterateParents);
        if (parents) {
          found = ufsMediaHasAncestor (*parents, window);
          parents->release ();
        }
      }
      iterator->release ();
    }
  }
  OSSafeReleaseNULL (wanted);
  OSSafeReleaseNULL (properties);
  OSSafeReleaseNULL (matching);
  return found;
}

static void captureMissingMedia (const char *uuid)
{
  int used = snprintf (gRootMediaText, sizeof (gRootMediaText), "Waiting for signed UUID %s\n", uuid);
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  unsigned count = 0;
  while (iterator && used >= 0 && static_cast<size_t> (used) < sizeof (gRootMediaText) - 180 &&
         count++ < 8) {
    IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
    if (!media) break;
    OSString *id = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSString *bsd = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
    OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
    const int written = snprintf (gRootMediaText + used, sizeof (gRootMediaText) - static_cast<size_t> (used),
                                  "media %.24s bytes=%llx uuid=%.36s\n",
                                  bsd ? bsd->getCStringNoCopy () : "-",
                                  size ? size->unsigned64BitValue () : 0ULL,
                                  id ? id->getCStringNoCopy () : "-");
    if (written < 0 || static_cast<size_t> (written) >= sizeof (gRootMediaText) - static_cast<size_t> (used)) {
      break;
    }
    used += written;
  }
  OSSafeReleaseNULL (iterator);
  OSSafeReleaseNULL (matching);
}

// Used only for objects that were never started/published. Started objects
// retain their owner references even on a later failure; clients can have I/O.
static void discardPendingWindow (unsigned index)
{
  NativeUfsBlock *device = gPendingDevice[index];
  if (!device) return;
  if (gPendingAttached[index]) device->detach (gPendingProvider);
  gPendingAttached[index] = false;
  gPendingDevice[index] = nullptr;
  device->release ();
}

static void publishThread (void *, wait_result_t)
{
  struct Operations {
    bool start (unsigned index) {
      NativeUfsBlock *device = gPendingDevice[index];
      gRootDiagnostic.stage = "root window service start";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Start);
      gRootDiagnostic.publicationWindow = index;
      if (!device->start (gPendingProvider)) return false;
      native_ufs_blocks_published = native_ufs_blocks_published + 1;
      IOLog ("NativeUfs: published %s\n", device->windowLabel ());
      return true;
    }
    bool wait (unsigned index) {
      NativeUfsBlock *device = gPendingDevice[index];
      const char *uuid = gPendingUuid[index];
      gRootDiagnostic.stage = "media publication";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Media);
      gRootDiagnostic.publicationWindow = index;
      bool ready = false;
      // Diagnostic mode affects reporting only, never the dependency gate.
      for (unsigned tick = 0; tick <= 300; ++tick) {
        ready = mediaWithUuidExists (uuid, device);
        if (ready || tick == 300) break;
        IOSleep (100);
      }
      IOLog ("NativeUfs: %s volume %s %s\n", device->windowLabel (), uuid, ready ? "ready" : "not ready");
      if (!ready) captureMissingMedia (uuid);
      return ready;
    }
    void discard (unsigned index) { discardPendingWindow (index); }
  } operations;
  UfsRootPublicationResult result = ufsPublishRootWindows (operations, gPublicationPlan);
  if (result.phase != UfsRootPhase::Complete) {
    gRootDiagnostic.result = static_cast<uint32_t> (rootPublishResult (result.phase));
    const char *reason = result.phase == UfsRootPhase::Start ? "root window service start failed" :
      (!strcmp (gPendingDevice[result.index]->windowLabel (), "SystemOS") ?
       "SystemOS signed volume not found" : "signed root window volume not found");
    IOLog ("NativeUfs: root publisher stopped at window %u; published=%u\n", result.index, result.published);
    // Start failures have already released their unpublished object; do not
    // dereference it here. The media-failure object is deliberately retained.
    rootDiagnosticFailure (reason);
  }
  gPendingProvider->release ();  // Balanced with successful thread dispatch.
}
#endif

#if !defined(NATIVE_UFS_PROBE_ONLY) || !NATIVE_UFS_PROBE_ONLY
// Memory snapshot (native-ufs-probe=13): root mode as usual, then, after a delay, the kernel's own
// page counters on the panic screen. Observation only.
extern "C" {
extern unsigned int vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
  vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count;
extern uint32_t vm_page_pages;
extern unsigned int memorystatus_available_pages;
extern int nprocs;
}

static void statsThread (void *argument, wait_result_t)
{
  unsigned delay = static_cast<unsigned> (reinterpret_cast<uintptr_t> (argument));
  IOSleep (delay * 1000);
  panic ("NATIVE_UFS memory snapshot after %u s\n"
         "pages total=%u free=%u active=%u inactive=%u wired=%u spec=%u purgeable=%u\n"
         "available=%u max_mem=%llx procs=%d blocks=%llu\n",
         delay, vm_page_pages, vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
         vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count,
         memorystatus_available_pages, max_mem, nprocs, native_ufs_block_reads);
}

// Storage discovery report (native-ufs-probe=12): a separate thread waits for the original
// IOStorageFamily to publish IOMedia for the root windows, then shows what exists on the screen.
// It must not run inside the matching thread that is starting NativeUfs.
static char gMediaReport[3000];

static void mediaReportThread (void *, wait_result_t)
{
  unsigned count = 0;
  for (unsigned tick = 0; tick < 200; tick++) {      // up to 20 s
    OSDictionary *matching = IOService::serviceMatching ("IOMedia");
    OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
    count = 0;
    if (iterator) {
      while (iterator->getNextObject ()) {
        count++;
      }
      iterator->release ();
    }
    if (matching) {
      matching->release ();
    }
    if (count >= 4) {
      break;
    }
    IOSleep (100);
  }
  IOSleep (500);
  int used = snprintf (gMediaReport, sizeof (gMediaReport),
                       "NATIVE_UFS media report: published=%u reads=%llu failures=%llu media=%u\n",
                       native_ufs_blocks_published, native_ufs_block_reads, native_ufs_block_failures, count);
  OSDictionary *matching = IOService::serviceMatching ("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices (matching) : nullptr;
  while (iterator && used >= 0 && static_cast<size_t> (used) < sizeof (gMediaReport) - 200) {
    IOService *media = OSDynamicCast (IOService, iterator->getNextObject ());
    if (!media) {
      break;
    }
    OSString *uuid = OSDynamicCast (OSString, media->getProperty ("UUID"));
    OSString *bsd = OSDynamicCast (OSString, media->getProperty (kIOBSDNameKey));
    OSString *content = OSDynamicCast (OSString, media->getProperty ("Content"));
    OSNumber *size = OSDynamicCast (OSNumber, media->getProperty ("Size"));
    const int written = snprintf (gMediaReport + used, sizeof (gMediaReport) - static_cast<size_t> (used),
                                  "%s %s size=%llx uuid=%s\n",
                                  bsd ? bsd->getCStringNoCopy () : "-",
                                  content ? content->getCStringNoCopy () : "-",
                                  size ? size->unsigned64BitValue () : 0ULL,
                                  uuid ? uuid->getCStringNoCopy () : "-");
    if (written < 0 || static_cast<size_t> (written) >= sizeof (gMediaReport) - static_cast<size_t> (used)) {
      break;
    }
    used += written;
  }
  if (iterator) {
    iterator->release ();
  }
  if (matching) {
    matching->release ();
  }
  panic ("%s", gMediaReport);
}
#endif

bool NativeUfs::start (IOService *provider)
{
  unsigned probe = 0, rootMode = 0, wholeUserdataRoot = 0;
  PE_parse_boot_argn ("native-ufs-probe", &probe, sizeof (probe));
  PE_parse_boot_argn ("native-ufs-root", &rootMode, sizeof (rootMode));
  bool hasWholeUserdataRoot =
    PE_parse_boot_argn ("native-ufs-whole-userdata-root", &wholeUserdataRoot, sizeof (wholeUserdataRoot));
  if (hasWholeUserdataRoot && wholeUserdataRoot > 1) {
    IOLog ("NativeUfs: native-ufs-whole-userdata-root must be 0 or 1 (got %u)\n", wholeUserdataRoot);
    return false;
  }
  // The HFS whole-userdata route and the SHNGROOT window route must never
  // publish two root personalities at once. Probes are also exclusive so a
  // diagnostic run cannot be mistaken for a requested root.
  if (wholeUserdataRoot && (probe || rootMode)) {
    IOLog ("NativeUfs: native-ufs-whole-userdata-root conflicts with probe=%u root=%u\n",
           probe, rootMode);
    return false;
  }
  UfsStreamConfig streams = ufsStreams ();
  IOLog ("NativeUfs: start provider=%s probe=%u root=%u whole-root=%u stream=%04x stream2=%04x\n",
         provider && OSDynamicCast (IOPlatformExpertDevice, provider) ? "platform" : "device",
         probe, rootMode, wholeUserdataRoot, streams.primary, streams.secondary);
  native_ufs_probe_requested = probe != 0;
  bool mutatesTransport = (probe >= 3 && probe <= 5) || probe == 9 || probe == 10 ||
                          probe == 11 || probe == 12 || probe == 13 ||
                          (rootMode && !probe) || wholeUserdataRoot;
  if (streams.secondary && mutatesTransport) {
    requireDualStreamReuse (probe, rootMode ? rootMode : wholeUserdataRoot, "start");
  }
  // Default: match for observability only, never touch the controller. Hardware work runs once,
  // from the root-device personality; the ufshc nub match stays observation-only.
  if ((!probe && !rootMode && !wholeUserdataRoot) ||
      !OSDynamicCast (IOPlatformExpertDevice, provider)) {
    return false;
  }
  // Past this point the call is going to touch the controller. The catalog publishes two
  // personalities that both name this class, so this can be entered twice; the bring-up below
  // rewrites UTRLBA/UTRLBAU, the doorbell and the SMMU S2CR/SMR entries, and a second pass over
  // a queue that is already running is what deadlocked the transport on sheng. Take the gate
  // here, not at function entry: the observation-only path above must stay free to run first.
  static bool hardwareOwnerTaken = false;
  if (__atomic_exchange_n (&hardwareOwnerTaken, true, __ATOMIC_ACQ_REL)) {
    IOLog ("NativeUfs: start re-entered; the transport is owned by the first entry\n");
    return false;
  }
  unsigned diagnostic = 0;
  PE_parse_boot_argn ("native-ufs-diagnostics", &diagnostic, sizeof (diagnostic));
  gRootDiagnosticEnabled = diagnostic == 1;
  if (probe == 2) {
    inspect ();
  }
  if (probe == 8) {
    inspectContextBanks ();
  }
  if (probe == 7) {
    inspectSmmu ();
  }
  if (probe == 6) {
    dumpFirmwareList ();
  }
  if ((probe >= 3 && probe <= 5) || probe == 9) {
    adoptAndRead (probe);
  }
  if (probe == 10) {
    // Non-data-mutating check before anything is flashed: LUN 0 GPT -> "userdata" -> its first block.
    // Controller, SMMU and device state may still be touched while establishing the transport.
    OSSharedPtr<IOMemoryMap> beforeHcMap, beforeSmmuMap;
    uintptr_t beforeHc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, beforeHcMap);
    uintptr_t beforeSmmu = mapPhys (0x15000000ULL, 0xd0000, beforeSmmuMap);
    if (!beforeHc || !beforeSmmu) {
      panic ("NATIVE_UFS root check: pre-probe map failed hc=%p smmu=%p", (void *) beforeHc, (void *) beforeSmmu);
    }
    UfsProbeSnapshot before = probeSnapshot (beforeHc, beforeSmmu, streams.primary, streams.secondary);
    IOLog ("NativeUfs: probe10 before cap=%08x hcs=%08x hce=%08x utrlba=%08x utrlbau=%08x db=%08x rsr=%08x s0=%04x/%08x/%08x s1=%04x/%08x/%08x gfsr=%08x\n",
           before.cap, before.hcs, before.hce, before.utrlba, before.utrlbau,
           before.doorbell, before.rsr,
           before.stream[0].sid, before.stream[0].smr, before.stream[0].s2cr,
           before.stream[1].sid, before.stream[1].smr, before.stream[1].s2cr, before.gfsr);
    const char *why = "";
    if (!hostBringUp (&why)) {
      panic ("NATIVE_UFS root check: bring-up failed: %s", why);
    }
    UfsReadCapacity capacity = {};
    IOReturn capacityStatus = hostReadCapacity (0, &capacity);
    UfsProbeSnapshot after = probeSnapshot (gHost.hc, gHost.smmu, streams.primary, streams.secondary);
    IOLog ("NativeUfs: probe10 after cap=%08x hcs=%08x hce=%08x utrlba=%08x utrlbau=%08x db=%08x rsr=%08x s0=%04x/%08x/%08x s1=%04x/%08x/%08x gfsr=%08x\n",
           after.cap, after.hcs, after.hce, after.utrlba, after.utrlbau,
           after.doorbell, after.rsr,
           after.stream[0].sid, after.stream[0].smr, after.stream[0].s2cr,
           after.stream[1].sid, after.stream[1].smr, after.stream[1].s2cr, after.gfsr);
    RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
    if (!layout || !findRootLayout (layout, &why)) {
      panic ("NATIVE_UFS root check: %s", why);
    }
    // Preserve the first block in gScratch and use the following scratch block
    // for userdataFirst + 1. Both reads are SCSI READ(10); no write is issued.
    uint8_t *repeat = gScratch + 4096;
    uint8_t *second = gScratch + 8192;
    IOReturn repeatStatus = hostRead4k (0, layout->userdataFirst, 1, repeat);
    bool repeatMatch = repeatStatus == kIOReturnSuccess && memcmp (gScratch, repeat, 4096) == 0;
    IOReturn secondStatus = hostRead4k (0, layout->userdataFirst + 1, 1, second);
    UfsProbeSnapshot final = probeSnapshot (gHost.hc, gHost.smmu, streams.primary, streams.secondary);
    static char evidence[4096];
    unsigned used = 0;
    auto append = [&] (int written) {
      if (used >= sizeof (evidence)) {
        used = sizeof (evidence) - 1;
      } else if (written >= 0 && size_t (written) < sizeof (evidence) - used) {
        used += unsigned (written);
      } else {
        used = sizeof (evidence) - 1;
      }
    };
    append (snprintf (evidence, sizeof (evidence),
                      "NATIVE_UFS raw probe\n"
                      "capacity last=%u bytes=%u total=%llu scsi=%04x ocs=%02x status=%08x\n"
                      "userdata first=%llx last=%llx gpt_lba_bytes=%u manifest=%u windows=%u\n"
                      "magic %08x %08x ext4magic=%04x ext4blocks=%u f2fsmagic=%08x\n"
                      "repeat status=%08x match=%u second=%08x\n",
                      capacity.lastLba, capacity.blockBytes,
                      (uint64_t (capacity.lastLba) + 1) * capacity.blockBytes,
                      capacity.scsi, capacity.ocs, capacityStatus,
                      layout->userdataFirst, layout->userdataLast, layout->lbaBytes,
                      layout->manifestStatus, layout->windows,
                      layout->magic0, layout->magic1, layout->ext4Magic,
                      layout->ext4Blocks, layout->f2fsMagic,
                      repeatStatus, repeatMatch ? 1U : 0U, secondStatus));
    append (snprintf (evidence + used, sizeof (evidence) - used, "first 0000..007f\n"));
    int hex = rootHexWindow (evidence + used, sizeof (evidence) - used,
                             gScratch, 4096, 0, 0x80, 0);
    append (hex);
    append (snprintf (evidence + used, sizeof (evidence) - used, "first 03f0..043f\n"));
    hex = rootHexWindow (evidence + used, sizeof (evidence) - used,
                         gScratch, 4096, 0x3f0, 0x50, 0);
    append (hex);
    append (snprintf (evidence + used, sizeof (evidence) - used, "second 13f0..143f\n"));
    hex = rootHexWindow (evidence + used, sizeof (evidence) - used,
                         second, 4096, 0x3f0, 0x50, 0x1000);
    append (hex);
    append (snprintf (evidence + used, sizeof (evidence) - used,
                      "before hcs=%08x db=%08x rsr=%08x gfsr=%08x\n"
                      "before %04x=%08x/%08x %04x=%08x/%08x\n"
                      "after  hcs=%08x db=%08x rsr=%08x gfsr=%08x\n"
                      "after  %04x=%08x/%08x %04x=%08x/%08x\n"
                      "final  hcs=%08x db=%08x rsr=%08x gfsr=%08x\n"
                      "final  %04x=%08x/%08x %04x=%08x/%08x\n",
                      before.hcs, before.doorbell, before.rsr, before.gfsr,
                      before.stream[0].sid, before.stream[0].smr, before.stream[0].s2cr,
                      before.stream[1].sid, before.stream[1].smr, before.stream[1].s2cr,
                      after.hcs, after.doorbell, after.rsr, after.gfsr,
                      after.stream[0].sid, after.stream[0].smr, after.stream[0].s2cr,
                      after.stream[1].sid, after.stream[1].smr, after.stream[1].s2cr,
                      final.hcs, final.doorbell, final.rsr, final.gfsr,
                      final.stream[0].sid, final.stream[0].smr, final.stream[0].s2cr,
                      final.stream[1].sid, final.stream[1].smr, final.stream[1].s2cr));
    panic ("%s", evidence);
  }
  if (probe == 11) {
    // After the root image is flashed: manifest, then each window's own GPT (sector 1 of the
    // attached dmg disk image carries "EFI PART"). Read-only.
    const char *why = "";
    if (!hostBringUp (&why)) {
      panic ("NATIVE_UFS root windows: bring-up failed: %s", why);
    }
    RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
    if (!layout || !findRootLayout (layout, &why)) {
      panic ("NATIVE_UFS root windows: %s", why);
    }
    uint32_t sig[4][2] = {};
    uint32_t seen[4] = {};
    for (uint32_t index = 0; index < layout->windows && index < 4; index++) {
      uint64_t absolute = layout->userdataFirst * 4096ULL + layout->window[index].offset;
      if (hostRead4k (0, absolute / 4096, 1, gScratch) == kIOReturnSuccess) {
        seen[index] = 1;
        sig[index][0] = le32 (gScratch + 512);
        sig[index][1] = le32 (gScratch + 516);
      }
    }
    panic ("NATIVE_UFS root windows\n"
           "userdata first=%llx last=%llx manifest status=%u windows=%u\n"
           "w0 %s off=%llx size=%llx gpt=%08x %08x read=%u\n"
           "w1 %s off=%llx size=%llx gpt=%08x %08x read=%u\n"
           "w0 sha %08x %08x %08x %08x\n"
           "w1 sha %08x %08x %08x %08x",
           layout->userdataFirst, layout->userdataLast, layout->manifestStatus, layout->windows,
           layout->window[0].name, layout->window[0].offset, layout->window[0].size, sig[0][0], sig[0][1], seen[0],
           layout->window[1].name, layout->window[1].offset, layout->window[1].size, sig[1][0], sig[1][1], seen[1],
           be32w (layout->window[0].sha256), be32w (layout->window[0].sha256 + 4), be32w (layout->window[0].sha256 + 8), be32w (layout->window[0].sha256 + 12),
           be32w (layout->window[1].sha256), be32w (layout->window[1].sha256 + 4), be32w (layout->window[1].sha256 + 8), be32w (layout->window[1].sha256 + 12));
  }
#if !defined(NATIVE_UFS_PROBE_ONLY) || !NATIVE_UFS_PROBE_ONLY
  if (probe == 14) {
    // Immediate memory snapshot at root-device start, before the userspace that triggers jetsam.
    panic ("NATIVE_UFS early memory snapshot\n"
           "pages total=%u free=%u active=%u inactive=%u wired=%u spec=%u purgeable=%u\n"
           "available=%u max_mem=%llx procs=%d",
           vm_page_pages, vm_page_free_count, vm_page_active_count, vm_page_inactive_count,
           vm_page_wire_count, vm_page_speculative_count, vm_page_purgeable_count,
           memorystatus_available_pages, max_mem, nprocs);
  }
#endif
#if !defined(NATIVE_UFS_PROBE_ONLY) || !NATIVE_UFS_PROBE_ONLY
  if (probe == 13) {
    unsigned delay = 12;
    PE_parse_boot_argn ("native-ufs-stats-delay", &delay, sizeof (delay));
    if (!startRoot (provider)) {
      panic ("NATIVE_UFS memory snapshot: root start failed");
    }
    thread_t thread = nullptr;
    if (kernel_thread_start (statsThread, reinterpret_cast<void *> (static_cast<uintptr_t> (delay)), &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS memory snapshot: cannot start the thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (probe == 12) {
    bool published = startRoot (provider);
    if (!published) {
      panic ("NATIVE_UFS media report: root start failed (blocks=%u)", native_ufs_blocks_published);
    }
    thread_t thread = nullptr;
    if (kernel_thread_start (mediaReportThread, nullptr, &thread) != KERN_SUCCESS) {
      panic ("NATIVE_UFS media report: cannot start the report thread");
    }
    thread_deallocate (thread);
    return true;
  }
  if (rootMode && !probe) {
    return startRoot (provider);
  }
  if (wholeUserdataRoot) {
    return startWholeUserdataRoot (provider);
  }
#endif
#if defined(NATIVE_UFS_PROBE_ONLY) && NATIVE_UFS_PROBE_ONLY
  // Probe-only build: no block device to publish, but the log writer still needs the transport and the
  // userdata geometry. Bring those up without creating any IOBlockStorageDevice.
  if (!rootMode && !wholeUserdataRoot) return false;
  const char *why = "";
  if (!hostBringUp (&why)) {
    IOLog ("NativeUfs: log-only bring-up failed: %s\n", why);
    return false;
  }
  RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
  if (!layout || !findRootLayout (layout, &why)) {
    IOLog ("NativeUfs: log-only root layout failed: %s\n", why);
    if (layout) IOFree (layout, sizeof (RootLayout));
    return false;
  }
  bool usable = layout->manifestStatus == 0;
  if (usable) {
    gStoreGeometry = { true, layout->userdataFirst, layout->userdataLast, layout->imageBytes, layout->gptEntriesLba,
                       layout->gptHeaderCrc, layout->gptEntriesCrc, layout->gptEntryCount, layout->logFirst, layout->logLast };
  }
  IOFree (layout, sizeof (RootLayout));
  if (!usable) {
    IOLog ("NativeUfs: log-only root manifest rejected; the log window stays closed\n");
    return false;
  }
  // Nothing to publish, so there is no child to start. Keep the personality alive for the log writer.
  return IOService::start (provider);
#endif
  return false;
}

#if !defined(NATIVE_UFS_PROBE_ONLY) || !NATIVE_UFS_PROBE_ONLY
// Boot the root image's windows as block devices. Soft-fails (logs, publishes nothing).
bool NativeUfs::startRoot (IOService *provider)
{
  const char *why = "";
  native_ufs_root_step = 1;
  if (!hostBringUp (&why)) {
    rootDiagnosticFailure (why);
    IOLog ("NativeUfs: root bring-up failed: %s\n", why);
    return false;
  }
  native_ufs_root_step = 2;
  RootLayout *layout = static_cast<RootLayout *> (IOMalloc (sizeof (RootLayout)));
  if (!layout || !findRootLayout (layout, &why)) {
    rootDiagnosticFailure (layout ? why : "root layout allocation failed");
    IOLog ("NativeUfs: root layout failed: %s\n", why);
    if (layout) IOFree (layout, sizeof (RootLayout));
    return false;
  }
  native_ufs_root_step = 3;
  if (layout->manifestStatus != 0) {
    rootDiagnosticFailure ("root manifest rejected");
    IOLog ("NativeUfs: no valid root manifest in userdata (status %u, magic %08x %08x)\n",
           layout->manifestStatus, layout->magic0, layout->magic1);
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  gStoreGeometry = { true, layout->userdataFirst, layout->userdataLast, layout->imageBytes, layout->gptEntriesLba,
                     layout->gptHeaderCrc, layout->gptEntriesCrc, layout->gptEntryCount, layout->logFirst, layout->logLast };
  const char *names[4] = {};
  for (unsigned i = 0; i < layout->windows; ++i) names[i] = layout->window[i].name;
  if (!ufsBuildRootPlan (names, layout->windows, gPublicationPlan)) {
    rootDiagnosticFailure ("root windows require distinct OS and SystemOS");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  if (!IOService::start (provider)) {
    rootDiagnosticFailure ("root IOService start failed");
    IOFree (layout, sizeof (RootLayout));
    return false;
  }
  // Prepare all windows or publish none. Diagnostics never decide policy.
  gPending = 0;
  gPendingProvider = this;
  struct Operations {
    NativeUfs *owner;
    RootLayout *layout;
    bool prepare (unsigned index) {
      gRootDiagnostic.stage = "root window allocation/configuration";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Prepare);
      gRootDiagnostic.publicationWindow = index;
      gRootDiagnostic.windowBytes = layout->window[index].size;
      NativeUfsBlock *device = OSTypeAlloc (NativeUfsBlock);
      gPendingDevice[index] = device;
      gPendingAttached[index] = false;
      gPendingUuid[index][0] = 0;
      uint64_t absolute = layout->userdataFirst * 4096ULL + layout->window[index].offset;
      if (!device || !device->init (static_cast<OSDictionary *> (nullptr)) ||
          !device->configure (layout->window[index].name, absolute, layout->window[index].size)) return false;
      gRootDiagnostic.stage = "root window attachment";
      if (!device->attach (owner)) return false;
      gPendingAttached[index] = true;
      ++gPending;
      gRootDiagnostic.prepared = gPending;
      return true;
    }
    bool validate () {
      gRootDiagnostic.stage = "signed window identities";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Identity);
#if defined(NATIVE_VERIFIED_BLOCK) && NATIVE_VERIFIED_BLOCK
      for (unsigned i = 0; i < gPending; ++i) {
        gRootDiagnostic.publicationWindow = i;
        if (!gPendingDevice[i]->signedVolumeUuid (gPendingUuid[i])) return false;
        for (unsigned j = 0; j < i; ++j) if (!strcmp (gPendingUuid[i], gPendingUuid[j])) return false;
      }
      return true;
#else
      return false; // Root publication requires owner verification in every build.
#endif
    }
    bool schedule () {
      gRootDiagnostic.stage = "root publisher thread";
      gRootDiagnostic.publicationPhase = static_cast<unsigned> (UfsRootPhase::Schedule);
      gRootDiagnostic.publicationWindow = 0xffffffff;
      thread_t thread = nullptr;
      owner->retain ();
      if (kernel_thread_start (publishThread, nullptr, &thread) != KERN_SUCCESS) {
        owner->release ();
        return false;
      }
      thread_deallocate (thread);
      return true;
    }
    void discard (unsigned index) { discardPendingWindow (index); }
  } operations = { this, layout };
  UfsRootPublicationResult result = ufsPrepareRootWindows (operations, layout->windows);
  // gts7xlwifi: the Data container after the image, once the image windows are committed.
  if (result.phase == UfsRootPhase::Complete && !t970PublishData (this, layout)) {
    rootDiagnosticFailure ("t970 Data container publication failed");
  }
  IOFree (layout, sizeof (RootLayout));
  if (result.phase != UfsRootPhase::Complete) {
    gPending = 0;
    gRootDiagnostic.result = static_cast<uint32_t> (rootPublishResult (result.phase));
    IOLog ("NativeUfs: root preparation failed phase=%u window=%u; nothing published\n",
           static_cast<unsigned> (result.phase), result.index);
    IOService::stop (provider);
    rootDiagnosticFailure (result.phase == UfsRootPhase::Schedule ? "root publisher thread creation failed" :
                          result.phase == UfsRootPhase::Identity ? "signed window identities rejected" :
                          "root window preparation failed");
    return false;
  }
  return true;
}
#endif

void NativeUfs::stop (IOService *provider)
{
#if !defined(NATIVE_UFS_PROBE_ONLY) || !NATIVE_UFS_PROBE_ONLY
  if (gWholeUserdataDevice) {
    NativeUfsWholeUserdataBlock *device = gWholeUserdataDevice;
    gWholeUserdataDevice = nullptr;
    device->terminate ();
    device->detach (this);
    device->release ();
  }
#endif
  fCmdDesc.reset ();
  fMap.reset ();
  IOService::stop (provider);
}
