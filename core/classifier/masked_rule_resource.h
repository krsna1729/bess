// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_MASKED_RULE_RESOURCE_H_
#define BESS_CLASSIFIER_MASKED_RULE_RESOURCE_H_

#include <any>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "classifier/concurrent_masked.h"
#include "dataplane/resource.h"

namespace bess::classifier {

// A ConcurrentMaskedTable exposed as a transactional resource (Decision
// D-024): key = the rule's mask then its value (2 x key_len bytes, value
// canonical under the mask), value = MaskedRuleResource::Value.
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
    uint16_t result = 0;
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
    std::vector<std::string> may_reference;
  };

  MaskedRuleResource(std::string name, Hooks hooks)
      : Resource(std::move(name), std::move(hooks.may_reference)),
        table_(std::move(hooks.table)),
        check_value_(std::move(hooks.check_value)),
        references_(std::move(hooks.references)) {}

  static dataplane::ResourceKey Key(ConstBytes mask, ConstBytes value) {
    dataplane::ResourceKey key(reinterpret_cast<const char *>(mask.data()),
                               mask.size());
    key.append(reinterpret_cast<const char *>(value.data()), value.size());
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
    return rule ? references_(Value{rule->priority, rule->result})
                : std::vector<dataplane::Reference>{};
  }
  void VisitReferences(
      const std::function<void(const dataplane::Reference &)> &visit) const
      override {
    if (!references_) {
      return;
    }
    table_()->ForEach([&](ConstBytes, ConstBytes,
                          const ConcurrentMaskedTable::Rule &rule) {
      for (const auto &ref : references_(Value{rule.priority, rule.result})) {
        visit(ref);
      }
    });
  }


  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    std::shared_ptr<ConcurrentMaskedTable> table = table_();
    const size_t len = table->key_len();
    if (op.key.size() != 2 * len) {
      return std::unexpected("key is " + std::to_string(op.key.size()) +
                             " bytes, want mask and value of " +
                             std::to_string(len) + " each");
    }
    const ConstBytes mask = Mask(op.key, len), value = ValueBytes(op.key, len);
    const auto previous = table->FindRule(mask, value);
    std::vector<dataplane::Reference> previous_references;
    if (previous && references_) {
      previous_references =
          references_(Value{previous->priority, previous->result});
    }
    if (op.kind == dataplane::OpKind::kErase) {
      if (!previous) {
        return std::unexpected("not found");
      }
      auto staged = std::make_unique<StagedRule>(table);
      auto prepared = table->PrepareErase(mask, value);
      if (!prepared) {
        return std::unexpected("not found");
      }
      staged->Arm(std::move(*prepared));
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
    // Everything that can throw first; preparing the rule, the step with a
    // physical effect, is last (and Arm() cannot throw).
    std::vector<dataplane::Reference> references;
    if (references_) {
      references = references_(*v);
    }
    auto staged = std::make_unique<StagedRule>(table);
    auto prepared = table->PrepareUpsert(mask, value, v->priority, v->result);
    if (!prepared) {
      return std::unexpected(Describe(prepared.error()));
    }
    const bool replaces = prepared->old != 0;
    staged->Arm(std::move(*prepared));
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

  static std::optional<ConcurrentMaskedTable::Rule> Find(
      const ConcurrentMaskedTable &table, const dataplane::ResourceKey &key) {
    const size_t len = table.key_len();
    if (key.size() != 2 * len) {
      return std::nullopt;
    }
    return table.FindRule(Mask(key, len), ValueBytes(key, len));
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
    explicit StagedRule(std::shared_ptr<ConcurrentMaskedTable> table)
        : table_(std::move(table)) {}
    void Arm(ConcurrentMaskedTable::Prepared prepared) noexcept {
      prepared_ = std::move(prepared);
      armed_ = true;
    }
    void Publish(dataplane::Retirer &retirer) noexcept override {
      const ConcurrentMaskedTable::RuleId freed = table_->Commit(prepared_);
      armed_ = false;
      if (freed != 0) {
        retirer.RemoveLater(
            [table = std::weak_ptr<ConcurrentMaskedTable>(table_),
             freed](dataplane::Retirer &) {
              if (auto t = table.lock()) {
                t->Recycle(freed);
              }
            });
      }
    }
    void Abort() noexcept override {
      if (armed_) {
        table_->Cancel(prepared_);
        armed_ = false;
      }
    }

   private:
    std::shared_ptr<ConcurrentMaskedTable> table_;
    ConcurrentMaskedTable::Prepared prepared_;
    bool armed_ = false;
  };

  std::function<std::shared_ptr<ConcurrentMaskedTable>()> table_;
  std::function<Result(const Value &)> check_value_;
  ReferencesFn references_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_MASKED_RULE_RESOURCE_H_
