// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/concurrent_masked.h"

#include <limits>

#include <algorithm>
#include <climits>
#include <cstring>
#include <new>

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
      records_(sizeof(Rule), domain) {
  tuples_.Initialize(std::make_unique<TupleList>());
}

ConcurrentMaskedTable::~ConcurrentMaskedTable() {
  // The table is destroyed with the last generation that maps it, which is
  // itself retired past a grace period: no reader can hold the tuple list,
  // nor a chain one of its values names.
  for (const Tuple &tuple : tuples_.Read()->tuples) {
    tuple.table->ForEach([](ConstBytes, uint64_t packed) {
      if (IsChain(packed)) {
        Chain::Free{}(const_cast<Chain *>(&ChainOf(packed)));
      }
    });
  }
  tuples_.ResetQuiesced();
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
  // Highest order first: the lookup's skip then saves the most lookups.
  std::stable_sort(list->tuples.begin(), list->tuples.end(),
                   [](const Tuple &a, const Tuple &b) {
                     return a.ceiling->order > b.ceiling->order;
                   });
  tuples_.Publish(std::move(list));
  domain_.ReclaimReady();
}

void ConcurrentMaskedTable::RaiseOrder(const TupleList &list, size_t index,
                                       int64_t priority) {
  Ceiling &ceiling = *list.tuples[index].ceiling;
  if (priority <= ceiling.order) {
    return;
  }
  const int64_t before = ceiling.order;
  ceiling.order = priority;
  if (index > 0 && list.tuples[index - 1].ceiling->order < priority) {
    try {
      PublishTuples(std::make_unique<TupleList>(list));
    } catch (...) {
      ceiling.order = before;  // the published list stays sorted
      throw;
    }
  }
}

std::optional<uint64_t> ConcurrentMaskedTable::FindValue(
    const ConcurrentExactTable &table, ConstBytes value) {
  uint64_t packed = 0;
  if (table.LookupBatch(value, value.size(), &packed, 1) == 0) {
    return std::nullopt;
  }
  return packed;
}

ConcurrentMaskedTable::ChainPtr ConcurrentMaskedTable::Chain::New(
    uint32_t entries, uint32_t ranges) {
  const size_t bytes = sizeof(Chain) + size_t{entries} * sizeof(ChainEntry) +
                       size_t{ranges} * sizeof(KeyRange);
  auto *chain = new (::operator new(bytes)) Chain{0, 0, entries, ranges};
  // Its table value carries the 0xFFFF tag in bits 48-63 (ChainValue).
  CHECK_EQ(reinterpret_cast<uintptr_t>(chain) & kChainTag, uintptr_t{0});
  return ChainPtr(chain);
}

ConcurrentMaskedTable::RuleId ConcurrentMaskedTable::IdentityIn(
    uint64_t packed, Ranges ranges) const noexcept {
  if (IsChain(packed)) {
    const Chain &chain = ChainOf(packed);
    for (uint32_t e = 0; e < chain.count; e++) {
      const ChainEntry &entry = chain.entries()[e];
      if (std::ranges::equal(chain.RangesOf(entry), ranges)) {
        return IdOf(entry.packed);
      }
    }
    return 0;
  }
  return RulesIn(packed) == 1 && ranges.empty() ? IdOf(packed) : 0;
}

uint32_t ConcurrentMaskedTable::RulesIn(uint64_t packed) const noexcept {
  if (IsChain(packed)) {
    return ChainOf(packed).count;
  }
  return packed == 0 || IdOf(packed) == pending_id_ ? 0 : 1;
}

uint32_t ConcurrentMaskedTable::RangesIn(uint64_t packed) noexcept {
  return IsChain(packed) ? ChainOf(packed).range_count : 0;
}

bool ConcurrentMaskedTable::CanonicalRanges(ConstBytes mask,
                                            Ranges ranges) const noexcept {
  uint32_t next = 0;  // the first byte the next range may start at
  for (const KeyRange &r : ranges) {
    if (r.width == 0 || r.width > 8 || r.offset < next ||
        r.offset > key_len_ || r.width > key_len_ - r.offset) {
      return false;
    }
    const uint64_t max =
        r.width == 8 ? ~uint64_t{0} : (uint64_t{1} << (8 * r.width)) - 1;
    if (r.low > r.high || r.high > max || (r.low == 0 && r.high == max)) {
      return false;
    }
    for (uint32_t b = r.offset; b < r.offset + r.width; b++) {
      if (mask[b] != std::byte{0}) {
        return false;
      }
    }
    next = r.offset + r.width;
  }
  return true;
}

ConcurrentMaskedTable::KeyStates::iterator ConcurrentMaskedTable::PrepareKey(
    ConstBytes mask, ConstBytes value, uint64_t live, size_t new_ranges,
    bool upsert) {
  std::vector<std::byte> bytes(mask.begin(), mask.end());
  bytes.insert(bytes.end(), value.begin(), value.end());
  const auto key = pending_keys_.try_emplace(std::move(bytes)).first;
  try {
    KeyState &state = key->second;
    // Every outstanding change may retire one chain when it commits.
    retired_chains_.reserve(retired_chains_.size() + prepared_changes_ + 1);
    if (IsChain(live) || state.ranged_upserts > 0 ||
        (upsert && new_ranges > 0)) {
      // The key can hold a chain, and any of its changes may be the one
      // that writes it: each gets a spare large enough for every rule and
      // range the key can have once all of them commit, in any order.
      const auto entries = static_cast<uint32_t>(RulesIn(live) + state.upserts +
                                                 (upsert ? 1 : 0));
      const auto ranges = static_cast<uint32_t>(
          RangesIn(live) + state.upsert_ranges + (upsert ? new_ranges : 0));
      for (ChainPtr &spare : state.spares) {
        if (spare->entry_capacity < entries || spare->range_capacity < ranges) {
          spare = Chain::New(entries, ranges);
        }
      }
      while (state.spares.size() < state.changes + 1) {
        state.spares.push_back(Chain::New(entries, ranges));
      }
    }
  } catch (...) {
    Untrack(key);
    throw;
  }
  return key;
}

void ConcurrentMaskedTable::Track(KeyStates::iterator key, size_t new_ranges,
                                  bool upsert) noexcept {
  KeyState &state = key->second;
  state.changes++;
  prepared_changes_++;
  if (upsert) {
    state.upserts++;
    state.upsert_ranges += new_ranges;
    state.ranged_upserts += new_ranges > 0 ? 1 : 0;
  }
}

void ConcurrentMaskedTable::Untrack(KeyStates::iterator key) noexcept {
  if (key->second.changes == 0) {
    pending_keys_.erase(key);
  }
}

void ConcurrentMaskedTable::Settle(const Prepared &prepared) noexcept {
  KeyState &state = prepared.key->second;
  state.changes--;
  prepared_changes_--;
  if (prepared.id != 0) {
    state.upserts--;
    state.upsert_ranges -= prepared.ranges.size();
    state.ranged_upserts -= prepared.ranges.empty() ? 0 : 1;
  }
  while (state.spares.size() > state.changes) {
    state.spares.pop_back();
  }
  Untrack(prepared.key);
}

void ConcurrentMaskedTable::BuildChain(uint64_t live, RuleId drop, uint64_t add,
                                       Ranges add_ranges,
                                       Chain &out) const noexcept {
  uint32_t count = 0;
  out.range_count = 0;
  const auto emit = [&](uint64_t packed, Ranges ranges) {
    CHECK_LT(count, out.entry_capacity);
    CHECK_LE(out.range_count + ranges.size(), out.range_capacity);
    out.entries()[count++] = {packed, out.range_count,
                              static_cast<uint32_t>(ranges.size())};
    std::ranges::copy(ranges, out.ranges() + out.range_count);
    out.range_count += static_cast<uint32_t>(ranges.size());
  };
  bool added = add == 0;
  const auto visit = [&](uint64_t packed, Ranges ranges) {
    if (IdOf(packed) == drop) {
      return;
    }
    if (!added && Better(Record(IdOf(add)), Record(IdOf(packed)))) {
      emit(add, add_ranges);
      added = true;
    }
    emit(packed, ranges);
  };
  if (IsChain(live)) {
    const Chain &chain = ChainOf(live);
    for (uint32_t e = 0; e < chain.count; e++) {
      visit(chain.entries()[e].packed, chain.RangesOf(chain.entries()[e]));
    }
  } else if (RulesIn(live) == 1) {
    visit(live, {});
  }
  if (!added) {
    emit(add, add_ranges);
  }
  // Last, with release: a reader loads the count with acquire before it
  // reads anything else of the chain (Resolve). The table store that
  // publishes the chain orders this too, but inside DPDK, where a race
  // detector cannot see it.
  __atomic_store_n(&out.count, count, __ATOMIC_RELEASE);
}

void ConcurrentMaskedTable::RetireChain(const Chain *chain) noexcept {
  retired_chains_.push_back(
      {ChainPtr(const_cast<Chain *>(chain)),
       domain_.StartGracePeriod()});
}

void ConcurrentMaskedTable::ReclaimChains() noexcept {
  size_t done = 0;
  while (done < retired_chains_.size() &&
         domain_.IsComplete(retired_chains_[done].token)) {
    done++;
  }
  retired_chains_.erase(retired_chains_.begin(),
                        retired_chains_.begin() + static_cast<ptrdiff_t>(done));
}

ConcurrentMaskedTable::UpsertResult ConcurrentMaskedTable::UpsertViaCommit(
    ConstBytes mask, ConstBytes value, int64_t priority, uint16_t gate,
    uint32_t result_id, Ranges ranges) {
  auto prepared =
      PrepareUpsert(mask, value, priority, gate, result_id, ranges);
  if (!prepared) {
    return prepared.error();
  }
  const RuleId old = Commit(*prepared);
  if (old == 0) {
    return UpsertResult::kInserted;
  }
  records_.Retire(old);
  return UpsertResult::kUpdated;
}

ConcurrentMaskedTable::UpsertResult ConcurrentMaskedTable::Upsert(
    ConstBytes mask, ConstBytes value, int64_t priority, uint16_t gate,
    uint32_t result_id, Ranges ranges) {
  promise(mask.size() == key_len_ && value.size() == key_len_);
  if (gate == kPendingResult) {
    return UpsertResult::kReservedResult;
  }
  for (size_t b = 0; b < key_len_; b++) {
    if ((value[b] & ~mask[b]) != std::byte{0}) {
      return UpsertResult::kNotCanonical;
    }
  }
  if (!CanonicalRanges(mask, ranges)) {
    return UpsertResult::kBadRange;
  }
  if (!ranges.empty()) {
    return UpsertViaCommit(mask, value, priority, gate, result_id, ranges);
  }
  DropEmptyTuples();  // a transaction's erase or cancel may have left one
  ReclaimChains();

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
  // The key's live value, looked up once: a rule it holds is replaced below.
  std::optional<uint64_t> live;
  if (!new_tuple) {
    live = FindValue(*list->tuples[index].table, value);
    // A key that holds a chain keeps one: the rule joins it.
    if (live && IsChain(*live)) {
      return UpsertViaCommit(mask, value, priority, gate, result_id, ranges);
    }
    // Before any live change: it may republish, which allocates.
    RaiseOrder(*list, index, priority);
    list = tuples_.Read();
    index = TupleIndex(*list, mask);
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

  const RuleId id = records_.Allocate();
  if (id == 0) {
    return UpsertResult::kFull;
  }
  // Written before the table add that publishes the id (its release store).
  MutableRecord(id) = {.priority = priority,
                       .sequence = next_sequence_++,
                       .gate = gate,
                       .result_id = result_id};
  const RuleId old = live ? IdOf(*live) : 0;  // a grown table holds the same
  // The tuple's ceiling first: a reader that sees the rule sees it raised.
  auto ceiling = new_tuple ? std::make_shared<Ceiling>(priority)
                           : list->tuples[index].ceiling;
  RaiseCeiling(ceiling->value, priority);
  if (table->Upsert(value, Pack(id, gate)) ==
      ConcurrentExactTable::UpsertResult::kFull) {
    table = Grown(*table);
    if (table == nullptr || table->Upsert(value, Pack(id, gate)) ==
                                ConcurrentExactTable::UpsertResult::kFull) {
      records_.Abort(id);  // never published: reusable at once
      return UpsertResult::kFull;
    }
    replaced = true;
  }

  if (new_tuple || replaced) {
    auto next = std::make_unique<TupleList>(*list);
    if (new_tuple) {
      next->tuples.push_back(
          {std::vector<std::byte>(mask.begin(), mask.end()), table, ceiling});
    } else {
      next->tuples[index].table = table;
    }
    PublishTuples(std::move(next));
  }
  if (old != 0) {
    records_.Retire(old);
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
    ConstBytes mask, ConstBytes value, Ranges ranges) const {
  const TupleList *list = tuples_.Read();
  const size_t t = TupleIndex(*list, mask);
  if (t == list->tuples.size()) {
    return std::nullopt;
  }
  const auto live = FindValue(*list->tuples[t].table, value);
  const RuleId id = live ? IdentityIn(*live, ranges) : 0;
  if (id == 0) {
    return std::nullopt;
  }
  return Record(id);
}

std::expected<ConcurrentMaskedTable::Prepared,
              ConcurrentMaskedTable::UpsertResult>
ConcurrentMaskedTable::PrepareUpsert(ConstBytes mask, ConstBytes value,
                                     int64_t priority, uint16_t gate,
                                     uint32_t result_id, Ranges ranges) {
  promise(mask.size() == key_len_ && value.size() == key_len_);
  if (gate == kPendingResult) {
    return std::unexpected(UpsertResult::kReservedResult);
  }
  for (size_t b = 0; b < key_len_; b++) {
    if ((value[b] & ~mask[b]) != std::byte{0}) {
      return std::unexpected(UpsertResult::kNotCanonical);
    }
  }
  if (!CanonicalRanges(mask, ranges)) {
    return std::unexpected(UpsertResult::kBadRange);
  }
  DropEmptyTuples();
  ReclaimChains();
  // Everything that allocates without a visible effect first.
  Prepared prepared{std::vector<std::byte>(mask.begin(), mask.end()),
                    std::vector<std::byte>(value.begin(), value.end()),
                    std::vector<KeyRange>(ranges.begin(), ranges.end())};
  if (pending_id_ == 0) {
    pending_id_ = records_.Allocate();
    if (pending_id_ == 0) {
      return std::unexpected(UpsertResult::kFull);
    }
    // Loses to every rule: the lowest priority, and a sequence below every
    // rule's (ties go to the later command).
    MutableRecord(pending_id_) = {.priority = INT64_MIN,
                                  .sequence = 0,
                                  .gate = kPendingResult,
                                  .result_id = 0};
  }

  const TupleList *list = tuples_.Read();
  size_t index = TupleIndex(*list, mask);
  const bool new_tuple = index == list->tuples.size();
  if (new_tuple && list->tuples.size() >= max_tuples_) {
    return std::unexpected(UpsertResult::kTooManyTuples);
  }
  if (!new_tuple) {
    // The order counts the rule now, as Commit() cannot republish; before
    // any live change, as a republish allocates.
    RaiseOrder(*list, index, priority);
    list = tuples_.Read();
    index = TupleIndex(*list, mask);
  }
  std::shared_ptr<ConcurrentExactTable> table =
      new_tuple ? NewTupleTable(1) : list->tuples[index].table;
  if (table == nullptr) {
    return std::unexpected(UpsertResult::kFull);
  }
  bool replaced = false;
  // The key's value (absent, one rule, a chain, or a pending entry another
  // change of this transaction left), and the rule this one replaces: the
  // one with exactly these ranges.
  const std::optional<uint64_t> live =
      new_tuple ? std::nullopt : FindValue(*table, value);
  prepared.old = live ? IdentityIn(*live, ranges) : 0;
  if (!live && !new_tuple && !table->HasRoomForOne()) {
    table = Grown(*table);
    if (table == nullptr) {
      return std::unexpected(UpsertResult::kFull);
    }
    replaced = true;
  }
  // The spare chains a commit of this key fills (nothing counted yet: a
  // failure below drops the key state again).
  const KeyStates::iterator key =
      PrepareKey(mask, value, live.value_or(0), ranges.size(), true);
  const RuleId id = records_.Allocate();
  if (id == 0) {
    Untrack(key);
    return std::unexpected(UpsertResult::kFull);
  }
  // From the id's allocation on, a refused allocation must return the id:
  // the record is not named by any published table until Commit(), so
  // Abort() is exact. A new rule's pending entry is only ever placed in a
  // table that is published below (an existing tuple is republished with
  // PublishTuples, a grown one is not live yet), so the id is all that can
  // leak. (External review: a failure in the tuple-list copy used to leave
  // the record allocated.)
  try {
    // Written now, named by the table only at Commit() (its release store).
    MutableRecord(id) = {.priority = priority,
                         .sequence = next_sequence_++,
                         .gate = gate,
                         .result_id = result_id};
    if (!live) {
      // A new key: present but pending, so capacity is settled here.
      const uint64_t pending = Pack(pending_id_, kPendingResult);
      if (table->Upsert(value, pending) ==
          ConcurrentExactTable::UpsertResult::kFull) {
        table = Grown(*table);
        if (table == nullptr ||
            table->Upsert(value, pending) ==
                ConcurrentExactTable::UpsertResult::kFull) {
          records_.Abort(id);  // never named: reusable at once
          Untrack(key);
          return std::unexpected(UpsertResult::kFull);
        }
        replaced = true;
      }
    }
    if (new_tuple || replaced) {
      auto next = std::make_unique<TupleList>(*list);
      if (new_tuple) {
        // A pending entry never wins: the ceiling rises at Commit(). The
        // order counts the rule now.
        auto ceiling =
            std::make_shared<Ceiling>(std::numeric_limits<int64_t>::min());
        ceiling->order = priority;
        next->tuples.push_back({prepared.mask, table, std::move(ceiling)});
      } else {
        next->tuples[index].table = std::move(table);
      }
      PublishTuples(std::move(next));
    }
  } catch (...) {
    records_.Abort(id);
    Untrack(key);
    throw;
  }
  Track(key, ranges.size(), true);
  prepared.id = id;
  prepared.gate = gate;
  prepared.result_id = result_id;
  prepared.key = key;
  return prepared;
}

std::optional<ConcurrentMaskedTable::Prepared>
ConcurrentMaskedTable::PrepareErase(ConstBytes mask, ConstBytes value,
                                    Ranges ranges) {
  promise(mask.size() == key_len_ && value.size() == key_len_);
  const TupleList *list = tuples_.Read();
  const size_t index = TupleIndex(*list, mask);
  if (index == list->tuples.size()) {
    return std::nullopt;
  }
  const auto live = FindValue(*list->tuples[index].table, value);
  if (!live) {
    return std::nullopt;
  }
  const RuleId old = IdentityIn(*live, ranges);
  if (old == 0) {
    return std::nullopt;
  }
  ReclaimChains();
  Prepared prepared{std::vector<std::byte>(mask.begin(), mask.end()),
                    std::vector<std::byte>(value.begin(), value.end()),
                    std::vector<KeyRange>(ranges.begin(), ranges.end())};
  prepared.old = old;
  prepared.key = PrepareKey(mask, value, *live, 0, false);
  Track(prepared.key, 0, false);
  return prepared;
}

ConcurrentMaskedTable::RuleId ConcurrentMaskedTable::Commit(
    const Prepared &prepared) noexcept {
  const TupleList *list = tuples_.Read();
  const size_t index = TupleIndex(*list, prepared.mask);
  // Prepare*() left the tuple and the entry in place, and an entry stays
  // while any change to it is prepared; nothing else writes between prepare
  // and commit (one writer).
  CHECK_LT(index, list->tuples.size());
  ConcurrentExactTable &table = *list->tuples[index].table;
  const ConstBytes value(prepared.value.data(), prepared.value.size());
  const std::optional<uint64_t> found = FindValue(table, value);
  CHECK(found.has_value());
  const uint64_t live = *found;
  uint64_t add = 0;
  if (prepared.id != 0) {
    RaiseCeiling(list->tuples[index].ceiling->value, Record(prepared.id).priority);
    add = Pack(prepared.id, prepared.gate);
  }
  // The key's next value, built from what is live now, so that changes
  // prepared together for this key commit in any order; 0 when no rule is
  // left.
  KeyState &state = prepared.key->second;
  uint64_t next = 0;
  if (!IsChain(live) && prepared.ranges.empty()) {
    // At most one rule, unranged: the direct value, as without ranges.
    next = add;
  } else {
    CHECK(!state.spares.empty());
    Chain &spare = *state.spares.back();
    BuildChain(live, prepared.old, add, prepared.ranges, spare);
    if (spare.count == 1 && spare.entries()[0].range_count == 0) {
      next = spare.entries()[0].packed;
    } else if (spare.count != 0) {
      next = ChainValue(state.spares.back().release());
      state.spares.pop_back();
    }
  }
  if (next == 0 && state.changes > 1) {
    // Other changes to this key are prepared (an upsert among them): it
    // stays, pending, so their commits are in-place stores too.
    CHECK_NE(pending_id_, 0u);
    next = Pack(pending_id_, kPendingResult);
  }
  if (next != 0) {
    // An in-place value store of a present key: cannot fail.
    CHECK(table.Upsert(value, next) ==
          ConcurrentExactTable::UpsertResult::kUpdated);
  } else {
    CHECK(table.Erase(value));
    // An emptied tuple stays until the next prepare or command drops it
    // (republishing the list would allocate here).
  }
  if (IsChain(live)) {
    RetireChain(&ChainOf(live));
  }
  if (prepared.id == 0) {
    size_--;
  } else if (prepared.old == 0) {
    size_++;
  }
  Settle(prepared);
  return prepared.old;
}

void ConcurrentMaskedTable::Cancel(const Prepared &prepared) noexcept {
  if (prepared.id != 0) {
    records_.Abort(prepared.id);  // never named
  }
  if (prepared.key->second.changes == 1) {
    // The key's last prepared change: a pending entry left for the key's
    // new rules goes with it.
    const TupleList *list = tuples_.Read();
    const size_t index = TupleIndex(*list, prepared.mask);
    CHECK_LT(index, list->tuples.size());
    ConcurrentExactTable &table = *list->tuples[index].table;
    const ConstBytes value(prepared.value.data(), prepared.value.size());
    if (const auto live = FindValue(table, value);
        live && !IsChain(*live) && IdOf(*live) == pending_id_) {
      table.Erase(value);
    }
  }
  Settle(prepared);
}

void ConcurrentMaskedTable::Recycle(RuleId id) {
  records_.Abort(id);
  DropEmptyTuples();
  ReclaimChains();
}

bool ConcurrentMaskedTable::Erase(ConstBytes mask, ConstBytes value,
                                  Ranges ranges) {
  promise(mask.size() == key_len_ && value.size() == key_len_);
  const TupleList *list = tuples_.Read();
  for (size_t t = 0; t < list->tuples.size(); t++) {
    const Tuple &tuple = list->tuples[t];
    if (std::memcmp(tuple.mask.data(), mask.data(), key_len_) != 0) {
      continue;
    }
    const auto live = FindValue(*tuple.table, value);
    if (!ranges.empty() || (live && IsChain(*live))) {
      // A rule of a chain: the key's next value is built as a commit does.
      auto prepared = PrepareErase(mask, value, ranges);
      if (!prepared) {
        return false;
      }
      records_.Retire(Commit(*prepared));
      DropEmptyTuples();
      return true;
    }
    const RuleId id = live ? IdOf(*live) : 0;
    if (id == 0 || !tuple.table->Erase(value)) {
      return false;
    }
    records_.Retire(id);
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
  std::vector<const Chain *> chains;
  ids.reserve(size_);
  for (const Tuple &tuple : tuples_.Read()->tuples) {
    tuple.table->ForEach([&](ConstBytes, uint64_t packed) {
      if (!IsChain(packed)) {
        ids.push_back(IdOf(packed));
        return;
      }
      const Chain &chain = ChainOf(packed);
      chains.push_back(&chain);
      for (uint32_t e = 0; e < chain.count; e++) {
        ids.push_back(IdOf(chain.entries()[e].packed));
      }
    });
  }
  retired_chains_.reserve(retired_chains_.size() + chains.size() +
                          prepared_changes_);
  // Publish first, then start the grace periods that retire the ids: a
  // grace period started earlier could complete while a reader still walks
  // the old list and loads one of these ids.
  PublishTuples(std::make_unique<TupleList>());
  for (const RuleId id : ids) {
    records_.Retire(id);
  }
  for (const Chain *chain : chains) {
    RetireChain(chain);
  }
  size_ = 0;
}

}  // namespace bess::classifier
