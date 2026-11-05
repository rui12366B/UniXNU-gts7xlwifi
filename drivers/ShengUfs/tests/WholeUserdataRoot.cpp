// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../WholeUserdataRoot.h"

#include <cassert>
#include <cstdio>

namespace wur = alioth::whole_userdata_root;

namespace {

constexpr uint64_t kFirstMediaLba = 0x2a8000ULL;
constexpr uint64_t kLastMediaLba = 0x3b037faULL;
constexpr uint64_t kCapacityBytes = 242053263360ULL;

void TestGeometry() {
  uint64_t arithmetic = 0;
  assert(!wur::CheckedAdd(UINT64_MAX, 1, &arithmetic));
  assert(!wur::CheckedMultiply(UINT64_MAX, 2, &arithmetic));

  wur::Geometry geometry = {};
  assert(wur::ProjectGeometry(kFirstMediaLba, kLastMediaLba, kCapacityBytes,
                              &geometry) == wur::Status::Ok);
  assert(geometry.first_media_lba == kFirstMediaLba);
  assert(geometry.last_media_lba == kLastMediaLba);
  assert(geometry.media_block_count == 0x385b7fbULL);
  assert(geometry.first_logical_sector == 0x1540000ULL);
  assert(geometry.logical_sector_count == 0x1c2dbfd8ULL);
  assert(geometry.capacity_bytes == kCapacityBytes);
  assert(wur::ValidateGeometry(geometry) == wur::Status::Ok);

  assert(wur::ProjectGeometry(kFirstMediaLba, kLastMediaLba,
                              kCapacityBytes + 512,
                              &geometry) ==
         wur::Status::CapacityMismatch);
  assert(wur::ProjectGeometry(kLastMediaLba, kFirstMediaLba, 0,
                              &geometry) ==
         wur::Status::InvalidGeometry);
  assert(wur::ProjectGeometry(0, UINT64_C(0x100000000), 0, &geometry) ==
         wur::Status::MediaLbaOutOfRange);
  assert(wur::ProjectGeometry(0, 0, wur::kMediaBlockBytes, nullptr) ==
         wur::Status::NullArgument);
  assert(wur::ProjectGeometry(
             UINT64_C(0x1fffffffffffffff), UINT64_C(0x1fffffffffffffff),
             wur::kMediaBlockBytes, &geometry) ==
         wur::Status::MediaLbaOutOfRange);

  assert(wur::ProjectGeometry(0, 0, wur::kMediaBlockBytes, &geometry) ==
         wur::Status::Ok);
  geometry.logical_sector_count++;
  assert(wur::ValidateGeometry(geometry) ==
         wur::Status::InvalidGeometry);
}

void TestProjection(const wur::Geometry &geometry) {
  uint64_t sector = 0;
  assert(wur::MediaLbaToLogicalSector(geometry, kFirstMediaLba, &sector) ==
         wur::Status::Ok);
  assert(sector == 0x1540000ULL);

  assert(wur::MediaLbaToLogicalSector(geometry, kLastMediaLba, &sector) ==
         wur::Status::Ok);
  assert(sector == 0x1d81bfd0ULL);
  assert(wur::MediaLbaToLogicalSector(geometry, kLastMediaLba + 1,
                                      &sector) ==
         wur::Status::OutOfBounds);

  uint64_t media_lba = 0;
  uint32_t offset = 0;
  assert(wur::LogicalSectorToMediaBlock(geometry, 0x1540000ULL, &media_lba,
                                        &offset) == wur::Status::Ok);
  assert(media_lba == kFirstMediaLba && offset == 0);
  assert(wur::LogicalSectorToMediaBlock(geometry, 0x1540007ULL, &media_lba,
                                        &offset) == wur::Status::Ok);
  assert(media_lba == kFirstMediaLba && offset == 7 * 512);
  assert(wur::LogicalSectorToMediaBlock(geometry, 0x1540008ULL, &media_lba,
                                        &offset) == wur::Status::Ok);
  assert(media_lba == kFirstMediaLba + 1 && offset == 0);
  assert(wur::LogicalSectorToMediaBlock(geometry, 0x1d81bfd7ULL, &media_lba,
                                        &offset) == wur::Status::Ok);
  assert(media_lba == kLastMediaLba && offset == 7 * 512);
  assert(wur::LogicalSectorToMediaBlock(geometry, 0x1d81bfd8ULL,
                                        &media_lba,
                                        &offset) ==
         wur::Status::OutOfBounds);
  assert(wur::LogicalSectorToMediaBlock(geometry, 0x153ffffULL,
                                        &media_lba,
                                        &offset) ==
         wur::Status::OutOfBounds);
  assert(wur::LogicalSectorToMediaBlock(geometry, 0x1540000ULL, nullptr,
                                        &offset) ==
         wur::Status::NullArgument);
}

void TestRangePolicy(const wur::Geometry &geometry) {
  wur::SectorRange accepted = {};

  assert(wur::ClampSectorRange(
             geometry, {0x1540000ULL, 1}, &accepted) ==
         wur::Status::Ok);
  assert(accepted.first_logical_sector == 0x1540000ULL &&
         accepted.sector_count == 1);
  assert(wur::ClampSectorRange(
             geometry, {0x1d81bfd7ULL, 1}, &accepted) ==
         wur::Status::Ok);
  assert(wur::ClampSectorRange(
             geometry, {0x1540000ULL, geometry.logical_sector_count},
             &accepted) == wur::Status::Ok);

  assert(wur::ClampSectorRange(
             geometry, {0x153ffffULL, 1}, &accepted) ==
         wur::Status::OutOfBounds);
  assert(wur::ClampSectorRange(
             geometry, {0x1d81bfd8ULL, 1}, &accepted) ==
         wur::Status::OutOfBounds);
  assert(wur::ClampSectorRange(
             geometry, {0x1d81bfd7ULL, 2}, &accepted) ==
         wur::Status::OutOfBounds);
  assert(wur::ClampSectorRange(
             geometry, {0x1540000ULL, 0}, &accepted) ==
         wur::Status::ZeroLength);
  assert(wur::ClampSectorRange(
             geometry, {UINT64_MAX - 1, 2}, &accepted) ==
         wur::Status::Overflow);
  assert(wur::ClampSectorRange(
             geometry, {0x1540000ULL, 1}, nullptr) ==
         wur::Status::NullArgument);
}

void TestAccessPolicy(const wur::Geometry &geometry) {
  wur::SectorRange accepted = {};
  assert(wur::ValidateAccess(geometry, wur::AccessKind::Read,
                             {0x1540000ULL, 1}, &accepted) ==
         wur::Status::Ok);
  assert(wur::ValidateAccess(geometry, wur::AccessKind::Write,
                             {0x1540007ULL, 2}, &accepted) ==
         wur::Status::Ok);

  assert(wur::ValidateAccess(geometry, wur::AccessKind::FuaWrite,
                             {0x1540000ULL, 1}, &accepted) ==
         wur::Status::UnalignedMediaBlock);
  assert(wur::ValidateAccess(geometry, wur::AccessKind::FuaWrite,
                             {0x1540001ULL, 8}, &accepted) ==
         wur::Status::UnalignedMediaBlock);
  assert(wur::ValidateAccess(geometry, wur::AccessKind::FuaWrite,
                             {0x1540000ULL, 9}, &accepted) ==
         wur::Status::UnalignedMediaBlock);
  assert(wur::ValidateAccess(geometry, wur::AccessKind::FuaWrite,
                             {0x1540000ULL, 8}, &accepted) ==
         wur::Status::Ok);

  assert(wur::ValidateAccess(
             geometry, wur::AccessKind::SynchronizeCache,
             {0x1540000ULL, 8}, &accepted) ==
         wur::Status::Ok);
  assert(wur::ValidateAccess(
             geometry, wur::AccessKind::SynchronizeCache,
             {geometry.first_logical_sector, geometry.logical_sector_count},
             &accepted) == wur::Status::TransferTooLarge);
  assert(wur::ValidateAccess(geometry, wur::AccessKind::SynchronizeCache,
                             {0x1540004ULL, 8}, &accepted) ==
         wur::Status::UnalignedMediaBlock);
  assert(wur::ValidateAccess(geometry, wur::AccessKind::SynchronizeCache,
                             {0x1540000ULL, 0}, &accepted) ==
         wur::Status::ZeroLength);
  assert(wur::ValidateAccess(
             geometry, wur::AccessKind::Read,
             {0x1d81bfd8ULL, wur::kMaxTransferSectors}, &accepted) ==
         wur::Status::OutOfBounds);

  assert(wur::ValidateAccess(
             geometry, wur::AccessKind::Read,
             {geometry.first_logical_sector, wur::kMaxTransferSectors},
             &accepted) == wur::Status::Ok);
  assert(wur::ValidateAccess(
             geometry, wur::AccessKind::Read,
             {geometry.first_logical_sector,
              static_cast<uint64_t>(wur::kMaxTransferSectors) + 1},
             &accepted) == wur::Status::TransferTooLarge);
  assert(wur::ValidateAccess(
             geometry, static_cast<wur::AccessKind>(0xff),
             {geometry.first_logical_sector, 1}, &accepted) ==
         wur::Status::InvalidAccess);
}

void TestChunking(const wur::Geometry &geometry) {
  wur::ChunkCursor cursor = {};
  wur::Chunk chunk = {};
  assert(wur::BeginChunks(
             geometry, wur::AccessKind::Read,
             {geometry.first_logical_sector + 1, 11}, &cursor) ==
         wur::Status::Ok);

  assert(wur::NextChunk(geometry, &cursor, &chunk) == wur::Status::Ok);
  assert(chunk.first_logical_sector == geometry.first_logical_sector + 1);
  assert(chunk.media_lba == kFirstMediaLba);
  assert(chunk.media_offset_bytes == 1 * 512);
  assert(chunk.sector_count == 7);
  assert(chunk.byte_count == 7 * 512);
  assert(!chunk.whole_media_block);
  assert(!chunk.needs_read_modify_write);

  assert(wur::NextChunk(geometry, &cursor, &chunk) == wur::Status::Ok);
  assert(chunk.first_logical_sector == geometry.first_logical_sector + 8);
  assert(chunk.media_lba == kFirstMediaLba + 1);
  assert(chunk.media_offset_bytes == 0);
  assert(chunk.sector_count == 4);
  assert(chunk.byte_count == 4 * 512);
  assert(!chunk.whole_media_block);
  assert(wur::NextChunk(geometry, &cursor, &chunk) == wur::Status::End);

  assert(wur::BeginChunks(
             geometry, wur::AccessKind::Write,
             {geometry.first_logical_sector, 9}, &cursor) ==
         wur::Status::Ok);
  assert(wur::NextChunk(geometry, &cursor, &chunk) == wur::Status::Ok);
  assert(chunk.whole_media_block && !chunk.needs_read_modify_write);
  assert(wur::NextChunk(geometry, &cursor, &chunk) == wur::Status::Ok);
  assert(!chunk.whole_media_block && chunk.needs_read_modify_write);
  assert(wur::NextChunk(geometry, &cursor, &chunk) == wur::Status::End);

  assert(wur::BeginChunks(
             geometry, wur::AccessKind::FuaWrite,
             {geometry.first_logical_sector, 16}, &cursor) ==
         wur::Status::Ok);
  assert(wur::NextChunk(geometry, &cursor, &chunk) == wur::Status::Ok);
  assert(chunk.media_lba == kFirstMediaLba && chunk.whole_media_block &&
         !chunk.needs_read_modify_write);
  assert(wur::NextChunk(geometry, &cursor, &chunk) == wur::Status::Ok);
  assert(chunk.media_lba == kFirstMediaLba + 1 &&
         chunk.whole_media_block && !chunk.needs_read_modify_write);
  assert(wur::NextChunk(geometry, &cursor, &chunk) == wur::Status::End);

  assert(wur::BeginChunks(
             geometry, wur::AccessKind::Read,
             {geometry.first_logical_sector, wur::kMaxTransferSectors},
             &cursor) == wur::Status::Ok);
  uint64_t chunk_count = 0;
  uint64_t total_bytes = 0;
  wur::Status chunk_status = wur::Status::Ok;
  while ((chunk_status = wur::NextChunk(geometry, &cursor, &chunk)) ==
         wur::Status::Ok) {
    ++chunk_count;
    total_bytes += chunk.byte_count;
    assert(chunk.whole_media_block);
  }
  assert(chunk_status == wur::Status::End);
  assert(chunk_count == wur::kMaxTransferBytes / wur::kMediaBlockBytes);
  assert(total_bytes == wur::kMaxTransferBytes);

  const uint64_t last_sector =
      geometry.first_logical_sector + geometry.logical_sector_count - 1;
  assert(wur::BeginChunks(geometry, wur::AccessKind::Read,
                          {last_sector, 1}, &cursor) == wur::Status::Ok);
  assert(wur::NextChunk(geometry, &cursor, &chunk) == wur::Status::Ok);
  assert(chunk.media_lba == kLastMediaLba);
  assert(chunk.media_offset_bytes == 7 * 512);
  assert(chunk.sector_count == 1 && chunk.byte_count == 512);
  assert(wur::NextChunk(geometry, &cursor, &chunk) == wur::Status::End);
}

}  // namespace

int main() {
  wur::Geometry geometry = {};
  assert(wur::ProjectGeometry(kFirstMediaLba, kLastMediaLba, kCapacityBytes,
                              &geometry) == wur::Status::Ok);
  TestGeometry();
  TestProjection(geometry);
  TestRangePolicy(geometry);
  TestAccessPolicy(geometry);
  TestChunking(geometry);
  std::puts(
      "PASS: whole-userdata geometry, exact 4K/512 projection, fail-closed "
      "ranges, FUA/sync alignment, 128 KiB cap and 4K chunk boundaries");
  return 0;
}
