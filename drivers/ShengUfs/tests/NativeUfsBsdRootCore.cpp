// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../NativeUfsBsdRootCore.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace abi = alioth::native_ufs_bsd_root;

namespace {

constexpr uint32_t kEntryCount = 32;
constexpr uint64_t kGptHeaderLba = 1;
constexpr uint64_t kEntriesLba = 2;
constexpr uint64_t kUserdataFirst = 8;
constexpr uint64_t kUserdataLast = 506;
constexpr uint64_t kBackupLba = 511;

struct Device {
  std::vector<uint8_t> media;
  unsigned reads = 0;
  unsigned writes = 0;
  unsigned syncs = 0;
  bool fail_read = false;
  bool fail_write = false;
  bool fail_sync = false;

  void Reset() {
    media.assign(static_cast<size_t>(kBackupLba + 1) * abi::kMediaBlockBytes, 0);
    reads = 0;
    writes = 0;
    syncs = 0;
    fail_read = false;
    fail_write = false;
    fail_sync = false;
  }

  uint8_t *Block(uint64_t lba) {
    return media.data() + static_cast<size_t>(lba) * abi::kMediaBlockBytes;
  }
};

void Put32(uint8_t *at, uint32_t value) {
  for (unsigned index = 0; index < 4; ++index) {
    at[index] = static_cast<uint8_t>(value >> (index * 8));
  }
}

void Put64(uint8_t *at, uint64_t value) {
  for (unsigned index = 0; index < 8; ++index) {
    at[index] = static_cast<uint8_t>(value >> (index * 8));
  }
}

void PutName(uint8_t *entry, const char *name) {
  for (size_t index = 0; index <= strlen(name); ++index) {
    entry[56 + index * 2] = static_cast<uint8_t>(name[index]);
    entry[57 + index * 2] = 0;
  }
}

void SetUserdataEntry(uint8_t *entry, uint64_t first, uint64_t last,
                      const char *name = "userdata") {
  std::memset(entry, 0, abi::kGptEntryBytes);
  entry[0] = 0xaf;
  entry[15] = 0x3b;
  entry[16] = 0x11;
  entry[31] = 0xee;
  Put64(entry + 32, first);
  Put64(entry + 40, last);
  PutName(entry, name);
}

void SetOtherEntry(uint8_t *entry, uint64_t first, uint64_t last,
                   uint8_t seed) {
  std::memset(entry, seed, abi::kGptEntryBytes);
  entry[0] = seed;
  entry[15] = seed;
  entry[16] = static_cast<uint8_t>(seed + 1);
  entry[31] = static_cast<uint8_t>(seed + 2);
  std::memset(entry + 56, 0, abi::kGptEntryBytes - 56);
  Put64(entry + 32, first);
  Put64(entry + 40, last);
  PutName(entry, "boot_a");
}

void SealGpt(Device &device) {
  uint8_t *header = device.Block(kGptHeaderLba);
  const size_t entries_bytes = kEntryCount * abi::kGptEntryBytes;
  const uint32_t entries_crc =
      abi::Crc32(device.Block(kEntriesLba), entries_bytes, false);
  Put32(header + 88, entries_crc);
  Put32(header + 16, 0);
  Put32(header + 16,
        abi::Crc32(header, 92, true));
}

void Fixture(Device &device) {
  device.Reset();
  uint8_t *header = device.Block(kGptHeaderLba);
  std::memcpy(header, "EFI PART", 8);
  Put32(header + 8, 0x00010000);
  Put32(header + 12, 92);
  Put64(header + 24, kGptHeaderLba);
  Put64(header + 32, kBackupLba);
  Put64(header + 40, 6);
  Put64(header + 48, 506);
  Put64(header + 56, 0x123456789abcdef0ULL);
  Put64(header + 72, kEntriesLba);
  Put32(header + 80, kEntryCount);
  Put32(header + 84, abi::kGptEntryBytes);
  SetUserdataEntry(device.Block(kEntriesLba), kUserdataFirst, kUserdataLast);
  device.Block(kUserdataFirst)[abi::kHfsSignatureOffset] = 0x48;
  device.Block(kUserdataFirst)[abi::kHfsSignatureOffset + 1] = 0x58;
  SealGpt(device);
}

abi::Status Read(uint64_t lba, uint32_t blocks, void *dst, void *context) {
  Device &device = *static_cast<Device *>(context);
  ++device.reads;
  if (device.fail_read || dst == nullptr || blocks == 0 ||
      blocks > abi::kMaxBlocks ||
      lba > device.media.size() / abi::kMediaBlockBytes ||
      blocks > device.media.size() / abi::kMediaBlockBytes - lba) {
    return abi::Status::TransportError;
  }
  std::memcpy(dst, device.Block(lba),
              static_cast<size_t>(blocks) * abi::kMediaBlockBytes);
  return abi::Status::Ok;
}

abi::Status Write(uint64_t lba, uint32_t blocks, const void *src,
                  void *context) {
  Device &device = *static_cast<Device *>(context);
  ++device.writes;
  if (device.fail_write || src == nullptr || blocks == 0 ||
      blocks > abi::kMaxBlocks ||
      lba > device.media.size() / abi::kMediaBlockBytes ||
      blocks > device.media.size() / abi::kMediaBlockBytes - lba) {
    return abi::Status::TransportError;
  }
  std::memcpy(device.Block(lba), src,
              static_cast<size_t>(blocks) * abi::kMediaBlockBytes);
  return abi::Status::Ok;
}

abi::Status Sync(void *context) {
  Device &device = *static_cast<Device *>(context);
  ++device.syncs;
  return device.fail_sync ? abi::Status::TransportError : abi::Status::Ok;
}

abi::Media MediaFor(Device &device) {
  return {Read, Write, Sync, &device};
}

void Open(abi::RootState &state, Device &device, bool writable = true) {
  abi::Geometry geometry = {};
  assert(state.Open(MediaFor(device), writable, &geometry) ==
         abi::Status::Ok);
  assert(geometry.block_bytes == abi::kMediaBlockBytes);
  assert(geometry.first_lba == kUserdataFirst);
  assert(geometry.last_lba == kUserdataLast);
  assert(geometry.block_count == kUserdataLast - kUserdataFirst + 1);
  assert(geometry.capacity_bytes ==
         geometry.block_count * abi::kMediaBlockBytes);
  assert(geometry.writable == (writable ? 1U : 0U));
}

void ExpectOpen(Device &device, abi::Status expected) {
  abi::RootState state;
  abi::Geometry geometry = {};
  const abi::Status actual = state.Open(MediaFor(device), true, &geometry);
  if (actual != expected) {
    std::fprintf(stderr, "expected %u, actual %u (%s), reads=%u writes=%u\n",
                 static_cast<unsigned>(expected), static_cast<unsigned>(actual),
                 abi::ReasonForStatus(actual), device.reads, device.writes);
  }
  assert(actual == expected);
  assert(!state.Opened());
}

void TestChecksum() {
  assert(abi::Crc32("123456789", 9, false) == 0xcbf43926U);
}

void TestGptHeader() {
  Device device;
  Fixture(device);
  abi::GptHeader header = {};
  assert(abi::ParseGptHeader(device.Block(kGptHeaderLba), &header) ==
         abi::Status::Ok);
  assert(header.entries_lba == kEntriesLba);
  assert(header.entries_count == kEntryCount);
  assert(header.entry_bytes == abi::kGptEntryBytes);

  device.Block(kGptHeaderLba)[16] ^= 1;
  assert(abi::ParseGptHeader(device.Block(kGptHeaderLba), &header) ==
         abi::Status::BadGptCrc);
  Fixture(device);
  Put32(device.Block(kGptHeaderLba) + 12, 91);
  SealGpt(device);
  assert(abi::ParseGptHeader(device.Block(kGptHeaderLba), &header) ==
         abi::Status::BadGptHeader);
  Fixture(device);
  Put32(device.Block(kGptHeaderLba) + 84, 64);
  SealGpt(device);
  assert(abi::ParseGptHeader(device.Block(kGptHeaderLba), &header) ==
         abi::Status::BadGptGeometry);
  Fixture(device);
  Put64(device.Block(kGptHeaderLba) + 32, UINT32_MAX + 1ULL);
  SealGpt(device);
  assert(abi::ParseGptHeader(device.Block(kGptHeaderLba), &header) ==
         abi::Status::BadGptGeometry);
}

void TestOpenAndRead() {
  Device device;
  Fixture(device);
  abi::RootState state;
  Open(state, device);
  assert(state.Opened() && state.Writable());
  const unsigned reads_after_open = device.reads;
  std::array<uint8_t, abi::kMaxBlocks * abi::kMediaBlockBytes> bytes = {};
  bytes[0] = 0xa5;
  assert(state.Read(kUserdataFirst, 1, bytes.data()) == abi::Status::Ok);
  assert(device.reads == reads_after_open + 1);
  assert(bytes[0] == 0);
  assert(state.Read(kUserdataLast, 1, bytes.data()) == abi::Status::Ok);
  assert(state.Read(kUserdataFirst, 32, bytes.data()) == abi::Status::Ok);

  abi::Geometry ignored = {};
  assert(state.Open(MediaFor(device), true, &ignored) ==
         abi::Status::AlreadyOpen);
  assert(state.Read(kUserdataFirst - 1, 1, bytes.data()) ==
         abi::Status::OutOfBounds);
  assert(state.Read(kUserdataFirst, 0, bytes.data()) ==
         abi::Status::OutOfBounds);
  assert(state.Read(kUserdataFirst, 33, bytes.data()) ==
         abi::Status::TransferTooLarge);
  assert(state.Read(kUserdataLast, 2, bytes.data()) ==
         abi::Status::OutOfBounds);
  assert(state.Read(UINT64_MAX, 1, bytes.data()) ==
         abi::Status::OutOfBounds);
  assert(state.Read(kUserdataFirst, 1, nullptr) ==
         abi::Status::InvalidArgument);
}

void TestWritesAndSync() {
  Device device;
  Fixture(device);
  abi::RootState state;
  Open(state, device);
  std::array<uint8_t, abi::kMaxBlocks * abi::kMediaBlockBytes> bytes = {};
  bytes.fill(0x5a);
  assert(state.Write(kUserdataFirst, 1, bytes.data()) == abi::Status::Ok);
  assert(device.writes == 1);
  assert(device.Block(kUserdataFirst)[0] == 0x5a);
  assert(state.Write(kUserdataLast, 2, bytes.data()) ==
         abi::Status::OutOfBounds);
  assert(device.writes == 1);
  assert(state.Sync() == abi::Status::Ok);
  assert(device.syncs == 1);

  device.fail_write = true;
  assert(state.Write(kUserdataFirst, 1, bytes.data()) ==
         abi::Status::TransportError);
  device.fail_write = false;
  device.fail_sync = true;
  assert(state.Sync() == abi::Status::TransportError);
}

void TestUserdataOutsideUsableRange() {
  std::array<uint8_t, kEntryCount * abi::kGptEntryBytes> entries = {};
  SetUserdataEntry(entries.data(), kUserdataFirst, kUserdataLast);
  abi::Geometry geometry = {};
  assert(abi::ParseUserdataPartition(entries.data(), abi::kGptEntryBytes,
                                     kEntryCount, kUserdataFirst, kUserdataLast,
                                     &geometry) == abi::Status::Ok);

  entries = {};
  SetUserdataEntry(entries.data(), kUserdataLast + 1, kUserdataLast + 10);
  assert(abi::ParseUserdataPartition(entries.data(), abi::kGptEntryBytes,
                                     kEntryCount, kUserdataFirst, kUserdataLast,
                                     &geometry) == abi::Status::OutOfBounds);
}

void TestReadOnly() {
  Device device;
  Fixture(device);
  abi::RootState state;
  Open(state, device, false);
  std::array<uint8_t, abi::kMediaBlockBytes> bytes = {};
  assert(state.Read(kUserdataFirst, 1, bytes.data()) == abi::Status::Ok);
  assert(state.Write(kUserdataFirst, 1, bytes.data()) ==
         abi::Status::NotWritable);
  assert(state.Sync() == abi::Status::NotWritable);
  assert(device.writes == 0);
  assert(device.syncs == 0);
}

void TestGptFailures() {
  Device device;

  Fixture(device);
  device.Block(kEntriesLba)[16] ^= 1;
  ExpectOpen(device, abi::Status::BadGptCrc);

  Fixture(device);
  SetUserdataEntry(device.Block(kEntriesLba), kUserdataFirst, kUserdataLast);
  SetUserdataEntry(device.Block(kEntriesLba) + abi::kGptEntryBytes,
                   kUserdataFirst + 100, kUserdataLast - 100, "userdata");
  SealGpt(device);
  ExpectOpen(device, abi::Status::PartitionOverlap);

  Fixture(device);
  SetUserdataEntry(device.Block(kEntriesLba), kUserdataFirst, kUserdataLast);
  SetUserdataEntry(device.Block(kEntriesLba) + abi::kGptEntryBytes,
                   kUserdataLast + 1, kUserdataLast + 10, "userdata");
  Put64(device.Block(kGptHeaderLba) + 48, kBackupLba - 1);
  SealGpt(device);
  ExpectOpen(device, abi::Status::DuplicateUserdata);

  Fixture(device);
  SetUserdataEntry(device.Block(kEntriesLba), kUserdataFirst, kUserdataLast,
                   "boot_b");
  std::memset(device.Block(kEntriesLba) + abi::kGptEntryBytes, 0,
              abi::kGptEntryBytes);
  Put64(device.Block(kGptHeaderLba) + 32, 1023);
  Put64(device.Block(kGptHeaderLba) + 48, 1018);
  SealGpt(device);
  ExpectOpen(device, abi::Status::NoUserdata);


  Fixture(device);
  SetUserdataEntry(device.Block(kEntriesLba), kUserdataFirst,
                   UINT32_MAX + 1ULL);
  SealGpt(device);
  ExpectOpen(device, abi::Status::Overflow);

  Fixture(device);
  SetOtherEntry(device.Block(kEntriesLba) + abi::kGptEntryBytes,
                kUserdataFirst + 1, kUserdataLast, 0x33);
  SealGpt(device);
  ExpectOpen(device, abi::Status::PartitionOverlap);

  Fixture(device);
  device.Block(kUserdataFirst)[abi::kHfsSignatureOffset] = 0x48;
  device.Block(kUserdataFirst)[abi::kHfsSignatureOffset + 1] = 0x2b;
  ExpectOpen(device, abi::Status::WrongFilesystem);

  Fixture(device);
  device.fail_read = true;
  ExpectOpen(device, abi::Status::TransportError);
}

void TestStatusContract() {
  assert(abi::ErrnoForStatus(abi::Status::Ok) == 0);
  assert(abi::ErrnoForStatus(abi::Status::InvalidArgument) == -22);
  assert(abi::ErrnoForStatus(abi::Status::AlreadyOpen) == -16);
  assert(abi::ErrnoForStatus(abi::Status::NotWritable) == -30);
  assert(abi::ErrnoForStatus(abi::Status::Overflow) == -75);
  assert(abi::ErrnoForStatus(abi::Status::OutOfBounds) == -28);
  assert(abi::ErrnoForStatus(abi::Status::WrongFilesystem) == -19);
  assert(abi::ErrnoForStatus(abi::Status::TransportError) == -5);
  for (unsigned value = 0; value <= static_cast<unsigned>(abi::Status::WrongFilesystem);
       ++value) {
    const auto status = static_cast<abi::Status>(value);
    assert(abi::ReasonForStatus(status) != nullptr);
    assert(abi::ReasonForStatus(status)[0] != '\0');
  }
  abi::RootState state;
  abi::Geometry geometry = {};
  std::array<uint8_t, abi::kMediaBlockBytes> block = {};
  assert(state.Open({}, true, &geometry) == abi::Status::InvalidArgument);
  assert(state.Read(0, 1, block.data()) == abi::Status::NotReady);
  assert(state.Write(0, 1, block.data()) == abi::Status::NotReady);
  assert(state.Sync() == abi::Status::NotReady);
}

}  // namespace

int main() {
  TestChecksum();
  TestGptHeader();
  TestOpenAndRead();
  TestWritesAndSync();
  TestUserdataOutsideUsableRange();
  TestReadOnly();
  TestGptFailures();
  TestStatusContract();
  std::puts(
      "PASS: NativeUfs BSD-root ABI core, GPT CRC/uniqueness/overlap, HFSX, "
      "range limits, duplicate open, write protection, sync failure and "
      "fixed errno reasons");
  return 0;
}
