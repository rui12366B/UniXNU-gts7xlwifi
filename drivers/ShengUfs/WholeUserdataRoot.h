// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
//
// Pure geometry and range policy for mounting the whole alioth userdata
// partition as a Darwin block device.
//
// Units and ABI:
//   * GPT media LBAs address 4096-byte blocks and are inclusive on both ends.
//   * Darwin/Sweet storage requests address 512-byte logical sectors.
//   * Sector ranges are half-open: [first_logical_sector, first + count).
//   * One media block is exactly eight logical sectors (4096 = 8 * 512).
//
// This header has no device, MMIO, register, kernel-service, allocation or I/O
// dependency. It is intentionally fail-closed: a partially outside range is
// refused rather than silently truncated. Callers that want short I/O must
// select a contained range before calling here.

#include <stdint.h>

namespace alioth {
namespace whole_userdata_root {

inline constexpr uint32_t kMediaBlockBytes = 4096;
inline constexpr uint32_t kLogicalSectorBytes = 512;
inline constexpr uint32_t kLogicalSectorsPerMediaBlock =
    kMediaBlockBytes / kLogicalSectorBytes;

// RootCommands currently packs one PRDT entry into a 128 KiB DMA area.
// A caller may submit a larger logical request by starting another policy
// batch, but one batch is deliberately capped at this value.
inline constexpr uint32_t kMaxTransferBytes = 128U * 1024U;
inline constexpr uint32_t kMaxTransferSectors =
    kMaxTransferBytes / kLogicalSectorBytes;

// READ(10)/WRITE(10) in the current NativeUfs transport carry a 32-bit LBA.
inline constexpr uint64_t kMaxCurrentTransportMediaLba = UINT32_MAX;

static_assert(kLogicalSectorsPerMediaBlock == 8,
              "one 4K media block must contain exactly eight 512-byte sectors");
static_assert(kLogicalSectorBytes * kLogicalSectorsPerMediaBlock ==
                  kMediaBlockBytes,
              "logical-sector projection must be exact");
static_assert(kMaxTransferBytes % kMediaBlockBytes == 0,
              "transfer cap must contain whole media blocks");
static_assert(kMaxTransferBytes % kLogicalSectorBytes == 0,
              "transfer cap must contain whole logical sectors");

enum class Status : uint8_t {
  Ok = 0,
  NullArgument,
  InvalidGeometry,
  CapacityMismatch,
  MediaLbaOutOfRange,
  ZeroLength,
  Overflow,
  OutOfBounds,
  UnalignedMediaBlock,
  TransferTooLarge,
  InvalidAccess,
  NotReady,
  ReadOnly,
  IoError,
  End,
};

struct Geometry {
  // Input geometry: inclusive 4K media LBAs from the userdata GPT entry.
  uint64_t first_media_lba;
  uint64_t last_media_lba;

  // Derived values. They are populated only after all checks pass.
  uint64_t media_block_count;
  uint64_t first_logical_sector;
  uint64_t logical_sector_count;
  uint64_t capacity_bytes;
};

struct SectorRange {
  uint64_t first_logical_sector;
  uint64_t sector_count;
};

enum class AccessKind : uint8_t {
  Read = 0,
  Write,
  FuaWrite,
  SynchronizeCache,
};

struct Chunk {
  uint64_t first_logical_sector;
  uint64_t media_lba;
  uint32_t media_offset_bytes;  // offset within the 4096-byte media block
  uint32_t sector_count;
  uint32_t byte_count;
  bool whole_media_block;
  bool needs_read_modify_write;
};

struct ChunkCursor {
  uint64_t next_logical_sector;
  uint64_t end_logical_sector;
  AccessKind access;
};

inline bool CheckedAdd(uint64_t left, uint64_t right, uint64_t *result) {
  if (result == nullptr || left > UINT64_MAX - right) {
    return false;
  }
  *result = left + right;
  return true;
}

inline bool CheckedMultiply(uint64_t left, uint64_t right,
                            uint64_t *result) {
  if (result == nullptr || (left != 0 && right > UINT64_MAX / left)) {
    return false;
  }
  *result = left * right;
  return true;
}

inline bool IsKnownAccess(AccessKind access) {
  switch (access) {
    case AccessKind::Read:
    case AccessKind::Write:
    case AccessKind::FuaWrite:
    case AccessKind::SynchronizeCache:
      return true;
  }
  return false;
}

// Validate the supplied GPT endpoints and project them into 512-byte logical
// sectors. expected_capacity_bytes is the GPT-derived partition capacity and
// must match exactly.
inline Status ProjectGeometry(uint64_t first_media_lba,
                              uint64_t last_media_lba,
                              uint64_t expected_capacity_bytes,
                              Geometry *out) {
  if (out == nullptr) {
    return Status::NullArgument;
  }
  *out = {};
  if (first_media_lba > last_media_lba) {
    return Status::InvalidGeometry;
  }
  if (last_media_lba > kMaxCurrentTransportMediaLba) {
    return Status::MediaLbaOutOfRange;
  }

  const uint64_t media_block_count =
      last_media_lba - first_media_lba + 1;
  uint64_t first_logical_sector = 0;
  uint64_t logical_sector_count = 0;
  uint64_t capacity_bytes = 0;
  if (!CheckedMultiply(first_media_lba,
                       static_cast<uint64_t>(kLogicalSectorsPerMediaBlock),
                       &first_logical_sector) ||
      !CheckedMultiply(media_block_count,
                       static_cast<uint64_t>(kLogicalSectorsPerMediaBlock),
                       &logical_sector_count) ||
      !CheckedMultiply(media_block_count,
                       static_cast<uint64_t>(kMediaBlockBytes),
                       &capacity_bytes)) {
    return Status::Overflow;
  }
  if (capacity_bytes != expected_capacity_bytes) {
    return Status::CapacityMismatch;
  }

  out->first_media_lba = first_media_lba;
  out->last_media_lba = last_media_lba;
  out->media_block_count = media_block_count;
  out->first_logical_sector = first_logical_sector;
  out->logical_sector_count = logical_sector_count;
  out->capacity_bytes = capacity_bytes;
  return Status::Ok;
}

// Recompute all derived fields so an inconsistent Geometry is refused. The
// first/last LBAs must still originate from a separately validated GPT entry;
// this object is policy state, not a replacement for GPT authentication.
inline Status ValidateGeometry(const Geometry &geometry) {
  Geometry canonical = {};
  const Status status =
      ProjectGeometry(geometry.first_media_lba, geometry.last_media_lba,
                      geometry.capacity_bytes, &canonical);
  if (status != Status::Ok) {
    return status;
  }
  if (geometry.media_block_count != canonical.media_block_count ||
      geometry.first_logical_sector != canonical.first_logical_sector ||
      geometry.logical_sector_count != canonical.logical_sector_count ||
      geometry.capacity_bytes != canonical.capacity_bytes) {
    return Status::InvalidGeometry;
  }
  return Status::Ok;
}

// Project a 4K media LBA to the first 512-byte sector of that block.
inline Status MediaLbaToLogicalSector(const Geometry &geometry,
                                      uint64_t media_lba,
                                      uint64_t *first_logical_sector) {
  if (first_logical_sector == nullptr) {
    return Status::NullArgument;
  }
  const Status status = ValidateGeometry(geometry);
  if (status != Status::Ok) {
    return status;
  }
  if (media_lba < geometry.first_media_lba ||
      media_lba > geometry.last_media_lba) {
    return Status::OutOfBounds;
  }
  uint64_t projected = 0;
  if (!CheckedMultiply(media_lba,
                       static_cast<uint64_t>(kLogicalSectorsPerMediaBlock),
                       &projected)) {
    return Status::Overflow;
  }
  *first_logical_sector = projected;
  return Status::Ok;
}

// Project any 512-byte logical sector to its containing 4K media block and
// exact byte offset inside that block. This function intentionally accepts a
// non-block-aligned sector; alignment requirements belong to the access kind.
inline Status LogicalSectorToMediaBlock(const Geometry &geometry,
                                        uint64_t logical_sector,
                                        uint64_t *media_lba,
                                        uint32_t *media_offset_bytes) {
  if (media_lba == nullptr || media_offset_bytes == nullptr) {
    return Status::NullArgument;
  }
  const Status status = ValidateGeometry(geometry);
  if (status != Status::Ok) {
    return status;
  }
  uint64_t partition_end = 0;
  if (!CheckedAdd(geometry.first_logical_sector,
                  geometry.logical_sector_count, &partition_end)) {
    return Status::Overflow;
  }
  if (logical_sector < geometry.first_logical_sector ||
      logical_sector >= partition_end) {
    return Status::OutOfBounds;
  }

  const uint64_t relative =
      logical_sector - geometry.first_logical_sector;
  const uint64_t block = relative / kLogicalSectorsPerMediaBlock;
  const uint32_t sector_in_block =
      static_cast<uint32_t>(relative % kLogicalSectorsPerMediaBlock);
  if (block > UINT64_MAX - geometry.first_media_lba) {
    return Status::Overflow;
  }

  *media_lba = geometry.first_media_lba + block;
  *media_offset_bytes = sector_in_block * kLogicalSectorBytes;
  return Status::Ok;
}

// Strict containment check. Despite the name, this never truncates: a range
// that touches either side of the partition is refused as a whole.
inline Status ClampSectorRange(const Geometry &geometry,
                               SectorRange requested,
                               SectorRange *accepted) {
  if (accepted == nullptr) {
    return Status::NullArgument;
  }
  *accepted = {};
  if (requested.sector_count == 0) {
    return Status::ZeroLength;
  }
  const Status status = ValidateGeometry(geometry);
  if (status != Status::Ok) {
    return status;
  }

  uint64_t requested_end = 0;
  uint64_t partition_end = 0;
  if (!CheckedAdd(requested.first_logical_sector,
                  requested.sector_count, &requested_end) ||
      !CheckedAdd(geometry.first_logical_sector,
                  geometry.logical_sector_count, &partition_end)) {
    return Status::Overflow;
  }
  if (requested.first_logical_sector < geometry.first_logical_sector ||
      requested_end > partition_end) {
    return Status::OutOfBounds;
  }

  *accepted = requested;
  return Status::Ok;
}

// Validate one policy batch.
//   * Read and ordinary Write may start or end on any 512-byte sector.
//   * FUA Write and Synchronize Cache must consist only of whole 4K media
//     blocks, so neither operation can escape or partially cover a block.
//   * Every operation is non-zero, wholly inside userdata and at most 128 KiB.
inline Status ValidateAccess(const Geometry &geometry, AccessKind access,
                             SectorRange requested,
                             SectorRange *accepted) {
  if (!IsKnownAccess(access)) {
    return Status::InvalidAccess;
  }
  SectorRange contained = {};
  const Status status = ClampSectorRange(geometry, requested, &contained);
  if (status != Status::Ok) {
    return status;
  }

  uint64_t transfer_bytes = 0;
  if (!CheckedMultiply(contained.sector_count,
                       static_cast<uint64_t>(kLogicalSectorBytes),
                       &transfer_bytes)) {
    return Status::Overflow;
  }
  if (transfer_bytes > kMaxTransferBytes) {
    return Status::TransferTooLarge;
  }

  if (access == AccessKind::FuaWrite ||
      access == AccessKind::SynchronizeCache) {
    if ((contained.first_logical_sector %
             kLogicalSectorsPerMediaBlock !=
         0) ||
        contained.sector_count % kLogicalSectorsPerMediaBlock != 0) {
      return Status::UnalignedMediaBlock;
    }
  }

  *accepted = contained;
  return Status::Ok;
}

// Begin a bounded batch that splits at every 4K media-block boundary. Each
// returned chunk maps to exactly one media LBA and is therefore safe to pass
// to a transport that addresses 4K blocks.
inline Status BeginChunks(const Geometry &geometry, AccessKind access,
                          SectorRange requested, ChunkCursor *cursor) {
  if (cursor == nullptr) {
    return Status::NullArgument;
  }
  *cursor = {};
  SectorRange accepted = {};
  const Status status =
      ValidateAccess(geometry, access, requested, &accepted);
  if (status != Status::Ok) {
    return status;
  }

  uint64_t end = 0;
  if (!CheckedAdd(accepted.first_logical_sector, accepted.sector_count,
                  &end)) {
    return Status::Overflow;
  }
  cursor->next_logical_sector = accepted.first_logical_sector;
  cursor->end_logical_sector = end;
  cursor->access = access;
  return Status::Ok;
}

inline Status NextChunk(const Geometry &geometry, ChunkCursor *cursor,
                        Chunk *chunk) {
  if (cursor == nullptr || chunk == nullptr) {
    return Status::NullArgument;
  }
  *chunk = {};
  const Status status = ValidateGeometry(geometry);
  if (status != Status::Ok) {
    return status;
  }
  if (!IsKnownAccess(cursor->access)) {
    return Status::InvalidAccess;
  }
  if (cursor->next_logical_sector == cursor->end_logical_sector) {
    return Status::End;
  }
  if (cursor->next_logical_sector > cursor->end_logical_sector) {
    return Status::InvalidGeometry;
  }

  uint64_t partition_end = 0;
  if (!CheckedAdd(geometry.first_logical_sector,
                  geometry.logical_sector_count, &partition_end)) {
    return Status::Overflow;
  }
  if (cursor->next_logical_sector < geometry.first_logical_sector ||
      cursor->end_logical_sector > partition_end) {
    return Status::OutOfBounds;
  }
  if ((cursor->access == AccessKind::FuaWrite ||
       cursor->access == AccessKind::SynchronizeCache) &&
      (cursor->next_logical_sector % kLogicalSectorsPerMediaBlock != 0 ||
       cursor->end_logical_sector % kLogicalSectorsPerMediaBlock != 0)) {
    return Status::UnalignedMediaBlock;
  }

  uint64_t media_lba = 0;
  uint32_t media_offset_bytes = 0;
  const Status projection =
      LogicalSectorToMediaBlock(geometry, cursor->next_logical_sector,
                                &media_lba, &media_offset_bytes);
  if (projection != Status::Ok) {
    return projection;
  }

  const uint64_t sectors_to_block_end =
      kLogicalSectorsPerMediaBlock -
      (cursor->next_logical_sector % kLogicalSectorsPerMediaBlock);
  const uint64_t remaining =
      cursor->end_logical_sector - cursor->next_logical_sector;
  const uint64_t chunk_sectors =
      remaining < sectors_to_block_end ? remaining : sectors_to_block_end;

  chunk->first_logical_sector = cursor->next_logical_sector;
  chunk->media_lba = media_lba;
  chunk->media_offset_bytes = media_offset_bytes;
  chunk->sector_count = static_cast<uint32_t>(chunk_sectors);
  chunk->byte_count =
      static_cast<uint32_t>(chunk_sectors * kLogicalSectorBytes);
  chunk->whole_media_block =
      chunk->sector_count == kLogicalSectorsPerMediaBlock;
  chunk->needs_read_modify_write =
      (cursor->access == AccessKind::Write ||
       cursor->access == AccessKind::FuaWrite) &&
      !chunk->whole_media_block;

  cursor->next_logical_sector += chunk_sectors;
  return Status::Ok;
}

}  // namespace whole_userdata_root
}  // namespace alioth
