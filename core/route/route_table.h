// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_ROUTE_TABLE_H_
#define BESS_ROUTE_ROUTE_TABLE_H_

#include <atomic>
#include <cerrno>
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
#include <unordered_map>
#include <vector>

#include <rte_fib.h>
#include <rte_fib6.h>
#include <rte_lpm.h>
#include <rte_memory.h>

#include "arch/cpu.h"
#include "rcu/rcu_domain.h"
#include "rcu/rcu_ptr.h"
#include "utils/common.h"
#include "utils/dpdk_memory.h"
#include "utils/ip.h"

namespace bess::route {

enum class RouteError : uint8_t {
  kInvalidPrefixLength,  // > 32
  kHostBitsSet,          // address has bits beyond the prefix length
  kValueOutOfRange,      // route values are 24-bit (rte_lpm next-hop width;
                         // kept for rte_fib too, D-132)
  kTableFull,            // out of rules or tbl8 groups
  kNotFound,
  kBackendFailure,       // rte_lpm or rte_fib refused for another reason
  kInvalidId,            // zero or beyond the configured capacity
  kUnknownNextHop,       // a route names a next hop that does not exist
  kNextHopInUse,         // removing a next hop routes still reference
  kNextHopRetiring,      // reusing a removed next hop's id before readers are
                         // done with it (retry after a grace period)
  kEnrolled,             // the router is written through its transaction
                         // engine (Router::Enroll)
  kUnknownDomain,        // the route domain does not exist (never created,
                         // removed, or beyond the router's max_domains)
  kDomainExists,         // creating a route domain that already exists
  kDomainInUse,          // removing the default domain, or one with routes
  kUnsupportedBackend,   // the backend is not safe on this CPU (D-132)
  kFamilyDisabled,       // the address family is not enabled on the router
};

const char *RouteErrorName(RouteError error);

// The errno a module reports for a route error (the command/RPC error model).
inline int RouteErrno(RouteError error) {
  switch (error) {
    case RouteError::kTableFull:
      return ENOSPC;
    case RouteError::kNotFound:
    case RouteError::kUnknownDomain:
      return ENOENT;
    case RouteError::kDomainExists:
      return EEXIST;
    case RouteError::kBackendFailure:
      return EIO;
    case RouteError::kNextHopInUse:
    case RouteError::kNextHopRetiring:
    case RouteError::kEnrolled:
    case RouteError::kDomainInUse:
      return EBUSY;
    case RouteError::kUnsupportedBackend:
      return ENOTSUP;
    case RouteError::kFamilyDisabled:
      return EAFNOSUPPORT;
    default:
      return EINVAL;
  }
}

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

// A validated IPv6 prefix, stored as network-order bytes. Like Ipv4Prefix,
// only Make() builds one, so every prefix a table sees has length <= 128 and
// no host bits set.
class Ipv6Prefix {
 public:
  static std::expected<Ipv6Prefix, RouteError> Make(
      utils::Ipv6Address addr, uint8_t length);

  const utils::Ipv6Address &addr() const noexcept { return addr_; }
  uint8_t length() const noexcept { return length_; }
  friend auto operator<=>(const Ipv6Prefix &, const Ipv6Prefix &) = default;

 private:
  Ipv6Prefix(utils::Ipv6Address addr, uint8_t length)
      : addr_(addr), length_(length) {}

  utils::Ipv6Address addr_;
  uint8_t length_;
};

// Which DPDK longest-prefix-match structure holds a table's rules (D-132).
enum class RouteBackend : uint8_t {
  kRteLpm,  // rte_lpm (the default; D-003)
  kRteFib,  // rte_fib DIR24_8 with 4-byte next hops (deps/dpdk-patches)
};

// rte_fib writes its entries with plain stores (lib/fib/dir24_8.c
// write_to_fib), so a lock-free reader sees a new tbl8 group's entries before
// the tbl24 entry that points to it only where plain stores stay in program
// order (x86); rte_lpm uses release stores. Elsewhere the rte_fib backend is
// refused. (D-132)
inline constexpr bool RouteBackendSupported(RouteBackend backend) {
  return backend != RouteBackend::kRteFib || arch::kStoresInProgramOrder;
}

// IPv4 longest-prefix match on DPDK `rte_lpm` or `rte_fib` (K7, D-132),
// mapping a prefix to a 24-bit value. Untyped; RouteTable<Value> is the typed
// face. Decisions D-003 and D-132 (docs/decisions.md).
//
// DPDK owns the algorithm and the concurrency design: lookups are lock-free
// and run while a single writer adds or deletes rules in place -- every entry
// is written with one aligned store, a new tbl8 group is filled before the
// tbl24 entry that points to it is written, and freed tbl8 groups are
// recycled only after a QSBR grace period. BESS attaches the runtime's own
// RcuDomain QSBR (defer-queue mode) to either structure, so that grace period
// is the same worker quiescence K1 uses, and reclamation happens inside later
// writer calls with no blocking wait. (rte_lpm orders its stores with atomic
// release stores; rte_fib uses plain stores, which x86 keeps in order. D-132.)
//
// Why in place and not rebuild-and-swap like the K2/K3 generations: rte_lpm's
// build cost is quadratic in the rule count (entry 34: 5.8 us/route at 64K,
// 39.6 us/route and ~21 s total at 512K), so rebuilding per update does not
// scale, while one in-place add is a single rule insertion. The consequence,
// stated plainly: each route change is atomic to readers (a lookup sees the
// old or the new answer, never a mix or garbage), but a *sequence* of changes
// is not one atomic transaction. Wholesale replacement -- Clear(), or
// ReplaceAll() with a new rule set -- still builds a fresh table and publishes
// it through RcuPtr, since that is both faster than deleting rule by rule and
// atomic.
//
// The /0 route is kept beside the DPDK structure (rte_lpm's depth starts at
// 1; rte_fib keeps the same model) as an atomic default value.
//
// The backend is chosen per table (Config::backend) and fixed for its life.
// Readers branch on it once per Read()-batch call, never per packet in a
// batch.
//
// Writers are serialized internally. Readers call Read() once per batch and
// must be RcuDomain readers (workers are).
class LpmRouteTable {
 public:
  static constexpr uint32_t kMaxValue = (uint32_t{1} << 24) - 1;

  struct Config {
    uint32_t max_routes = 1024;
    uint32_t tbl8_groups = 256;  // one per /24 that has longer prefixes
    int socket = bess::utils::MemoryNode();  // DPDK socket id (D-123)
    RouteBackend backend = RouteBackend::kRteLpm;
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
  // default route is kept (what rte_lpm_delete_all meant).
  std::expected<void, RouteError> Clear();

  struct Rule {
    Ipv4Prefix prefix;
    uint32_t value;
  };

  // Replaces every route -- the default included -- with `rules` as one
  // publication. The replacement is built in a fresh rte_lpm beside the live
  // one, so every rule and tbl8 group it needs is placed (and a shortage
  // found) before anything is visible: on any error the live table is
  // untouched. Readers then see the old rule set or the new one, never a mix,
  // and the old table is retired through the RCU domain. A prefix named more
  // than once takes its last value. Cost is a table build (see above), not an
  // in-place update.
  std::expected<void, RouteError> ReplaceAll(std::span<const Rule> rules);

  // Control-side reads of the authoritative rule set (rte_lpm cannot be read
  // back, so the table keeps its own).
  std::optional<uint32_t> Find(Ipv4Prefix prefix) const;
  size_t size() const;
  const Config &config() const noexcept { return config_; }
  // Control-side enumeration, with the same writer lock as Find().
  template <typename Fn>
  void ForEach(Fn &&fn) const {
    std::lock_guard<std::mutex> lock(writer_mutex_);
    if (default_) {
      fn(*Ipv4Prefix::Make(0, 0), *default_);
    }
    for (const auto &[prefix, value] : rules_) {
      fn(prefix, value);
    }
  }


  // What readers get today for the addresses of `prefix` that no longer
  // rule covers: the value of the longest rule strictly containing it, else
  // the default route; nullopt for a miss. Adding `prefix` with this value
  // changes no lookup (a transaction places new routes so, D-023).
  std::optional<uint32_t> CoveringValue(Ipv4Prefix prefix) const;

  // -- reader -----------------------------------------------------------------

  class View;

  // One acquire load; valid until the calling worker's next quiescent state.
  View Read() const noexcept;

  // Largest batch LookupBatch() accepts: the result is a 64-bit mask.
  static constexpr size_t kMaxBatch = 64;

  // Stored in Instance::default_value when no default route is set.
  static constexpr uint32_t kNoDefault = UINT32_MAX;

 private:
  // rte_fib's answer for an address no rule covers: above every value, so it
  // fits rte_fib's 4-byte next hops (at most 2^31 - 1) and is never a route.
  static constexpr uint32_t kFibMiss = kMaxValue + 1;

  // One DPDK structure (exactly one of `lpm` and `fib`) and the default route
  // that goes with it.
  struct Instance {
    ~Instance();
    struct rte_lpm *lpm = nullptr;
    struct rte_fib *fib = nullptr;
    std::atomic<uint32_t> default_value{kNoDefault};
  };

  LpmRouteTable(std::string name, const Config &config, rcu::RcuDomain &domain)
      : name_(std::move(name)), config_(config), domain_(domain),
        live_(domain) {}

  std::expected<std::unique_ptr<Instance>, RouteError> NewInstance(
      uint32_t default_value);
  // The backend's add and delete, with its return codes (0 or -errno).
  static int AddRule(Instance &instance, Ipv4Prefix prefix, uint32_t value);
  static int DeleteRule(Instance &instance, Ipv4Prefix prefix);
  // Rules longer than /24, per /24 block.
  using FibBlocks = std::unordered_map<uint32_t, uint32_t>;
  // rte_fib does not hold rte_lpm's limits exactly: it rounds its tbl8 pool
  // up to a multiple of 64 groups, and its rib takes up to 2 x max_routes
  // nodes. With that backend Config's limits are enforced here instead, the
  // way rte_lpm counts them: `routes` rules at most, and one tbl8 group per
  // /24 block holding a rule longer than /24. False when adding `prefix` to
  // `routes` rules and `blocks` would exceed them. (D-132)
  bool FibAdmits(const FibBlocks &blocks, size_t routes,
                 Ipv4Prefix prefix) const;

  const std::string name_;
  const Config config_;
  rcu::RcuDomain &domain_;
  rcu::RcuPtr<Instance> live_;

  mutable std::mutex writer_mutex_;
  std::map<Ipv4Prefix, uint32_t> rules_;  // /0 is not in here
  std::optional<uint32_t> default_;
  // rte_fib only: the blocks FibAdmits counts.
  FibBlocks fib_blocks_;
};

class LpmRouteTable::View {
 public:
  bool valid() const noexcept { return instance_ != nullptr; }

  // The value for `addr` (host order), falling back to the default route, or
  // kNoDefault (above every value) for a miss: Lookup() without the optional,
  // for per-packet callers.
  uint32_t LookupOrMiss(uint32_t addr) const noexcept {
    if (instance_->fib != nullptr) {
      uint64_t nh;
      rte_fib_lookup_bulk(instance_->fib, &addr, &nh, 1);
      if (nh != kFibMiss) {
        return static_cast<uint32_t>(nh);
      }
      return instance_->default_value.load(std::memory_order_relaxed);
    }
    uint32_t value;
    if (rte_lpm_lookup(instance_->lpm, addr, &value) == 0) {
      return value;
    }
    return instance_->default_value.load(std::memory_order_relaxed);
  }

  // The value for `addr` (host order), falling back to the default route.
  std::optional<uint32_t> Lookup(uint32_t addr) const noexcept {
    const uint32_t value = LookupOrMiss(addr);
    return value == kNoDefault ? std::nullopt : std::optional<uint32_t>(value);
  }

  // Looks up every address (host order), writing `values[i]`. Returns a mask
  // with bit i set where a route (or the default) matched; `values` at other
  // positions is unspecified. Equal sizes, at most kMaxBatch (preconditions).
  uint64_t LookupBatch(std::span<const uint32_t> addrs,
                       std::span<uint32_t> values) const noexcept;

 private:
  uint64_t LookupBatchFib(std::span<const uint32_t> addrs,
                          std::span<uint32_t> values,
                          uint32_t def) const noexcept;

  friend class LpmRouteTable;
  explicit View(const Instance *instance) : instance_(instance) {}

  const Instance *instance_;
};

inline uint64_t LpmRouteTable::View::LookupBatch(
    std::span<const uint32_t> addrs, std::span<uint32_t> values) const noexcept {
  promise(addrs.size() == values.size() && addrs.size() <= kMaxBatch);
  const uint32_t def = instance_->default_value.load(std::memory_order_relaxed);
  uint64_t hits = 0;
  if (instance_->fib != nullptr) {
    hits = LookupBatchFib(addrs, values, def);
  } else {
    const struct rte_lpm *lpm = instance_->lpm;
    for (size_t i = 0; i < addrs.size(); i++) {
      uint32_t v;
      const uint32_t hit = rte_lpm_lookup(lpm, addrs[i], &v) == 0;
      hits |= uint64_t{hit} << i;
      values[i] = hit ? v : def;
    }
  }
  if (def != kNoDefault) {
    hits = addrs.size() == 64 ? ~uint64_t{0}
                              : (uint64_t{1} << addrs.size()) - 1;
  }
  return hits;
}

// One rte_fib_lookup_bulk() for the batch: rte_fib's lookup is reached through
// a function pointer (scalar, or AVX-512 where the CPU has it), so one call
// per batch, not per address.
inline uint64_t LpmRouteTable::View::LookupBatchFib(
    std::span<const uint32_t> addrs, std::span<uint32_t> values,
    uint32_t def) const noexcept {
  uint64_t nh[kMaxBatch];
  // rte_fib_lookup_bulk() only reads the addresses; its parameter is not const.
  rte_fib_lookup_bulk(instance_->fib, const_cast<uint32_t *>(addrs.data()), nh,
                      static_cast<int>(addrs.size()));
  uint64_t hits = 0;
  for (size_t i = 0; i < addrs.size(); i++) {
    const bool hit = nh[i] != kFibMiss;
    hits |= uint64_t{hit} << i;
    values[i] = hit ? static_cast<uint32_t>(nh[i]) : def;
  }
  return hits;
}

inline LpmRouteTable::View LpmRouteTable::Read() const noexcept {
  return View(live_.Read());
}

// One route domain's IPv6 routes on DPDK rte_fib6 (TRIE), mapping a prefix to
// a value no larger than Config::max_value. Lookups are lock-free and batch
// through one rte_fib6_lookup_bulk(); ::/0 is kept beside the trie as each
// instance's default value, as LpmRouteTable keeps it.
//
// Why two instances and not in-place updates. rte_fib6's trie (unpatched DPDK)
// cannot change a rule in place without possibly allocating: rewriting a rule
// takes a transient tbl8 group (a byte-aligned prefix descends one level past
// itself, a prefix of /24 or shorter one level on each edge) and folds it back,
// and a rule added with its parent's value takes its groups on a later change
// -- when the pool may be empty while freed groups wait for a grace period. A
// change or a delete at publication could then fail half written. So a table
// owns two instances: `live`, which readers use through RCU and nobody edits,
// and a `spare`, never visible while it is edited. A change edits the spare
// (all allocation and every capacity check happen there) and publication is
// one pointer exchange: the spare becomes live, and the old live becomes the
// spare once a grace period has passed, remembering which prefixes it lags
// behind the rules by; the next change replays those (or rebuilds the spare
// from the rules when its grace period has not completed, retiring it, or a
// replay fails). No QSBR is attached to the instances: one is edited only
// while unpublished and after its grace period, so the trie may recycle tbl8
// groups at once. Readers see the whole old table or the whole new one.
//
// Entries are 2 bytes when both `max_value` + 1 (the miss marker) and
// `tbl8_groups` fit rte_fib6's 2-byte next hop (2^15 - 1: entries hold value
// << 1, and trie_create refuses more tbl8 groups than a next hop can name),
// else 4 bytes. Memory per instance: tbl24 is 2^24 entries (32 MiB at 2 bytes,
// 64 MiB at 4) plus `tbl8_groups` x 256 entries (4 MiB / 8 MiB for the default
// 8192 groups) plus the rib's 2 x max_routes nodes. A table holds one instance
// until its first change and two after; a spare retired before its grace
// period ended lives until the grace period does.
//
// Two ways to write. Upsert()/Erase() are one-change transactions (the direct
// API). A Stage composes several: BeginStage() readies the spare, the Stage's
// own Upsert()/Erase() edit it, and Commit() publishes it -- no allocation,
// cannot fail -- or Abort() drops the edits. Writers are serialized
// internally and only one stage is open at a time. Readers call Read() once
// per batch and must be RcuDomain readers (workers are).
class Ipv6RouteTable {
 public:
  struct Config {
    uint32_t max_routes = 1024;  // prefixes other than ::/0
    uint32_t tbl8_groups = 8192;  // per instance; see the class comment
    // The largest value the owner stores (at most LpmRouteTable::kMaxValue):
    // it decides the entry size.
    uint32_t max_value = LpmRouteTable::kMaxValue;
    int socket = bess::utils::MemoryNode();  // DPDK socket id (D-123)
  };

  static constexpr size_t kMaxBatch = LpmRouteTable::kMaxBatch;

  // The bytes per trie entry `config` gets: 2 or 4 (see the class comment).
  static size_t EntryBytes(const Config &config) noexcept;

  class Stage;
  class View;

  // kValueOutOfRange for a max_value past LpmRouteTable::kMaxValue;
  // kTableFull when DPDK has no memory for the instance.
  static std::expected<std::unique_ptr<Ipv6RouteTable>, RouteError> Create(
      std::string name, const Config &config, rcu::RcuDomain &domain);

  // The owner guarantees no reader still uses the table and no stage is open.
  ~Ipv6RouteTable();

  Ipv6RouteTable(const Ipv6RouteTable &) = delete;
  Ipv6RouteTable &operator=(const Ipv6RouteTable &) = delete;

  // -- direct writer (control threads) ---------------------------------------

  // Adds `prefix` or changes its value, published at once. kValueOutOfRange
  // past max_value; kTableFull for a new prefix past max_routes or tbl8
  // groups the spare lacks (the live table is then unchanged).
  std::expected<void, RouteError> Upsert(Ipv6Prefix prefix, uint32_t value);
  std::expected<void, RouteError> Erase(Ipv6Prefix prefix);

  // Control-side reads of the committed rules (::/0 included).
  std::optional<uint32_t> Find(Ipv6Prefix prefix) const;
  size_t size() const;
  template <typename Fn>
  void ForEach(Fn &&fn) const {
    std::lock_guard<std::mutex> lock(writer_mutex_);
    for (const auto &[prefix, value] : rules_) {
      fn(prefix, value);
    }
  }
  const Config &config() const noexcept { return config_; }
  size_t entry_bytes() const noexcept { return entry_bytes_; }
  // rte_fib6 instances built so far: the first live one, then a spare
  // whenever none could be reused (see the class comment).
  size_t builds() const;

  // -- transactions ------------------------------------------------------------

  // Readies the spare and opens a stage on it. All allocation and capacity
  // work happens here and in the Stage's edits; nothing is visible until
  // Commit(). One stage at a time.
  std::expected<std::unique_ptr<Stage>, RouteError> BeginStage();
  // Publishes `stage`: one pointer exchange. Does not allocate and cannot
  // fail. The replaced instance is not edited again until Committed() hands
  // the grace period that covers it.
  void Commit(Stage &stage) noexcept;
  // Drops `stage`'s edits; readers never saw them.
  void Abort(Stage &stage) noexcept;
  // The grace period started after the last Commit(); called once per
  // Commit, before the next BeginStage (Retirer::AfterGracePeriodStarts). A
  // BeginStage that comes first starts one itself.
  void Committed(rcu::GracePeriod token) noexcept;

  // -- reader ------------------------------------------------------------------

  // One acquire load; valid until the calling worker's next quiescent state.
  View Read() const noexcept;

 private:
  using Rules = std::map<Ipv6Prefix, uint32_t>;

  // One rte_fib6 and the default route that goes with it. Readers see it
  // only while it is live, and it is edited only while it is not.
  struct Instance {
    ~Instance();
    struct rte_fib6 *fib = nullptr;
    uint32_t default_value = LpmRouteTable::kNoDefault;
    uint32_t miss = 0;  // rte_fib6's answer where no rule matches
  };

  Ipv6RouteTable(std::string name, const Config &config, rcu::RcuDomain &domain)
      : name_(std::move(name)),
        config_(config),
        entry_bytes_(EntryBytes(config)),
        domain_(domain),
        live_(domain) {}

  // With writer_mutex_ held.
  std::expected<std::unique_ptr<Instance>, RouteError> NewInstance();
  std::expected<std::unique_ptr<Instance>, RouteError> Build();
  bool Replay(Instance &instance);
  std::expected<void, RouteError> PrepareSpare();
  std::expected<std::unique_ptr<Stage>, RouteError> BeginStageLocked();
  void CommitLocked(Stage &stage) noexcept;
  void AbortLocked(Stage &stage) noexcept;
  size_t RoutesLocked() const noexcept;
  // rte_fib6_add (a value) or rte_fib6_delete (none), or the default for
  // ::/0. 0 or -errno; deleting an absent rule is 0.
  static int Apply(Instance &instance, Ipv6Prefix prefix,
                   std::optional<uint32_t> value);

  const std::string name_;
  const Config config_;
  const size_t entry_bytes_;
  rcu::RcuDomain &domain_;
  rcu::RcuPtr<Instance> live_;

  mutable std::mutex writer_mutex_;
  Rules rules_;  // ::/0 included
  std::unique_ptr<Instance> spare_;
  // The spare's reuse token: unset from a Commit until Committed(). Not used
  // while the spare has never been published.
  std::optional<rcu::GracePeriod> spare_token_;
  // The spare was built and never published: no reader has seen it, so it is
  // editable without a grace period (an aborted stage leaves it so).
  bool spare_unpublished_ = false;
  // Prefixes where the spare may differ from rules_.
  std::vector<Ipv6Prefix> spare_lag_;
  bool stage_open_ = false;
  size_t builds_ = 0;
};

class Ipv6RouteTable::Stage {
 public:
  // Aborts a stage neither committed nor aborted.
  ~Stage();
  Stage(const Stage &) = delete;
  Stage &operator=(const Stage &) = delete;

  // kValueOutOfRange past max_value; kTableFull for a new prefix past
  // max_routes or tbl8 groups the spare lacks. A refused DPDK edit leaves the
  // stage unusable (kBackendFailure for every later edit): abort it.
  std::expected<void, RouteError> Upsert(Ipv6Prefix prefix, uint32_t value);
  // kNotFound for a prefix the staged table does not hold.
  std::expected<void, RouteError> Erase(Ipv6Prefix prefix);
  // The staged table's value for `prefix`.
  std::optional<uint32_t> Find(Ipv6Prefix prefix) const;

 private:
  friend class Ipv6RouteTable;

  struct Change {
    std::optional<uint32_t> value;  // none: erased
    // For a prefix the committed rules lack: its rules node, made here so
    // Commit() only links it.
    Rules::node_type node;
  };

  explicit Stage(Ipv6RouteTable &table) : table_(table) {}

  // With the table's writer_mutex_ held.
  std::expected<void, RouteError> UpsertLocked(Ipv6Prefix prefix,
                                               uint32_t value);
  std::expected<void, RouteError> EraseLocked(Ipv6Prefix prefix);
  std::optional<uint32_t> FindLocked(Ipv6Prefix prefix) const;

  Ipv6RouteTable &table_;
  std::map<Ipv6Prefix, Change> changes_;
  std::vector<Ipv6Prefix> touched_;  // changes_' keys: the spare's next lag
  // Room for the committed nodes Commit() unlinks; freed with the stage.
  std::vector<Rules::node_type> erased_;
  size_t routes_ = 0;  // prefixes other than ::/0 in the staged table
  bool broken_ = false;  // a DPDK edit failed: the spare is unusable
  bool done_ = true;  // committed, aborted, or not yet opened
};

class Ipv6RouteTable::View {
 public:
  // The value for `addr`, falling back to the default route, or
  // LpmRouteTable::kNoDefault for a miss.
  uint32_t LookupOrMiss(const utils::Ipv6Address &addr) const noexcept {
    uint64_t nh;
    rte_fib6_lookup_bulk(instance_->fib, AsDpdk(&addr), &nh, 1);
    return nh != instance_->miss ? static_cast<uint32_t>(nh)
                                 : instance_->default_value;
  }

  std::optional<uint32_t> Lookup(const utils::Ipv6Address &addr) const noexcept {
    const uint32_t value = LookupOrMiss(addr);
    return value == LpmRouteTable::kNoDefault ? std::nullopt
                                              : std::optional<uint32_t>(value);
  }

  // One rte_fib6_lookup_bulk() for the batch (the lookup is reached through a
  // function pointer, AVX-512 where the CPU has it). Writes `values[i]` and
  // returns a mask with bit i set where a route (or the default) matched;
  // other positions are unspecified. Equal sizes, at most kMaxBatch.
  uint64_t LookupBatch(std::span<const utils::Ipv6Address> addrs,
                       std::span<uint32_t> values) const noexcept {
    promise(addrs.size() == values.size() && addrs.size() <= kMaxBatch);
    uint64_t nh[kMaxBatch];
    rte_fib6_lookup_bulk(instance_->fib, AsDpdk(addrs.data()), nh,
                         static_cast<int>(addrs.size()));
    const uint32_t def = instance_->default_value;
    uint64_t hits = 0;
    for (size_t i = 0; i < addrs.size(); i++) {
      const bool hit = nh[i] != instance_->miss;
      hits |= uint64_t{hit} << i;
      values[i] = hit ? static_cast<uint32_t>(nh[i]) : def;
    }
    if (def != LpmRouteTable::kNoDefault) {
      hits = addrs.size() == 64 ? ~uint64_t{0}
                                : (uint64_t{1} << addrs.size()) - 1;
    }
    return hits;
  }

 private:
  friend class Ipv6RouteTable;
  explicit View(const Instance *instance) : instance_(instance) {}

  // The same 16 network-order bytes.
  static_assert(sizeof(utils::Ipv6Address) == sizeof(struct rte_ipv6_addr) &&
                alignof(utils::Ipv6Address) == alignof(struct rte_ipv6_addr));
  static const struct rte_ipv6_addr *AsDpdk(
      const utils::Ipv6Address *addr) noexcept {
    return reinterpret_cast<const struct rte_ipv6_addr *>(addr);
  }

  const Instance *instance_;
};

inline Ipv6RouteTable::View Ipv6RouteTable::Read() const noexcept {
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
  template <typename Fn>
  void ForEach(Fn &&fn) const {
    impl_->ForEach([&](Ipv4Prefix prefix, uint32_t value) {
      fn(prefix, Wrap(value));
    });
  }


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
