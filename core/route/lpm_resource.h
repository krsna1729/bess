// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_LPM_RESOURCE_H_
#define BESS_ROUTE_LPM_RESOURCE_H_

#include <any>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include "dataplane/resource.h"
#include "route/route_table.h"
#include "utils/logging.h"

namespace bess::route {

// An LpmRouteTable as a transaction resource (D-111): key = Key(prefix),
// value = std::any(uint32_t), the route's value (at most
// LpmRouteTable::kMaxValue; the module's codec gives it meaning, a gate for
// IPLookup). Values refer to nothing the engine tracks.
//
// Neither backend can promise room for a set of prefixes in advance, so a new
// prefix is placed during Reserve() with what its addresses resolve to today
// (CoveringValue: the covering route, else the default, else `miss_value`, a
// value the module's readers treat exactly as a miss -- IPLookup's DROP_GATE):
// no lookup changes until Publish() stores the value in place. Changing or
// deleting an existing rule needs no tbl8 group in either backend (rte_fib
// with deps/dpdk-patches/0005, D-132), so Publish() and Abort() cannot fail.
// A prefix that does not fit refuses the transaction with nothing visible;
// Abort() deletes the placeholder. The default route (/0) is a stored value
// and needs no placement. (Router's routes resource does the same over route
// domains and next-hop references.)
class LpmTableResource final : public dataplane::Resource {
 public:
  LpmTableResource(std::string name, LpmRouteTable &table, uint32_t miss_value)
      : Resource(std::move(name)), table_(table), miss_value_(miss_value) {}

  static dataplane::ResourceKey Key(Ipv4Prefix prefix) {
    return dataplane::EncodeKey(uint64_t{prefix.addr()} << 8 | prefix.length());
  }

  size_t LiveCount() const override { return table_.size(); }

  bool Contains(const dataplane::ResourceKey &key) const override {
    const auto prefix = Decode(key);
    return prefix && Find(*prefix).has_value();
  }

  std::expected<Reservation, std::string> Reserve(const dataplane::Op &op) override {
    const auto prefix = Decode(op.key);
    if (!prefix) {
      return std::unexpected("not a route key of a valid prefix");
    }
    const std::optional<uint32_t> previous = Find(*prefix);
    if (op.kind == dataplane::OpKind::kErase) {
      if (!previous) {
        return std::unexpected("not found");
      }
      Reservation erase{std::make_unique<EraseOp>(table_, *prefix), {}};
      erase.existed = true;
      return erase;
    }
    const uint32_t *value = std::any_cast<uint32_t>(&op.value);
    if (value == nullptr) {
      return std::unexpected("wrong value type (want uint32_t)");
    }
    if (*value > LpmRouteTable::kMaxValue) {
      return std::unexpected(RouteErrorName(RouteError::kValueOutOfRange));
    }
    // Everything that can throw first; placing the prefix is the last step.
    auto upsert = std::make_unique<UpsertOp>(table_, *prefix, *value);
    Reservation reservation{nullptr, {}};
    reservation.existed = previous.has_value();
    if (!previous && prefix->length() != 0) {
      pending_.insert(*prefix);
      const uint32_t placeholder = table_.CoveringValue(*prefix).value_or(miss_value_);
      if (auto placed = table_.Upsert(*prefix, placeholder); !placed) {
        pending_.erase(*prefix);
        return std::unexpected(RouteErrorName(placed.error()));
      }
      upsert->MarkPlaced();  // Abort() now deletes it
    }
    reservation.staged = std::move(upsert);
    return reservation;
  }

  void EndTransaction() noexcept override { pending_.clear(); }

 private:
  static std::optional<Ipv4Prefix> Decode(const dataplane::ResourceKey &key) {
    uint64_t raw = 0;
    if (!dataplane::DecodeKey(key, &raw) || raw >> 40 != 0) {
      return std::nullopt;
    }
    const auto prefix = Ipv4Prefix::Make(static_cast<uint32_t>(raw >> 8),
                                         static_cast<uint8_t>(raw & 0xff));
    return prefix ? std::optional<Ipv4Prefix>(*prefix) : std::nullopt;
  }

  std::optional<uint32_t> Find(Ipv4Prefix prefix) const {
    return pending_.contains(prefix) ? std::nullopt : table_.Find(prefix);
  }

  class UpsertOp final : public dataplane::StagedOp {
   public:
    UpsertOp(LpmRouteTable &table, Ipv4Prefix prefix, uint32_t value)
        : table_(table), prefix_(prefix), value_(value) {}
    void MarkPlaced() noexcept { placed_ = true; }
    void Publish(dataplane::Retirer &) noexcept override {
      // Present (placed in Reserve(), already a route, or /0): an in-place
      // store, which cannot fail.
      auto set = table_.Upsert(prefix_, value_);
      CHECK(set) << RouteErrorName(set.error());
    }
    void Abort() noexcept override {
      if (placed_) {
        auto erased = table_.Erase(prefix_);
        CHECK(erased) << RouteErrorName(erased.error());
      }
    }

   private:
    LpmRouteTable &table_;
    Ipv4Prefix prefix_;
    uint32_t value_;
    bool placed_ = false;
  };

  class EraseOp final : public dataplane::StagedOp {
   public:
    EraseOp(LpmRouteTable &table, Ipv4Prefix prefix) : table_(table), prefix_(prefix) {}
    void Publish(dataplane::Retirer &) noexcept override {
      auto erased = table_.Erase(prefix_);
      CHECK(erased) << RouteErrorName(erased.error());
    }

   private:
    LpmRouteTable &table_;
    Ipv4Prefix prefix_;
  };

  LpmRouteTable &table_;
  const uint32_t miss_value_;
  std::set<Ipv4Prefix> pending_;  // placed in this transaction
};

}  // namespace bess::route

#endif  // BESS_ROUTE_LPM_RESOURCE_H_
