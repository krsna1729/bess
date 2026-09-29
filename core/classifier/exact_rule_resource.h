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

#ifndef BESS_CLASSIFIER_EXACT_RULE_RESOURCE_H_
#define BESS_CLASSIFIER_EXACT_RULE_RESOURCE_H_

#include <any>
#include <expected>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glog/logging.h>

#include "classifier/concurrent_exact.h"
#include "dataplane/resource.h"

namespace bess::classifier {

// A ConcurrentExactTable exposed as a transactional resource (G1.2b): key =
// the rule's key bytes (exactly key_len), value = uint64_t. Changes are made in
// place in the shared table (mode C); rte_hash's QSBR defer queue already
// delays slot reuse past a grace period, so nothing goes to the retirer.
// Decision D-021.
//
// Reserve places new keys for real: aggregate capacity cannot promise that a
// set of keys fits (cuckoo placement: keys crafted to share one bucket pair
// fill it at 16, whatever the table's size), so a new key is inserted during
// Reserve with the value kPending, which readers treat as a miss. A key that
// does not fit rejects the transaction with nothing visible; Abort erases
// the pending keys. Publish then only updates the value of a key that is
// already present -- an in-place store in rte_hash, which cannot fail
// (pinned by ConcurrentExactTableTest.UpsertOfAPresentKeySucceedsWhenFull).
//
// Readers of a table registered as a resource must drop kPending hits:
// VisibleHits() does it. kPending is not a valid rule value.
//
// `references` maps a value to the keys it names in other resources. (A
// rule table is a root: its erases take effect at once, so nothing may
// reference it; see Resource::DefersErase.)
//
// A module that replaces its table (ExactMatch grows by copying into a
// larger one) passes Hooks: the table is then fetched at every use, and
// make_room may replace it while a transaction is being prepared. Pending
// keys are copied like any other, and staged operations find them in the
// new table (they look the table up when they run). Decision D-022.
class ExactRuleResource final : public dataplane::Resource {
 public:
  using ReferencesFn =
      std::function<std::vector<dataplane::Reference>(uint64_t value)>;
  using Result = std::expected<void, std::string>;

  struct Hooks {
    // The owner's current table.
    std::function<ConcurrentExactTable &()> table;
    // Refuses values the owner does not accept (e.g. an invalid gate).
    std::function<Result(uint64_t value)> check_value;
    // Makes room for one more key -- by growing the table -- when a new key
    // does not fit. `force`: the key's buckets were full although the table
    // had its headroom (a cuckoo placement failure). An error rejects the
    // transaction.
    std::function<Result(bool force)> make_room;
    ReferencesFn references;
    // The resources `references` can name (declared, see Resource).
    std::vector<std::string> may_reference;
  };

  // `may_reference`: the resources `references` can name (declared, see
  // Resource).
  ExactRuleResource(std::string name, ConcurrentExactTable &table,
                    ReferencesFn references = {},
                    std::vector<std::string> may_reference = {})
      : ExactRuleResource(
            std::move(name),
            Hooks{.table = [&table]() -> ConcurrentExactTable & {
                    return table;
                  },
                  .references = std::move(references),
                  .may_reference = std::move(may_reference)}) {}

  ExactRuleResource(std::string name, Hooks hooks)
      : Resource(std::move(name), std::move(hooks.may_reference)),
        table_(std::move(hooks.table)),
        check_value_(std::move(hooks.check_value)),
        make_room_(std::move(hooks.make_room)),
        references_(std::move(hooks.references)) {}

  // Committed keys: no transaction is in flight when the engine asks, so no
  // kPending placeholder is counted.
  size_t LiveCount() const override { return table_().size(); }

  // The value a key holds while a transaction that adds it is being
  // prepared. Readers treat it as a miss.
  static constexpr uint64_t kPending = ~uint64_t{0};

  // `hits` from LookupBatch with pending keys cleared.
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
    const std::optional<uint64_t> value = Find(key);
    return value ? references_(*value) : std::vector<dataplane::Reference>{};
  }
  void VisitReferences(
      const std::function<void(const dataplane::Reference &)> &visit) const
      override {
    if (!references_) {
      return;
    }
    table_().ForEach([&](ConstBytes, uint64_t value) {
      for (const auto &ref : references_(value)) {
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
    const std::optional<uint64_t> previous = Find(op.key);
    if (op.kind == dataplane::OpKind::kErase) {
      if (!previous) {
        return std::unexpected("not found");
      }
      Reservation erase{std::make_unique<EraseOp>(table_, op.key), {}};
      erase.existed = true;
      if (references_) {
        erase.previous_references = references_(*previous);
      }
      return erase;
    }
    const uint64_t *value = std::any_cast<uint64_t>(&op.value);
    if (value == nullptr) {
      return std::unexpected("wrong value type (want uint64_t)");
    }
    if (*value == kPending) {
      return std::unexpected("value is reserved (kPending)");
    }
    if (check_value_) {
      if (Result ok = check_value_(*value); !ok) {
        return std::unexpected(ok.error());
      }
    }
    // Everything that can throw comes first -- the staged operation and the
    // references -- so that placing the key, the one step with a physical
    // effect, is the last thing that can fail: nothing can leak a pending key
    // (external review: an allocation or reference-callback failure after the
    // insertion used to leave one behind).
    auto upsert = std::make_unique<UpsertOp>(table_, op.key, *value);
    std::vector<dataplane::Reference> references;
    std::vector<dataplane::Reference> previous_references;
    if (references_) {
      references = references_(*value);
      if (previous) {
        previous_references = references_(*previous);
      }
    }
    if (!previous) {
      if (Result placed = PlacePending(op.key); !placed) {
        return std::unexpected(placed.error());
      }
      upsert->MarkInsertedPending();  // Abort() now erases it
    }
    Reservation reservation{std::move(upsert), std::move(references), {}};
    reservation.existed = previous.has_value();
    reservation.previous_references = std::move(previous_references);
    return reservation;
  }

 private:
  using TableFn = std::function<ConcurrentExactTable &()>;

  // Inserts `key` with kPending, invisible to readers. When it does not fit
  // and the owner can make room, makes room once and tries again.
  Result PlacePending(const std::string &key) {
    std::string why;
    for (int attempt = 0; attempt < 2; attempt++) {
      ConcurrentExactTable &table = table_();
      // Headroom for deletes still in the defer queue (D-010).
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

  std::optional<uint64_t> Find(const dataplane::ResourceKey &key) const {
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

  class UpsertOp final : public dataplane::StagedOp {
   public:
    // `table` is the resource's (it outlives its staged operations), looked
    // up when the operation runs: the owner may have replaced the table
    // since Reserve().
    UpsertOp(const TableFn &table, std::string key, uint64_t value)
        : table_(table), key_(std::move(key)), value_(value) {}
    void MarkInsertedPending() noexcept { inserted_pending_ = true; }
    void Publish(dataplane::Retirer &) noexcept override {
      // The key is present (placed in Reserve, or already there): this is an
      // in-place value store, which cannot fail.
      CHECK(table_().Upsert(Bytes(key_), value_) ==
            ConcurrentExactTable::UpsertResult::kUpdated);
    }
    void Abort() noexcept override {
      if (inserted_pending_) {
        table_().Erase(Bytes(key_));
      }
    }

   private:
    const TableFn &table_;
    std::string key_;
    uint64_t value_;
    bool inserted_pending_ = false;
  };

  class EraseOp final : public dataplane::StagedOp {
   public:
    EraseOp(const TableFn &table, std::string key)
        : table_(table), key_(std::move(key)) {}
    void Publish(dataplane::Retirer &) noexcept override {
      table_().Erase(Bytes(key_));
    }

   private:
    const TableFn &table_;
    std::string key_;
  };

  TableFn table_;
  std::function<Result(uint64_t)> check_value_;
  std::function<Result(bool)> make_room_;
  ReferencesFn references_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_EXACT_RULE_RESOURCE_H_
