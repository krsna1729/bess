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
// the rule's key bytes (exactly key_len), value = uint64_t. Upserts and
// erases change the shared table in place (mode C); rte_hash's QSBR defer
// queue already delays slot reuse past a grace period, so nothing is handed
// to the transaction's retirer. Capacity is reserved per transaction: a
// transaction whose new keys would not fit (with the table's headroom) is
// rejected before anything is published. Decision D-021.
//
// `references` maps a value to the keys it names in other resources (for
// example, a rule's value holding an action id).
class ExactRuleResource final : public dataplane::Resource {
 public:
  using ReferencesFn =
      std::function<std::vector<dataplane::Reference>(uint64_t value)>;

  ExactRuleResource(std::string name, int rank, ConcurrentExactTable &table,
                    ReferencesFn references = {})
      : Resource(std::move(name), rank),
        table_(table),
        references_(std::move(references)) {}

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

  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    if (op.key.size() != table_.key_len()) {
      return std::unexpected("key is " + std::to_string(op.key.size()) +
                             " bytes, the table's are " +
                             std::to_string(table_.key_len()));
    }
    if (op.kind == dataplane::OpKind::kErase) {
      if (!Contains(op.key)) {
        return std::unexpected("not found");
      }
      return Reservation{std::make_unique<EraseOp>(table_, op.key), {}};
    }
    const uint64_t *value = std::any_cast<uint64_t>(&op.value);
    if (value == nullptr) {
      return std::unexpected("wrong value type (want uint64_t)");
    }
    if (!Contains(op.key)) {
      // Live keys, deletes still in the defer queue, this transaction's
      // earlier new keys, and the headroom must all fit (D-010).
      table_.Reclaim();
      const size_t needed = table_.slots_in_use() + new_keys_ + 1 +
                            ConcurrentExactTable::Headroom(table_.capacity());
      if (needed > table_.capacity()) {
        return std::unexpected("table full (" +
                               std::to_string(table_.capacity()) + " slots)");
      }
      new_keys_++;
    }
    Reservation reservation{
        std::make_unique<UpsertOp>(table_, op.key, *value), {}};
    if (references_) {
      reservation.references = references_(*value);
    }
    return reservation;
  }

  void EndTransaction() noexcept override { new_keys_ = 0; }

 private:
  static ConstBytes Bytes(const std::string &key) {
    return ConstBytes(reinterpret_cast<const Byte *>(key.data()), key.size());
  }

  std::optional<uint64_t> Find(const dataplane::ResourceKey &key) const {
    if (key.size() != table_.key_len()) {
      return std::nullopt;
    }
    uint64_t value = 0;
    if (table_.LookupBatch(Bytes(key), key.size(), &value, 1) == 0) {
      return std::nullopt;
    }
    return value;
  }

  class UpsertOp final : public dataplane::StagedOp {
   public:
    UpsertOp(ConcurrentExactTable &table, std::string key, uint64_t value)
        : table_(table), key_(std::move(key)), value_(value) {}
    void Publish(dataplane::Retirer &) noexcept override {
      // Capacity was reserved: a full table here is a broken invariant.
      CHECK(table_.Upsert(Bytes(key_), value_) !=
            ConcurrentExactTable::UpsertResult::kFull);
    }

   private:
    ConcurrentExactTable &table_;
    std::string key_;
    uint64_t value_;
  };

  class EraseOp final : public dataplane::StagedOp {
   public:
    EraseOp(ConcurrentExactTable &table, std::string key)
        : table_(table), key_(std::move(key)) {}
    void Publish(dataplane::Retirer &) noexcept override {
      table_.Erase(Bytes(key_));
    }

   private:
    ConcurrentExactTable &table_;
    std::string key_;
  };

  ConcurrentExactTable &table_;
  ReferencesFn references_;
  size_t new_keys_ = 0;  // new keys reserved by the current transaction
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_EXACT_RULE_RESOURCE_H_
