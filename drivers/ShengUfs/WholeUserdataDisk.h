// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
//
// Injectable 512-byte-sector disk frontend for the whole userdata partition.
//
// The operations object supplies storage I/O and copying to/from the caller's
// buffer. This file contains no IOKit, MMIO, device or allocation dependency,
// so every range and read-modify-write decision can be tested on the host.

#include <stdint.h>

#include "WholeUserdataRoot.h"

namespace alioth {
namespace whole_userdata_root {

struct DiskResult {
  Status status;
  uint64_t bytes_done;
};

// Ops must provide:
//   Status readBlock(uint64_t media_lba, uint8_t *out);
//   Status writeBlock(uint64_t media_lba, const uint8_t *in, bool fua);
//   Status copyIn(uint64_t request_offset, uint8_t *out, uint32_t bytes);
//   Status copyOut(uint64_t request_offset, const uint8_t *in, uint32_t bytes);
//   Status synchronize();
template <class Ops>
class Disk {
 public:
  Disk() = default;
  Disk(const Disk &) = delete;
  Disk &operator=(const Disk &) = delete;

  // Configuration never touches the device. A disabled frontend can be
  // configured without operations, but any access fails closed.
  Status Configure(const Geometry &geometry, Ops *ops, uint8_t *bounce,
                   bool selected, bool writable) {
    Reset();
    const Status status = ValidateGeometry(geometry);
    if (status != Status::Ok) {
      return status;
    }
    if (selected && (ops == nullptr || bounce == nullptr)) {
      return Status::NullArgument;
    }
    if (writable && !selected) {
      return Status::InvalidAccess;
    }
    geometry_ = geometry;
    ops_ = ops;
    bounce_ = bounce;
    selected_ = selected;
    writable_ = writable;
    return Status::Ok;
  }

  void Reset() {
    geometry_ = {};
    ops_ = nullptr;
    bounce_ = nullptr;
    selected_ = false;
    writable_ = false;
  }

  bool Selected() const { return selected_; }
  bool Writable() const { return writable_; }
  const Geometry &GetGeometry() const { return geometry_; }

  DiskResult Read(uint64_t first_logical_sector, uint64_t sector_count) {
    return Transfer(AccessKind::Read, first_logical_sector, sector_count,
                    false);
  }

  DiskResult Write(uint64_t first_logical_sector, uint64_t sector_count,
                   bool fua) {
    return Transfer(fua ? AccessKind::FuaWrite : AccessKind::Write,
                    first_logical_sector, sector_count, fua);
  }

  // Synchronize Cache is accepted only for a writable selection and a
  // whole-media-block range wholly inside userdata.
  DiskResult Synchronize(uint64_t first_logical_sector,
                         uint64_t sector_count) {
    if (!selected_) {
      return {Status::NotReady, 0};
    }
    if (!writable_) {
      return {Status::ReadOnly, 0};
    }
    SectorRange accepted = {};
    const Status status =
        ValidateAccess(geometry_, AccessKind::SynchronizeCache,
                       {first_logical_sector, sector_count}, &accepted);
    if (status != Status::Ok) {
      return {status, 0};
    }
    return {ops_->synchronize(), 0};
  }

 private:
  DiskResult Transfer(AccessKind access, uint64_t first_logical_sector,
                      uint64_t sector_count, bool fua) {
    if (!selected_) {
      return {Status::NotReady, 0};
    }
    if ((access == AccessKind::Write || access == AccessKind::FuaWrite) &&
        !writable_) {
      return {Status::ReadOnly, 0};
    }

    ChunkCursor cursor = {};
    const Status begin =
        BeginChunks(geometry_, access, {first_logical_sector, sector_count},
                    &cursor);
    if (begin != Status::Ok) {
      return {begin, 0};
    }

    DiskResult result = {Status::Ok, 0};
    Chunk chunk = {};
    for (;;) {
      const Status next = NextChunk(geometry_, &cursor, &chunk);
      if (next == Status::End) {
        return result;
      }
      if (next != Status::Ok) {
        result.status = next;
        return result;
      }

      if (access == AccessKind::Read) {
        result.status = ops_->readBlock(chunk.media_lba, bounce_);
        if (result.status != Status::Ok) {
          return result;
        }
        result.status =
            ops_->copyOut(result.bytes_done, bounce_ + chunk.media_offset_bytes,
                          chunk.byte_count);
        if (result.status != Status::Ok) {
          return result;
        }
      } else {
        if (chunk.needs_read_modify_write) {
          result.status = ops_->readBlock(chunk.media_lba, bounce_);
          if (result.status != Status::Ok) {
            return result;
          }
        }
        result.status =
            ops_->copyIn(result.bytes_done, bounce_ + chunk.media_offset_bytes,
                         chunk.byte_count);
        if (result.status != Status::Ok) {
          return result;
        }
        result.status = ops_->writeBlock(chunk.media_lba, bounce_, fua);
        if (result.status != Status::Ok) {
          return result;
        }
      }
      result.bytes_done += chunk.byte_count;
    }
  }

  Geometry geometry_ = {};
  Ops *ops_ = nullptr;
  uint8_t *bounce_ = nullptr;
  bool selected_ = false;
  bool writable_ = false;
};

}  // namespace whole_userdata_root
}  // namespace alioth
