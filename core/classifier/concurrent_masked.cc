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


#include "classifier/concurrent_masked.h"

#include <algorithm>
#include <cstring>

namespace bess::classifier {

std::expected<std::unique_ptr<ConcurrentMaskedTable>, std::string>
ConcurrentMaskedTable::Create(uint32_t key_len, size_t max_tuples,
                              rcu::RcuDomain &domain) {
  if (key_len == 0 || key_len > detail::kMaskedMaxKeyBytes) {
    return std::unexpected("key length must be 1.." +
                           std::to_string(detail::kMaskedMaxKeyBytes));
  }
  if (max_tuples == 0) {
    return std::unexpected("max_tuples must be positive");
  }
  return std::unique_ptr<ConcurrentMaskedTable>(
      new ConcurrentMaskedTable(key_len, max_tuples, domain));
}

ConcurrentMaskedTable::ConcurrentMaskedTable(uint32_t key_len,
                                             size_t max_tuples,
                                             rcu::RcuDomain &domain)
    : key_len_(key_len),
      max_tuples_(max_tuples),
      domain_(domain),
      mask_batch_(detail::SelectMaskBatch(key_len)),
      tuples_(domain),
      chunks_(new std::atomic<Rule *>[kMaxChunks]) {
  for (uint32_t c = 0; c < kMaxChunks; c++) {
    chunks_[c].store(nullptr, std::memory_order_relaxed);
  }
  tuples_.Initialize(std::make_unique<TupleList>());
}

ConcurrentMaskedTable::~ConcurrentMaskedTable() {
  // The table is destroyed with the last generation that maps it, which is
  // itself retired past a grace period: no reader can hold the tuple list.
  tuples_.ResetQuiesced();
}

ConcurrentMaskedTable::Rule &ConcurrentMaskedTable::MutableRecord(RuleId id) {
  return chunk_storage_[id >> kChunkBits][id & ((1u << kChunkBits) - 1)];
}

ConcurrentMaskedTable::RuleId ConcurrentMaskedTable::AllocateId() {
  ReclaimIds();
  if (!free_ids_.empty()) {
    const RuleId id = free_ids_.back();
    free_ids_.pop_back();
    return id;
  }
  const RuleId id = next_id_;
  const uint32_t chunk = id >> kChunkBits;
  if (chunk >= kMaxChunks) {
    return 0;
  }
  if (chunk == chunk_storage_.size()) {
    chunk_storage_.push_back(std::make_unique<Rule[]>(1u << kChunkBits));
    // Published before any id in it is: readers reach a chunk only through
    // an id they loaded from a table, which the table add releases later.
    chunks_[chunk].store(chunk_storage_.back().get(),
                         std::memory_order_release);
  }
  next_id_++;
  return id;
}

void ConcurrentMaskedTable::RetireId(RuleId id) {
  retiring_.push_back({id, domain_.StartGracePeriod()});
}

void ConcurrentMaskedTable::ReclaimIds() {
  while (!retiring_.empty() && domain_.IsComplete(retiring_.front().token)) {
    free_ids_.push_back(retiring_.front().id);
    retiring_.pop_front();
  }
}

std::shared_ptr<ConcurrentExactTable> ConcurrentMaskedTable::NewTupleTable(
    size_t rules) const {
  auto table = ConcurrentExactTable::Create(
      key_len_, ConcurrentExactTable::CapacityFor(rules), domain_);
  return table ? std::shared_ptr<ConcurrentExactTable>(std::move(*table))
               : nullptr;
}

std::shared_ptr<ConcurrentExactTable> ConcurrentMaskedTable::Grown(
    const ConcurrentExactTable &from) const {
  // Same retry-at-twice-the-size rule as ExactMatch: a displacement failure
  // never drops a rule (D-010).
  size_t rules = from.capacity();
  for (int attempt = 0; attempt < 4; attempt++) {
    auto next = NewTupleTable(rules);
    if (next == nullptr) {
      return nullptr;
    }
    bool ok = true;
    from.ForEach([&](ConstBytes key, uint64_t packed) {
      ok = ok && next->Upsert(key, packed) !=
                     ConcurrentExactTable::UpsertResult::kFull;
    });
    if (ok) {
      return next;
    }
    rules = next->capacity();
  }
  return nullptr;
}

void ConcurrentMaskedTable::PublishTuples(std::unique_ptr<TupleList> list) {
  tuples_.Publish(std::move(list));
  domain_.ReclaimReady();
}

ConcurrentMaskedTable::RuleId ConcurrentMaskedTable::Find(
    const ConcurrentExactTable &table, ConstBytes value) {
  uint64_t packed = 0;
  return table.LookupBatch(value, value.size(), &packed, 1) ? IdOf(packed)
                                                            : 0;
}

ConcurrentMaskedTable::UpsertResult ConcurrentMaskedTable::Upsert(
    ConstBytes mask, ConstBytes value, int64_t priority, uint16_t result) {
  promise(mask.size() == key_len_ && value.size() == key_len_);
  for (size_t b = 0; b < key_len_; b++) {
    if ((value[b] & ~mask[b]) != std::byte{0}) {
      return UpsertResult::kNotCanonical;
    }
  }

  const TupleList *list = tuples_.Read();
  size_t index = list->tuples.size();
  for (size_t t = 0; t < list->tuples.size(); t++) {
    if (std::memcmp(list->tuples[t].mask.data(), mask.data(), key_len_) == 0) {
      index = t;
      break;
    }
  }
  const bool new_tuple = index == list->tuples.size();
  if (new_tuple && list->tuples.size() >= max_tuples_) {
    return UpsertResult::kTooManyTuples;
  }

  // The tuple's table, grown first if a delete burst or growth is due. A
  // grown or new table is published with the tuple list below.
  std::shared_ptr<ConcurrentExactTable> table =
      new_tuple ? NewTupleTable(1) : list->tuples[index].table;
  if (table == nullptr) {
    return UpsertResult::kFull;
  }
  bool replaced = false;
  if (!new_tuple && !table->HasRoomForOne()) {
    table = Grown(*table);
    if (table == nullptr) {
      return UpsertResult::kFull;
    }
    replaced = true;
  }

  const RuleId id = AllocateId();
  if (id == 0) {
    return UpsertResult::kFull;
  }
  // Written before the table add that publishes the id (its release store).
  MutableRecord(id) = {.priority = priority,
                       .sequence = next_sequence_++,
                       .result = result};
  const RuleId old = Find(*table, value);
  if (table->Upsert(value, Pack(id, result)) ==
      ConcurrentExactTable::UpsertResult::kFull) {
    table = Grown(*table);
    if (table == nullptr || table->Upsert(value, Pack(id, result)) ==
                                ConcurrentExactTable::UpsertResult::kFull) {
      free_ids_.push_back(id);  // never published: reusable at once
      return UpsertResult::kFull;
    }
    replaced = true;
  }

  if (new_tuple || replaced) {
    auto next = std::make_unique<TupleList>(*list);
    if (new_tuple) {
      next->tuples.push_back(
          {std::vector<std::byte>(mask.begin(), mask.end()), table});
    } else {
      next->tuples[index].table = table;
    }
    PublishTuples(std::move(next));
  }
  if (old != 0) {
    RetireId(old);
    return UpsertResult::kUpdated;
  }
  size_++;
  return UpsertResult::kInserted;
}

bool ConcurrentMaskedTable::Erase(ConstBytes mask, ConstBytes value) {
  promise(mask.size() == key_len_ && value.size() == key_len_);
  const TupleList *list = tuples_.Read();
  for (size_t t = 0; t < list->tuples.size(); t++) {
    const Tuple &tuple = list->tuples[t];
    if (std::memcmp(tuple.mask.data(), mask.data(), key_len_) != 0) {
      continue;
    }
    const RuleId id = Find(*tuple.table, value);
    if (id == 0 || !tuple.table->Erase(value)) {
      return false;
    }
    RetireId(id);
    size_--;
    if (tuple.table->size() == 0) {
      // The mask's last rule: the tuple goes, and with it a slot of the
      // tuple ceiling. Its table is freed with the old list.
      auto next = std::make_unique<TupleList>(*list);
      next->tuples.erase(next->tuples.begin() + static_cast<ptrdiff_t>(t));
      PublishTuples(std::move(next));
    }
    return true;
  }
  return false;
}

void ConcurrentMaskedTable::Clear() {
  std::vector<RuleId> ids;
  ids.reserve(size_);
  for (const Tuple &tuple : tuples_.Read()->tuples) {
    tuple.table->ForEach(
        [&](ConstBytes, uint64_t packed) { ids.push_back(IdOf(packed)); });
  }
  // Publish first, then start the grace periods that retire the ids: a
  // grace period started earlier could complete while a reader still walks
  // the old list and loads one of these ids.
  PublishTuples(std::make_unique<TupleList>());
  for (const RuleId id : ids) {
    RetireId(id);
  }
  size_ = 0;
}

}  // namespace bess::classifier
