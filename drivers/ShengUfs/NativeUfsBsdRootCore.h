// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
//
// Host-testable policy and state machine for the NativeUfs BSD root ABI.
//
// The transport is injected as a small set of C++ callbacks. This file has no
// IOKit, Mach, allocation or logging dependency. The production translation
// unit binds these callbacks to the existing NativeUfs command path; tests bind
// them to an in-memory GPT/HFSX fixture.

#include <stddef.h>
#include <stdint.h>

#include "NativeUfsBsdRootABI.h"

namespace alioth {
namespace native_ufs_bsd_root {

inline constexpr uint32_t kMediaBlockBytes = 4096;
inline constexpr uint32_t kMaxBlocks = 32;
inline constexpr uint32_t kGptHeaderLba = 1;
inline constexpr uint32_t kMaxGptEntries = 128;
inline constexpr uint32_t kGptEntryBytes = 128;
inline constexpr uint32_t kHfsSignatureOffset = 1024;
inline constexpr uint16_t kHfsxSignature = 0x4858;

// Fixed negative errno values are part of the C ABI. Host code may use any
// positive internal status; conversion happens once in this core.
inline constexpr int kSuccess = 0;
inline constexpr int kInvalidArgument = -22;   // EINVAL
inline constexpr int kNotReady = -6;           // ENXIO
inline constexpr int kAlreadyOpen = -16;       // EBUSY
inline constexpr int kReadOnly = -30;          // EROFS
inline constexpr int kNoDevice = -19;          // ENODEV
inline constexpr int kIoError = -5;            // EIO
inline constexpr int kNoSpace = -28;           // ENOSPC
inline constexpr int kOverflow = -75;          // EOVERFLOW

enum class Status : uint8_t {
  Ok = 0,
  InvalidArgument,
  NotReady,
  AlreadyOpen,
  TransportError,
  BadGptHeader,
  BadGptGeometry,
  BadGptCrc,
  DuplicateUserdata,
  NoUserdata,
  PartitionOverlap,
  OutOfBounds,
  TransferTooLarge,
  Overflow,
  NotWritable,
  WrongFilesystem,
};

struct Media {
  Status (*read)(uint64_t lba, uint32_t blocks, void *dst, void *context);
  Status (*write)(uint64_t lba, uint32_t blocks, const void *src,
                  void *context);
  Status (*sync)(void *context);
  void *context;
};

struct GptHeader {
  uint64_t current_lba;
  uint64_t backup_lba;
  uint64_t first_usable_lba;
  uint64_t last_usable_lba;
  uint64_t entries_lba;
  uint32_t entries_count;
  uint32_t entry_bytes;
  uint32_t header_crc;
  uint32_t entries_crc;
  uint32_t header_bytes;
};

struct Geometry {
  uint32_t block_bytes;
  uint32_t writable;
  uint64_t first_lba;
  uint64_t last_lba;
  uint64_t block_count;
  uint64_t capacity_bytes;
};

uint32_t Crc32(const void *data, size_t bytes, bool gpt_header);
Status ParseGptHeader(const void *block, GptHeader *header);
Status ParseUserdataPartition(const void *entries, size_t entry_bytes,
                              uint32_t entry_count, uint64_t first_usable,
                              uint64_t last_usable, Geometry *geometry);
Status IsHfsxBlock(const void *block);
int ErrnoForStatus(Status status);
const char *ReasonForStatus(Status status);

class RootState {
 public:
  RootState() = default;
  RootState(const RootState &) = delete;
  RootState &operator=(const RootState &) = delete;

  void Reset();

  Status Open(const Media &media, bool writable, Geometry *geometry);
  Status Read(uint64_t lba, uint32_t blocks, void *dst);
  Status Write(uint64_t lba, uint32_t blocks, const void *src);
  Status Sync();

  bool Opened() const { return opened_; }
  bool Writable() const { return writable_; }
  const Geometry &GetGeometry() const { return geometry_; }

 private:
  Status ValidateRange(uint64_t lba, uint32_t blocks) const;

  Media media_ = {};
  Geometry geometry_ = {};
  bool opened_ = false;
  bool writable_ = false;
};

}  // namespace native_ufs_bsd_root
}  // namespace alioth
