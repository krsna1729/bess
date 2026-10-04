// SPDX-License-Identifier: BSD-3-Clause

#include "flow/shared_exact_index.h"

#include <new>
#include <utility>

#include "classifier/concurrent_exact.h"

namespace bess::flow {

using classifier::ConcurrentExactTable;

std::unique_ptr<SharedExactIndex> SharedExactIndex::Create(uint32_t key_len, size_t keys,
                                                           rcu::RcuDomain &domain, int socket) {
  auto table = ConcurrentExactTable::Create(key_len, ConcurrentExactTable::CapacityFor(keys),
                                            domain, socket,
                                            ConcurrentExactTable::Writers::kSingle);
  if (!table.has_value()) {
    return nullptr;
  }
  return std::unique_ptr<SharedExactIndex>(
      new (std::nothrow) SharedExactIndex(std::move(*table), key_len));
}

SharedExactIndex::SharedExactIndex(std::unique_ptr<ConcurrentExactTable> table,
                                   uint32_t key_len) noexcept
    : table_(std::move(table)),
      table_dpdk_(table_->dpdk_table()),
      hash_batch_(table_->hash_batch()),
      key_len_(key_len) {}

SharedExactIndex::~SharedExactIndex() = default;

SharedExactIndex::InsertStatus SharedExactIndex::InsertIfAbsent(const std::byte *key,
                                                                uint64_t value) {
  switch (table_->InsertIfAbsent(classifier::ConstBytes(key, key_len_), value).status) {
    case ConcurrentExactTable::InsertResult::Status::kInserted:
      return InsertStatus::kInserted;
    case ConcurrentExactTable::InsertResult::Status::kExists:
      return InsertStatus::kExists;
    case ConcurrentExactTable::InsertResult::Status::kFull:
      break;
  }
  return InsertStatus::kFull;
}

bool SharedExactIndex::Erase(const std::byte *key) {
  return table_->Erase(classifier::ConstBytes(key, key_len_));
}

void SharedExactIndex::ReclaimAll() { table_->ReclaimAll(); }

size_t SharedExactIndex::size() const noexcept { return table_->size(); }

}  // namespace bess::flow
