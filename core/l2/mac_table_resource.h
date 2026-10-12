// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_L2_MAC_TABLE_RESOURCE_H_
#define BESS_L2_MAC_TABLE_RESOURCE_H_

#include <any>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

#include "dataplane/resource.h"
#include "utils/logging.h"

namespace bess::l2 {

// A PackedMacTable as a transaction resource (D-114): key = Key(mac word),
// value = std::any(uint16_t), the stored value (1..Table::kMaxValue; the
// module's codec gives it meaning, gate + 1 for L2Forward). Values refer to
// nothing the engine tracks.
//
// Cuckoo placement cannot promise room for a set of keys in advance, so a new
// key is inserted during Reserve() with `miss_value()`: the value whose lookup
// does exactly what a miss does at that moment (L2Forward: its default gate +
// 1). No lookup changes until Publish() stores the real value in place, which
// cannot fail. That holds because nothing changes what a miss does between
// Reserve() and Publish(): transactions and the module's commands run under
// the one control-plane lock. A key that does not fit refuses the transaction
// with nothing visible; Abort() erases the placed key.
//
// Inserts move other entries, so staged operations find their key again when
// they run; they never keep a slot.
template <typename Table>
class MacTableResource final : public dataplane::Resource {
 public:
  using MissValueFn = std::function<uint16_t()>;

  MacTableResource(std::string name, Table &table, MissValueFn miss_value)
      : Resource(std::move(name)), table_(table), miss_value_(std::move(miss_value)) {}

  // `mac`: the table's key word (six address bytes above 16 zero bits).
  static dataplane::ResourceKey Key(uint64_t mac) { return dataplane::EncodeKey(mac); }

  size_t LiveCount() const override { return table_.size(); }

  bool Contains(const dataplane::ResourceKey &key) const override {
    uint64_t mac = 0;
    return Decode(key, &mac) && Present(mac);
  }

  std::expected<Reservation, std::string> Reserve(const dataplane::Op &op) override {
    uint64_t mac = 0;
    if (!Decode(op.key, &mac)) {
      return std::unexpected("not a MAC key");
    }
    const bool present = Present(mac);
    if (op.kind == dataplane::OpKind::kErase) {
      if (!present) {
        return std::unexpected("not found");
      }
      Reservation erase{std::make_unique<EraseOp>(table_, mac), {}};
      erase.existed = true;
      return erase;
    }
    const uint16_t *value = std::any_cast<uint16_t>(&op.value);
    if (value == nullptr) {
      return std::unexpected("wrong value type (want uint16_t)");
    }
    if (*value == 0 || *value > Table::kMaxValue) {
      return std::unexpected("value out of range");
    }
    // Everything that can throw first; placing the key is the last step.
    auto upsert = std::make_unique<UpsertOp>(table_, mac, *value);
    Reservation reservation{nullptr, {}};
    reservation.existed = present;
    if (!present) {
      pending_.insert(mac);
      if (table_.Insert(mac, miss_value_(), 0) == Table::kNotFound) {
        pending_.erase(mac);
        return std::unexpected("table full");
      }
      upsert->MarkPlaced();  // Abort() now erases it
    }
    reservation.staged = std::move(upsert);
    return reservation;
  }

  void EndTransaction() noexcept override { pending_.clear(); }

 private:
  static bool Decode(const dataplane::ResourceKey &key, uint64_t *mac) {
    // Six address bytes above a zero domain: anything else is no MAC key.
    return dataplane::DecodeKey(key, mac) && (*mac & 0xFFFF) == 0;
  }

  // A key placed by this transaction is not there yet.
  bool Present(uint64_t mac) const {
    return !pending_.contains(mac) && table_.Find(mac) != Table::kNotFound;
  }

  class UpsertOp final : public dataplane::StagedOp {
   public:
    UpsertOp(Table &table, uint64_t mac, uint16_t value)
        : table_(table), mac_(mac), value_(value) {}
    void MarkPlaced() noexcept { placed_ = true; }
    void Publish(dataplane::Retirer &) noexcept override {
      // Present (placed in Reserve(), or already an entry): an in-place store.
      const uint32_t slot = table_.Find(mac_);
      CHECK_NE(slot, Table::kNotFound);
      table_.set_value(slot, value_);
    }
    void Abort() noexcept override {
      if (placed_) {
        const uint32_t slot = table_.Find(mac_);
        CHECK_NE(slot, Table::kNotFound);
        table_.Erase(slot);
      }
    }

   private:
    Table &table_;
    uint64_t mac_;
    uint16_t value_;
    bool placed_ = false;
  };

  class EraseOp final : public dataplane::StagedOp {
   public:
    EraseOp(Table &table, uint64_t mac) : table_(table), mac_(mac) {}
    void Publish(dataplane::Retirer &) noexcept override {
      const uint32_t slot = table_.Find(mac_);
      CHECK_NE(slot, Table::kNotFound);
      table_.Erase(slot);
    }

   private:
    Table &table_;
    uint64_t mac_;
  };

  Table &table_;
  const MissValueFn miss_value_;
  std::unordered_set<uint64_t> pending_;  // placed in this transaction
};

}  // namespace bess::l2

#endif  // BESS_L2_MAC_TABLE_RESOURCE_H_
