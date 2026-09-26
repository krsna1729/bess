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

#ifndef BESS_ROUTE_ROUTE_TABLE_H_
#define BESS_ROUTE_ROUTE_TABLE_H_

#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>

#include <rte_lpm.h>
#include <rte_memory.h>

#include "rcu/rcu_domain.h"
#include "rcu/rcu_ptr.h"
#include "utils/common.h"

namespace bess::route {

enum class RouteError : uint8_t {
  kInvalidPrefixLength,  // > 32
  kHostBitsSet,          // address has bits beyond the prefix length
  kValueOutOfRange,      // route values are 24-bit (rte_lpm next-hop width)
  kTableFull,            // out of rules or tbl8 groups
  kNotFound,
  kBackendFailure,       // rte_lpm refused for another reason
  kInvalidId,            // zero or beyond the configured capacity
  kUnknownNextHop,       // a route names a next hop that does not exist
  kNextHopInUse,         // removing a next hop routes still reference
  kNextHopRetiring,      // reusing a removed next hop's id before readers are
                         // done with it (retry after a grace period)
};

const char *RouteErrorName(RouteError error);

// A validated IPv4 prefix, address in host byte order (`be32_t::value()`).
class Ipv4Prefix {
 public:
  static std::expected<Ipv4Prefix, RouteError> Make(uint32_t addr,
                                                    uint8_t length);

  uint32_t addr() const noexcept { return addr_; }
  uint8_t length() const noexcept { return length_; }

  friend auto operator<=>(const Ipv4Prefix &, const Ipv4Prefix &) = default;

 private:
  Ipv4Prefix(uint32_t addr, uint8_t length) : addr_(addr), length_(length) {}

  uint32_t addr_;
  uint8_t length_;
};

// IPv4 longest-prefix match on DPDK `rte_lpm` (K7), mapping a prefix to a
// 24-bit value. Untyped; RouteTable<Value> is the typed face. Decision D-003
// (docs/decisions.md).
//
// DPDK owns the algorithm and the concurrency design: lookups are lock-free
// and run while a single writer adds or deletes rules in place -- every entry
// is written with an atomic store, a new tbl8 group is fully populated before
// the tbl24 entry that points to it is published, and freed tbl8 groups are
// recycled only after a QSBR grace period. BESS attaches the runtime's own
// RcuDomain QSBR (defer-queue mode), so that grace period is the same worker
// quiescence K1 uses, and reclamation happens inside later writer calls with
// no blocking wait.
//
// Why in place and not rebuild-and-swap like the K2/K3 generations: rte_lpm's
// build cost is quadratic in the rule count (entry 34: 5.8 us/route at 64K,
// 39.6 us/route and ~21 s total at 512K), so rebuilding per update does not
// scale, while one in-place add is a single rule insertion. The consequence,
// stated plainly: each route change is atomic to readers (a lookup sees the
// old or the new answer, never a mix or garbage), but a *sequence* of changes
// is not one atomic transaction. Wholesale replacement -- Clear() -- still
// builds a fresh table and publishes it through RcuPtr, since that is both
// faster than deleting rule by rule and atomic.
//
// The /0 route is kept beside rte_lpm (whose depth starts at 1) as an atomic
// default value.
//
// Writers are serialized internally. Readers call Read() once per batch and
// must be RcuDomain readers (workers are).
class LpmRouteTable {
 public:
  static constexpr uint32_t kMaxValue = (uint32_t{1} << 24) - 1;

  struct Config {
    uint32_t max_routes = 1024;
    uint32_t tbl8_groups = 256;  // one per /24 that has longer prefixes
    int socket = SOCKET_ID_ANY;
  };

  static std::expected<std::unique_ptr<LpmRouteTable>, RouteError> Create(
      std::string name, const Config &config, rcu::RcuDomain &domain);

  ~LpmRouteTable();

  LpmRouteTable(const LpmRouteTable &) = delete;
  LpmRouteTable &operator=(const LpmRouteTable &) = delete;

  // -- writer (control threads; serialized internally) ----------------------

  // Adds `prefix`, or changes its value if it exists.
  std::expected<void, RouteError> Upsert(Ipv4Prefix prefix, uint32_t value);
  std::expected<void, RouteError> Erase(Ipv4Prefix prefix);

  // Removes every non-default route by publishing a fresh empty table; the
  // default route is kept (what rte_lpm_delete_all meant for IPLookup).
  std::expected<void, RouteError> Clear();

  // Control-side reads of the authoritative rule set (rte_lpm cannot be read
  // back, so the table keeps its own).
  std::optional<uint32_t> Find(Ipv4Prefix prefix) const;
  size_t size() const;
  const Config &config() const noexcept { return config_; }

  // -- reader -----------------------------------------------------------------

  class View;

  // One acquire load; valid until the calling worker's next quiescent state.
  View Read() const noexcept;

  // Largest batch LookupBatch() accepts: the result is a 64-bit mask.
  static constexpr size_t kMaxBatch = 64;

  // Stored in Instance::default_value when no default route is set.
  static constexpr uint32_t kNoDefault = UINT32_MAX;

 private:
  // One rte_lpm and the default route that goes with it.
  struct Instance {
    ~Instance();
    struct rte_lpm *lpm = nullptr;
    std::atomic<uint32_t> default_value{kNoDefault};
  };

  LpmRouteTable(std::string name, const Config &config, rcu::RcuDomain &domain)
      : name_(std::move(name)), config_(config), domain_(domain),
        live_(domain) {}

  std::expected<std::unique_ptr<Instance>, RouteError> NewInstance(
      uint32_t default_value);

  const std::string name_;
  const Config config_;
  rcu::RcuDomain &domain_;
  rcu::RcuPtr<Instance> live_;

  mutable std::mutex writer_mutex_;
  std::map<Ipv4Prefix, uint32_t> rules_;  // /0 is not in here
  std::optional<uint32_t> default_;
};

class LpmRouteTable::View {
 public:
  bool valid() const noexcept { return instance_ != nullptr; }

  // The value for `addr` (host order), falling back to the default route.
  std::optional<uint32_t> Lookup(uint32_t addr) const noexcept {
    uint32_t value;
    if (rte_lpm_lookup(instance_->lpm, addr, &value) == 0) {
      return value;
    }
    const uint32_t def =
        instance_->default_value.load(std::memory_order_relaxed);
    return def == kNoDefault ? std::nullopt : std::optional<uint32_t>(def);
  }

  // Looks up every address (host order), writing `values[i]`. Returns a mask
  // with bit i set where a route (or the default) matched; `values` at other
  // positions is unspecified (it holds rte_lpm's raw miss entry).
  // Equal sizes, at most kMaxBatch (preconditions).
  uint64_t LookupBatch(std::span<const uint32_t> addrs,
                       std::span<uint32_t> values) const noexcept;

 private:
  friend class LpmRouteTable;
  explicit View(const Instance *instance) : instance_(instance) {}

  const Instance *instance_;
};

inline uint64_t LpmRouteTable::View::LookupBatch(
    std::span<const uint32_t> addrs, std::span<uint32_t> values) const noexcept {
  promise(addrs.size() == values.size() && addrs.size() <= kMaxBatch);
  const uint32_t def = instance_->default_value.load(std::memory_order_relaxed);
  const struct rte_lpm *lpm = instance_->lpm;
  uint64_t hits = 0;
  for (size_t i = 0; i < addrs.size(); i++) {
    uint32_t v;
    const uint32_t hit = rte_lpm_lookup(lpm, addrs[i], &v) == 0;
    hits |= uint64_t{hit} << i;
    values[i] = hit ? v : def;
  }
  if (def != kNoDefault) {
    hits = addrs.size() == 64 ? ~uint64_t{0}
                              : (uint64_t{1} << addrs.size()) - 1;
  }
  return hits;
}

inline LpmRouteTable::View LpmRouteTable::Read() const noexcept {
  return View(live_.Read());
}

// The typed face: `Value` is a StrongId-like type (value() and an explicit
// constructor from its representation) whose values fit in 24 bits.
template <typename Value>
class RouteTable {
 public:
  using Config = LpmRouteTable::Config;

  static std::expected<std::unique_ptr<RouteTable>, RouteError> Create(
      std::string name, const Config &config, rcu::RcuDomain &domain) {
    auto table = LpmRouteTable::Create(std::move(name), config, domain);
    if (!table) {
      return std::unexpected(table.error());
    }
    return std::unique_ptr<RouteTable>(new RouteTable(std::move(*table)));
  }

  std::expected<void, RouteError> Upsert(Ipv4Prefix prefix, Value value) {
    if (value.value() > LpmRouteTable::kMaxValue) {
      return std::unexpected(RouteError::kValueOutOfRange);
    }
    return impl_->Upsert(prefix, static_cast<uint32_t>(value.value()));
  }
  std::expected<void, RouteError> Erase(Ipv4Prefix prefix) {
    return impl_->Erase(prefix);
  }
  std::expected<void, RouteError> Clear() { return impl_->Clear(); }

  std::optional<Value> Find(Ipv4Prefix prefix) const {
    const auto v = impl_->Find(prefix);
    return v ? std::optional<Value>(Wrap(*v)) : std::nullopt;
  }
  size_t size() const { return impl_->size(); }

  class View {
   public:
    std::optional<Value> Lookup(uint32_t addr) const noexcept {
      const auto v = view_.Lookup(addr);
      return v ? std::optional<Value>(Wrap(*v)) : std::nullopt;
    }
    uint64_t LookupBatch(std::span<const uint32_t> addrs,
                         std::span<uint32_t> raw_values) const noexcept {
      return view_.LookupBatch(addrs, raw_values);
    }
    static Value Wrap(uint32_t raw) noexcept { return RouteTable::Wrap(raw); }

   private:
    friend class RouteTable;
    explicit View(LpmRouteTable::View view) : view_(view) {}
    LpmRouteTable::View view_;
  };

  View Read() const noexcept { return View(impl_->Read()); }

  LpmRouteTable &untyped() noexcept { return *impl_; }

 private:
  explicit RouteTable(std::unique_ptr<LpmRouteTable> impl)
      : impl_(std::move(impl)) {}

  static Value Wrap(uint32_t raw) noexcept {
    return Value(static_cast<typename Value::rep_type>(raw));
  }

  std::unique_ptr<LpmRouteTable> impl_;
};

}  // namespace bess::route

#endif  // BESS_ROUTE_ROUTE_TABLE_H_
