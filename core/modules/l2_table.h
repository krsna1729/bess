// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_MODULES_L2_TABLE_H_
#define BESS_MODULES_L2_TABLE_H_

// L2Forward's MAC table, moved out of l2_forward.cc unchanged so tests and
// benchmarks can reach it (K4.6 follow-up).

#include <immintrin.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include <glog/logging.h>
#include <rte_hash_crc.h>

#include "../gate.h"

using bess::gate_idx_t;

// One 8-byte slot. A bucket is `bucket` consecutive slots, and the 4-way
// lookup reads a bucket as four consecutive uint64_t -- so a slot must be
// exactly 8 bytes. (2017's `alignas(32)` here made each slot 32 bytes, which
// left slots 1-3 of every 4-way bucket unreachable by that lookup: 75% of
// entries in a full table could not be found and could be added again.) The
// AVX load's 32-byte alignment comes from the table allocation instead.
struct l2_entry {
  union {
    struct {
      uint64_t addr : 48;
      uint64_t gate : 15;
      uint64_t occupied : 1;
    };
    uint64_t entry;
  };
};

static_assert(sizeof(l2_entry) == sizeof(uint64_t), "a slot is one uint64_t");

struct l2_table {
  struct l2_entry *table;
  uint64_t size;
  uint64_t size_power;
  uint64_t bucket;
  uint64_t count;
};

#define MAX_TABLE_SIZE (1048576 * 64)
#define DEFAULT_TABLE_SIZE 1024
#define MAX_BUCKET_SIZE 4

typedef uint64_t mac_addr_t;

inline int is_power_of_2(uint64_t n) {
  return (n != 0 && ((n & (n - 1)) == 0));
}

/*
 * l2_init:
 *  Initilizes the l2_table.
 *  It creates the slots of MAX_TABLE_SIZE multiplied by MAX_BUCKET_SIZE.
 *
 * @l2tbl: pointer to
 * @size: number of hash value entries. must be power of 2, greater than 0, and
 *        less than equal to MAX_TABLE_SIZE (2^30)
 * @bucket: number of slots per hash value. must be power of 2, greater than 0,
 *        and less than equal to MAX_BUCKET_SIZE (4)
 */
inline int l2_init(struct l2_table *l2tbl, int size, int bucket) {
  if (size <= 0 || size > MAX_TABLE_SIZE || !is_power_of_2(size)) {
    return -EINVAL;
  }

  if (bucket <= 0 || bucket > MAX_BUCKET_SIZE || !is_power_of_2(bucket)) {
    return -EINVAL;
  }

  if (l2tbl == nullptr) {
    return -EINVAL;
  }

  // 64-byte aligned, so every 4-slot (32-byte) bucket is 32-byte aligned for
  // the AVX load and never straddles a cache line.
  const size_t bytes = sizeof(l2_entry) * static_cast<size_t>(size) * bucket;
  l2tbl->table = static_cast<l2_entry *>(std::aligned_alloc(64, bytes));
  if (l2tbl->table == nullptr) {
    return -ENOMEM;
  }
  memset(l2tbl->table, 0, bytes);

  l2tbl->size = size;
  l2tbl->bucket = bucket;

  /* calculates the log_2 (size) */
  l2tbl->size_power = 0;
  while (size > 1) {
    size = size >> 1;
    l2tbl->size_power += 1;
  }

  return 0;
}

inline int l2_deinit(struct l2_table *l2tbl) {
  if (l2tbl == nullptr || l2tbl->table == nullptr || l2tbl->size == 0 ||
      l2tbl->bucket == 0) {
    return -EINVAL;
  }

  std::free(l2tbl->table);
  *l2tbl = {};
  return 0;
}

inline uint32_t l2_ib_to_offset(struct l2_table *l2tbl, int index, int bucket) {
  return index * l2tbl->bucket + bucket;
}

inline uint32_t l2_hash(mac_addr_t addr) {
  return rte_hash_crc_8byte(addr, 0);
}

inline uint32_t l2_hash_to_index(uint32_t hash, uint32_t size) {
  return hash & (size - 1);
}

inline uint32_t l2_alt_index(uint32_t hash, uint32_t size_power,
                             uint32_t index) {
  uint64_t tag = (hash >> size_power) + 1;
  tag = tag * 0x5bd1e995;
  return (index ^ tag) & ((0x1lu << (size_power - 1)) - 1);
}

#if __AVX__
inline const union {
  uint64_t val[4];
  __m256d _mask;
} _mask = {.val = {0x8000ffffFFFFffffull, 0x8000ffffFFFFffffull,
                   0x8000ffffFFFFffffull, 0x8000ffffFFFFffffull}};

// Do not call these functions directly. Use find_index() instead. See below.
inline int find_index_avx(uint64_t addr, uint64_t *table) {
  DCHECK(reinterpret_cast<uintptr_t>(table) % 32 == 0);
  __m256d _addr = (__m256d)_mm256_set1_epi64x(addr | (1ull << 63));
  __m256d _table = _mm256_load_pd((double *)table);
  _table = _mm256_and_pd(_table, _mask._mask);
  __m256d cmp = _mm256_cmp_pd(_addr, _table, _CMP_EQ_OQ);

  return __builtin_ffs(_mm256_movemask_pd(cmp));
}
#else
inline int find_index_basic(uint64_t addr, uint64_t *table) {
  for (int i = 0; i < 4; i++) {
    if ((addr | (1ull << 63)) == (table[i] & 0x8000ffffFFFFffffull)) {
      return i + 1;
    }
  }

  return 0;
}
#endif

// Finds addr from a 4-way bucket *table and returns its index + 1.
// Returns zero if not found.
inline int find_index(uint64_t addr, uint64_t *table, const uint64_t) {
#if __AVX__
  return find_index_avx(addr, table);
#else
  return find_index_basic(addr, table);
#endif
}

// Prefetch hint for a later l2_find() of `addr`: the primary bucket's line.
inline void l2_prefetch(const struct l2_table *l2tbl, uint64_t addr) {
  const uint32_t idx = l2_hash_to_index(l2_hash(addr), l2tbl->size);
  __builtin_prefetch(&l2tbl->table[idx * l2tbl->bucket], 0, 3);
}

// Bytes of slot storage: what lookups spread over.
inline size_t l2_table_bytes(const struct l2_table *l2tbl) {
  return sizeof(l2_entry) * l2tbl->size * l2tbl->bucket;
}

inline int l2_find(struct l2_table *l2tbl, uint64_t addr,
                          gate_idx_t *gate) {
  size_t i;
  int ret = -ENOENT;
  uint32_t hash, idx1, offset;
  struct l2_entry *tbl = l2tbl->table;

  hash = l2_hash(addr);
  idx1 = l2_hash_to_index(hash, l2tbl->size);

  offset = l2_ib_to_offset(l2tbl, idx1, 0);

  if (l2tbl->bucket == 4) {
    int tmp1 = find_index(addr, &tbl[offset].entry, l2tbl->count);
    if (tmp1) {
      *gate = tbl[offset + tmp1 - 1].gate;
      return 0;
    }

    idx1 = l2_alt_index(hash, l2tbl->size_power, idx1);
    offset = l2_ib_to_offset(l2tbl, idx1, 0);

    int tmp2 = find_index(addr, &tbl[offset].entry, l2tbl->count);

    if (tmp2) {
      *gate = tbl[offset + tmp2 - 1].gate;
      return 0;
    }

  } else {
    /* search buckets for first index */
    for (i = 0; i < l2tbl->bucket; i++) {
      if (tbl[offset].occupied && addr == tbl[offset].addr) {
        *gate = tbl[offset].gate;
        return 0;
      }

      offset++;
    }

    idx1 = l2_alt_index(hash, l2tbl->size_power, idx1);
    offset = l2_ib_to_offset(l2tbl, idx1, 0);
    /* search buckets for alternate index */
    for (i = 0; i < l2tbl->bucket; i++) {
      if (tbl[offset].occupied && addr == tbl[offset].addr) {
        *gate = tbl[offset].gate;
        return 0;
      }

      offset++;
    }
  }

  return ret;
}

inline int l2_find_offset(struct l2_table *l2tbl, uint64_t addr,
                          uint32_t *offset_out) {
  size_t i;
  uint32_t hash, idx1, offset;
  struct l2_entry *tbl = l2tbl->table;

  hash = l2_hash(addr);
  idx1 = l2_hash_to_index(hash, l2tbl->size);

  offset = l2_ib_to_offset(l2tbl, idx1, 0);
  /* search buckets for first index */
  for (i = 0; i < l2tbl->bucket; i++) {
    if (tbl[offset].occupied && addr == tbl[offset].addr) {
      *offset_out = offset;
      return 0;
    }

    offset++;
  }

  idx1 = l2_alt_index(hash, l2tbl->size_power, idx1);
  offset = l2_ib_to_offset(l2tbl, idx1, 0);
  /* search buckets for alternate index */
  for (i = 0; i < l2tbl->bucket; i++) {
    if (tbl[offset].occupied && addr == tbl[offset].addr) {
      *offset_out = offset;
      return 0;
    }

    offset++;
  }

  return -ENOENT;
}

inline int l2_find_slot(struct l2_table *l2tbl, mac_addr_t addr, uint32_t *idx,
                        uint32_t *bucket) {
  size_t i, j;
  uint32_t hash;
  uint32_t idx1, idx_v1, idx_v2;
  uint32_t offset1, offset2;
  struct l2_entry *tbl = l2tbl->table;

  hash = l2_hash(addr);
  idx1 = l2_hash_to_index(hash, l2tbl->size);

  /* if there is available slot */
  for (i = 0; i < l2tbl->bucket; i++) {
    offset1 = l2_ib_to_offset(l2tbl, idx1, i);
    if (!tbl[offset1].occupied) {
      *idx = idx1;
      *bucket = i;
      return 0;
    }
  }

  offset1 = l2_ib_to_offset(l2tbl, idx1, 0);

  /* try moving */
  for (i = 0; i < l2tbl->bucket; i++) {
    offset1 = l2_ib_to_offset(l2tbl, idx1, i);
    hash = l2_hash(tbl[offset1].addr);
    idx_v1 = l2_hash_to_index(hash, l2tbl->size);
    idx_v2 = l2_alt_index(hash, l2tbl->size_power, idx_v1);

    /* if the alternate bucket is same as original skip it */
    if (idx_v1 == idx_v2 || idx1 == idx_v2)
      break;

    for (j = 0; j < l2tbl->bucket; j++) {
      offset2 = l2_ib_to_offset(l2tbl, idx_v2, j);
      if (!tbl[offset2].occupied) {
        /* move offset1 to offset2 */
        tbl[offset2] = tbl[offset1];
        /* clear offset1 */
        tbl[offset1].occupied = 0;

        // The slot just vacated is slot i, not slot 0 (which may be
        // occupied: returning 0 overwrote it).
        *idx = idx1;
        *bucket = static_cast<uint32_t>(i);
        return 0;
      }
    }
  }

  /* TODO:if alternate index is also full then start move */
  return -ENOMEM;
}

inline int l2_add_entry(struct l2_table *l2tbl, mac_addr_t addr,
                        gate_idx_t gate) {
  uint32_t offset;
  uint32_t index;
  uint32_t bucket;
  gate_idx_t gate_idx_tmp;

  /* if addr already exist then fail */
  if (l2_find(l2tbl, addr, &gate_idx_tmp) == 0) {
    return -EEXIST;
  }

  /* find slots to put entry */
  if (l2_find_slot(l2tbl, addr, &index, &bucket) != 0) {
    return -ENOMEM;
  }

  /* insert entry into empty slot */
  offset = l2_ib_to_offset(l2tbl, index, bucket);

  l2tbl->table[offset].addr = addr;
  l2tbl->table[offset].gate = gate;
  l2tbl->table[offset].occupied = 1;
  l2tbl->count++;
  return 0;
}

inline int l2_del_entry(struct l2_table *l2tbl, uint64_t addr) {
  uint32_t offset = 0xFFFFFFFF;

  if (l2_find_offset(l2tbl, addr, &offset)) {
    return -ENOENT;
  }

  l2tbl->table[offset].addr = 0;
  l2tbl->table[offset].gate = 0;
  l2tbl->table[offset].occupied = 0;
  l2tbl->count--;
  return 0;
}

inline int l2_flush(struct l2_table *l2tbl) {
  if (nullptr == l2tbl || nullptr == l2tbl->table) {
    return -EINVAL;
  }

  memset(l2tbl->table, 0,
         sizeof(struct l2_entry) * l2tbl->size * l2tbl->bucket);

  return 0;
}

inline uint64_t l2_addr_to_u64(char *addr) {
  uint64_t a = *(reinterpret_cast<uint32_t *>(addr));
  uint64_t b = *(reinterpret_cast<uint16_t *>(addr + 4));

  return a | (b << 32);
}


#endif  // BESS_MODULES_L2_TABLE_H_
