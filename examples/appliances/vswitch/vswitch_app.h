// SPDX-License-Identifier: BSD-3-Clause

#ifndef APPLIANCES_VSWITCH_VSWITCH_APP_H_
#define APPLIANCES_VSWITCH_VSWITCH_APP_H_

// Reference appliance R3 (roadmap M24): a policy vSwitch, the neutrality test.
// Its concepts are deliberately its own -- Layer, Group, Rule and
// CompiledDecision -- and BESS knows none of them. BESS supplies scope
// snapshots, worker-owned flow storage, RCU reclamation and the checked parse.
//
//   packet -> parse/key -> bind one tenant PolicyVersion -> decision cache
//                -> same version -> CompiledDecision -> execute
//                -> miss/stale -> compile that bound policy -> install ->
//                execute
//
// A policy and its cache revision are one immutable scope version. A packet
// never combines a separately published generation with another policy root.
// Replacing one tenant leaves the other tenant's cached decisions usable.
//
// R5, Hoverboard-style hierarchical mode (EnableHierarchy): a cold flow takes
// the default path -- evaluated per packet, nothing cached -- while the
// application counts it; when the application's own hotness rule says so, the
// decision is installed in the cache (the fast path) and handed to the
// application's promotion hook (a hardware flow, for example). Its direct
// group switch revokes the tenant's promoted decisions. Hierarchical apps
// are not enrolled as wire resources: a generic scope publisher cannot revoke
// application-owned hardware copies. Cold counts age: a flow idle for
// `idle_after` default-path packets is forgotten (an incremental sweep), so
// one-off flows cannot fill the table. BESS defines
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

#include "dataplane/resource_registry.h"
#include "dataplane/scope.h"
#include "framework/memory_scope.h"
#include "flow/worker_flow_table.h"
#include "packet_parse.h"
#include "rcu/rcu_domain.h"
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

// Copying a prepared version only copies this off-path owner. Packets borrow
// it from ScopeTable and never increment a shared_ptr reference count.
class PolicyVersion {
public:
  uint64_t revision() const noexcept { return revision_; }
  const Policy &policy() const noexcept { return *policy_; }

private:
  friend class VswitchApp;
  PolicyVersion(uint64_t revision, Policy policy)
      : revision_(revision),
        policy_(std::make_shared<const Policy>(std::move(policy))) {}
  uint64_t revision_;
  std::shared_ptr<const Policy> policy_;
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

  // Admit caches and scope storage before allocation. `memory` outlives the app.
  static std::expected<std::unique_ptr<VswitchApp>, std::string>
  Create(std::string name, bess::rcu::RcuDomain &rcu,
         bess::framework::MemoryScope &memory) {
    auto scopes = memory.LeaseFor<bess::dataplane::ScopeTable<PolicyVersion>>(
        "policies", kTenants);
    std::array<bess::framework::MemoryLease, kTenants> leases;
    for (auto &lease : leases) {
      lease = memory.LeaseFor<Cache>("decision_cache", kCacheCapacity);
    }
    std::unique_ptr<VswitchApp> app(
        new VswitchApp(std::move(name), rcu, memory, std::move(scopes)));
    for (size_t t = 0; t < kTenants; t++) {
      (void)app->policies_.Publish(
          ScopeOf(t), app->policies_.Make(app->MakePolicyVersion(StaticPolicy())));
      auto cache = Cache::Create(kCacheCapacity);
      if (!cache) {
        return std::unexpected(std::string("decision cache"));
      }
      app->caches_[t] = bess::framework::MemoryScope::Adopt(std::move(leases[t]), std::move(*cache));
    }
    return app;
  }

  ~VswitchApp() {
    const auto released = Release();
    CHECK(released) << released.error();
  }

  // The registry/engine becomes the only publisher. Direct R5 publication has
  // a hardware-revocation hook; generic ScopeResource publication does not.
  std::expected<void, std::string>
  Enroll(bess::dataplane::ResourceRegistry &registry) {
    if (cold_) {
      return std::unexpected("hierarchical policies use direct publication");
    }
    if (auto enrolled = registry.Register(&policy_resource_); !enrolled) {
      return enrolled;
    }
    registry_ = &registry;
    return {};
  }

  std::expected<void, std::string> Release() {
    if (registry_ == nullptr) {
      return {};
    }
    const std::array<std::string, 1> mine = {policy_resource_.name()};
    auto released = registry_->ReleaseForTeardown(mine);
    registry_ = nullptr;
    return released;
  }

  PolicyVersion MakePolicyVersion(Policy policy) {
    return PolicyVersion(next_revision_.fetch_add(1, std::memory_order_relaxed),
                         std::move(policy));
  }
  bess::dataplane::Resource &policy_resource() noexcept {
    return policy_resource_;
  }
  const bess::dataplane::ScopeTable<PolicyVersion> &
  policy_table() const noexcept {
    return policies_;
  }

  struct Hierarchy {
    uint32_t hot_after = 3;      // packets on the default path before a flow is hot
    uint32_t idle_after = 4096;  // default-path packets after which a cold count is forgotten
    // A hot flow's decision, just cached.
    std::function<void(const VswitchKey &, CompiledDecision)> promote;
    // A tenant's policy changed: its promoted decisions are stale.
    std::function<void(size_t tenant)> revoke;
  };

  // R5: flows start cold; a flow's decision is cached once it is hot by the
  // application's rule, and the hooks see promotions and revocations.
  bool EnableHierarchy(Hierarchy h) {
    if (registry_ != nullptr) {
      return false;
    }
    auto lease = memory_->LeaseFor<ColdTable>("cold_flows", kColdCapacity);
    auto cold = ColdTable::Create(kColdCapacity);
    if (!cold) {
      return false;
    }
    cold_ = bess::framework::MemoryScope::Adopt(std::move(lease), std::move(*cold));
    hierarchy_ = std::move(h);
    return true;
  }

  // The direct path: parse, look up, compile on a miss, execute. Returns the
  // decision (allow false: drop). A DSCP mark rewrites the IPv4 TOS in place.
  // `frame` is the packet's first segment, `total_len` its length (0: the span's).
  CompiledDecision Process(size_t tenant, std::span<uint8_t> frame, size_t total_len = 0) noexcept {
    bess::packet::ParsedFrame p;
    if (tenant >= kTenants ||
        bess::packet::ParseFrame(frame, p, total_len) != bess::packet::ParseStatus::kOk ||
        p.l3 != bess::packet::L3Kind::kIpv4) {
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
    const auto scope = ScopeOf(tenant);
    const PolicyVersion *version = policies_.Lookup(scope);
    if (version == nullptr) {
      return {}; // an erased scope never reuses a cached allow decision
    }
    CompiledDecision d;
    const CachedDecision *cached = caches_[tenant]->Find(key);
    if (cached != nullptr && cached->revision == version->revision()) {
      d = cached->decision;
      hits_[tenant]++;
    } else {
      d = Compile(version->policy(), key);
      compiles_[tenant]++;
      if (!cold_ || Hot(key)) {
        auto made = caches_[tenant]->Emplace(key);
        if (made.state != nullptr) {
          *made.state = CachedDecision{version->revision(), d};
          if (hierarchy_.promote) {
            const PolicyVersion *current = policies_.Lookup(scope);
            if (current != nullptr &&
                current->revision() == version->revision()) {
              hierarchy_.promote(key, d);
            }
          }
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

  // Direct, single-writer R5 control path. Enrolled R3 uses ScopeResource
  // transactions instead; both publish the same whole immutable version.
  bool SwitchGroup(size_t tenant, size_t layer, size_t group, Group replacement) {
    if (registry_ != nullptr || tenant >= kTenants) {
      return false;
    }
    const auto id = ScopeOf(tenant);
    const PolicyVersion *current = policies_.Current(id);
    if (current == nullptr || layer >= current->policy().layers.size() ||
        group >= current->policy().layers[layer].groups.size()) {
      return false;
    }
    Policy policy = current->policy();
    policy.layers[layer].groups[group] = std::move(replacement);
    auto next = policies_.Make(MakePolicyVersion(std::move(policy)));
    rcu_.ReserveRetirements(1);
    auto old = policies_.Publish(id, std::move(next));
    const auto grace = rcu_.StartGracePeriod();
    rcu_.Retire(grace, std::move(old));
    if (hierarchy_.revoke) {
      hierarchy_.revoke(tenant);
    }
    return true;
  }

  uint64_t hits(size_t t) const noexcept { return hits_[t]; }
  size_t cold_flows() const noexcept { return cold_ ? cold_->size() : 0; }
  uint64_t compiles(size_t t) const noexcept { return compiles_[t]; }
  size_t cached(size_t t) const noexcept { return caches_[t]->size(); }

 private:
  struct CachedDecision {
    uint64_t revision = 0;
    CompiledDecision decision{};
  };
  using Cache = bess::flow::WorkerFlowTable<VswitchKey, CachedDecision>;
  static constexpr size_t kCacheCapacity = 4096, kColdCapacity = 4096;
  static bess::dataplane::ScopeId ScopeOf(size_t tenant) noexcept {
    return bess::dataplane::ScopeId(static_cast<uint32_t>(tenant + 1));
  }
  struct ColdCount {
    uint32_t packets = 0;
    uint32_t last_tick = 0;  // the default-path packet that last saw the flow
  };
  using ColdTable = bess::flow::WorkerFlowTable<VswitchKey, ColdCount>;

  // The application's hotness rule: a flow is hot at its `hot_after`th
  // packet; until then its packets are counted (a full table: stays cold
  // until the sweep frees a slot).
  bool Hot(const VswitchKey &key) noexcept {
    const uint32_t now = ++tick_;
    Sweep(now);
    auto made = cold_->Emplace(key);
    if (made.state == nullptr) {
      return false;
    }
    made.state->last_tick = now;
    if (++made.state->packets < hierarchy_.hot_after) {
      return false;
    }
    (void)cold_->Erase(key);
    return true;
  }

  // Two slots per default-path packet: the whole table every capacity/2
  // packets, so a cold count lives at most idle_after + capacity/2 of them.
  void Sweep(uint32_t now) noexcept {
    std::array<bess::flow::FlowHandle, 2> idle;
    size_t n = 0;
    sweep_ = cold_->VisitRange(sweep_, idle.size(),
                               [&](bess::flow::FlowHandle h, const VswitchKey &, ColdCount &c) {
                                 if (now - c.last_tick > hierarchy_.idle_after) {
                                   idle[n++] = h;
                                 }
                               });
    if (sweep_ >= cold_->capacity()) {
      sweep_ = 0;
    }
    for (size_t i = 0; i < n; i++) {
      (void)cold_->Erase(idle[i]);
    }
  }

  VswitchApp(std::string name, bess::rcu::RcuDomain &rcu,
             bess::framework::MemoryScope &memory,
             bess::framework::MemoryLease scopes)
      : rcu_(rcu), scope_memory_(std::move(scopes)), policies_(kTenants),
        policy_resource_(std::move(name) + "/policies", policies_),
        memory_(&memory) {
    policies_.set_memory_growth(scope_memory_.growth());
    scope_memory_.Track(policies_);
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

  bess::rcu::RcuDomain &rcu_;
  bess::framework::MemoryLease scope_memory_;
  bess::dataplane::ScopeTable<PolicyVersion> policies_;
  bess::dataplane::ScopeResource<PolicyVersion> policy_resource_;
  bess::dataplane::ResourceRegistry *registry_ = nullptr;
  std::atomic<uint64_t> next_revision_{1};
  std::array<bess::framework::Sized<Cache>, kTenants> caches_;
  std::array<uint64_t, kTenants> hits_{}, compiles_{};
  bess::framework::Sized<ColdTable> cold_;
  bess::framework::MemoryScope *memory_;
  Hierarchy hierarchy_;
  uint32_t tick_ = 0;
  size_t sweep_ = 0;
};

}  // namespace appliance

#endif  // APPLIANCES_VSWITCH_VSWITCH_APP_H_
