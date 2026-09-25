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

// rte_hash_rcu_qsbr_dq_reclaim is experimental in DPDK 25.11.
#define ALLOW_EXPERIMENTAL_API 1

#include "classifier/concurrent_exact.h"

#include <rte_hash_crc.h>


#include <atomic>

#include "dpdk.h"

namespace bess::classifier {

std::expected<std::unique_ptr<ConcurrentExactTable>, std::string>
ConcurrentExactTable::Create(uint32_t key_len, uint32_t capacity,
                             rcu::RcuDomain &domain, int socket) {
  if (!IsDpdkInitialized()) {
    InitDpdk();  // rte_hash lives in EAL memory
  }
  static std::atomic<uint64_t> seq{0};
  const std::string name = "cext_" + std::to_string(seq++);
  rte_hash_parameters params{};
  params.name = name.c_str();
  params.entries = std::max<uint32_t>(capacity, 8);
  params.key_len = key_len;
  params.hash_func = rte_hash_crc;
  params.hash_func_init_val = 0;
  params.socket_id = socket;
  params.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF;
  rte_hash *table = rte_hash_create(&params);
  if (table == nullptr) {
    return std::unexpected("rte_hash_create failed (capacity " +
                           std::to_string(capacity) + ")");
  }
  rte_hash_rcu_config rcu{};
  rcu.v = domain.dpdk_qsbr();
  rcu.mode = RTE_HASH_QSBR_MODE_DQ;
  if (rte_hash_rcu_qsbr_add(table, &rcu) != 0) {
    rte_hash_free(table);
    return std::unexpected("rte_hash_rcu_qsbr_add failed");
  }
  return std::unique_ptr<ConcurrentExactTable>(
      new ConcurrentExactTable(table, key_len, params.entries));
}

ConcurrentExactTable::~ConcurrentExactTable() {
  // Frees the defer queue too. Its entries must be past their grace period,
  // or the queue's memory leaks (rte_rcu_qsbr_dq_delete does not wait).
  // Owners guarantee that: a table is dropped only with the last generation
  // that maps it, itself retired through the same QSBR variable after every
  // delete on the table was enqueued.
  rte_hash_free(table_);
}

ConcurrentExactTable::UpsertResult ConcurrentExactTable::Upsert(
    ConstBytes key, uint64_t value) {
  promise(key.size() == key_len_);
  void *const data = reinterpret_cast<void *>(static_cast<uintptr_t>(value));
  // One hash for both steps. rte_hash_add_key_data returns 0 for insert
  // and update alike; the lookup tells them apart (size_ accounting).
  const hash_sig_t sig = rte_hash_hash(table_, key.data());
  const bool present = rte_hash_lookup_with_hash(table_, key.data(), sig) >= 0;
  int ret = rte_hash_add_key_with_hash_data(table_, key.data(), sig, data);
  if (ret != 0) {
    // Deleted slots return to the free list only after readers pass a grace
    // period; reclaim what is ready and retry once before reporting full.
    Reclaim();
    ret = rte_hash_add_key_with_hash_data(table_, key.data(), sig, data);
    if (ret != 0) {
      return UpsertResult::kFull;
    }
  }
  if (present) {
    return UpsertResult::kUpdated;
  }
  size_++;
  return UpsertResult::kInserted;
}

uint32_t ConcurrentExactTable::CapacityFor(size_t rules) noexcept {
  uint64_t pow2 = 1024;
  while (pow2 / 4 * 3 - Headroom(static_cast<uint32_t>(pow2 / 4 * 3)) <
         rules + 1) {
    pow2 *= 2;
  }
  return static_cast<uint32_t>(pow2 / 4 * 3);
}

bool ConcurrentExactTable::HasRoomForOne() {
  ReclaimAll();
  return slots_in_use() + 1 + Headroom(capacity_) <= capacity_;
}

void ConcurrentExactTable::ReclaimAll() {
  unsigned freed = 0, pending = 0, available = 0;
  do {
    rte_hash_rcu_qsbr_dq_reclaim(table_, &freed, &pending, &available);
  } while (freed > 0 && pending > 0);
}

void ConcurrentExactTable::Reclaim() {
  unsigned freed, pending, available;
  rte_hash_rcu_qsbr_dq_reclaim(table_, &freed, &pending, &available);
}

bool ConcurrentExactTable::Erase(ConstBytes key) {
  promise(key.size() == key_len_);
  if (rte_hash_del_key(table_, key.data()) < 0) {
    return false;
  }
  size_--;
  return true;
}

}  // namespace bess::classifier
