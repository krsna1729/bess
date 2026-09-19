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
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
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

// rte_lpm vs rte_fib for `IPLookup` -- the table-level half of the "rte_fib
// vs rte_lpm" experiment (MODERNIZATION.md, Phase D). The module-level
// question is whether FIB can replace LPM *and* delete BESS's x86 SIMD
// block, so this binary measures the exact code shapes the module would use,
// not just "a lookup":
//
//   BM_LookupLpmVec     the current module path: 4-wide SSE byte swap
//                       (_mm_shuffle_epi8, see modules/ip_lookup.cc)
//                       followed by rte_lpm_lookupx4(), 32 packets at a time
//   BM_LookupLpmScalar  the module's tail path: read each address as a value
//                       (be32_t::value(): a load plus byte swap on this host)
//                       and call rte_lpm_lookup()
//   BM_LookupFib        rte_fib_lookup_bulk() over a contiguous uint32_t[] of
//                       *raw* packet-order keys, FIB created with
//                       RTE_FIB_F_LOOKUP_NETWORK_ORDER -- verified in DPDK
//                       25.11 (lib/fib/rte_fib.c: dir24_8_get_lookup_fn(...,
//                       be_addr)) that the flag changes only how the *lookup
//                       key* is addressed; rules go in as values exactly as
//                       for rte_lpm, so the packet path needs no swap at all
//
// The two key forms are what this experiment is really about, so both are
// modelled exactly (ToRaw()/ToValue() below):
//   raw    the bytes as they sit in a packet header (be32_t::raw_value()),
//          which a dataplane module reads with one load
//   value  the host-order numeric address (be32_t::value()), which is what
//          IPLookup hands rte_lpm_add() and what DPDK's LPM indexes with
//          (`ip >> 8`: the address's top byte first)
// The vector path exists to get from raw to value four at a time; the scalar
// tail path pays a per-packet swap; FIB's network-order flag is exactly
// "accept raw". If fib_bulk is competitive the swap disappears, rather than
// merely changing which DPDK call comes after it.
//
// Measured per (kind, table size) -- 1K / 64K / 512K routes, generated
// deterministically as a realistic mix (mostly /24, some shorter, a minority
// of longer prefixes nested under earlier /24s, which is also what keeps
// dir24_8/tbl8 extension counts sane):
//   - lookup throughput over a fixed 64K-key stream that hits the table
//     (items/s from Google Benchmark's own clock, plus this file's
//     thread-CPU-time counter `cpu_ns_pkt`), keys walked from a 256KB array
//     that is identical for every kind
//   - `build_ns_route`: route-insertion cost during construction
//   - `bytes_route`: allocation footprint, from rte_malloc_get_socket_stats()
//     before and after construction (both DPDK tables allocate out of the EAL
//     heap, so this is the whole cost)
//   - separately, add/delete cost per operation
//
// Correctness gate before any timing: each case cross-checks the DPDK result
// against an independent longest-prefix match computed here from the
// generated route list, and CHECKs that the table agrees. A wrong key
// convention -- the failure mode that silently turns every lookup into a
// miss and makes one kind look artificially fast -- fails the run instead of
// producing numbers. (It earned its keep once already: the first version of
// this file used the raw form for rules, which is wrong, and the gate said so
// on the first case.)
//
// Excluded on purpose, so the comparison is honest about itself: the cost of
// *gathering* keys out of packet headers (all kinds are fed an already
// contiguous uint32_t[] here, as a module would build before calling
// rte_fib_lookup_bulk), lookup misses (a different branch in both tables, and
// the real workload is hit-dominated), live updates (Phase J is a separate,
// later change -- settle LPM-vs-FIB first), and module dispatch overhead
// (needs a live daemon).
//
// Single-threaded on purpose: this is a per-packet table cost, and one core
// is what makes it comparable across table kinds and hosts. Numbers are
// relative-comparable only, as everywhere in this tree: EAL runs --no-huge
// (`bessd -m 0` shape), so this builds and runs in CI and the sandbox alike.

#include <benchmark/benchmark.h>
#include <glog/logging.h>

#include <rte_errno.h>
#include <rte_fib.h>
#include <rte_lpm.h>
#include <rte_malloc.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <time.h>

#include <emmintrin.h>
#include <tmmintrin.h>

#include "dpdk.h"
#include "utils/endian.h"

namespace {

const size_t kBatch = 32;            // PacketBatch::kMaxBurst
const size_t kLookupKeys = 1 << 16;  // 256KB of keys: L2-resident by design
// BESS's DROP_GATE (core/gate.h: MAX_GATES), i.e. what IPLookup emits when
// there is no match. Deliberately outside the generated route range
// ([1, kMaxNextHop]): a sentinel that collides with a real next hop would
// make a lookup miss indistinguishable from a correct hit, in both the gate
// and any counter that counts hops.
const uint32_t kDefaultNextHop = 8192;  // DROP_GATE
const uint32_t kMaxNextHop = 8191;      // highest next hop a rule may use

// dir24_8 geometry, chosen to be comparable with rte_lpm rather than merely
// convenient: 4-byte next hops (LPM stores uint32_t), so dir24_8's tbl24 is
// the same 16M x 4B = 64MB as rte_lpm's, and the tbl8 pool is sized for the
// nested prefixes below. (2-byte hops would halve tbl24 but cap the pool at
// get_max_nh(2B) = 32767 groups, which a 512K-route table with a realistic
// share of /25../28 rules exceeds; that, and an off-by-one on that cap, is
// what rte_fib_create(EINVAL) reported the first time this ran.)
const unsigned kFibNumTbl8 = 1 << 15;
const uint32_t kLpmTbl8s = 1 << 15;

// Distinct /24s that may serve as parents for nested /25../28 rules. Bounding
// this bounds the tbl8 groups both tables need (~16K groups here, against
// kFibNumTbl8 = 32K), which is what a real FIB's /24-with-longer-prefixes
// count looks like anyway.
const size_t kMaxNestedParents = 1 << 14;

// --------------------------------------------------------------------------
// Key forms
// --------------------------------------------------------------------------

// The two forms, expressed through the same translation BESS's be32_t
// performs on its stored bytes (utils/endian.h). The static_asserts below pin
// that equivalence, so a change to endian.h fails the build here rather than
// silently changing what this benchmark measures. (The first version used
// memcpy into/out of the class; g++ rejects writing into it under
// -Werror=class-memaccess -- and CI builds with -Werror, without the -w this
// session's local builds needed for unrelated g++16 warnings.)
constexpr uint32_t ToRaw(uint32_t value) {
  return bess::utils::is_be_system() ? value : __builtin_bswap32(value);
}

// Same translation: byte order conversion is its own inverse.
constexpr uint32_t ToValue(uint32_t raw) { return ToRaw(raw); }

static_assert(ToRaw(0x12345678u) ==
                  bess::utils::be32_t(0x12345678u).raw_value(),
              "ToRaw must match be32_t's stored byte image");
static_assert(ToValue(bess::utils::be32_t(0x12345678u).raw_value()) ==
                  0x12345678u,
              "ToValue must invert be32_t's raw_value()");

// A rule as both tables want it: the host-order numeric prefix.
uint32_t PrefixValue(uint32_t host_addr, uint8_t len) {
  const uint32_t mask = len == 0 ? 0 : 0xFFFFFFFFu << (32 - len);
  return host_addr & mask;
}

// --------------------------------------------------------------------------
// Deterministic route/key generation
// --------------------------------------------------------------------------

struct Route {
  uint32_t prefix;  // value form: what IPLookup passes to rte_lpm_add()
  uint8_t len;
  uint32_t next_hop;
};

uint64_t NextRand(uint64_t *state) {
  // xorshift64*: deterministic across hosts, no library dependency.
  uint64_t x = *state;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  *state = x;
  return x * 0x2545F4914F6CDD1DULL;
}

std::vector<Route> MakeRoutes(size_t n) {
  std::vector<Route> routes;
  routes.reserve(n);
  // De-duplicated: adding an existing prefix would fail the setup CHECK, and
  // a duplicate rule would also make the independent reference ambiguous.
  std::unordered_map<uint64_t, bool> seen;
  std::vector<uint32_t> slash24s;  // parents for the nested longer prefixes
  slash24s.reserve(n);
  uint64_t state = 0x9E3779B97F4A7C15ULL;

  while (routes.size() < n) {
    const uint32_t addr = static_cast<uint32_t>(NextRand(&state) >> 32);
    const uint32_t pick = static_cast<uint32_t>(NextRand(&state) % 100);
    uint8_t len;
    uint32_t base;
    if (pick < 10) {  // 10% short prefixes
      len = static_cast<uint8_t>(8 + NextRand(&state) % 16);  // /8../23
      base = addr;
    } else if (pick < 85 || slash24s.empty()) {  // 75% /24
      len = 24;
      base = addr;
      if (slash24s.size() < kMaxNestedParents) {
        slash24s.push_back(addr & 0xFFFFFF00u);
      }
    } else {  // 15% nested /25../28
      len = static_cast<uint8_t>(25 + NextRand(&state) % 4);
      base = slash24s[NextRand(&state) % slash24s.size()] | (addr & 0xFFu);
    }
    const uint32_t prefix = PrefixValue(base, len);
    if (!seen.emplace((static_cast<uint64_t>(len) << 32) | prefix, true)
             .second) {
      continue;  // already have this exact rule
    }
    const uint32_t hop =
        1 + static_cast<uint32_t>(NextRand(&state) % kMaxNextHop);
    routes.push_back({prefix, len, hop});
  }
  return routes;
}

// Both forms of the same key stream: `raw` as it would be read from packet
// headers, `value` as the tables and the reference want it.
struct KeyStream {
  std::vector<uint32_t> raw;
  std::vector<uint32_t> value;
};

KeyStream MakeKeys(const std::vector<Route> &routes) {
  KeyStream keys;
  keys.raw.reserve(kLookupKeys);
  keys.value.reserve(kLookupKeys);
  uint64_t state = 0xD1B54A32D192ED03ULL;
  while (keys.raw.size() < kLookupKeys) {
    const Route &r = routes[NextRand(&state) % routes.size()];
    const uint32_t mask = r.len == 0 ? 0 : 0xFFFFFFFFu << (32 - r.len);
    // Prefix bits from the rule, host bits random: a plausible destination
    // address inside it, not just the network address.
    const uint32_t value =
        (r.prefix & mask) | (static_cast<uint32_t>(NextRand(&state)) & ~mask);
    keys.value.push_back(value);
    keys.raw.push_back(ToRaw(value));
  }
  return keys;
}

// Independent longest-prefix match over the generated rules (value form).
// Used only to validate the key convention before anything is timed.
class Reference {
 public:
  explicit Reference(const std::vector<Route> &routes) {
    for (const Route &r : routes) {
      table_.emplace((static_cast<uint64_t>(r.len) << 32) | r.prefix,
                     r.next_hop);
    }
  }

  uint32_t Lookup(uint32_t value) const {
    for (int len = 32; len >= 1; len--) {
      const uint32_t mask = 0xFFFFFFFFu << (32 - len);
      auto it =
          table_.find((static_cast<uint64_t>(len) << 32) | (value & mask));
      if (it != table_.end()) {
        return it->second;
      }
    }
    return kDefaultNextHop;
  }

 private:
  std::unordered_map<uint64_t, uint32_t> table_;
};

// --------------------------------------------------------------------------
// Table wrappers: identical surface, so the driver below is shared
// --------------------------------------------------------------------------

class LpmTable {
 public:
  LpmTable(const std::string &name, size_t routes) : name_(name) {
    struct rte_lpm_config conf = {
        .max_rules = static_cast<uint32_t>(routes) + 1024,
        .number_tbl8s = kLpmTbl8s,
        .flags = 0,
    };
    lpm_ = rte_lpm_create(name_.c_str(), 0, &conf);
    CHECK(lpm_ != nullptr) << "rte_lpm_create: " << rte_strerror(rte_errno);
  }
  ~LpmTable() { rte_lpm_free(lpm_); }
  LpmTable(const LpmTable &) = delete;
  LpmTable &operator=(const LpmTable &) = delete;

  int Add(const Route &r) {
    return rte_lpm_add(lpm_, r.prefix, r.len, r.next_hop);
  }
  int Delete(const Route &r) { return rte_lpm_delete(lpm_, r.prefix, r.len); }

  // The module's vector path: raw packet-order keys, SSE byte swap, then
  // rte_lpm_lookupx4() (which wants value form).
  void LookupBatch(const uint32_t *keys, uint32_t *next_hops) {
    const __m128i bswap_mask =
        _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
    for (size_t i = 0; i + 3 < kBatch; i += 4) {
      __m128i ip_addr =
          _mm_set_epi32(keys[i + 3], keys[i + 2], keys[i + 1], keys[i]);
      ip_addr = _mm_shuffle_epi8(ip_addr, bswap_mask);
      rte_lpm_lookupx4(lpm_, ip_addr, next_hops + i, kDefaultNextHop);
    }
  }

  // The module's tail path: value form, one lookup at a time.
  void LookupBatchScalar(const uint32_t *keys, uint32_t *next_hops) {
    for (size_t i = 0; i < kBatch; i++) {
      uint32_t nh;
      if (rte_lpm_lookup(lpm_, ToValue(keys[i]), &nh) == 0) {
        next_hops[i] = nh;
      } else {
        next_hops[i] = kDefaultNextHop;
      }
    }
  }

 private:
  std::string name_;
  struct rte_lpm *lpm_ = nullptr;
};

class FibTable {
 public:
  FibTable(const std::string &name, size_t routes) : name_(name) {
    struct rte_fib_conf conf = {};
    conf.type = RTE_FIB_DIR24_8;
    conf.default_nh = kDefaultNextHop;
    conf.max_routes = static_cast<int>(routes) + 1024;
    conf.rib_ext_sz = 0;
    conf.dir24_8.nh_sz = RTE_FIB_DIR24_8_4B;
    conf.dir24_8.num_tbl8 = kFibNumTbl8;
    // The whole point of the variant: the *lookup key* stays in packet byte
    // order, so a caller holding a header needs no swap (rules are still
    // added as values -- see dir24_8_get_lookup_fn()).
    conf.flags = RTE_FIB_F_LOOKUP_NETWORK_ORDER;
    fib_ = rte_fib_create(name_.c_str(), 0, &conf);
    CHECK(fib_ != nullptr) << "rte_fib_create: " << rte_strerror(rte_errno);
  }
  ~FibTable() { rte_fib_free(fib_); }
  FibTable(const FibTable &) = delete;
  FibTable &operator=(const FibTable &) = delete;

  int Add(const Route &r) {
    return rte_fib_add(fib_, r.prefix, r.len, r.next_hop);
  }
  int Delete(const Route &r) { return rte_fib_delete(fib_, r.prefix, r.len); }

  void LookupBatch(const uint32_t *keys, uint32_t *next_hops) {
    uint64_t hop64[kBatch];
    // The API returns -EINVAL for bad arguments and 0 otherwise (it discards
    // the internal lookup's count -- checked here, not assumed).
    CHECK_EQ(rte_fib_lookup_bulk(fib_, const_cast<uint32_t *>(keys), hop64,
                                 kBatch),
             0)
        << "rte_fib_lookup_bulk() failed: " << rte_strerror(rte_errno);
    for (size_t i = 0; i < kBatch; i++) {
      next_hops[i] = static_cast<uint32_t>(hop64[i]);
    }
  }

 private:
  std::string name_;
  struct rte_fib *fib_ = nullptr;
};

// --------------------------------------------------------------------------
// Shared setup / driver
// --------------------------------------------------------------------------

struct Prepared {
  std::vector<Route> routes;
  KeyStream keys;
  Reference reference;
  uint64_t build_ns = 0;
  double bytes_per_route = 0;

  explicit Prepared(size_t n)
      : routes(MakeRoutes(n)), keys(MakeKeys(routes)), reference(routes) {}
};

// rte_lpm/rte_fib names have their own short limit and must not collide, so
// neither the Google Benchmark name nor the table size goes in.
std::string TableName() {
  static std::atomic<uint64_t> next{0};
  char buf[32];
  snprintf(buf, sizeof(buf), "fibbench_%lu",
           static_cast<unsigned long>(next.fetch_add(1)));
  return buf;
}

uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

uint64_t ThreadCpuNs() {
  struct timespec ts;
  CHECK_EQ(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts), 0);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull +
         static_cast<uint64_t>(ts.tv_nsec);
}

size_t EalHeapUsed() {
  struct rte_malloc_socket_stats stats = {};
  CHECK_EQ(rte_malloc_get_socket_stats(0, &stats), 0);
  return stats.heap_totalsz_bytes - stats.heap_freesz_bytes;
}

// Gates a table against the independent longest-prefix reference over the
// *entire* key stream -- not a sample: this benchmark's whole reason to exist
// is that it can veto a table that is fast but wrong, so it pays the ~32 hash
// probes per key rather than risking a stale entry in the unchecked part.
// Templated so LPM and FIB share every line, and so a failure prints the
// rules that could explain both answers (which is what makes the finding
// minimizable instead of just a number mismatch).
template <typename Table>
void VerifyAgainstReference(Prepared *prep, Table *table) {
  uint32_t hops[kBatch];
  const size_t n = prep->keys.raw.size();
  for (size_t i = 0; i + kBatch <= n; i += kBatch) {
    table->LookupBatch(&prep->keys.raw[i], hops);
    for (size_t j = 0; j < kBatch; j++) {
      const uint32_t key = prep->keys.value[i + j];
      const uint32_t want = prep->reference.Lookup(key);
      if (hops[j] == want) {
        continue;
      }
      std::string rules;
      for (const Route &r : prep->routes) {
        if (r.next_hop != want && r.next_hop != hops[j]) {
          continue;
        }
        const uint32_t mask = r.len == 0 ? 0 : 0xFFFFFFFFu << (32 - r.len);
        if ((key & mask) == r.prefix) {
          char buf[64];
          snprintf(buf, sizeof(buf), " [/%u nh=%u]", r.len, r.next_hop);
          rules += buf;
        }
      }
      LOG(FATAL) << "table/independent-LPM disagreement: key 0x" << std::hex
                 << key << std::dec << " table=" << hops[j]
                 << " reference=" << want << " matching rules:" << rules;
    }
  }
}

// Builds the table, records build cost and footprint, and gates it.
template <typename Table>
Table *Prepare(Prepared *prep, const std::string &name) {
  const size_t before = EalHeapUsed();
  auto *table = new Table(name, prep->routes.size());

  const uint64_t t0 = NowNs();
  for (const Route &r : prep->routes) {
    CHECK_EQ(table->Add(r), 0)
        << "add failed for prefix len " << static_cast<int>(r.len);
  }
  prep->build_ns = NowNs() - t0;
  prep->bytes_per_route = static_cast<double>(EalHeapUsed() - before) /
                          static_cast<double>(prep->routes.size());

  VerifyAgainstReference(prep, table);
  return table;
}

template <typename Table, void (Table::*Lookup)(const uint32_t *, uint32_t *)>
void RunLookup(benchmark::State &state) {
  const size_t routes = static_cast<size_t>(state.range(0));

  // Setup and teardown sit *outside* the `for (auto _ : state)` loop, which
  // is where Google Benchmark's timer runs: pausing before the first
  // iteration is not a valid region and corrupts the accounting (first run
  // of this file reported 5.2 hours of "real time" that way).
  Prepared prep(routes);
  Table *table = Prepare<Table>(&prep, TableName());

  uint32_t next_hops[kBatch];
  uint64_t items = 0;
  const uint64_t cpu0 = ThreadCpuNs();
  for (auto _ : state) {
    for (size_t base = 0; base < prep.keys.raw.size(); base += kBatch) {
      (table->*Lookup)(&prep.keys.raw[base], next_hops);
      benchmark::DoNotOptimize(next_hops);
      items += kBatch;
    }
  }
  const uint64_t cpu_ns = ThreadCpuNs() - cpu0;
  delete table;  // after the loop: untimed, like the setup

  state.SetItemsProcessed(items);
  state.counters["cpu_ns_pkt"] = benchmark::Counter(
      static_cast<double>(cpu_ns) / static_cast<double>(items));
  state.counters["build_ns_route"] = benchmark::Counter(
      static_cast<double>(prep.build_ns) / static_cast<double>(routes));
  state.counters["bytes_route"] = benchmark::Counter(prep.bytes_per_route);
}

template <typename Table>
void RunAddDelete(benchmark::State &state) {
  const size_t routes = static_cast<size_t>(state.range(0));
  Prepared prep(routes);
  Table *table = Prepare<Table>(&prep, TableName());

  uint64_t items = 0;
  uint64_t errors = 0;  // counted, not CHECKed, inside the timed loop
  const uint64_t cpu0 = ThreadCpuNs();
  for (auto _ : state) {
    for (const Route &r : prep.routes) {
      errors += table->Delete(r) != 0;
      errors += table->Add(r) != 0;
      items += 2;
    }
  }
  const uint64_t cpu_ns = ThreadCpuNs() - cpu0;

  // A failed add/delete could be cheaper than a successful one, so without
  // these two checks the update-cost numbers would be unfalsifiable: first
  // that no operation failed during the timed loop, then that the table is
  // still the one the reference describes after all that churn.
  CHECK_EQ(errors, 0) << errors << " add/delete operations failed";
  VerifyAgainstReference(&prep, table);
  delete table;

  state.SetItemsProcessed(items);
  state.counters["cpu_ns_op"] = benchmark::Counter(static_cast<double>(cpu_ns) /
                                                   static_cast<double>(items));
}

// Sizes: 1K (small edge/ToR table), 64K (plausible transit FIB), 512K (large
// IPv4 FIB). 512K is this host's ceiling: both tables' tbl24 is fixed at
// 2^24 entries, which is 64MB for rte_lpm (4-byte entries) and -- because
// this bench configures dir24_8 with 4-byte next hops, for comparability --
// also 64MB for rte_fib; both come out of EAL's --no-huge heap.
void BM_LookupLpmVec(benchmark::State &state) {
  RunLookup<LpmTable, &LpmTable::LookupBatch>(state);
}
BENCHMARK(BM_LookupLpmVec)
    ->ArgNames({"routes"})
    ->Arg(1024)
    ->Arg(1 << 16)
    ->Arg(1 << 19);

void BM_LookupLpmScalar(benchmark::State &state) {
  RunLookup<LpmTable, &LpmTable::LookupBatchScalar>(state);
}
BENCHMARK(BM_LookupLpmScalar)
    ->ArgNames({"routes"})
    ->Arg(1024)
    ->Arg(1 << 16)
    ->Arg(1 << 19);

// FIB is registered only up to 64K routes: at 512K, with rules inserted in
// the generator's arbitrary (realistic) order -- what a control plane may do,
// adding a /24 after a /28 under it -- rte_fib (DPDK 25.11.3, DIR24_8,
// 4-byte next hops) returns a next hop matching no rule at all for some
// keys. Observed, not root-caused: deterministic across runs, unchanged by a
// 4x larger tbl8 pool, and gone when the same rule set goes in
// shortest-prefix-first, while rte_lpm and the independent reference agree in
// both orders. Re-verified 2026-09-19 with this file's full-key-stream gate
// and with add/delete return values checked. DPDK 25.11.3 already carries the
// upstream "fib: fix prefix addition handling" fix, so this is not that known
// defect. See MODERNIZATION.md entry 34; re-register 1 << 19 here after a DPDK
// fix that this gate accepts.
void BM_LookupFib(benchmark::State &state) {
  RunLookup<FibTable, &FibTable::LookupBatch>(state);
}
BENCHMARK(BM_LookupFib)->ArgNames({"routes"})->Arg(1024)->Arg(1 << 16);

void BM_AddDeleteLpm(benchmark::State &state) {
  RunAddDelete<LpmTable>(state);
}
BENCHMARK(BM_AddDeleteLpm)->ArgNames({"routes"})->Arg(1024)->Arg(1 << 16);

void BM_AddDeleteFib(benchmark::State &state) {
  RunAddDelete<FibTable>(state);
}
// FIB's add/delete comparison stops at 1K routes: at 64K, the delete+add
// churn of the generated rule set leaves some keys resolving to a next hop
// matching no rule -- caught by this benchmark's post-timing verification
// (added in the same review-hardening pass), while rte_lpm returns exactly the
// reference for the same churn at every size. Same defect class as the 512K
// build finding, but in the *update* path -- the path whose measured cost
// advantage motivated this experiment -- so FIB's 64K update numbers are not
// reportable. See MODERNIZATION.md entry 34.
BENCHMARK(BM_AddDeleteFib)->ArgNames({"routes"})->Arg(1024);

}  // namespace

int main(int argc, char **argv) {
  google::InitGoogleLogging(argv[0]);

  // Same sandbox-safe EAL as the other benchmarks in this tree: --no-huge,
  // malloc-backed, which is also what rte_lpm/rte_fib allocate from.
  bess::InitDpdk(0);

  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
