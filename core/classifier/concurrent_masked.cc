// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/concurrent_masked.h"

#include <algorithm>
#include <climits>
#include <cstring>

#include "utils/logging.h"

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
  if (result == kPendingResult) {
    return UpsertResult::kReservedResult;
  }
  for (size_t b = 0; b < key_len_; b++) {
    if ((value[b] & ~mask[b]) != std::byte{0}) {
      return UpsertResult::kNotCanonical;
    }
  }
  DropEmptyTuples();  // a transaction's erase or cancel may have left one

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

size_t ConcurrentMaskedTable::TupleIndex(const TupleList &list,
                                         ConstBytes mask) const {
  for (size_t t = 0; t < list.tuples.size(); t++) {
    if (std::memcmp(list.tuples[t].mask.data(), mask.data(), key_len_) == 0) {
      return t;
    }
  }
  return list.tuples.size();
}

void ConcurrentMaskedTable::DropEmptyTuples() {
  const TupleList *list = tuples_.Read();
  if (std::none_of(list->tuples.begin(), list->tuples.end(),
                   [](const Tuple &t) { return t.table->size() == 0; })) {
    return;
  }
  auto next = std::make_unique<TupleList>();
  for (const Tuple &tuple : list->tuples) {
    if (tuple.table->size() != 0) {
      next->tuples.push_back(tuple);
    }
  }
  PublishTuples(std::move(next));
}

std::optional<ConcurrentMaskedTable::Rule> ConcurrentMaskedTable::FindRule(
    ConstBytes mask, ConstBytes value) const {
  const TupleList *list = tuples_.Read();
  const size_t t = TupleIndex(*list, mask);
  if (t == list->tuples.size()) {
    return std::nullopt;
  }
  const RuleId id = Find(*list->tuples[t].table, value);
  if (id == 0 || id == pending_id_) {
    return std::nullopt;
  }
  return Record(id);
}

std::expected<ConcurrentMaskedTable::Prepared,
              ConcurrentMaskedTable::UpsertResult>
ConcurrentMaskedTable::PrepareUpsert(ConstBytes mask, ConstBytes value,
                                     int64_t priority, uint16_t result) {
  promise(mask.size() == key_len_ && value.size() == key_len_);
  if (result == kPendingResult) {
    return std::unexpected(UpsertResult::kReservedResult);
  }
  for (size_t b = 0; b < key_len_; b++) {
    if ((value[b] & ~mask[b]) != std::byte{0}) {
      return std::unexpected(UpsertResult::kNotCanonical);
    }
  }
  DropEmptyTuples();
  // Everything that allocates without a visible effect first.
  Prepared prepared{std::vector<std::byte>(mask.begin(), mask.end()),
                    std::vector<std::byte>(value.begin(), value.end())};
  free_ids_.reserve(free_ids_.size() + prepared_ + 2);
  if (pending_id_ == 0) {
    pending_id_ = AllocateId();
    if (pending_id_ == 0) {
      return std::unexpected(UpsertResult::kFull);
    }
    // Loses to every rule: the lowest priority, and a sequence below every
    // rule's (ties go to the later command).
    MutableRecord(pending_id_) = {.priority = INT64_MIN,
                                  .sequence = 0,
                                  .result = kPendingResult};
  }

  const TupleList *list = tuples_.Read();
  const size_t index = TupleIndex(*list, mask);
  const bool new_tuple = index == list->tuples.size();
  if (new_tuple && list->tuples.size() >= max_tuples_) {
    return std::unexpected(UpsertResult::kTooManyTuples);
  }
  std::shared_ptr<ConcurrentExactTable> table =
      new_tuple ? NewTupleTable(1) : list->tuples[index].table;
  if (table == nullptr) {
    return std::unexpected(UpsertResult::kFull);
  }
  bool replaced = false;
  prepared.old = Find(*table, value);
  if (prepared.old == 0 && !new_tuple && !table->HasRoomForOne()) {
    table = Grown(*table);
    if (table == nullptr) {
      return std::unexpected(UpsertResult::kFull);
    }
    replaced = true;
  }
  const RuleId id = AllocateId();
  if (id == 0) {
    return std::unexpected(UpsertResult::kFull);
  }
  // Written now, named by the table only at Commit() (its release store).
  MutableRecord(id) = {.priority = priority,
                       .sequence = next_sequence_++,
                       .result = result};
  if (prepared.old == 0) {
    // A new rule: present but pending, so capacity is settled here.
    const uint64_t pending = Pack(pending_id_, kPendingResult);
    if (table->Upsert(value, pending) ==
        ConcurrentExactTable::UpsertResult::kFull) {
      table = Grown(*table);
      if (table == nullptr ||
          table->Upsert(value, pending) ==
              ConcurrentExactTable::UpsertResult::kFull) {
        free_ids_.push_back(id);  // never named: reusable at once
        return std::unexpected(UpsertResult::kFull);
      }
      replaced = true;
    }
  }
  if (new_tuple || replaced) {
    auto next = std::make_unique<TupleList>(*list);
    if (new_tuple) {
      next->tuples.push_back({prepared.mask, table});
    } else {
      next->tuples[index].table = table;
    }
    PublishTuples(std::move(next));
  }
  prepared.id = id;
  prepared.result = result;
  prepared_++;
  return prepared;
}

std::optional<ConcurrentMaskedTable::Prepared>
ConcurrentMaskedTable::PrepareErase(ConstBytes mask, ConstBytes value) {
  promise(mask.size() == key_len_ && value.size() == key_len_);
  const TupleList *list = tuples_.Read();
  const size_t index = TupleIndex(*list, mask);
  if (index == list->tuples.size()) {
    return std::nullopt;
  }
  const RuleId old = Find(*list->tuples[index].table, value);
  if (old == 0 || old == pending_id_) {
    return std::nullopt;
  }
  Prepared prepared{std::vector<std::byte>(mask.begin(), mask.end()),
                    std::vector<std::byte>(value.begin(), value.end())};
  prepared.old = old;
  prepared_++;
  return prepared;
}

ConcurrentMaskedTable::RuleId ConcurrentMaskedTable::Commit(
    const Prepared &prepared) noexcept {
  const TupleList *list = tuples_.Read();
  const size_t index = TupleIndex(*list, prepared.mask);
  // Prepare*() left the tuple and the entry in place; nothing else writes
  // between prepare and commit (one writer).
  CHECK_LT(index, list->tuples.size());
  ConcurrentExactTable &table = *list->tuples[index].table;
  const ConstBytes value(prepared.value.data(), prepared.value.size());
  if (prepared.id != 0) {
    // An in-place value store of a present key: cannot fail.
    CHECK(table.Upsert(value, Pack(prepared.id, prepared.result)) ==
          ConcurrentExactTable::UpsertResult::kUpdated);
    if (prepared.old == 0) {
      size_++;
    }
  } else {
    CHECK(table.Erase(value));
    size_--;
    // An emptied tuple stays until the next prepare or command drops it
    // (republishing the list would allocate here).
  }
  prepared_--;
  return prepared.old;
}

void ConcurrentMaskedTable::Cancel(const Prepared &prepared) noexcept {
  if (prepared.id != 0) {
    if (prepared.old == 0) {
      const TupleList *list = tuples_.Read();
      const size_t index = TupleIndex(*list, prepared.mask);
      CHECK_LT(index, list->tuples.size());
      list->tuples[index].table->Erase(
          ConstBytes(prepared.value.data(), prepared.value.size()));
    }
    free_ids_.push_back(prepared.id);  // never named; room reserved
  }
  prepared_--;
}

void ConcurrentMaskedTable::Recycle(RuleId id) {
  free_ids_.push_back(id);
  DropEmptyTuples();
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
