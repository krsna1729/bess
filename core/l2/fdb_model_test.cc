// SPDX-License-Identifier: BSD-3-Clause

// Fdb (M14, D-064) against a reference model of its whole public surface
// (M22). fdb_test.cc's DifferentialAgainstLegacyBridgeSemantics compares
// forwarding in one domain against the legacy Bridge (learn, move, statics
// programmed once, aging with an unlimited budget, learning stopped at the
// table size); this model adds what that one does not reach: learn results
// (refreshed vs moved vs static), static programming over dynamic entries,
// Remove and Flush, multicast and invalid-interface refusal, unknown
// domains, a learn limit below capacity, budgeted aging and its order, a
// coarse wheel and tick wraparound, flood groups, the VLAN -> domain map,
// batch lookups, size() and dynamic_entries().
//
// Contract under test, as the model encodes it (fdb.h's comments):
//   key = (domain, MAC) -> {interface, static or dynamic, deadline}.
//   - Learn(domain, mac, interface, now): a multicast MAC, the invalid
//     interface or domain >= max_domains -> kIgnored; a static entry ->
//     kStatic, unchanged; a dynamic entry: the deadline becomes now + aging + 1
//     (usable while now - learned <= aging) and kRefreshed (same interface) or
//     kMoved (interface replaced); no entry: size() >= learn_limit -> kFull,
//     else kLearned.
//   - AddStatic: invalid interface, multicast MAC or unknown domain ->
//     kInvalid; an existing entry (static or dynamic) becomes static with the
//     interface -> kReplaced (it never ages); else size() == capacity -> kFull,
//     else kAdded.
//   - Remove: true iff the (known-domain) key had an entry.
//   - Flush(static_too): removes every dynamic entry, and the static ones too
//     when asked.
//   - Age(now, budget): a dynamic entry is due when its deadline rounded up to
//     the wheel granularity 2^g is at or before now rounded down (serial
//     arithmetic; ticks may wrap). Unlimited budget: exactly the due entries
//     leave. Budget b: at most b leave, only due ones, in non-decreasing
//     rounded deadline order. Returns how many left.
//   - Lookup / LookupBatch: the entry's interface, else kInvalidInterfaceId
//     (unknown domains, multicast and the (0xFFFF, broadcast) key included);
//     batch bit i set iff a hit.
//   - SetFloodGroup(domain, members): false and unchanged for an unknown
//     domain, more than kMaxFloodGroup members or an invalid member; else the
//     group is replaced. FloodGroup: the group, empty for an unknown domain.
//   - MapVlan(vlan, domain) for vlan < 4096 sets the map (others ignored);
//     DomainOfVlan: the mapped domain, kNoDomain if unmapped or vlan >= 4096.
//   - size() counts every entry; dynamic_entries() the dynamic ones.

#include "l2/fdb.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace bess::l2 {
namespace {

using dataplane::InterfaceId;
using dataplane::kInvalidInterfaceId;

// MAC i: unicast for i < 1000; 1000 + j: multicast; 2000: broadcast.
MacAddress Mac(uint32_t i) {
  if (i == 2000) return MacAddress{{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
  const uint8_t first = i >= 1000 ? 0x01 : 0x02;
  return MacAddress{{first, 0x00, 0x5e, 0x10, static_cast<uint8_t>(i >> 8), static_cast<uint8_t>(i)}};
}

struct ModelEntry {
  uint16_t iface;
  bool is_static;
  uint64_t deadline;
};

struct FdbModelConfig {
  uint32_t seed;
  size_t capacity;
  size_t learn_limit;  // 0: capacity
  uint64_t aging;
  unsigned shift;
  size_t max_domains;
  uint64_t start;
  size_t steps;
};

class FdbModel {
 public:
  explicit FdbModel(const FdbModelConfig &cfg) : cfg_(cfg), rng_(cfg.seed), now_(cfg.start) {
    Fdb::Config c;
    c.capacity = cfg.capacity;
    c.learn_limit = cfg.learn_limit;
    c.aging = cfg.aging;
    c.granularity_shift = cfg.shift;
    c.max_domains = cfg.max_domains;
    c.start = cfg.start;
    auto made = Fdb::Create(c);
    EXPECT_TRUE(made.has_value());
    fdb_ = std::move(*made);
    limit_ = cfg.learn_limit ? cfg.learn_limit : cfg.capacity;
    flood_.resize(cfg.max_domains);
  }

  void Run() {
    for (size_t step = 0; step < cfg_.steps; step++) {
      SCOPED_TRACE(::testing::Message() << "step " << step << " now " << now_);
      const uint32_t op = rng_() % 100;
      if (op < 38) {
        Learn();
      } else if (op < 46) {
        AddStatic();
      } else if (op < 51) {
        Remove();
      } else if (op < 52) {
        Flush();
      } else if (op < 62) {
        Age();
      } else if (op < 76) {
        Lookup();
      } else if (op < 80) {
        LookupBatch();
      } else if (op < 84) {
        Flood();
      } else if (op < 88) {
        Vlan();
      } else {
        now_ += rng_() % 20 == 0 ? cfg_.aging / 2 + rng_() % cfg_.aging : rng_() % 6;
      }
      if (::testing::Test::HasFatalFailure()) return;
      ASSERT_EQ(entries_.size(), fdb_->size());
      ASSERT_EQ(Dynamic(), fdb_->dynamic_entries());
    }
    for (const auto &[k, e] : entries_) {
      ASSERT_EQ(InterfaceId(e.iface), fdb_->Lookup(BridgeDomainId(k.first), Mac(k.second)));
    }
    for (const char *what : {"learned", "refreshed", "moved", "static", "ignored", "full",
                             "replaced", "aged", "budget stop", "flushed"}) {
      EXPECT_GT(counts_[what], 0u) << what;
    }
  }

 private:
  using Key = std::pair<uint16_t, uint32_t>;  // (domain, MAC index)

  uint64_t Mask() const { return (uint64_t{1} << cfg_.shift) - 1; }
  static bool NotAfter(uint64_t a, uint64_t b) { return static_cast<int64_t>(b - a) >= 0; }
  uint64_t RoundUp(uint64_t t) const { return (t + Mask()) & ~Mask(); }
  bool Due(const ModelEntry &e) const {
    return !e.is_static && NotAfter(RoundUp(e.deadline), now_ & ~Mask());
  }
  size_t Dynamic() const {
    return static_cast<size_t>(std::count_if(entries_.begin(), entries_.end(),
                                             [](const auto &kv) { return !kv.second.is_static; }));
  }

  uint16_t RandomDomain() {
    const uint32_t r = rng_() % 20;
    if (r == 0) return static_cast<uint16_t>(cfg_.max_domains);  // unknown
    if (r == 1) return 0xFFFF;
    return static_cast<uint16_t>(rng_() % cfg_.max_domains);
  }
  uint32_t RandomMac() {
    const uint32_t r = rng_() % 20;
    if (r == 0) return 1000 + rng_() % 3;
    if (r == 1) return 2000;
    return rng_() % 24;
  }
  uint16_t RandomIface() { return rng_() % 25 == 0 ? 0 : static_cast<uint16_t>(1 + rng_() % 5); }
  bool Known(uint16_t domain) const { return domain < cfg_.max_domains; }

  void Learn() {
    const uint16_t d = RandomDomain();
    const uint32_t m = RandomMac();
    const uint16_t i = RandomIface();
    LearnResult want;
    if (Mac(m).multicast() || i == 0 || !Known(d)) {
      want = LearnResult::kIgnored;
      counts_["ignored"]++;
    } else if (auto it = entries_.find({d, m}); it != entries_.end()) {
      if (it->second.is_static) {
        want = LearnResult::kStatic;
        counts_["static"]++;
      } else {
        it->second.deadline = now_ + cfg_.aging + 1;
        want = it->second.iface == i ? LearnResult::kRefreshed : LearnResult::kMoved;
        counts_[it->second.iface == i ? "refreshed" : "moved"]++;
        it->second.iface = i;
      }
    } else if (entries_.size() >= limit_) {
      want = LearnResult::kFull;
      counts_["full"]++;
    } else {
      want = LearnResult::kLearned;
      entries_[{d, m}] = {i, false, now_ + cfg_.aging + 1};
      counts_["learned"]++;
    }
    ASSERT_EQ(want, fdb_->Learn(BridgeDomainId(d), Mac(m), InterfaceId(i), now_))
        << "domain " << d << " mac " << m << " iface " << i;
  }

  void AddStatic() {
    const uint16_t d = RandomDomain();
    const uint32_t m = RandomMac();
    const uint16_t i = RandomIface();
    ProgramResult want;
    if (Mac(m).multicast() || i == 0 || !Known(d)) {
      want = ProgramResult::kInvalid;
    } else if (auto it = entries_.find({d, m}); it != entries_.end()) {
      it->second = {i, true, 0};
      want = ProgramResult::kReplaced;
      counts_["replaced"]++;
    } else if (entries_.size() == cfg_.capacity) {
      want = ProgramResult::kFull;
      counts_["full"]++;
    } else {
      entries_[{d, m}] = {i, true, 0};
      want = ProgramResult::kAdded;
    }
    ASSERT_EQ(want, fdb_->AddStatic(BridgeDomainId(d), Mac(m), InterfaceId(i)))
        << "domain " << d << " mac " << m << " iface " << i;
  }

  void Remove() {
    uint16_t d = RandomDomain();
    uint32_t m = RandomMac();
    if (!entries_.empty() && rng_() % 2) {
      auto it = entries_.begin();
      std::advance(it, rng_() % entries_.size());
      d = it->first.first, m = it->first.second;
    }
    const bool want = Known(d) && entries_.erase({d, m}) != 0;
    ASSERT_EQ(want, fdb_->Remove(BridgeDomainId(d), Mac(m))) << "domain " << d << " mac " << m;
  }

  void Flush() {
    const bool static_too = rng_() % 3 == 0;
    fdb_->Flush(static_too);
    std::erase_if(entries_, [&](const auto &kv) { return static_too || !kv.second.is_static; });
    counts_["flushed"]++;
  }

  void Age() {
    std::vector<Key> due;
    for (const auto &[k, e] : entries_) {
      if (Due(e)) due.push_back(k);
    }
    const bool limited = rng_() % 2;
    const size_t budget = limited ? 1 + rng_() % 3 : size_t{1} << 30;
    const size_t removed = fdb_->Age(now_, budget);
    std::vector<Key> gone;
    for (const auto &[k, e] : entries_) {
      if (fdb_->Lookup(BridgeDomainId(k.first), Mac(k.second)) == kInvalidInterfaceId) {
        gone.push_back(k);
      }
    }
    ASSERT_EQ(gone.size(), removed);
    ASSERT_LE(removed, budget);
    for (const Key &k : gone) {
      ASSERT_TRUE(std::find(due.begin(), due.end(), k) != due.end())
          << "entry (" << k.first << ", " << k.second << ") aged before its deadline (static "
          << entries_.at(k).is_static << ")";
    }
    if (!limited) {
      ASSERT_EQ(due.size(), removed) << "an unlimited Age leaves nothing due";
    } else {
      if (removed < due.size()) counts_["budget stop"]++;
      for (const Key &g : gone) {
        for (const Key &k : due) {
          if (std::find(gone.begin(), gone.end(), k) != gone.end()) continue;
          ASSERT_TRUE(NotAfter(RoundUp(entries_.at(g).deadline), RoundUp(entries_.at(k).deadline)))
              << "aged out of deadline order";
        }
      }
    }
    for (const Key &k : gone) entries_.erase(k);
    counts_["aged"] += removed;
  }

  InterfaceId Want(uint16_t d, uint32_t m) const {
    auto it = entries_.find({d, m});
    return it == entries_.end() ? kInvalidInterfaceId : InterfaceId(it->second.iface);
  }

  void Lookup() {
    const uint16_t d = RandomDomain();
    const uint32_t m = RandomMac();
    ASSERT_EQ(Want(d, m), fdb_->Lookup(BridgeDomainId(d), Mac(m))) << "domain " << d << " mac " << m;
  }

  void LookupBatch() {
    const size_t n = 1 + rng_() % Fdb::kMaxBatch;
    std::vector<FdbKey> keys(n);
    std::vector<Key> model_keys(n);
    for (size_t i = 0; i < n; i++) {
      model_keys[i] = {RandomDomain(), RandomMac()};
      keys[i] = MakeKey(BridgeDomainId(model_keys[i].first), Mac(model_keys[i].second));
    }
    std::vector<InterfaceId> out(n);
    const uint64_t hits = fdb_->LookupBatch(keys, out);
    for (size_t i = 0; i < n; i++) {
      const InterfaceId want = Want(model_keys[i].first, model_keys[i].second);
      ASSERT_EQ(want, out[i]) << "position " << i;
      ASSERT_EQ(want != kInvalidInterfaceId, (hits >> i & 1) != 0) << "position " << i;
    }
  }

  void Flood() {
    const uint16_t d = rng_() % 6 == 0 ? static_cast<uint16_t>(cfg_.max_domains) : static_cast<uint16_t>(rng_() % cfg_.max_domains);
    if (rng_() % 2) {
      const uint32_t r = rng_() % 10;
      const size_t n = r == 0 ? Fdb::kMaxFloodGroup + 1 : r == 1 ? Fdb::kMaxFloodGroup : rng_() % 6;
      std::vector<InterfaceId> members(n);
      for (auto &m : members) m = InterfaceId(static_cast<uint16_t>(1 + rng_() % 300));
      if (n > 0 && rng_() % 6 == 0) members[rng_() % n] = kInvalidInterfaceId;
      const bool valid = Known(d) && n <= Fdb::kMaxFloodGroup &&
                         std::find(members.begin(), members.end(), kInvalidInterfaceId) == members.end();
      if (valid) flood_[d] = members;
      ASSERT_EQ(valid, fdb_->SetFloodGroup(BridgeDomainId(d), members)) << "domain " << d << " n " << n;
    }
    const auto got = fdb_->FloodGroup(BridgeDomainId(d));
    const std::vector<InterfaceId> want = Known(d) ? flood_[d] : std::vector<InterfaceId>{};
    ASSERT_EQ(want, std::vector<InterfaceId>(got.begin(), got.end())) << "domain " << d;
  }

  void Vlan() {
    const uint32_t r = rng_() % 10;
    const uint16_t vlan = r == 0 ? static_cast<uint16_t>(4096 + rng_() % 4) : r == 1 ? 4095 : static_cast<uint16_t>(rng_() % 16);
    if (rng_() % 2) {
      const uint16_t d = static_cast<uint16_t>(rng_() % (cfg_.max_domains + 1));
      fdb_->MapVlan(vlan, BridgeDomainId(d));
      if (vlan < 4096) vlans_[vlan] = d;
    }
    auto it = vlans_.find(vlan);
    const BridgeDomainId want = it == vlans_.end() ? Fdb::kNoDomain : BridgeDomainId(it->second);
    ASSERT_EQ(want, fdb_->DomainOfVlan(vlan)) << "vlan " << vlan;
  }

  FdbModelConfig cfg_;
  std::mt19937 rng_;
  uint64_t now_;
  size_t limit_;
  std::unique_ptr<Fdb> fdb_;
  std::map<Key, ModelEntry> entries_;
  std::vector<std::vector<InterfaceId>> flood_;
  std::map<uint16_t, uint16_t> vlans_;
  std::map<std::string, size_t> counts_;
};

TEST(FdbModelTest, RandomLearningProgrammingAgingAndMapsMatchAModel) {
  const uint64_t kNearWrap = ~uint64_t{0} - 2000;
  const FdbModelConfig configs[] = {
      // seed, capacity, learn_limit, aging, shift, max_domains, start, steps
      {1, 32, 0, 300, 0, 3, 0, 40000},
      {2, 32, 20, 300, 0, 3, 0, 40000},
      {3, 32, 24, 300, 3, 3, kNearWrap & ~uint64_t{7}, 40000},
      {4, 16, 0, 120, 0, 2, kNearWrap, 40000},
      {5, 48, 40, 500, 5, 4, 0, 40000},
  };
  for (const auto &cfg : configs) {
    SCOPED_TRACE(::testing::Message() << "seed " << cfg.seed);
    FdbModel model(cfg);
    model.Run();
    if (::testing::Test::HasFatalFailure()) return;
  }
}

}  // namespace
}  // namespace bess::l2
