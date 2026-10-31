// SPDX-License-Identifier: BSD-3-Clause

#include "route/route_table.h"

#include <rte_eal.h>
#include <rte_errno.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "utils/logging.h"

#include "dpdk.h"

namespace bess::route {

const char *RouteErrorName(RouteError error) {
  switch (error) {
    case RouteError::kInvalidPrefixLength:
      return "prefix length exceeds the address-family limit";
    case RouteError::kHostBitsSet:
      return "address has bits set beyond the prefix length";
    case RouteError::kValueOutOfRange:
      return "route value does not fit in 24 bits";
    case RouteError::kTableFull:
      return "route table is full (rules or tbl8 groups)";
    case RouteError::kNotFound:
      return "no such route";
    case RouteError::kBackendFailure:
      return "route backend (rte_lpm or rte_fib) failure";
    case RouteError::kInvalidId:
      return "id is zero or beyond the configured capacity";
    case RouteError::kUnknownNextHop:
      return "no such next hop";
    case RouteError::kNextHopInUse:
      return "next hop is still referenced by routes";
    case RouteError::kEnrolled:
      return "the router is enrolled in transactions: write it through them";
    case RouteError::kNextHopRetiring:
      return "next hop retiring";
    case RouteError::kUnknownDomain:
      return "no such route domain";
    case RouteError::kDomainExists:
      return "route domain already exists";
    case RouteError::kDomainInUse:
      return "route domain is the default or still has routes";
    case RouteError::kUnsupportedBackend:
      return "the rte_fib route backend needs x86-64 (D-132)";
    case RouteError::kFamilyDisabled:
      return "the address family is not enabled on this router";
  }
  return "unknown route error";
}

std::expected<Ipv4Prefix, RouteError> Ipv4Prefix::Make(uint32_t addr,
                                                        uint8_t length) {
  if (length > 32) {
    return std::unexpected(RouteError::kInvalidPrefixLength);
  }
  const uint32_t mask = length == 0 ? 0 : ~uint32_t{0} << (32 - length);
  if ((addr & ~mask) != 0) {
    return std::unexpected(RouteError::kHostBitsSet);
  }
  return Ipv4Prefix(addr, length);
}

std::expected<Ipv6Prefix, RouteError> Ipv6Prefix::Make(
    utils::Ipv6Address addr, uint8_t length) {
  if (length > 128) {
    return std::unexpected(RouteError::kInvalidPrefixLength);
  }
  for (size_t bit = length; bit < 128; bit++) {
    if ((addr.bytes[bit / 8] & (uint8_t{1} << (7 - bit % 8))) != 0) {
      return std::unexpected(RouteError::kHostBitsSet);
    }
  }
  return Ipv6Prefix(addr, length);
}

LpmRouteTable::Instance::~Instance() {
  // Either free also drains the structure's QSBR defer queue.
  if (lpm != nullptr) {
    rte_lpm_free(lpm);
  }
  if (fib != nullptr) {
    rte_fib_free(fib);
  }
}

namespace {

// rte_lpm, rte_fib and rte_fib6 names are global to the EAL, and a replacement
// instance exists alongside the one it replaces, so the counter suffix must
// survive every name DPDK derives from it. The tightest are the defer-queue
// rings ("LPM_RCU_<name>", "FIB_RCU_<name>", cut to RTE_RCU_QSBR_DQ_NAMESIZE -
// 1 = 28 characters) and the RIB mempools of rte_fib and rte_fib6 ("MP_<name>",
// at most 25): 20.
constexpr size_t kMaxInstanceName = 20;

std::string InstanceName(const std::string &base) {
  static std::atomic<uint64_t> next{0};
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "_r%lu",
           static_cast<unsigned long>(next.fetch_add(1)));
  const size_t room = kMaxInstanceName - strlen(suffix);
  return base.substr(0, room) + suffix;
}

// A DPDK routing structure under the memory policy (D-029, D-206):
// `create(socket)` makes one of about `bytes`. Under --no-huge the bytes are
// admitted against the process memory limit first. Created where this
// thread's admission placed it (utils::PlacementSocket), and a creation that
// fails for lack of memory (rte_errno ENOMEM) on `socket` is retried once on
// the normal-page fallback heap, which utils::FallbackSocket grants or
// refuses (MemoryRefused, propagated to the control boundary); where it
// landed is recorded against the admission. Other failures return nullptr
// with rte_errno as the creator left it.
template <typename Create>
auto CreateOnPolicy(int socket, size_t bytes, Create create) {
  if (rte_eal_has_hugepages() == 0) {
    utils::AdmitNormalMemory(bytes);
  }
  socket = utils::PlacementSocket(socket, bytes);
  // Under the heap lock: the structure may land on "bess-normal"
  // (utils::HeapGuard).
  auto guarded = [&create](int on) {
    const utils::HeapGuard guard;
    return create(on);
  };
  rte_errno = 0;
  auto *made = guarded(socket);
  if (made == nullptr && rte_errno == ENOMEM) {
    socket = utils::FallbackSocket(bytes);
    made = guarded(socket);
  }
  if (made != nullptr) {
    utils::NotePlacement(bytes, socket);
  }
  return made;
}

// The estimates below are the allocations DPDK 25.11 makes on the socket
// asked for: a 2^24-entry tbl24 of the entry size, tbl8 groups of 256
// entries, and the bookkeeping of each library. The RCU defer queues, small
// and allocated on the calling thread's native socket by DPDK whatever the
// table's socket (an ENOMEM there is utils::RefuseDpdkMemory), are not
// counted; a queue entry is about kDeferEntryBytes (an 8-byte token and the
// 4- or 8-byte element, ring-aligned).
constexpr size_t kDeferEntryBytes = 16;
constexpr size_t kTbl24Entries = size_t{1} << 24;

// rte_lpm: tbl24 and tbl8s of 4-byte entries, 8 bytes per rule.
size_t LpmBytes(const rte_lpm_config &conf) {
  return kTbl24Entries * 4 + size_t{256} * 4 * conf.number_tbl8s +
         size_t{8} * conf.max_rules;
}

// rte_fib DIR24_8 (4-byte entries): tbl24, the tbl8 pool (rounded to 64
// groups, plus one), its bitmap, the deep24 bitmap (one bit per tbl24 entry),
// and the RIB's node mempool: one cache line per route (a 40-byte node and
// its mempool header).
size_t FibBytes(const rte_fib_conf &conf) {
  const size_t tbl8s = (size_t{conf.dir24_8.num_tbl8} + 63) / 64 * 64;
  return kTbl24Entries * 4 + size_t{256} * 4 * (tbl8s + 1) + tbl8s / 8 +
         kTbl24Entries / 8 + size_t{64} * static_cast<size_t>(conf.max_routes);
}

// rte_fib6 TRIE (2- or 4-byte entries): tbl24, the tbl8 pool (plus one), its
// free list (4 bytes per group), and the RIB6 node mempool: two cache lines
// per route (a 50-byte node and its mempool header).
size_t Fib6Bytes(const rte_fib6_conf &conf, size_t entry_bytes) {
  return kTbl24Entries * entry_bytes +
         size_t{256} * entry_bytes * (size_t{conf.trie.num_tbl8} + 1) +
         size_t{4} * conf.trie.num_tbl8 +
         size_t{128} * static_cast<size_t>(conf.max_routes);
}

// A replacement built beside what it replaces (D-198): `bytes` admitted
// through `growth` (when set) for the scope, refused by a throw before
// anything is built, and given back when the scope ends -- the
// replacement's on failure, or, once it is published, the replaced
// instance's, retired to the RCU domain (which takes no callback, so its
// bytes are given back at retirement, not when the grace period frees it).
class ScopedGrowth {
 public:
  ScopedGrowth(utils::MemoryGrowth *growth, size_t bytes)
      : growth_(growth), bytes_(bytes) {
    if (growth_ != nullptr) {
      growth_->Grow(bytes_);
    }
  }
  ~ScopedGrowth() {
    if (growth_ != nullptr) {
      growth_->Shrink(bytes_);
    }
  }
  ScopedGrowth(const ScopedGrowth &) = delete;
  ScopedGrowth &operator=(const ScopedGrowth &) = delete;

 private:
  utils::MemoryGrowth *const growth_;
  const size_t bytes_;
};

}  // namespace

size_t LpmRouteTable::InstanceBytes(const Config &config) noexcept {
  // The configurations NewInstance creates with.
  if (config.backend == RouteBackend::kRteFib) {
    struct rte_fib_conf conf = {};
    conf.max_routes = static_cast<int>(config.max_routes);
    conf.dir24_8.num_tbl8 = config.tbl8_groups + 1;
    return FibBytes(conf);
  }
  struct rte_lpm_config conf = {};
  conf.max_rules = config.max_routes;
  conf.number_tbl8s = config.tbl8_groups;
  return LpmBytes(conf);
}

std::expected<std::unique_ptr<LpmRouteTable::Instance>, RouteError>
LpmRouteTable::NewInstance(uint32_t default_value) {
  // Every allocation before the DPDK create: a refusal must not leave a table
  // (and its global name) behind.
  const std::string name = InstanceName(name_);
  auto instance = std::make_unique<Instance>();
  instance->default_value.store(default_value, std::memory_order_relaxed);

  if (config_.backend == RouteBackend::kRteFib) {
    if (config_.max_routes > static_cast<uint32_t>(INT32_MAX)) {
      return std::unexpected(RouteError::kBackendFailure);
    }
    struct rte_fib_conf conf = {};
    conf.type = RTE_FIB_DIR24_8;
    conf.default_nh = kFibMiss;
    conf.max_routes = static_cast<int>(config_.max_routes);
    conf.rib_ext_sz = 0;
    conf.dir24_8.nh_sz = RTE_FIB_DIR24_8_4B;
    // One group beyond the budget (FibAdmits): install_to_fib() wants two
    // free groups to add a rule longer than /24 whose range ends on a /24
    // boundary, though it needs one, so with the budget spent that add would
    // otherwise fail where rte_lpm's cannot. (Changes and deletes never
    // allocate: deps/dpdk-patches/0005.) rte_fib rounds the pool up to a
    // multiple of 64.
    if (config_.tbl8_groups == UINT32_MAX) {
      return std::unexpected(RouteError::kBackendFailure);
    }
    conf.dir24_8.num_tbl8 = config_.tbl8_groups + 1;
    struct rte_fib *fib = CreateOnPolicy(config_.socket, FibBytes(conf), [&](int socket) {
      return rte_fib_create(name.c_str(), socket, &conf);
    });
    if (fib == nullptr) {
      return std::unexpected(rte_errno == ENOMEM ? RouteError::kTableFull
                                                 : RouteError::kBackendFailure);
    }
    struct rte_fib_rcu_config rcu = {};
    rcu.v = domain_.dpdk_qsbr();
    rcu.mode = RTE_FIB_QSBR_MODE_DQ;
    // Room for every group (rte_fib's default is 128 entries; rte_lpm's is
    // its pool): when the queue is full and a stalled reader blocks
    // reclamation, rte_fib logs and leaks the group it could not queue.
    rcu.dq_size = RTE_ALIGN_CEIL(conf.dir24_8.num_tbl8, 64);
    if (rte_fib_rcu_qsbr_add(fib, &rcu) != 0) {
      const int error = rte_errno;
      rte_fib_free(fib);
      if (error == ENOMEM) {
        utils::RefuseDpdkMemory(kDeferEntryBytes * rcu.dq_size);
      }
      return std::unexpected(RouteError::kBackendFailure);
    }
    instance->fib = fib;
    return instance;
  }

  struct rte_lpm_config conf = {};
  conf.max_rules = config_.max_routes;
  conf.number_tbl8s = config_.tbl8_groups;
  struct rte_lpm *lpm = CreateOnPolicy(config_.socket, LpmBytes(conf), [&](int socket) {
    return rte_lpm_create(name.c_str(), socket, &conf);
  });
  if (lpm == nullptr) {
    return std::unexpected(rte_errno == ENOMEM ? RouteError::kTableFull
                                               : RouteError::kBackendFailure);
  }

  struct rte_lpm_rcu_config rcu = {};
  rcu.v = domain_.dpdk_qsbr();
  rcu.mode = RTE_LPM_QSBR_MODE_DQ;
  if (rte_lpm_rcu_qsbr_add(lpm, &rcu) != 0) {
    const int error = rte_errno;
    rte_lpm_free(lpm);
    if (error == ENOMEM) {
      utils::RefuseDpdkMemory(kDeferEntryBytes * conf.number_tbl8s);
    }
    return std::unexpected(RouteError::kBackendFailure);
  }
  instance->lpm = lpm;
  return instance;
}

int LpmRouteTable::AddRule(Instance &instance, Ipv4Prefix prefix,
                           uint32_t value) {
  if (instance.fib != nullptr) {
    return rte_fib_add(instance.fib, prefix.addr(), prefix.length(), value);
  }
  return rte_lpm_add(instance.lpm, prefix.addr(), prefix.length(), value);
}

int LpmRouteTable::DeleteRule(Instance &instance, Ipv4Prefix prefix) {
  if (instance.fib != nullptr) {
    return rte_fib_delete(instance.fib, prefix.addr(), prefix.length());
  }
  return rte_lpm_delete(instance.lpm, prefix.addr(), prefix.length());
}

bool LpmRouteTable::FibAdmits(const FibBlocks &blocks, size_t routes,
                              Ipv4Prefix prefix) const {
  return routes < config_.max_routes &&
         (prefix.length() <= 24 || blocks.contains(prefix.addr() >> 8) ||
          blocks.size() < config_.tbl8_groups);
}

std::expected<std::unique_ptr<LpmRouteTable>, RouteError>
LpmRouteTable::Create(std::string name, const Config &config,
                      rcu::RcuDomain &domain) {
  if (!IsDpdkInitialized()) {
    InitDpdk();  // rte_lpm and rte_fib live in EAL memory
  }
  if (!RouteBackendSupported(config.backend)) {
    return std::unexpected(RouteError::kUnsupportedBackend);
  }
  std::unique_ptr<LpmRouteTable> table(
      new LpmRouteTable(std::move(name), config, domain));
  auto instance = table->NewInstance(kNoDefault);
  if (!instance) {
    return std::unexpected(instance.error());
  }
  table->live_.Initialize(std::move(*instance));
  return table;
}

LpmRouteTable::~LpmRouteTable() {
  // The owner guarantees no reader still uses the table (a module is
  // destroyed with workers paused), as with every RcuPtr teardown.
  live_.ResetQuiesced();
}

std::expected<void, RouteError> LpmRouteTable::Upsert(Ipv4Prefix prefix,
                                                      uint32_t value) {
  if (value > kMaxValue) {
    return std::unexpected(RouteError::kValueOutOfRange);
  }
  std::lock_guard<std::mutex> lock(writer_mutex_);
  // Whatever the caller published before this call (a next hop, say) must be
  // visible to any reader that sees the new route: the release fence orders
  // those earlier writes before the backend's entry stores, whatever memory
  // order the backend uses for them.
  std::atomic_thread_fence(std::memory_order_release);

  Instance *live = const_cast<Instance *>(live_.Read());
  if (prefix.length() == 0) {
    live->default_value.store(value, std::memory_order_release);
    default_ = value;
    return {};
  }
  // The shadow entries first: once the backend has the route, nothing can
  // fail (and an allocation that throws before it leaves nothing behind).
  const size_t routes = rules_.size();
  const auto [rule, inserted] = rules_.try_emplace(prefix, value);
  const bool fib = live->fib != nullptr;
  const bool deep = fib && inserted && prefix.length() > 24;
  if (fib && inserted && !FibAdmits(fib_blocks_, routes, prefix)) {
    rules_.erase(rule);
    return std::unexpected(RouteError::kTableFull);
  }
  FibBlocks::iterator block;
  if (deep) {
    try {
      block = fib_blocks_.try_emplace(prefix.addr() >> 8, 0).first;
    } catch (...) {
      rules_.erase(rule);
      throw;
    }
  }
  const int ret = AddRule(*live, prefix, value);
  if (ret != 0) {
    if (inserted) {
      rules_.erase(rule);
    }
    if (deep && block->second == 0) {
      fib_blocks_.erase(block);
    }
    return std::unexpected(ret == -ENOSPC ? RouteError::kTableFull
                                          : RouteError::kBackendFailure);
  }
  rule->second = value;
  if (deep) {
    block->second++;
  }
  return {};
}

std::expected<void, RouteError> LpmRouteTable::Erase(Ipv4Prefix prefix) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  Instance *live = const_cast<Instance *>(live_.Read());
  if (prefix.length() == 0) {
    if (!default_) {
      return std::unexpected(RouteError::kNotFound);
    }
    live->default_value.store(kNoDefault, std::memory_order_release);
    default_.reset();
    return {};
  }
  if (rules_.find(prefix) == rules_.end()) {
    return std::unexpected(RouteError::kNotFound);
  }
  const int ret = DeleteRule(*live, prefix);
  if (ret != 0) {
    return std::unexpected(RouteError::kBackendFailure);
  }
  rules_.erase(prefix);
  if (live->fib != nullptr && prefix.length() > 24) {
    const auto block = fib_blocks_.find(prefix.addr() >> 8);
    if (--block->second == 0) {
      fib_blocks_.erase(block);
    }
  }
  return {};
}

std::expected<void, RouteError> LpmRouteTable::Clear() {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  const ScopedGrowth replacement(growth_, InstanceBytes(config_));
  auto fresh = NewInstance(default_.value_or(kNoDefault));
  if (!fresh) {
    return std::unexpected(fresh.error());
  }
  live_.Publish(std::move(*fresh));
  domain_.ReclaimReady();
  rules_.clear();
  fib_blocks_.clear();
  return {};
}

std::expected<void, RouteError> LpmRouteTable::ReplaceAll(
    std::span<const Rule> rules) {
  std::map<Ipv4Prefix, uint32_t> next_rules;
  std::optional<uint32_t> next_default;
  for (const Rule &rule : rules) {
    if (rule.value > kMaxValue) {
      return std::unexpected(RouteError::kValueOutOfRange);
    }
    if (rule.prefix.length() == 0) {
      next_default = rule.value;
    } else {
      next_rules[rule.prefix] = rule.value;
    }
  }

  std::lock_guard<std::mutex> lock(writer_mutex_);
  const ScopedGrowth replacement(growth_, InstanceBytes(config_));
  auto fresh = NewInstance(next_default.value_or(kNoDefault));
  if (!fresh) {
    return std::unexpected(fresh.error());
  }
  FibBlocks next_blocks;
  const bool fib = (*fresh)->fib != nullptr;
  size_t placed = 0;
  for (const auto &[prefix, value] : next_rules) {
    const int ret = fib && !FibAdmits(next_blocks, placed, prefix)
                        ? -ENOSPC
                        : AddRule(**fresh, prefix, value);
    if (ret != 0) {
      // `fresh` is dropped (and freed): the live table was never touched.
      return std::unexpected(ret == -ENOSPC ? RouteError::kTableFull
                                            : RouteError::kBackendFailure);
    }
    placed++;
    if (fib && prefix.length() > 24) {
      next_blocks[prefix.addr() >> 8]++;
    }
  }
  // The one publication: a release store of the finished table.
  live_.Publish(std::move(*fresh));
  domain_.ReclaimReady();
  rules_ = std::move(next_rules);
  fib_blocks_ = std::move(next_blocks);
  default_ = next_default;
  return {};
}

std::optional<uint32_t> LpmRouteTable::Find(Ipv4Prefix prefix) const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (prefix.length() == 0) {
    return default_;
  }
  const auto it = rules_.find(prefix);
  return it == rules_.end() ? std::nullopt : std::optional<uint32_t>(it->second);
}

std::optional<uint32_t> LpmRouteTable::CoveringValue(Ipv4Prefix prefix) const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  for (int length = static_cast<int>(prefix.length()) - 1; length >= 1;
       length--) {
    const uint32_t mask = ~uint32_t{0} << (32 - length);
    const auto shorter =
        Ipv4Prefix::Make(prefix.addr() & mask, static_cast<uint8_t>(length));
    if (const auto it = rules_.find(*shorter); it != rules_.end()) {
      return it->second;
    }
  }
  return prefix.length() == 0 ? std::nullopt : default_;
}

size_t LpmRouteTable::size() const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return rules_.size() + (default_ ? 1 : 0);
}

// -- Ipv6RouteTable ------------------------------------------------------------

namespace {

// rte_fib6's largest 2-byte next hop (trie.h get_max_nh(RTE_FIB6_TRIE_2B)):
// entries hold value << 1, the low bit marking a tbl8 group index.
constexpr uint64_t kMaxTwoByteValue = (uint64_t{1} << 15) - 1;

const struct rte_ipv6_addr *AsDpdk(const utils::Ipv6Address &addr) {
  return reinterpret_cast<const struct rte_ipv6_addr *>(addr.bytes);
}

RouteError FromDpdk(int ret) {
  return ret == -ENOSPC || ret == -ENOMEM ? RouteError::kTableFull
                                          : RouteError::kBackendFailure;
}

}  // namespace

size_t Ipv6RouteTable::EntryBytes(const Config &config) noexcept {
  // trie_create refuses a default (the miss marker, max_value + 1) or a tbl8
  // pool larger than the entry's largest next hop.
  return uint64_t{config.max_value} + 1 <= kMaxTwoByteValue &&
                 config.tbl8_groups <= kMaxTwoByteValue
             ? 2
             : 4;
}

size_t Ipv6RouteTable::InstanceBytes(const Config &config) noexcept {
  // The configuration NewInstance creates with.
  struct rte_fib6_conf conf = {};
  conf.max_routes = static_cast<int>(std::max<uint32_t>(config.max_routes, 1));
  conf.trie.num_tbl8 = config.tbl8_groups;
  return Fib6Bytes(conf, EntryBytes(config));
}

Ipv6RouteTable::Instance::~Instance() { rte_fib6_free(fib); }

std::expected<std::unique_ptr<Ipv6RouteTable>, RouteError>
Ipv6RouteTable::Create(std::string name, const Config &config,
                       rcu::RcuDomain &domain) {
  if (!IsDpdkInitialized()) {
    InitDpdk();  // rte_fib6 lives in EAL memory
  }
  if (config.max_value > LpmRouteTable::kMaxValue) {
    return std::unexpected(RouteError::kValueOutOfRange);
  }
  // The rib holds 2 x max_routes nodes, an int.
  if (config.max_routes > static_cast<uint32_t>(INT32_MAX / 2)) {
    return std::unexpected(RouteError::kBackendFailure);
  }
  std::unique_ptr<Ipv6RouteTable> table(
      new Ipv6RouteTable(std::move(name), config, domain));
  std::lock_guard<std::mutex> lock(table->writer_mutex_);
  auto instance = table->NewInstance();
  if (!instance) {
    return std::unexpected(instance.error());
  }
  table->live_.Initialize(std::move(*instance));
  table->instances_.store(1, std::memory_order_relaxed);
  return table;
}

Ipv6RouteTable::~Ipv6RouteTable() {
  DCHECK(!stage_open_) << "a stage outlives its table";
  live_.ResetQuiesced();
}

std::expected<std::unique_ptr<Ipv6RouteTable::Instance>, RouteError>
Ipv6RouteTable::NewInstance() {
  // Every allocation before the DPDK create.
  const std::string name = InstanceName(name_);
  auto instance = std::make_unique<Instance>();
  instance->miss = config_.max_value + 1;
  struct rte_fib6_conf conf = {};
  conf.type = RTE_FIB6_TRIE;
  conf.default_nh = instance->miss;
  // The bound is ours (Stage::UpsertLocked); the rib wants a pool to exist.
  conf.max_routes = static_cast<int>(std::max<uint32_t>(config_.max_routes, 1));
  conf.rib_ext_sz = 0;
  conf.trie.nh_sz = entry_bytes_ == 2 ? RTE_FIB6_TRIE_2B : RTE_FIB6_TRIE_4B;
  conf.trie.num_tbl8 = config_.tbl8_groups;
  instance->fib = CreateOnPolicy(config_.socket, Fib6Bytes(conf, entry_bytes_), [&](int socket) {
    return rte_fib6_create(name.c_str(), socket, &conf);
  });
  if (instance->fib == nullptr) {
    return std::unexpected(rte_errno == ENOMEM || rte_errno == ENOSPC
                               ? RouteError::kTableFull
                               : RouteError::kBackendFailure);
  }
  builds_++;
  return instance;
}

int Ipv6RouteTable::Apply(Instance &instance, Ipv6Prefix prefix,
                          std::optional<uint32_t> value) {
  if (prefix.length() == 0) {
    instance.default_value = value.value_or(LpmRouteTable::kNoDefault);
    return 0;
  }
  if (value) {
    return rte_fib6_add(instance.fib, AsDpdk(prefix.addr()), prefix.length(),
                        *value);
  }
  const int ret =
      rte_fib6_delete(instance.fib, AsDpdk(prefix.addr()), prefix.length());
  return ret == -ENOENT ? 0 : ret;
}

std::expected<std::unique_ptr<Ipv6RouteTable::Instance>, RouteError>
Ipv6RouteTable::Build() {
  auto instance = NewInstance();
  if (!instance) {
    return instance;
  }
  for (const auto &[prefix, value] : rules_) {
    if (const int ret = Apply(**instance, prefix, value); ret != 0) {
      return std::unexpected(FromDpdk(ret));  // the instance is freed
    }
  }
  return instance;
}

// Brings `instance` from the rules it held to rules_, touching only the
// lagging prefixes. Deletes first, so it never holds more rules (or reserves
// more tbl8 groups) than rules_ needs.
bool Ipv6RouteTable::Replay(Instance &instance) {
  for (const Ipv6Prefix &prefix : spare_lag_) {
    if (!rules_.contains(prefix) && Apply(instance, prefix, std::nullopt) != 0) {
      return false;
    }
  }
  for (const Ipv6Prefix &prefix : spare_lag_) {
    const auto it = rules_.find(prefix);
    if (it != rules_.end() && Apply(instance, prefix, it->second) != 0) {
      return false;
    }
  }
  return true;
}

std::expected<void, RouteError> Ipv6RouteTable::PrepareSpare() {
  // A spare that was never published needs no grace period: no reader has
  // been in it. Only an instance a commit unpublished waits for one.
  if (spare_ && !spare_unpublished_) {
    if (!spare_token_.has_value()) {
      // Committed() has not been called: a grace period started now also
      // covers the exchange that unpublished the spare.
      spare_token_ = domain_.StartGracePeriod();
    }
    const auto token = spare_token_.value();
    if (!domain_.IsComplete(token)) {
      // A reader may still be in it: it goes to the RCU domain, and a fresh
      // spare is built from the rules.
      domain_.ReserveRetirements(1);
      domain_.Retire(token, std::move(spare_));
      SpareReleased();
      spare_lag_.clear();
    }
  }
  if (spare_ && !Replay(*spare_)) {
    // A replay may need a transient tbl8 group a fresh build does not.
    spare_.reset();  // unpublished, or past its grace period
    SpareReleased();
  }
  spare_lag_.clear();
  if (!spare_) {
    // Admitted before it is built (D-198); given back if the build fails.
    if (growth_ != nullptr) {
      growth_->Grow(instance_bytes_);
    }
    std::expected<std::unique_ptr<Instance>, RouteError> built;
    try {
      built = Build();
    } catch (...) {
      if (growth_ != nullptr) {
        growth_->Shrink(instance_bytes_);
      }
      throw;
    }
    if (!built) {
      if (growth_ != nullptr) {
        growth_->Shrink(instance_bytes_);
      }
      return std::unexpected(built.error());
    }
    spare_ = std::move(*built);
    instances_.fetch_add(1, std::memory_order_relaxed);
    spare_token_.reset();
    spare_unpublished_ = true;
  }
  return {};
}

void Ipv6RouteTable::SpareReleased() noexcept {
  instances_.fetch_sub(1, std::memory_order_relaxed);
  if (growth_ != nullptr) {
    growth_->Shrink(instance_bytes_);
  }
}

size_t Ipv6RouteTable::RoutesLocked() const noexcept {
  return rules_.size() - (rules_.empty() || rules_.begin()->first.length() != 0
                              ? 0
                              : 1);
}

std::expected<std::unique_ptr<Ipv6RouteTable::Stage>, RouteError>
Ipv6RouteTable::BeginStageLocked() {
  CHECK(!stage_open_) << "one Ipv6RouteTable stage at a time";
  std::unique_ptr<Stage> stage(new Stage(*this));
  if (auto ready = PrepareSpare(); !ready) {
    return std::unexpected(ready.error());
  }
  stage->routes_ = RoutesLocked();
  stage->done_ = false;
  stage_open_ = true;
  return stage;
}

std::expected<std::unique_ptr<Ipv6RouteTable::Stage>, RouteError>
Ipv6RouteTable::BeginStage() {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return BeginStageLocked();
}

void Ipv6RouteTable::CommitLocked(Stage &stage) noexcept {
  CHECK(stage_open_ && !stage.done_ && !stage.broken_)
      << "committing a stage that is not open or failed an edit";
  // The one store readers see. The old live instance becomes the spare, a
  // grace period from being editable, behind the rules by the stage's
  // prefixes.
  std::unique_ptr<const Instance> old = live_.Exchange(std::move(spare_));
  spare_.reset(const_cast<Instance *>(old.release()));
  spare_token_.reset();
  spare_unpublished_ = false;
  spare_lag_.swap(stage.touched_);
  // The rules follow, with nodes made in the stage: no allocation.
  for (auto &[prefix, change] : stage.changes_) {
    const auto it = rules_.find(prefix);
    if (change.value) {
      if (it != rules_.end()) {
        it->second = *change.value;
      } else {
        rules_.insert(std::move(change.node));
      }
    } else if (it != rules_.end()) {
      stage.erased_.push_back(rules_.extract(it));  // room reserved
    }
  }
  stage.done_ = true;
  stage_open_ = false;
}

void Ipv6RouteTable::Commit(Stage &stage) noexcept {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  CommitLocked(stage);
}

void Ipv6RouteTable::AbortLocked(Stage &stage) noexcept {
  if (stage.done_) {
    return;
  }
  stage.done_ = true;
  stage_open_ = false;
  if (stage.broken_) {
    // A refused DPDK edit may have left the trie half written.
    if (spare_) {
      spare_.reset();
      SpareReleased();
    }
    spare_lag_.clear();
    spare_token_.reset();
    spare_unpublished_ = false;
  } else {
    spare_lag_.swap(stage.touched_);  // empty since PrepareSpare()
  }
}

void Ipv6RouteTable::Abort(Stage &stage) noexcept {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  AbortLocked(stage);
}

void Ipv6RouteTable::Committed(rcu::GracePeriod token) noexcept {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (spare_ && !spare_unpublished_ && !spare_token_) {
    spare_token_ = token;
  }
}

std::expected<void, RouteError> Ipv6RouteTable::Upsert(Ipv6Prefix prefix,
                                                       uint32_t value) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (value > config_.max_value) {
    return std::unexpected(RouteError::kValueOutOfRange);
  }
  if (const auto it = rules_.find(prefix);
      it != rules_.end() && it->second == value) {
    return {};
  }
  auto stage = BeginStageLocked();
  if (!stage) {
    return std::unexpected(stage.error());
  }
  std::expected<void, RouteError> set;
  try {
    set = (*stage)->UpsertLocked(prefix, value);
  } catch (...) {
    AbortLocked(**stage);
    throw;
  }
  if (!set) {
    AbortLocked(**stage);
    return set;
  }
  CommitLocked(**stage);
  spare_token_ = domain_.StartGracePeriod();
  domain_.ReclaimReady();
  return {};
}

std::expected<void, RouteError> Ipv6RouteTable::Erase(Ipv6Prefix prefix) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (!rules_.contains(prefix)) {
    return std::unexpected(RouteError::kNotFound);
  }
  auto stage = BeginStageLocked();
  if (!stage) {
    return std::unexpected(stage.error());
  }
  std::expected<void, RouteError> erased;
  try {
    erased = (*stage)->EraseLocked(prefix);
  } catch (...) {
    AbortLocked(**stage);
    throw;
  }
  if (!erased) {
    AbortLocked(**stage);
    return erased;
  }
  CommitLocked(**stage);
  spare_token_ = domain_.StartGracePeriod();
  domain_.ReclaimReady();
  return {};
}

std::optional<uint32_t> Ipv6RouteTable::Find(Ipv6Prefix prefix) const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  const auto it = rules_.find(prefix);
  return it == rules_.end() ? std::nullopt : std::optional<uint32_t>(it->second);
}

size_t Ipv6RouteTable::size() const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return rules_.size();
}

size_t Ipv6RouteTable::builds() const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return builds_;
}

Ipv6RouteTable::Stage::~Stage() {
  if (!done_) {
    table_.Abort(*this);
  }
}

std::optional<uint32_t> Ipv6RouteTable::Stage::FindLocked(
    Ipv6Prefix prefix) const {
  if (const auto it = changes_.find(prefix); it != changes_.end()) {
    return it->second.value;
  }
  const auto it = table_.rules_.find(prefix);
  return it == table_.rules_.end() ? std::nullopt
                                   : std::optional<uint32_t>(it->second);
}

std::expected<void, RouteError> Ipv6RouteTable::Stage::UpsertLocked(
    Ipv6Prefix prefix, uint32_t value) {
  CHECK(!done_) << "editing a stage that is not open";
  if (broken_) {
    return std::unexpected(RouteError::kBackendFailure);
  }
  if (value > table_.config_.max_value) {
    return std::unexpected(RouteError::kValueOutOfRange);
  }
  const std::optional<uint32_t> current = FindLocked(prefix);
  if (current == value) {
    return {};
  }
  const bool counted = prefix.length() != 0;
  if (!current && counted && routes_ >= table_.config_.max_routes) {
    return std::unexpected(RouteError::kTableFull);
  }
  // Everything that allocates, before the spare changes.
  touched_.reserve(touched_.size() + 1);
  const auto [it, inserted] = changes_.try_emplace(prefix);
  Change &change = it->second;
  if (change.node.empty() && !table_.rules_.contains(prefix)) {
    try {
      Rules one;
      one.emplace(prefix, value);
      change.node = one.extract(one.begin());
    } catch (...) {
      if (inserted) {
        changes_.erase(it);
      }
      throw;
    }
  }
  if (const int ret = Apply(*table_.spare_, prefix, value); ret != 0) {
    broken_ = true;
    if (inserted) {
      changes_.erase(it);
    }
    return std::unexpected(FromDpdk(ret));
  }
  if (inserted) {
    touched_.push_back(prefix);
  }
  change.value = value;
  if (!change.node.empty()) {
    change.node.mapped() = value;
  }
  if (!current && counted) {
    routes_++;
  }
  return {};
}

std::expected<void, RouteError> Ipv6RouteTable::Stage::EraseLocked(
    Ipv6Prefix prefix) {
  CHECK(!done_) << "editing a stage that is not open";
  if (broken_) {
    return std::unexpected(RouteError::kBackendFailure);
  }
  if (!FindLocked(prefix)) {
    return std::unexpected(RouteError::kNotFound);
  }
  touched_.reserve(touched_.size() + 1);
  const auto [it, inserted] = changes_.try_emplace(prefix);
  if (table_.rules_.contains(prefix)) {
    // Commit() unlinks at most one committed node per change.
    try {
      erased_.reserve(changes_.size());
    } catch (...) {
      if (inserted) {
        changes_.erase(it);
      }
      throw;
    }
  }
  if (const int ret = Apply(*table_.spare_, prefix, std::nullopt); ret != 0) {
    broken_ = true;
    if (inserted) {
      changes_.erase(it);
    }
    return std::unexpected(FromDpdk(ret));
  }
  if (inserted) {
    touched_.push_back(prefix);
  }
  it->second.value.reset();
  if (prefix.length() != 0) {
    routes_--;
  }
  return {};
}

std::expected<void, RouteError> Ipv6RouteTable::Stage::Upsert(Ipv6Prefix prefix,
                                                              uint32_t value) {
  std::lock_guard<std::mutex> lock(table_.writer_mutex_);
  return UpsertLocked(prefix, value);
}

std::expected<void, RouteError> Ipv6RouteTable::Stage::Erase(Ipv6Prefix prefix) {
  std::lock_guard<std::mutex> lock(table_.writer_mutex_);
  return EraseLocked(prefix);
}

std::optional<uint32_t> Ipv6RouteTable::Stage::Find(Ipv6Prefix prefix) const {
  std::lock_guard<std::mutex> lock(table_.writer_mutex_);
  return FindLocked(prefix);
}

}  // namespace bess::route
