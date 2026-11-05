// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeUfsBsdRootCore.h"

#include <string.h>

namespace alioth {
namespace native_ufs_bsd_root {
namespace {

constexpr uint64_t kMaxCurrentTransportLba = UINT32_MAX;

uint32_t Le32(const uint8_t *at) {
  return static_cast<uint32_t>(at[0]) |
         (static_cast<uint32_t>(at[1]) << 8) |
         (static_cast<uint32_t>(at[2]) << 16) |
         (static_cast<uint32_t>(at[3]) << 24);
}

uint64_t Le64(const uint8_t *at) {
  return static_cast<uint64_t>(Le32(at)) |
         (static_cast<uint64_t>(Le32(at + 4)) << 32);
}

uint16_t Be16(const uint8_t *at) {
  return static_cast<uint16_t>((static_cast<uint16_t>(at[0]) << 8) | at[1]);
}

bool EntryUsed(const uint8_t *entry) {
  uint8_t used = 0;
  for (size_t index = 0; index < 16; ++index) {
    used = static_cast<uint8_t>(used | entry[index]);
  }
  return used != 0;
}

bool RangeOverlaps(uint64_t first, uint64_t last, uint64_t other_first,
                   uint64_t other_last) {
  return first <= other_last && other_first <= last;
}

bool IsUserdataName(const uint8_t *entry) {
  static constexpr char kName[] = "userdata";
  for (size_t index = 0; index < sizeof(kName); ++index) {
    if (entry[56 + 2 * index] != static_cast<uint8_t>(kName[index]) ||
        entry[57 + 2 * index] != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

uint32_t Crc32(const void *data, size_t bytes, bool gpt_header) {
  if (data == nullptr) {
    return 0;
  }
  const uint8_t *input = static_cast<const uint8_t *>(data);
  uint32_t crc = 0xffffffffU;
  for (size_t index = 0; index < bytes; ++index) {
    const bool zero_header_crc =
        gpt_header && index >= 16 && index < 20;
    crc ^= zero_header_crc ? 0 : input[index];
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

Status ParseGptHeader(const void *block, GptHeader *header) {
  if (block == nullptr || header == nullptr) {
    return Status::InvalidArgument;
  }
  *header = {};
  const uint8_t *bytes = static_cast<const uint8_t *>(block);
  if (memcmp(bytes, "EFI PART", 8) != 0) {
    return Status::BadGptHeader;
  }
  const uint32_t revision = Le32(bytes + 8);
  const uint32_t header_bytes = Le32(bytes + 12);
  if (revision != 0x00010000 || header_bytes < 92 ||
      header_bytes > kMediaBlockBytes || Le32(bytes + 20) != 0) {
    return Status::BadGptHeader;
  }
  const uint32_t header_crc = Le32(bytes + 16);
  if (Crc32(bytes, header_bytes, true) != header_crc) {
    return Status::BadGptCrc;
  }
  GptHeader parsed = {};
  parsed.current_lba = Le64(bytes + 24);
  parsed.backup_lba = Le64(bytes + 32);
  parsed.first_usable_lba = Le64(bytes + 40);
  parsed.last_usable_lba = Le64(bytes + 48);
  parsed.entries_lba = Le64(bytes + 72);
  parsed.entries_count = Le32(bytes + 80);
  parsed.entry_bytes = Le32(bytes + 84);
  parsed.entries_crc = Le32(bytes + 88);
  parsed.header_crc = header_crc;
  parsed.header_bytes = header_bytes;
  if (parsed.current_lba != kGptHeaderLba ||
      parsed.entries_lba <= parsed.current_lba ||
      parsed.entries_lba < 2 || parsed.entries_lba > 64 ||
      parsed.last_usable_lba < parsed.first_usable_lba ||
      parsed.backup_lba <= parsed.last_usable_lba ||
      parsed.backup_lba > kMaxCurrentTransportLba ||
      parsed.entries_count == 0 ||
      parsed.entries_count > kMaxGptEntries ||
      parsed.entry_bytes != kGptEntryBytes ||
      parsed.last_usable_lba <= kGptHeaderLba) {
    return Status::BadGptGeometry;
  }
  *header = parsed;
  return Status::Ok;
}

Status ParseUserdataPartition(const void *entries, size_t entry_bytes,
                              uint32_t entry_count, uint64_t first_usable,
                              uint64_t last_usable, Geometry *geometry) {
  if (entries == nullptr || geometry == nullptr || entry_count == 0 ||
      entry_count > kMaxGptEntries || entry_bytes != kGptEntryBytes) {
    return Status::InvalidArgument;
  }
  *geometry = {};
  const uint8_t *table = static_cast<const uint8_t *>(entries);
  bool found = false;
  bool userdata_seen = false;
  uint64_t selected_first = 0;
  uint64_t selected_last = 0;
  for (uint32_t index = 0; index < entry_count; ++index) {
    const uint8_t *entry = table + static_cast<size_t>(index) * entry_bytes;
    if (!EntryUsed(entry)) {
      continue;
    }
    const uint64_t first = Le64(entry + 32);
    const uint64_t last = Le64(entry + 40);
    if (last < first) {
      return Status::OutOfBounds;
    }
    if (last > kMaxCurrentTransportLba) {
      return Status::Overflow;
    }
    for (uint32_t prior = 0; prior < index; ++prior) {
      const uint8_t *other =
          table + static_cast<size_t>(prior) * entry_bytes;
      if (!EntryUsed(other)) {
        continue;
      }
      const uint64_t other_first = Le64(other + 32);
      const uint64_t other_last = Le64(other + 40);
      if (RangeOverlaps(first, last, other_first, other_last)) {
        return Status::PartitionOverlap;
      }
    }
    if (!IsUserdataName(entry)) {
      continue;
    }
    userdata_seen = true;
    if (found) {
      return Status::DuplicateUserdata;
    }
    found = true;
    selected_first = first;
    selected_last = last;
  }
  if (!found) {
    return userdata_seen ? Status::OutOfBounds : Status::NoUserdata;
  }
  if (selected_first < first_usable || selected_last > last_usable) {
    return Status::OutOfBounds;
  }
  if (selected_last - selected_first == UINT64_MAX) {
    return Status::Overflow;
  }
  const uint64_t blocks = selected_last - selected_first + 1;
  if (blocks > UINT64_MAX / kMediaBlockBytes) {
    return Status::Overflow;
  }
  geometry->block_bytes = kMediaBlockBytes;
  geometry->first_lba = selected_first;
  geometry->last_lba = selected_last;
  geometry->block_count = blocks;
  geometry->capacity_bytes = blocks * kMediaBlockBytes;
  return Status::Ok;
}

Status IsHfsxBlock(const void *block) {
  if (block == nullptr) {
    return Status::InvalidArgument;
  }
  const uint8_t *bytes = static_cast<const uint8_t *>(block);
  if (Be16(bytes + kHfsSignatureOffset) != kHfsxSignature) {
    return Status::WrongFilesystem;
  }
  return Status::Ok;
}

int ErrnoForStatus(Status status) {
  switch (status) {
    case Status::Ok:
      return kSuccess;
    case Status::InvalidArgument:
      return kInvalidArgument;
    case Status::NotReady:
      return kNotReady;
    case Status::AlreadyOpen:
      return kAlreadyOpen;
    case Status::Overflow:
      return kOverflow;
    case Status::NotWritable:
      return kReadOnly;
    case Status::WrongFilesystem:
      return kNoDevice;
    case Status::OutOfBounds:
    case Status::TransferTooLarge:
      return kNoSpace;
    default:
      return kIoError;
  }
}

const char *ReasonForStatus(Status status) {
  switch (status) {
    case Status::Ok:
      return "ok";
    case Status::InvalidArgument:
      return "invalid argument";
    case Status::NotReady:
      return "root transport not ready";
    case Status::AlreadyOpen:
      return "root already open";
    case Status::TransportError:
      return "root transport I/O error";
    case Status::BadGptHeader:
      return "invalid GPT header";
    case Status::BadGptGeometry:
      return "invalid GPT geometry";
    case Status::BadGptCrc:
      return "GPT checksum mismatch";
    case Status::DuplicateUserdata:
      return "duplicate userdata partition";
    case Status::NoUserdata:
      return "userdata partition not found";
    case Status::PartitionOverlap:
      return "GPT partition overlap";
    case Status::OutOfBounds:
      return "request outside userdata";
    case Status::TransferTooLarge:
      return "transfer exceeds 32 media blocks";
    case Status::Overflow:
      return "address arithmetic overflow";
    case Status::NotWritable:
      return "root is read-only";
    case Status::WrongFilesystem:
      return "userdata is not HFSX";
  }
  return "unknown root error";
}

void RootState::Reset() {
  media_ = {};
  geometry_ = {};
  opened_ = false;
  writable_ = false;
}

Status RootState::Open(const Media &media, bool writable, Geometry *geometry) {
  if (opened_) {
    return Status::AlreadyOpen;
  }
  if (media.read == nullptr || media.write == nullptr ||
      media.sync == nullptr || geometry == nullptr) {
    return Status::InvalidArgument;
  }
  *geometry = {};

  alignas(8) uint8_t header_block[kMediaBlockBytes] = {};
  Status status =
      media.read(kGptHeaderLba, 1, header_block, media.context);
  if (status != Status::Ok) {
    return status;
  }
  GptHeader header = {};
  status = ParseGptHeader(header_block, &header);
  if (status != Status::Ok) {
    return status;
  }
  const uint64_t entry_bytes64 =
      static_cast<uint64_t>(header.entries_count) * header.entry_bytes;
  if (entry_bytes64 > kMaxGptEntries * kGptEntryBytes ||
      header.entries_lba > UINT64_MAX - kMaxBlocks) {
    return Status::Overflow;
  }
  const uint64_t entry_blocks64 =
      (entry_bytes64 + kMediaBlockBytes - 1) / kMediaBlockBytes;
  if (entry_blocks64 == 0 || entry_blocks64 > kMaxBlocks ||
      header.entries_lba < header.current_lba + 1) {
    return Status::BadGptGeometry;
  }
  const uint32_t entry_blocks = static_cast<uint32_t>(entry_blocks64);
  alignas(8) uint8_t entries[kMaxBlocks * kMediaBlockBytes] = {};
  status = media.read(header.entries_lba, entry_blocks, entries,
                      media.context);
  if (status != Status::Ok) {
    return status;
  }
  if (header.last_usable_lba - header.entries_lba + 1 < entry_blocks64) {
    return Status::BadGptGeometry;
  }
  if (Crc32(entries, static_cast<size_t>(entry_bytes64), false) !=
      header.entries_crc) {
    return Status::BadGptCrc;
  }

  Geometry parsed = {};
  status = ParseUserdataPartition(entries, header.entry_bytes,
                                  header.entries_count,
                                  header.first_usable_lba,
                                  header.last_usable_lba, &parsed);
  if (status != Status::Ok) {
    return status;
  }
  alignas(8) uint8_t first_block[kMediaBlockBytes] = {};
  status = media.read(parsed.first_lba, 1, first_block, media.context);
  if (status != Status::Ok) {
    return status;
  }
  status = IsHfsxBlock(first_block);
  if (status != Status::Ok) {
    return status;
  }

  parsed.writable = writable ? 1U : 0U;
  media_ = media;
  geometry_ = parsed;
  *geometry = parsed;
  writable_ = writable;
  opened_ = true;
  return Status::Ok;
}

Status RootState::ValidateRange(uint64_t lba, uint32_t blocks) const {
  if (!opened_) {
    return Status::NotReady;
  }
  if (blocks == 0 || blocks > kMaxBlocks) {
    return blocks == 0 ? Status::OutOfBounds : Status::TransferTooLarge;
  }
  if (lba < geometry_.first_lba) {
    return Status::OutOfBounds;
  }
  const uint64_t offset = lba - geometry_.first_lba;
  if (offset >= geometry_.block_count ||
      blocks > geometry_.block_count - offset) {
    return Status::OutOfBounds;
  }
  if (lba > UINT64_MAX - (blocks - 1)) {
    return Status::Overflow;
  }
  return Status::Ok;
}

Status RootState::Read(uint64_t lba, uint32_t blocks, void *dst) {
  if (dst == nullptr) {
    return Status::InvalidArgument;
  }
  const Status status = ValidateRange(lba, blocks);
  if (status != Status::Ok) {
    return status;
  }
  return media_.read(lba, blocks, dst, media_.context);
}

Status RootState::Write(uint64_t lba, uint32_t blocks, const void *src) {
  if (src == nullptr) {
    return Status::InvalidArgument;
  }
  const Status status = ValidateRange(lba, blocks);
  if (status != Status::Ok) {
    return status;
  }
  if (!writable_) {
    return Status::NotWritable;
  }
  return media_.write(lba, blocks, src, media_.context);
}

Status RootState::Sync() {
  if (!opened_) {
    return Status::NotReady;
  }
  if (!writable_) {
    return Status::NotWritable;
  }
  return media_.sync(media_.context);
}

}  // namespace native_ufs_bsd_root
}  // namespace alioth
