// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_EXACT_RULE_RESOURCE_H_
#define BESS_CLASSIFIER_EXACT_RULE_RESOURCE_H_

#include <any>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "utils/logging.h"

#include "classifier/concurrent_exact.h"
#include "classifier/record_arena.h"
#include "classifier/rule_result.h"
#include "dataplane/resource.h"
#include "gate.h"

namespace bess::classifier {

// A ConcurrentExactTable exposed as a transactional resource (G1.2b): key =
// the rule's key bytes (exactly key_len), value = a packed {gate, result id}
// word. The result record lives in a RecordArena, zero-padded to the arena's
// record size; the table value carries its id, so an update swaps the id and
// retires the old record after a grace period, as ConcurrentMaskedTable does.
// Changes are made in place in the shared table (mode C); rte_hash's QSBR defer
// queue already delays slot reuse past a grace period. Decision D-021.
//
// Reserve places new keys for real: aggregate capacity cannot promise that a
// set of keys fits (cuckoo placement: keys crafted to share one bucket pair
// fill it at 16, whatever the table's size), so a new key is inserted during
// Reserve with the value kPending, which readers treat as a miss. A key that
// does not fit rejects the transaction with nothing visible; Abort erases
// the pending key. Publish then only updates the value of a key that is
// already present -- an in-place store in rte_hash, which cannot fail
// (pinned by ConcurrentExactTableTest.UpsertOfAPresentKeySucceedsWhenFull).
//
// `references` maps a value to the keys it names in other resources. (A
// rule table is a root: its erases take effect at once, so nothing may
// reference it; see Resource::DefersErase.)
class ExactRuleResource final : public dataplane::Resource {
 public:
  using Value = RuleResult;
  using ReferencesFn =
      std::function<std::vector<dataplane::Reference>(const Value &)>;
  using Result = std::expected<void, std::string>;

  struct Hooks {
    std::function<ConcurrentExactTable &()> table;
    std::function<Result(const Value &)> check_value;
    std::function<Result(bool force)> make_room;
    ReferencesFn references;
    // Where result records are stored; null for a gate-only table (a value's
    // bytes must then be empty).
    RecordArena *records = nullptr;
    std::vector<std::string> may_reference;
  };

  ExactRuleResource(std::string name, ConcurrentExactTable &table,
                    ReferencesFn references = {},
                    std::vector<std::string> may_reference = {},
                    RecordArena *records = nullptr)
      : ExactRuleResource(
            std::move(name),
            Hooks{.table = [&table]() -> ConcurrentExactTable & {
                    return table;
                  },
                  .references = std::move(references),
                  .records = records,
                  .may_reference = std::move(may_reference)}) {}

  ExactRuleResource(std::string name, Hooks hooks)
      : Resource(std::move(name), std::move(hooks.may_reference)),
        table_(std::move(hooks.table)),
        check_value_(std::move(hooks.check_value)),
        make_room_(std::move(hooks.make_room)),
        references_(std::move(hooks.references)),
        records_(hooks.records) {}

  size_t LiveCount() const override { return table_().size(); }

  std::optional<uint64_t> Capacity() const override {
    if (make_room_) {
      return std::nullopt;
    }
    const uint32_t slots = table_().capacity();
    const uint32_t headroom = ConcurrentExactTable::Headroom(slots);
    return slots > headroom ? slots - headroom : 0;
  }

  static constexpr uint64_t kPending = ~uint64_t{0};

  static uint64_t VisibleHits(uint64_t hits, const uint64_t *values) noexcept {
    for (uint64_t m = hits; m != 0; m &= m - 1) {
      const int i = __builtin_ctzll(m);
      if (values[i] == kPending) {
        hits &= ~(uint64_t{1} << i);
      }
    }
    return hits;
  }

  bool Contains(const dataplane::ResourceKey &key) const override {
    return Find(key).has_value();
  }

  std::vector<dataplane::Reference> ReferencesOf(
      const dataplane::ResourceKey &key) const override {
    if (!references_) {
      return {};
    }
    const std::optional<Value> value = Find(key);
    return value ? references_(*value) : std::vector<dataplane::Reference>{};
  }

  void VisitReferences(
      const std::function<void(const dataplane::Reference &)> &visit) const
      override {
    if (!references_) {
      return;
    }
    table_().ForEach([&](ConstBytes, uint64_t packed) {
      for (const auto &ref : references_(Load(packed))) {
        visit(ref);
      }
    });
  }

  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    if (op.key.size() != table_().key_len()) {
      return std::unexpected("key is " + std::to_string(op.key.size()) +
                             " bytes, the table's are " +
                             std::to_string(table_().key_len()));
    }
    const std::optional<uint64_t> previous = FindPacked(op.key);
    if (op.kind == dataplane::OpKind::kErase) {
      if (!previous) {
        return std::unexpected("not found");
      }
      std::vector<dataplane::Reference> previous_references;
      if (references_) {
        previous_references = references_(Load(*previous));
      }
      // Last fallible step: the erase op reserves the retirement slot and
      // cancels it in Abort() if the transaction does not publish.
      Reservation erase{
          std::make_unique<EraseOp>(table_, op.key, RecordIdOf(*previous),
                                    records_),
          {}};
      erase.existed = true;
      erase.previous_references = std::move(previous_references);
      return erase;
    }
    const Value *value = std::any_cast<Value>(&op.value);
    if (value == nullptr) {
      return std::unexpected("wrong value type (want RuleResult)");
    }
    if (check_value_) {
      if (Result ok = check_value_(*value); !ok) {
        return std::unexpected(ok.error());
      }
    }
    if (records_ != nullptr && !value->bytes.empty() &&
        value->bytes.size() > records_->record_size()) {
      return std::unexpected("result is " +
                             std::to_string(value->bytes.size()) +
                             " bytes, the schema's record is " +
                             std::to_string(records_->record_size()));
    }
    auto upsert = std::make_unique<UpsertOp>(table_, op.key, *value, records_);
    if (records_ != nullptr && upsert->record == 0) {
      return std::unexpected("result arena full");
    }
    std::vector<dataplane::Reference> references;
    std::vector<dataplane::Reference> previous_references;
    if (references_) {
      references = references_(*value);
      if (previous) {
        previous_references = references_(Load(*previous));
      }
    }
    if (!previous) {
      if (Result placed = PlacePending(op.key); !placed) {
        upsert->Abort();
        return std::unexpected(placed.error());
      }
      upsert->MarkInsertedPending();
    } else if (records_ != nullptr && RecordIdOf(*previous) != 0) {
      records_->ReserveRetirement(1);  // publication must not allocate
      upsert->RetireOnPublish(RecordIdOf(*previous));
    }
    Reservation reservation{std::move(upsert), std::move(references), {}};
    reservation.existed = previous.has_value();
    reservation.previous_references = std::move(previous_references);
    return reservation;
  }

 private:
  using TableFn = std::function<ConcurrentExactTable &()>;

  static uint32_t RecordIdOf(uint64_t packed) noexcept {
    return bess::ResultIdOfResult(packed);
  }

  Value Load(uint64_t packed) const {
    Value value;
    value.gate = bess::GateOfResult(packed);
    if (records_ != nullptr) {
      const ConstBytes bytes = records_->Lookup(RecordIdOf(packed));
      value.bytes.assign(bytes.begin(), bytes.end());
    }
    return value;
  }

  Result PlacePending(const std::string &key) {
    std::string why;
    for (int attempt = 0; attempt < 2; attempt++) {
      ConcurrentExactTable &table = table_();
      table.Reclaim();
      const bool room = table.slots_in_use() + 1 +
                            ConcurrentExactTable::Headroom(table.capacity()) <=
                        table.capacity();
      if (!room) {
        why = "table full (" + std::to_string(table.capacity()) + " slots)";
      } else if (table.Upsert(Bytes(key), kPending) !=
                 ConcurrentExactTable::UpsertResult::kFull) {
        return {};
      } else {
        why = "no room for this key (its buckets are full)";
      }
      if (!make_room_ || attempt == 1) {
        break;
      }
      if (Result grown = make_room_(/*force=*/room); !grown) {
        return grown;
      }
    }
    return std::unexpected(why);
  }

  static ConstBytes Bytes(const std::string &key) {
    return ConstBytes(reinterpret_cast<const Byte *>(key.data()), key.size());
  }

  std::optional<uint64_t> FindPacked(const dataplane::ResourceKey &key) const {
    const ConcurrentExactTable &table = table_();
    if (key.size() != table.key_len()) {
      return std::nullopt;
    }
    uint64_t value = 0;
    if (table.LookupBatch(Bytes(key), key.size(), &value, 1) == 0 ||
        value == kPending) {
      return std::nullopt;
    }
    return value;
  }

  std::optional<Value> Find(const dataplane::ResourceKey &key) const {
    const std::optional<uint64_t> packed = FindPacked(key);
    return packed ? std::optional<Value>(Load(*packed)) : std::nullopt;
  }

  class UpsertOp final : public dataplane::StagedOp {
   public:
    UpsertOp(const TableFn &table, std::string key, const Value &value,
             RecordArena *records)
        : table_(table), key_(std::move(key)), value_(value), records_(records) {
      if (records_ != nullptr) {
        record = records_->Allocate();
        if (record != 0) {
          MutableBytes slot = records_->Record(record);
          std::memset(slot.data(), 0, slot.size());
          if (!value_.bytes.empty()) {
            std::memcpy(slot.data(), value_.bytes.data(), value_.bytes.size());
          }
        }
      }
    }
    ~UpsertOp() noexcept override { Abort(); }
    void MarkInsertedPending() noexcept { inserted_pending_ = true; }
    void RetireOnPublish(uint32_t old) noexcept {
      old_record_ = old;
      retirement_reserved_ = true;
    }
    void Publish(dataplane::Retirer &) noexcept override {
      CHECK(table_().Upsert(Bytes(key_), Packed()) ==
            ConcurrentExactTable::UpsertResult::kUpdated);
      if (records_ != nullptr && old_record_ != 0) {
        records_->Retire(old_record_);
        retirement_reserved_ = false;
      }
      record = 0;  // owned by the table now
      inserted_pending_ = false;
    }
    void Abort() noexcept override {
      if (inserted_pending_) {
        table_().Erase(Bytes(key_));
        inserted_pending_ = false;
      }
      if (records_ != nullptr && record != 0) {
        records_->Abort(record);
        record = 0;
      }
      if (records_ != nullptr && retirement_reserved_) {
        records_->CancelRetirement(1);
        retirement_reserved_ = false;
      }
    }

    uint32_t record = 0;

   private:
    uint64_t Packed() const noexcept {
      return bess::MakeGateResult(value_.gate, record);
    }

    const TableFn &table_;
    std::string key_;
    Value value_;
    RecordArena *records_;
    bool retirement_reserved_ = false;
    uint32_t old_record_ = 0;
    bool inserted_pending_ = false;
  };

  class EraseOp final : public dataplane::StagedOp {
   public:
    EraseOp(const TableFn &table, std::string key, uint32_t old_record,
            RecordArena *records)
        : table_(table),
          key_(std::move(key)),
          old_record_(old_record),
          records_(records) {
      if (records_ != nullptr && old_record_ != 0) {
        records_->ReserveRetirement(1);  // publication must not allocate
        retirement_reserved_ = true;
      }
    }
    void Publish(dataplane::Retirer &) noexcept override {
      table_().Erase(Bytes(key_));
      if (records_ != nullptr && old_record_ != 0) {
        records_->Retire(old_record_);
        retirement_reserved_ = false;
      }
    }
    void Abort() noexcept override {
      if (records_ != nullptr && retirement_reserved_) {
        records_->CancelRetirement(1);
        retirement_reserved_ = false;
      }
    }

   private:
    const TableFn &table_;
    std::string key_;
    uint32_t old_record_ = 0;
    RecordArena *records_;
    bool retirement_reserved_ = false;
  };

  TableFn table_;
  std::function<Result(const Value &)> check_value_;
  std::function<Result(bool)> make_room_;
  ReferencesFn references_;
  RecordArena *records_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_EXACT_RULE_RESOURCE_H_
