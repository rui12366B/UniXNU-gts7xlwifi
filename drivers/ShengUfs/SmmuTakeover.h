// SPDX-License-Identifier: BSD-2-Clause-Patent
// Plan before any SMMU write. Interface register facts: ARM SMMUv2.
struct UfsSmmuPlan {
  unsigned slot = 0xffffffffU, bank = 0, pages = 0;
  uint32_t smr = 0, s2cr = 0;
  bool reuse = false;
  const char *error = nullptr;
};
struct UfsSmmuStreamEntry {
  unsigned index;
  uint32_t smr, s2cr;
};
static bool ufsSmmuStreamMatches(uint32_t smr, uint16_t stream)
{
  uint32_t mask = (smr >> 16) & 0x7fff;
  return (smr & 0x80000000U) && (((smr ^ stream) & 0xffffU) & ~mask) == 0;
}
// SMMUv2 CBAR: TYPE is bits[17:16]. A stage-1 translating bank with stage-2
// bypass is the firmware shape Linux leaves behind; SCTLR.M remains the gate
// that says stage 1 is disabled.
static bool ufsSmmuContextIsBypass(uint32_t cbar, uint32_t sctlr)
{
  return ((cbar >> 16) & 3) == 1 && !(sctlr & 1);
}
struct UfsSmmuSnapshot {
  unsigned slot = 0xffffffffU, bank = 0;
  uint32_t scr0 = 0, gfsr = 0;
  uint32_t smr = 0, s2cr = 0, cbar = 0, cba2r = 0, sctlr = 0;
};
struct UfsSmmuReuse {
  unsigned slot = 0xffffffffU, bank = 0, references = 0;
  uint32_t smr = 0, s2cr = 0, cbar = 0, cba2r = 0, sctlr = 0;
  const char *error = nullptr;
};
struct UfsSmmuConfigDiagnostic {
  const char *step = nullptr;
  unsigned offset = 0;
  uint32_t expected = 0, actual = 0;
};
template <class Read>
static unsigned collectUfsSmmuStreams(Read read, UfsSmmuStreamEntry *entries, unsigned capacity)
{
  unsigned count = read(0x20) & 0xff;
  if (count > 128) count = 128;
  unsigned valid = 0;
  for (unsigned index = 0; index < count; ++index) {
    uint32_t smr = read(0x800 + 4 * index);
    if (!(smr & 0x80000000U)) continue;
    if (entries && valid < capacity) {
      entries[valid] = {index, smr, read(0xc00 + 4 * index)};
    }
    ++valid;
  }
  return valid;
}
template <class Read>
static UfsSmmuSnapshot captureUfsSmmuSnapshot(const UfsSmmuPlan &p, Read read)
{
  UfsSmmuSnapshot snapshot;
  snapshot.slot = p.slot;
  snapshot.bank = p.bank;
  if (p.slot == 0xffffffffU || p.bank >= 128) return snapshot;
  snapshot.scr0 = read(0x00);
  snapshot.gfsr = read(0x48);
  snapshot.smr = read(0x800 + 4 * p.slot);
  snapshot.s2cr = read(0xc00 + 4 * p.slot);
  snapshot.cbar = read(0x1000 + 4 * p.bank);
  snapshot.cba2r = read(0x1800 + 4 * p.bank);
  snapshot.sctlr = read((p.pages + p.bank) * 4096);
  return snapshot;
}
// Validate a mapping the firmware already left in place. This path never
// writes SMMU registers: it is the preferred transport for a board whose UFS
// streams are already routed through exclusive bypass contexts.
template <class Read>
static UfsSmmuReuse reuseUfsSmmuStream(uint16_t stream, Read read)
{
  UfsSmmuReuse r;
  uint32_t idr0 = read(0x20), idr1 = read(0x24), idr2 = read(0x28);
  unsigned count = idr0 & 255;
  unsigned banks = idr1 & 255;
  unsigned pages = 1U << (((idr1 >> 28) & 7) + 1);
  if (!(idr0 & (1U << 30)) || !(idr2 & (1U << 12))) {
    r.error = "SMMU AArch64 stage 1 unsupported"; return r;
  }
  if (!count || pages > 128 || banks < 2 || banks > 128 ||
      (pages + banks) * 4096 > 0xd0000) {
    r.error = "unsupported SMMU geometry"; return r;
  }
  if (count > 128) count = 128;
  for (unsigned index = 0; index < count; ++index) {
    uint32_t smr = read(0x800 + 4 * index);
    if (!ufsSmmuStreamMatches(smr, stream)) continue;
    if (r.slot != 0xffffffffU) {
      r.error = "multiple UFS stream matches"; return r;
    }
    if ((smr >> 16) & 0x7fff) {
      r.error = "UFS stream match is not exact"; return r;
    }
    r.slot = index;
    r.smr = smr;
    r.s2cr = read(0xc00 + 4 * index);
  }
  if (r.slot == 0xffffffffU) {
    r.error = "UFS stream has no match"; return r;
  }
  if (((r.s2cr >> 16) & 3) != 0) {
    r.error = "UFS stream is not translating"; return r;
  }
  r.bank = r.s2cr & 255;
  if (r.bank >= banks) {
    r.error = "UFS stream context is out of range"; return r;
  }
  for (unsigned index = 0; index < count; ++index) {
    uint32_t smr = read(0x800 + 4 * index);
    if (!(smr & 0x80000000U)) continue;
    uint32_t s2cr = read(0xc00 + 4 * index);
    if (((s2cr >> 16) & 3) == 0 && (s2cr & 255) == r.bank) ++r.references;
  }
  if (r.references != 1) {
    r.error = "UFS context is shared"; return r;
  }
  r.cbar = read(0x1000 + 4 * r.bank);
  r.cba2r = read(0x1800 + 4 * r.bank);
  r.sctlr = read((pages + r.bank) * 4096);
  if (!ufsSmmuContextIsBypass(r.cbar, r.sctlr)) {
    r.error = "UFS context is not bypass"; return r;
  }
  if ((r.cba2r & ~1U) || !(r.cba2r & 1)) {
    r.error = "UFS context is not AArch64"; return r;
  }
  return r;
}
// ARM IHI 0062D.c sections 1.6 and 3.7: VA64=0 still rejects inputs above
// 32 bits with SCTLR.M=0. With VA64=1 the bypass range is bounded by all
// three hardware widths, not TCR2.PASize (which only limits translation).
static unsigned ufsSmmuAddressBits(unsigned code)
{
  switch (code) {
    case 0: return 32; case 1: return 36; case 2: return 40;
    case 3: return 42; case 4: return 44; case 5: return 48;
    default: return 0;
  }
}
// One attempt with a given context bank (counted from the top). The caller falls back to lower banks when this one is held by another stream.
template <class Read>
static UfsSmmuPlan planUfsSmmuBank(uint16_t stream, uint64_t dma, uint64_t bytes, Read read, unsigned bankFromTop)
{
  UfsSmmuPlan p;
  uint32_t idr0 = read(0x20), idr2 = read(0x28);
  unsigned count = idr0 & 255;
  if (!(idr0 & (1U << 30)) || !(idr2 & (1U << 12))) {
    p.error = "SMMU AArch64 stage 1 unsupported"; return p;
  }
  for (unsigned shift = 0; shift <= 8; shift += 4) {
    unsigned bits = ufsSmmuAddressBits((idr2 >> shift) & 15);
    if (!bits || !bytes || dma >= (1ULL << bits) || bytes > (1ULL << bits) - dma) {
      p.error = "DMA exceeds SMMU address width"; return p;
    }
  }
  uint32_t idr1 = read(0x24);
  unsigned banks = idr1 & 255;
  p.pages = 1U << (((idr1 >> 28) & 7) + 1);
  if ((idr1 >> 31) || !count || p.pages > 128 || banks < 2 || banks > 128 ||
      (p.pages + banks) * 4096 > 0xd0000) {
    p.error = "unsupported SMMU geometry"; return p;
  }
  if (count > 128) count = 128; // Firmware's reliably accessible groups only.
  if (bankFromTop >= banks - 1) {p.error = "no free context bank"; return p;}
  p.bank = banks - 1 - bankFromTop;
  unsigned matches = 0;
  for (unsigned i = 0; i < count; ++i) {
    uint32_t smr = read(0x800 + 4 * i);
    if (!(smr & 0x80000000U)) {
      if (!p.reuse) p.slot = i;
      continue;
    }
    uint32_t s2cr = read(0xc00 + 4 * i);
    uint32_t mask = (smr >> 16) & 0x7fff;
    bool match = ((smr ^ stream) & 0xffff & ~mask) == 0;
    bool ourBank = ((s2cr >> 16) & 3) == 0 && (s2cr & 255) == p.bank;
    if (match) {
      ++matches;
      if (matches > 1 || mask || !ourBank) {
        p.slot = i; p.smr = smr; p.s2cr = s2cr;
        p.error = "conflicting UFS stream mapping"; return p;
      }
      p.reuse = true; p.slot = i;
    } else if (ourBank) {
      p.slot = i; p.smr = smr; p.s2cr = s2cr;
      p.error = "reserved context bank already used"; return p;
    }
  }
  uint32_t sctlr = read((p.pages + p.bank) * 4096);
  if ((sctlr & 1) ||
      (p.reuse && (!ufsSmmuContextIsBypass(read(0x1000 + 4 * p.bank), sctlr) ||
                   (read(0x1800 + 4 * p.bank) & ~1U)))) {
    p.error = "reserved context bank not bypass"; return p;
  }
  if (p.slot == 0xffffffffU) p.error = "no free stream-match group";
  return p;
}

// The UFS stream's context bank: the top bank first. A stream mapping left behind by an earlier run (or by another OS that used the SMMU before a
// warm reset) can still point another stream at that bank; rather than refuse to boot, the next banks down are tried, each held to the same checks
// (no other valid stream maps it, translation off). Any other refusal (a conflicting mapping of our own stream, no free group, unsupported geometry)
// is final and is reported as the first attempt saw it.
template <class Read>
static UfsSmmuPlan planUfsSmmu(uint16_t stream, uint64_t dma, uint64_t bytes, Read read)
{
  enum {kBankAttempts = 8};
  UfsSmmuPlan first = planUfsSmmuBank(stream, dma, bytes, read, 0);
  if (!first.error) return first;
  // "conflicting" is retried too: our own stream may already sit on a lower bank from an earlier run that had to fall back.
  auto retryable = [](const char *e) {
    return !__builtin_strcmp(e, "reserved context bank already used") || !__builtin_strcmp(e, "reserved context bank not bypass") ||
           !__builtin_strcmp(e, "conflicting UFS stream mapping");
  };
  if (!retryable(first.error)) return first;
  for (unsigned attempt = 1; attempt < kBankAttempts; ++attempt) {
    UfsSmmuPlan next = planUfsSmmuBank(stream, dma, bytes, read, attempt);
    if (!next.error) return next;
    if (!retryable(next.error)) break;
  }
  return first;
}

// The caller must first quiesce the UFS queue. The planner refuses other
// streams sharing this bank and enabled translation; no live context is
// disabled here. Write callbacks must complete an MMIO barrier before return.
static const char *ufsSmmuFailure(UfsSmmuConfigDiagnostic *diagnostic, const char *step,
                                  unsigned offset, uint32_t expected, uint32_t actual)
{
  if (diagnostic) {
    diagnostic->step = step;
    diagnostic->offset = offset;
    diagnostic->expected = expected;
    diagnostic->actual = actual;
  }
  return step;
}
template <class Read, class Write>
static const char *configureUfsSmmu(const UfsSmmuPlan &p, uint16_t stream, Read read, Write write,
                                    UfsSmmuConfigDiagnostic *diagnostic = nullptr)
{
  if (diagnostic) *diagnostic = UfsSmmuConfigDiagnostic ();
  if (p.error) return ufsSmmuFailure(diagnostic, p.error, 0, 0, 0);
  unsigned cb = (p.pages + p.bank) * 4096;
  if (!p.reuse) write(cb, 0);
  uint32_t actual = read(cb);
  if (actual != 0) return ufsSmmuFailure(diagnostic, "SMMU bypass disable readback", cb, 0, actual);
  // Set VA64 before exposing a newly allocated stream. Repair an exclusive
  // existing 32-bit bypass too; do not silently reuse the faulty state.
  unsigned cba2r = 0x1800 + 4 * p.bank, cbar = 0x1000 + 4 * p.bank;
  if (read(cba2r) != 1) write(cba2r, 1);
  actual = read(cba2r);
  if (actual != 1) return ufsSmmuFailure(diagnostic, "CBA2R VA64 write", cba2r, 1, actual);
  if (!p.reuse) {
    write(cbar, 0x10000);
    actual = read(cbar);
    if (!ufsSmmuContextIsBypass(actual, read(cb))) {
      return ufsSmmuFailure(diagnostic, "CBAR bypass write", cbar, 0x10000, actual);
    }
  }
  actual = read(cb);
  if (actual != 0) return ufsSmmuFailure(diagnostic, "context SCTLR readback", cb, 0, actual);
  if (!p.reuse) {
    write(0xc00 + 4 * p.slot, p.bank);
    actual = read(0xc00 + 4 * p.slot);
    if (actual != p.bank) {
      return ufsSmmuFailure(diagnostic, "S2CR write", 0xc00 + 4 * p.slot, p.bank, actual);
    }
    write(0x800 + 4 * p.slot, 0x80000000U | stream);
    actual = read(0x800 + 4 * p.slot);
    if (actual != (0x80000000U | stream)) {
      return ufsSmmuFailure(diagnostic, "SMR write", 0x800 + 4 * p.slot,
                            0x80000000U | stream, actual);
    }
  }
  actual = read(0x800 + 4 * p.slot);
  if (actual != (0x80000000U | stream)) {
    return ufsSmmuFailure(diagnostic, "SMR final readback", 0x800 + 4 * p.slot,
                          0x80000000U | stream, actual);
  }
  actual = read(0xc00 + 4 * p.slot);
  if (actual != p.bank) {
    return ufsSmmuFailure(diagnostic, "S2CR final readback", 0xc00 + 4 * p.slot, p.bank, actual);
  }
  return nullptr;
}
