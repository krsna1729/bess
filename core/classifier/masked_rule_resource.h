// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_MASKED_RULE_RESOURCE_H_
#define BESS_CLASSIFIER_MASKED_RULE_RESOURCE_H_

#include <any>
#include <cstring>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "classifier/concurrent_masked.h"
#include "classifier/record_arena.h"
#include "dataplane/resource.h"

namespace bess::classifier {

// A ConcurrentMaskedTable exposed as a transactional resource (Decision
// D-024): key = the rule's mask, its value (2 x key_len bytes, value
// canonical under the mask), then its ranges (D-186), each a
// ConcurrentMaskedTable::KeyRange's bytes; value = MaskedRuleResource::Value.
//
// Reserve() prepares the rule in the table (ConcurrentMaskedTable::
// PrepareUpsert/PrepareErase): a new mask, a tuple table's growth, the rule
// id and its record all happen there, and a new rule sits in its tuple as a
// pending entry that loses to every rule and alone reads as a miss -- the
// table's own lookup filters it, so readers need nothing. Publish() commits
// with one in-place store or delete; the id it frees returns to the table
// through the removal cascade, a grace period later. A rule table is a root:
// its erases take effect at once, so nothing may reference it.
class MaskedRuleResource final : public dataplane::Resource {
 public:
  struct Value {
    int64_t priority = 0;
    uint16_t gate = 0;
    // The rule's result record: variable on the wire, zero-padded to the
    // arena's record when the rule is published.
    std::vector<std::byte> result;
  };
  using Result = std::expected<void, std::string>;
  using ReferencesFn =
      std::function<std::vector<dataplane::Reference>(const Value &)>;

  struct Hooks {
    // The owner's current table (it may replace it between transactions).
    std::function<std::shared_ptr<ConcurrentMaskedTable>()> table;
    // Refuses values the owner does not accept (e.g. an invalid gate).
    std::function<Result(const Value &)> check_value;
    ReferencesFn references;
    // Where result records live; null when the table has no result fields.
    std::shared_ptr<RecordArena> records;
    std::vector<std::string> may_reference;
  };

  MaskedRuleResource(std::string name, Hooks hooks)
      : Resource(std::move(name), std::move(hooks.may_reference)),
        table_(std::move(hooks.table)),
        check_value_(std::move(hooks.check_value)),
        references_(std::move(hooks.references)),
        records_(std::move(hooks.records)) {}

  static dataplane::ResourceKey Key(
      ConstBytes mask, ConstBytes value,
      ConcurrentMaskedTable::Ranges ranges = {}) {
    dataplane::ResourceKey key(reinterpret_cast<const char *>(mask.data()),
                               mask.size());
    key.append(reinterpret_cast<const char *>(value.data()), value.size());
    key.append(reinterpret_cast<const char *>(ranges.data()),
               ranges.size_bytes());
    return key;
  }

  size_t LiveCount() const override { return table_()->size(); }

  bool Contains(const dataplane::ResourceKey &key) const override {
    return Find(*table_(), key).has_value();
  }

  std::vector<dataplane::Reference> ReferencesOf(
      const dataplane::ResourceKey &key) const override {
    if (!references_) {
      return {};
    }
    const auto rule = Find(*table_(), key);
    return rule ? references_(Value{rule->priority, rule->gate,
                                    Load(rule->result_id)})
                : std::vector<dataplane::Reference>{};
  }
  void VisitReferences(
      const std::function<void(const dataplane::Reference &)> &visit) const
      override {
    if (!references_) {
      return;
    }
    table_()->ForEach([&](ConstBytes, ConstBytes,
                          const ConcurrentMaskedTable::Rule &rule,
                          ConcurrentMaskedTable::Ranges) {
      for (const auto &ref : references_(
               Value{rule.priority, rule.gate, Load(rule.result_id)})) {
        visit(ref);
      }
    });
  }


  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    std::shared_ptr<ConcurrentMaskedTable> table = table_();
    const size_t len = table->key_len();
    const auto ranges = RangesOf(op.key, len);
    if (!ranges) {
      return std::unexpected("key is " + std::to_string(op.key.size()) +
                             " bytes, want mask and value of " +
                             std::to_string(len) +
                             " each, then whole ranges");
    }
    const ConstBytes mask = Mask(op.key, len), value = ValueBytes(op.key, len);
    const auto previous = table->FindRule(mask, value, *ranges);
    std::vector<dataplane::Reference> previous_references;
    if (previous && references_) {
      previous_references =
          references_(Value{previous->priority, previous->gate,
                            Load(previous->result_id)});
    }
    // A reservation made before a fallible preparation: cancel it unless the
    // staged rule takes ownership. No allocation.
    struct ReservationGuard {
      RecordArena *arena = nullptr;
      bool held = false;
      ~ReservationGuard() noexcept {
        if (held) {
          arena->CancelRetirement(1);
        }
      }
      void Reserve(RecordArena *a) {
        a->ReserveRetirement(1);
        arena = a;
        held = true;
      }
      void Transfer() noexcept { held = false; }
    };

    if (op.kind == dataplane::OpKind::kErase) {
      if (!previous) {
        return std::unexpected("not found");
      }
      auto staged = std::make_unique<StagedRule>(table, records_);
      ReservationGuard retire_guard;
      if (records_ != nullptr && previous->result_id != 0) {
        retire_guard.Reserve(records_.get());
      }
      auto prepared = table->PrepareErase(mask, value, *ranges);
      if (!prepared) {
        return std::unexpected("not found");
      }
      if (retire_guard.held) {
        staged->RetireOnPublish();
        retire_guard.Transfer();
      }
      staged->Arm(std::move(*prepared), 0);
      Reservation erase{std::move(staged), {}};
      erase.footprint.removals = 1;  // the freed id, a grace period later
      erase.existed = true;
      erase.previous_references = std::move(previous_references);
      return erase;
    }
    const Value *v = std::any_cast<Value>(&op.value);
    if (v == nullptr) {
      return std::unexpected("wrong value type (want MaskedRuleResource::Value)");
    }
    if (check_value_) {
      if (Result ok = check_value_(*v); !ok) {
        return std::unexpected(ok.error());
      }
    }
    if (records_ != nullptr && v->result.size() > records_->record_size()) {
      return std::unexpected("result is " +
                             std::to_string(v->result.size()) +
                             " bytes, the schema's record is " +
                             std::to_string(records_->record_size()));
    }
    // Everything that can throw first; preparing the rule, the step with a
    // physical effect, is last (and Arm() cannot throw).
    std::vector<dataplane::Reference> references;
    if (references_) {
      references = references_(*v);
    }
    uint32_t record_id = 0;
    // Before ownership moves into the staged rule, every throw must return the
    // unpublished id exactly once.
    struct IdGuard {
      RecordArena *arena;
      uint32_t *id;
      ~IdGuard() noexcept {
        if (arena != nullptr && *id != 0) {
          arena->Abort(*id);
        }
      }
    } guard{records_.get(), &record_id};
    if (records_ != nullptr) {
      record_id = records_->Allocate();
      if (record_id == 0) {
        return std::unexpected("result arena full");
      }
      MutableBytes slot = records_->Record(record_id);
      std::memset(slot.data(), 0, slot.size());
      if (!v->result.empty()) {
        std::memcpy(slot.data(), v->result.data(), v->result.size());
      }
    }
    auto staged = std::make_unique<StagedRule>(table, records_);
    ReservationGuard retire_guard;
    if (records_ != nullptr && previous && previous->result_id != 0) {
      retire_guard.Reserve(records_.get());
    }
    auto prepared = table->PrepareUpsert(mask, value, v->priority, v->gate,
                                         record_id, *ranges);
    if (!prepared) {
      return std::unexpected(Describe(prepared.error()));
    }
    const bool replaces = prepared->old != 0;
    if (retire_guard.held && replaces) {
      staged->RetireOnPublish();
      retire_guard.Transfer();
    }
    staged->Arm(std::move(*prepared), record_id);
    record_id = 0;  // owned by the staged rule now
    Reservation upsert{std::move(staged), std::move(references)};
    upsert.footprint.removals = replaces ? 1 : 0;
    upsert.existed = previous.has_value();
    upsert.previous_references = std::move(previous_references);
    return upsert;
  }

 private:
  static ConstBytes Mask(const dataplane::ResourceKey &key, size_t len) {
    return ConstBytes(reinterpret_cast<const std::byte *>(key.data()), len);
  }
  static ConstBytes ValueBytes(const dataplane::ResourceKey &key, size_t len) {
    return ConstBytes(reinterpret_cast<const std::byte *>(key.data()) + len,
                      len);
  }
  // The ranges after the mask and value; nullopt when the key is not a mask,
  // a value and whole ranges.
  static std::optional<std::vector<ConcurrentMaskedTable::KeyRange>> RangesOf(
      const dataplane::ResourceKey &key, size_t len) {
    using KeyRange = ConcurrentMaskedTable::KeyRange;
    if (key.size() < 2 * len || (key.size() - 2 * len) % sizeof(KeyRange) != 0) {
      return std::nullopt;
    }
    std::vector<KeyRange> ranges((key.size() - 2 * len) / sizeof(KeyRange));
    // An empty vector's data() may be null, which memcpy may not receive.
    if (!ranges.empty()) {
      std::memcpy(ranges.data(), key.data() + 2 * len,
                  ranges.size() * sizeof(KeyRange));
    }
    return ranges;
  }

  // The stored record bytes for a rule's id.
  std::vector<std::byte> Load(uint32_t id) const {
    if (records_ == nullptr) {
      return {};
    }
    const ConstBytes bytes = records_->Lookup(id);
    return std::vector<std::byte>(bytes.begin(), bytes.end());
  }

  static std::optional<ConcurrentMaskedTable::Rule> Find(
      const ConcurrentMaskedTable &table, const dataplane::ResourceKey &key) {
    const size_t len = table.key_len();
    const auto ranges = RangesOf(key, len);
    if (!ranges) {
      return std::nullopt;
    }
    return table.FindRule(Mask(key, len), ValueBytes(key, len), *ranges);
  }

  static std::string Describe(ConcurrentMaskedTable::UpsertResult r) {
    using R = ConcurrentMaskedTable::UpsertResult;
    switch (r) {
      case R::kTooManyTuples:
        return "too many distinct masks";
      case R::kNotCanonical:
        return "value has bits outside its mask";
      case R::kReservedResult:
        return "result is reserved (kPendingResult)";
      case R::kBadRange:
        return "a range is not canonical (overlapping, masked, empty, or "
               "the whole field)";
      case R::kFull:
        return "rule table is full";
      case R::kInserted:
      case R::kUpdated:
        break;
    }
    return "unexpected table result";
  }

  // One prepared rule change. Holds the table it was prepared in: the owner
  // cannot replace it mid-transaction, and the freed id must go back to
  // that table, not a successor (hence the weak reference in the cascade).
  class StagedRule final : public dataplane::StagedOp {
   public:
    StagedRule(std::shared_ptr<ConcurrentMaskedTable> table,
               std::shared_ptr<RecordArena> records)
        : table_(std::move(table)), records_(std::move(records)) {}
    void RetireOnPublish() noexcept { retirement_reserved_ = true; }
    void Arm(ConcurrentMaskedTable::Prepared prepared,
             uint32_t record_id) noexcept {
      prepared_ = std::move(prepared);
      record_id_ = record_id;
      armed_ = true;
    }
    void Publish(dataplane::Retirer &retirer) noexcept override {
      const ConcurrentMaskedTable::RuleId freed = table_->Commit(prepared_);
      armed_ = false;
      if (freed != 0) {
        // The freed rule record is still readable; take the record id it
        // names now, and retire both ids after the removal cascade.
        const uint32_t old_record =
            records_ != nullptr ? table_->RuleAt(freed).result_id : 0;
        retirer.RemoveLater(
            [table = std::weak_ptr<ConcurrentMaskedTable>(table_),
             records = records_, freed, old_record](dataplane::Retirer &) {
              if (auto t = table.lock()) {
                if (records != nullptr && old_record != 0) {
                  records->Retire(old_record);
                }
                t->Recycle(freed);
              }
            });
        retirement_reserved_ = false;  // the callback consumes it
      }
    }
    void Abort() noexcept override {
      if (armed_) {
        table_->Cancel(prepared_);
        if (records_ != nullptr && record_id_ != 0) {
          records_->Abort(record_id_);
        }
        armed_ = false;
      }
      if (records_ != nullptr && retirement_reserved_) {
        records_->CancelRetirement(1);
        retirement_reserved_ = false;
      }
    }

   private:
    std::shared_ptr<ConcurrentMaskedTable> table_;
    std::shared_ptr<RecordArena> records_;
    ConcurrentMaskedTable::Prepared prepared_;
    uint32_t record_id_ = 0;
    bool armed_ = false;
    bool retirement_reserved_ = false;
  };

  std::function<std::shared_ptr<ConcurrentMaskedTable>()> table_;
  std::function<Result(const Value &)> check_value_;
  ReferencesFn references_;
  std::shared_ptr<RecordArena> records_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_MASKED_RULE_RESOURCE_H_
