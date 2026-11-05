// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../WholeUserdataDisk.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

namespace wur = alioth::whole_userdata_root;

namespace {

struct FakeOperations {
  std::vector<uint8_t> media;
  std::vector<uint8_t> request;
  uint64_t first_media_lba = 0;
  uint64_t media_blocks = 0;
  unsigned reads = 0;
  unsigned writes = 0;
  unsigned synchronizes = 0;
  bool last_fua = false;
  bool fail_read = false;
  bool fail_write = false;
  bool fail_copy_in = false;

  void Reset(uint64_t first, uint64_t blocks) {
    first_media_lba = first;
    media_blocks = blocks;
    media.assign(static_cast<size_t>(blocks * wur::kMediaBlockBytes), 0);
    request.assign(wur::kMaxTransferBytes, 0xa5);
    reads = 0;
    writes = 0;
    synchronizes = 0;
    last_fua = false;
    fail_read = false;
    fail_write = false;
    fail_copy_in = false;
  }

  wur::Status readBlock(uint64_t media_lba, uint8_t *out) {
    ++reads;
    if (fail_read || !out || media_lba < first_media_lba ||
        media_lba - first_media_lba >= media_blocks) {
      return wur::Status::IoError;
    }
    const size_t offset =
        static_cast<size_t>((media_lba - first_media_lba) *
                            wur::kMediaBlockBytes);
    std::memcpy(out, media.data() + offset, wur::kMediaBlockBytes);
    return wur::Status::Ok;
  }

  wur::Status writeBlock(uint64_t media_lba, const uint8_t *in, bool fua) {
    ++writes;
    last_fua = fua;
    if (fail_write || !in || media_lba < first_media_lba ||
        media_lba - first_media_lba >= media_blocks) {
      return wur::Status::IoError;
    }
    const size_t offset =
        static_cast<size_t>((media_lba - first_media_lba) *
                            wur::kMediaBlockBytes);
    std::memcpy(media.data() + offset, in, wur::kMediaBlockBytes);
    return wur::Status::Ok;
  }

  wur::Status copyIn(uint64_t request_offset, uint8_t *out, uint32_t bytes) {
    if (fail_copy_in || !out ||
        request_offset > request.size() ||
        bytes > request.size() - static_cast<size_t>(request_offset)) {
      return wur::Status::IoError;
    }
    std::memcpy(out, request.data() + request_offset, bytes);
    return wur::Status::Ok;
  }

  wur::Status copyOut(uint64_t request_offset, const uint8_t *in,
                      uint32_t bytes) {
    if (!in || request_offset > request.size() ||
        bytes > request.size() - static_cast<size_t>(request_offset)) {
      return wur::Status::IoError;
    }
    std::memcpy(request.data() + request_offset, in, bytes);
    return wur::Status::Ok;
  }

  wur::Status synchronize() {
    ++synchronizes;
    return wur::Status::Ok;
  }
};

struct Fixture {
  FakeOperations ops;
  std::array<uint8_t, wur::kMediaBlockBytes> bounce = {};
  wur::Geometry geometry = {};
  wur::Disk<FakeOperations> disk;

  void Open(uint64_t first = 8, uint64_t last = 11, bool writable = true) {
    const uint64_t capacity = (last - first + 1) * wur::kMediaBlockBytes;
    assert(wur::ProjectGeometry(first, last, capacity, &geometry) ==
           wur::Status::Ok);
    ops.Reset(first, last - first + 1);
    assert(disk.Configure(geometry, &ops, bounce.data(), true, writable) ==
           wur::Status::Ok);
  }

  void FillMedia() {
    for (size_t index = 0; index < ops.media.size(); ++index) {
      ops.media[index] = static_cast<uint8_t>((index * 29 + index / 4096) &
                                              0xff);
    }
  }
};

void TestConfiguration() {
  FakeOperations ops;
  ops.Reset(8, 4);
  std::array<uint8_t, wur::kMediaBlockBytes> bounce = {};
  wur::Geometry geometry = {};
  assert(wur::ProjectGeometry(8, 11, 4 * wur::kMediaBlockBytes, &geometry) ==
         wur::Status::Ok);
  wur::Disk<FakeOperations> disk;

  assert(disk.Read(geometry.first_logical_sector, 1).status ==
         wur::Status::NotReady);
  assert(disk.Write(geometry.first_logical_sector, 1, false).status ==
         wur::Status::NotReady);
  assert(disk.Configure(geometry, nullptr, nullptr, false, false) ==
         wur::Status::Ok);
  assert(!disk.Selected() && !disk.Writable());
  assert(disk.Read(geometry.first_logical_sector, 1).status ==
         wur::Status::NotReady);

  assert(disk.Configure(geometry, &ops, bounce.data(), false, true) ==
         wur::Status::InvalidAccess);
  assert(disk.Configure(geometry, nullptr, bounce.data(), true, false) ==
         wur::Status::NullArgument);
  assert(disk.Configure(geometry, &ops, nullptr, true, false) ==
         wur::Status::NullArgument);
  assert(disk.Configure(geometry, &ops, bounce.data(), true, false) ==
         wur::Status::Ok);
  assert(disk.Selected() && !disk.Writable());
  assert(disk.Read(geometry.first_logical_sector, 1).status ==
         wur::Status::Ok);
  assert(disk.Write(geometry.first_logical_sector, 1, false).status ==
         wur::Status::ReadOnly);
  assert(disk.Write(geometry.first_logical_sector, 8, true).status ==
         wur::Status::ReadOnly);
}

void TestReads() {
  Fixture fixture;
  fixture.Open();
  fixture.FillMedia();

  const uint64_t first = fixture.geometry.first_logical_sector;
  const uint64_t last =
      first + fixture.geometry.logical_sector_count - 1;

  fixture.ops.request.assign(fixture.ops.request.size(), 0xa5);
  wur::DiskResult result = fixture.disk.Read(first, 1);
  assert(result.status == wur::Status::Ok);
  assert(result.bytes_done == 512);
  assert(fixture.ops.reads == 1);
  assert(std::memcmp(fixture.ops.request.data(), fixture.ops.media.data(),
                     512) == 0);

  fixture.ops.request.assign(fixture.ops.request.size(), 0xa5);
  result = fixture.disk.Read(first + 7, 2);
  assert(result.status == wur::Status::Ok);
  assert(result.bytes_done == 1024);
  assert(fixture.ops.reads == 3);
  assert(std::memcmp(fixture.ops.request.data(),
                     fixture.ops.media.data() + 7 * 512, 512) == 0);
  assert(std::memcmp(fixture.ops.request.data() + 512,
                     fixture.ops.media.data() + 4096, 512) == 0);

  fixture.ops.request.assign(fixture.ops.request.size(), 0xa5);
  result = fixture.disk.Read(last, 1);
  assert(result.status == wur::Status::Ok);
  assert(result.bytes_done == 512);
  assert(fixture.ops.reads == 4);
  assert(std::memcmp(fixture.ops.request.data(),
                     fixture.ops.media.data() + fixture.ops.media.size() - 512,
                     512) == 0);

  const unsigned reads = fixture.ops.reads;
  assert(fixture.disk.Read(first - 1, 1).status ==
         wur::Status::OutOfBounds);
  assert(fixture.disk.Read(last + 1, 1).status ==
         wur::Status::OutOfBounds);
  assert(fixture.disk.Read(first, 0).status == wur::Status::ZeroLength);
  assert(fixture.disk.Read(UINT64_MAX - 1, 2).status ==
         wur::Status::Overflow);
  assert(fixture.ops.reads == reads);

  fixture.ops.fail_read = true;
  fixture.ops.request.assign(fixture.ops.request.size(), 0xa5);
  result = fixture.disk.Read(first, 1);
  assert(result.status == wur::Status::IoError);
  assert(result.bytes_done == 0);
  assert(fixture.ops.request[0] == 0xa5);
}

void TestWrites() {
  Fixture fixture;
  fixture.Open();
  fixture.FillMedia();
  const uint64_t first = fixture.geometry.first_logical_sector;

  for (unsigned index = 0; index < 1024; ++index) {
    fixture.ops.request[index] = static_cast<uint8_t>(0x40 + index);
  }
  const auto old = fixture.ops.media;
  wur::DiskResult result = fixture.disk.Write(first + 7, 2, false);
  assert(result.status == wur::Status::Ok);
  assert(result.bytes_done == 1024);
  assert(fixture.ops.reads == 2);
  assert(fixture.ops.writes == 2);
  assert(!fixture.ops.last_fua);
  assert(std::memcmp(fixture.ops.media.data(), old.data(), 7 * 512) == 0);
  assert(std::memcmp(fixture.ops.media.data() + 7 * 512,
                     fixture.ops.request.data(), 512) == 0);
  assert(std::memcmp(fixture.ops.media.data() + 4096,
                     fixture.ops.request.data() + 512, 512) == 0);
  assert(std::memcmp(fixture.ops.media.data() + 4608, old.data() + 4608,
                     4096 - 512) == 0);

  for (unsigned index = 0; index < 4096; ++index) {
    fixture.ops.request[index] = static_cast<uint8_t>(index * 7);
  }
  const unsigned reads = fixture.ops.reads;
  const unsigned writes = fixture.ops.writes;
  result = fixture.disk.Write(first, 8, false);
  assert(result.status == wur::Status::Ok);
  assert(fixture.ops.reads == reads);
  assert(fixture.ops.writes == writes + 1);
  assert(std::memcmp(fixture.ops.media.data(), fixture.ops.request.data(),
                     4096) == 0);

  result = fixture.disk.Write(first, 8, true);
  assert(result.status == wur::Status::Ok);
  assert(fixture.ops.writes == writes + 2);
  assert(fixture.ops.last_fua);

  const unsigned no_reads = fixture.ops.reads;
  const unsigned no_writes = fixture.ops.writes;
  assert(fixture.disk.Write(first + 1, 8, true).status ==
         wur::Status::UnalignedMediaBlock);
  assert(fixture.disk.Write(first, 7, true).status ==
         wur::Status::UnalignedMediaBlock);
  assert(fixture.disk.Write(first - 1, 8, false).status ==
         wur::Status::OutOfBounds);
  assert(fixture.disk.Write(first, 0, false).status ==
         wur::Status::ZeroLength);
  assert(fixture.ops.reads == no_reads);
  assert(fixture.ops.writes == no_writes);

  fixture.ops.fail_read = true;
  fixture.ops.writes = no_writes;
  result = fixture.disk.Write(first + 1, 1, false);
  assert(result.status == wur::Status::IoError);
  assert(result.bytes_done == 0);
  assert(fixture.ops.writes == no_writes);

  fixture.ops.fail_read = false;
  fixture.ops.fail_copy_in = true;
  result = fixture.disk.Write(first, 8, false);
  assert(result.status == wur::Status::IoError);
  assert(fixture.ops.writes == no_writes);

  fixture.ops.fail_copy_in = false;
  fixture.ops.fail_write = true;
  result = fixture.disk.Write(first, 8, false);
  assert(result.status == wur::Status::IoError);
  assert(result.bytes_done == 0);
}

void TestSynchronize() {
  Fixture fixture;
  fixture.Open();
  const uint64_t first = fixture.geometry.first_logical_sector;

  wur::DiskResult result = fixture.disk.Synchronize(first, 8);
  assert(result.status == wur::Status::Ok);
  assert(fixture.ops.synchronizes == 1);
  assert(fixture.disk.Synchronize(first + 1, 8).status ==
         wur::Status::UnalignedMediaBlock);
  assert(fixture.disk.Synchronize(first, 0).status ==
         wur::Status::ZeroLength);
  assert(fixture.disk.Synchronize(first - 8, 8).status ==
         wur::Status::OutOfBounds);
  assert(fixture.ops.synchronizes == 1);

  fixture.Open(8, 11, false);
  assert(fixture.disk.Synchronize(first, 8).status ==
         wur::Status::ReadOnly);
}

void TestTransferLimit() {
  Fixture fixture;
  fixture.Open(8, 47);
  const uint64_t first = fixture.geometry.first_logical_sector;
  fixture.ops.request.resize(wur::kMaxTransferBytes);
  wur::DiskResult result =
      fixture.disk.Read(first, wur::kMaxTransferSectors);
  assert(result.status == wur::Status::Ok);
  assert(result.bytes_done == wur::kMaxTransferBytes);

  const unsigned reads = fixture.ops.reads;
  result = fixture.disk.Read(first, wur::kMaxTransferSectors + 1);
  assert(result.status == wur::Status::TransferTooLarge);
  assert(result.bytes_done == 0);
  assert(fixture.ops.reads == reads);
}

}  // namespace

int main() {
  TestConfiguration();
  TestReads();
  TestWrites();
  TestSynchronize();
  TestTransferLimit();
  std::puts(
      "PASS: whole-userdata disk frontend configuration, 512B reads, partial "
      "write RMW, whole-block/FUA writes, sync gating, transfer limits and "
      "fail-closed error paths");
  return 0;
}
