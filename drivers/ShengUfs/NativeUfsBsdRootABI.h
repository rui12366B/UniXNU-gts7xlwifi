// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
//
// Narrow C ABI between the Darwin BSD root frontend and NativeUfs.
//
// This header deliberately depends only on fixed-width integers. IOKit types,
// Mach types, allocation primitives and C++ namespaces stay behind the
// implementation boundary.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct native_ufs_bsd_geometry {
  uint64_t first_lba;
  uint64_t last_lba;
  uint64_t block_count;
  uint64_t capacity_bytes;
  uint32_t block_bytes;
  uint32_t writable;
  uint32_t reserved[2];
};

// Absolute 4K GPT media LBAs. A successful open selects exactly one unique
// "userdata" partition and checks its first block for an HFSX signature.
int native_ufs_bsd_root_open(uint32_t writable,
                             struct native_ufs_bsd_geometry *geometry,
                             const char **reason);

// lba and blocks use 4096-byte media units. blocks must be in [1, 32], and the
// complete range must remain inside the partition selected by open().
int native_ufs_bsd_root_read(uint64_t lba, uint32_t blocks, void *dst);
int native_ufs_bsd_root_write(uint64_t lba, uint32_t blocks, const void *src);
int native_ufs_bsd_root_sync(void);

#ifdef __cplusplus
}
#endif
