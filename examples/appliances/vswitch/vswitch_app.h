// SPDX-License-Identifier: BSD-3-Clause

#ifndef APPLIANCES_VSWITCH_VSWITCH_APP_H_
#define APPLIANCES_VSWITCH_VSWITCH_APP_H_

// Reference appliance R3 (roadmap M24): a policy vSwitch, the neutrality test.
// Its concepts are deliberately its own -- Layer, Group, Rule and
// CompiledDecision -- and BESS knows none of them. BESS provides the flow
// table under the decision cache and its generation-based invalidation, RCU
// publication of immutable policy, and the checked parse.
//
//   packet -> parse/key -> DecisionCache
//                -> hit  -> CompiledDecision -> execute
//                -> miss -> compile (walk the layers) -> install -> execute
//
// Policy is per tenant (a scope): each tenant has its own published policy and
// its own DecisionGeneration, so switching one tenant's group invalidates that
// tenant's cached decisions in O(1) and leaves every other tenant's hits alone.
//
// R5, Hoverboard-style hierarchical mode (EnableHierarchy): a cold flow takes
// the default path -- evaluated per packet, nothing cached -- while the
// application counts it; when the application's own hotness rule says so, the
// decision is installed in the cache (the fast path) and handed to the
// application's promotion hook (a hardware flow, for example). BESS defines
// neither the rule nor the promotion.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "conntrack/packet_parse.h"
#include "flow/decision_cache.h"
#include "flow/worker_flow_table.h"
#include "rcu/rcu_domain.h"
#include "rcu/rcu_ptr.h"
#include "utils/checksum.h"

namespace appliance {

// -- the application's policy model (BESS knows none of it) -----------------------------

enum class Action : uint8_t { kAllow, kDeny, kMark, kRedirect };

struct Rule {
  uint32_t dst = 0, dst_mask = 0;  // host order
  uint16_t dport_lo = 0, dport_hi = 65535;
  uint8_t proto = 0;  // 0: any
  Action action = Action::kAllow;
  uint8_t dscp = 0;   // kMark
  uint16_t gate = 0;  // kRedirect
};
struct Group {
  std::vector<Rule> rules;  // first match decides the group
};
struct Layer {
  std::vector<Group> groups;  // the first group with a match decides the layer
};
struct Policy {
  std::vector<Layer> layers;  // every layer applies; a deny in any wins
};

// What a flow compiled to: a value, so the cache holds it as the decision id.
struct CompiledDecision {
  uint8_t allow = 0;
  uint8_t dscp = 0xff;  // 0xff: leave the DSCP
  uint16_t gate = 0;
};
static_assert(sizeof(CompiledDecision) == 4);

// The flow key the cache looks up: tenant and five-tuple, no padding.
struct VswitchKey {
  uint32_t src, dst;
  uint16_t sport, dport;
  uint8_t proto, tenant;
  uint16_t zero;
};
static_assert(std::has_unique_object_representations_v<VswitchKey>);

class VswitchApp {
 public:
  static constexpr size_t kTenants = 2;

  static std::expected<std::unique_ptr<VswitchApp>, std::string> Create(bess::rcu::RcuDomain &rcu) {
    std::unique_ptr<VswitchApp> app(new VswitchApp(rcu));
    for (size_t t = 0; t < kTenants; t++) {
      app->policies_[t]->Initialize(std::make_unique<const Policy>(StaticPolicy()));
      auto cache = Cache::Create(4096, app->generations_[t]);
      if (!cache) {
        return std::unexpected(std::string("decision cache"));
      }
      app->caches_[t] = std::move(*cache);
    }
    return app;
  }

  ~VswitchApp() {
    for (auto &p : policies_) {
      (void)p->ResetQuiesced();  // the owner destroys it with readers stopped
    }
  }

  using Promote = std::function<void(const VswitchKey &, CompiledDecision)>;

  // R5: flows start cold; a flow's decision is cached once it has sent
  // `hot_after` packets (the application's rule), and `promote` sees it then.
  bool EnableHierarchy(uint32_t hot_after, Promote promote) {
    auto cold = ColdTable::Create(4096);
    if (!cold) {
      return false;
    }
    cold_ = std::move(*cold);
    hot_after_ = hot_after;
    promote_ = std::move(promote);
    return true;
  }

  // The direct path: parse, look up, compile on a miss, execute. Returns the
  // decision (allow false: drop). A DSCP mark rewrites the IPv4 TOS in place.
  CompiledDecision Process(size_t tenant, std::span<uint8_t> frame) noexcept {
    bess::conntrack::ParsedFlowPacket p;
    if (tenant >= kTenants ||
        bess::conntrack::ParseFrame(frame, p) != bess::conntrack::ParseStatus::kOk ||
        p.l3 != bess::conntrack::L3Kind::kIpv4) {
      return {};
    }
    VswitchKey key{};
    std::memcpy(&key.src, p.src.data(), 4);
    std::memcpy(&key.dst, p.dst.data(), 4);
    key.src = __builtin_bswap32(key.src);
    key.dst = __builtin_bswap32(key.dst);
    key.sport = p.src_port;
    key.dport = p.dst_port;
    key.proto = p.protocol;
    key.tenant = static_cast<uint8_t>(tenant);
    CompiledDecision d;
    if (caches_[tenant]->Lookup(key, &d) == bess::flow::DecisionLookup::kHit) {
      hits_[tenant]++;
    } else {
      // Generation first, then the policy (published before the generation
      // moved): an install compiled against older policy is refused.
      const uint64_t g = generations_[tenant].Current();
      d = Compile(*policies_[tenant]->Read(), key);
      compiles_[tenant]++;
      if (cold_ == nullptr || Hot(key)) {
        if (caches_[tenant]->Install(key, d, g) != bess::flow::DecisionInstall::kStaleGeneration &&
            promote_) {
          promote_(key, d);
        }
      }
    }
    if (d.allow && d.dscp != 0xff) {
      uint8_t *ip = frame.data() + p.l3_offset;
      uint16_t before, after;
      std::memcpy(&before, ip, 2);
      ip[1] = static_cast<uint8_t>((d.dscp << 2) | (ip[1] & 0x3));
      std::memcpy(&after, ip, 2);
      uint16_t csum;
      std::memcpy(&csum, ip + 10, 2);
      csum = bess::utils::UpdateChecksum16(csum, before, after);
      std::memcpy(ip + 10, &csum, 2);
    }
    return d;
  }

  // The policy compiler's switch (control side): replaces one group of one
  // tenant's layer, atomically: the whole new policy is built, published
  // (RCU), and only then is the tenant's generation moved, so its cached
  // decisions read stale (O(1), no walk) and recompile against the new policy.
  // Other tenants' scopes are untouched.
  bool SwitchGroup(size_t tenant, size_t layer, size_t group, Group replacement) {
    const Policy &current = *policies_[tenant]->Read();
    if (layer >= current.layers.size() || group >= current.layers[layer].groups.size()) {
      return false;
    }
    auto next = std::make_unique<Policy>(current);
    next->layers[layer].groups[group] = std::move(replacement);
    (void)policies_[tenant]->Publish(std::move(next));
    generations_[tenant].Invalidate();
    return true;
  }

  uint64_t hits(size_t t) const noexcept { return hits_[t]; }
  size_t cold_flows() const noexcept { return cold_ != nullptr ? cold_->size() : 0; }
  uint64_t compiles(size_t t) const noexcept { return compiles_[t]; }
  size_t cached(size_t t) const noexcept { return caches_[t]->size(); }
  uint64_t generation(size_t t) const noexcept { return generations_[t].Current(); }

 private:
  using Cache = bess::flow::DecisionCache<VswitchKey, CompiledDecision>;
  using ColdTable = bess::flow::WorkerFlowTable<VswitchKey, uint32_t>;

  // The application's hotness rule: a flow is hot at its `hot_after`th
  // packet; until then its packets are counted (a full table: stays cold).
  bool Hot(const VswitchKey &key) noexcept {
    auto made = cold_->Emplace(key, 0u);
    if (made.state == nullptr) {
      return false;
    }
    if (++*made.state < hot_after_) {
      return false;
    }
    (void)cold_->Erase(key);
    return true;
  }

  explicit VswitchApp(bess::rcu::RcuDomain &rcu) {
    for (auto &p : policies_) {
      p = std::make_unique<bess::rcu::RcuPtr<Policy>>(rcu);
    }
  }

  // The static policy each tenant starts with. Layer 0 (an ACL): group 0
  // allows web (80) and DNS (53), group 1 denies SSH (22). Layer 1 (QoS):
  // marks web DSCP 10 and redirects 10.9.0.0/16 to gate 1.
  static Policy StaticPolicy() {
    Policy p;
    Layer acl;
    acl.groups.push_back(Group{{Rule{0, 0, 80, 80, 6, Action::kAllow}, Rule{0, 0, 53, 53, 0, Action::kAllow}}});
    acl.groups.push_back(Group{{Rule{0, 0, 22, 22, 6, Action::kDeny}}});
    Layer qos;
    qos.groups.push_back(Group{{Rule{0, 0, 80, 80, 6, Action::kMark, 10}}});
    qos.groups.push_back(Group{{Rule{0x0a090000, 0xffff0000, 0, 65535, 0, Action::kRedirect, 0, 1}}});
    p.layers = {acl, qos};
    return p;
  }

  static bool Matches(const Rule &r, const VswitchKey &k) noexcept {
    return (k.dst & r.dst_mask) == r.dst && k.dport >= r.dport_lo && k.dport <= r.dport_hi &&
           (r.proto == 0 || r.proto == k.proto);
  }

  // The application's semantics: in each layer, the first group with a
  // matching rule decides by its first matching rule; a deny in any layer
  // denies; marks and redirects accumulate, the last one wins. No match in a
  // layer: allow.
  static CompiledDecision Compile(const Policy &policy, const VswitchKey &k) noexcept {
    CompiledDecision d{1, 0xff, 0};
    for (const Layer &layer : policy.layers) {
      for (const Group &group : layer.groups) {
        const Rule *hit = nullptr;
        for (const Rule &r : group.rules) {
          if (Matches(r, k)) {
            hit = &r;
            break;
          }
        }
        if (hit == nullptr) {
          continue;
        }
        switch (hit->action) {
          case Action::kDeny:
            return CompiledDecision{0, 0xff, 0};
          case Action::kMark:
            d.dscp = hit->dscp;
            break;
          case Action::kRedirect:
            d.gate = hit->gate;
            break;
          case Action::kAllow:
            break;
        }
        break;  // this group decided the layer
      }
    }
    return d;
  }

  std::array<std::unique_ptr<bess::rcu::RcuPtr<Policy>>, kTenants> policies_;
  std::array<bess::flow::DecisionGeneration, kTenants> generations_;
  std::array<std::unique_ptr<Cache>, kTenants> caches_;
  std::array<uint64_t, kTenants> hits_{}, compiles_{};
  std::unique_ptr<ColdTable> cold_;
  uint32_t hot_after_ = 0;
  Promote promote_;
};

}  // namespace appliance

#endif  // APPLIANCES_VSWITCH_VSWITCH_APP_H_
