// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_OFFLOAD_RTE_FLOW_BACKEND_H_
#define BESS_OFFLOAD_RTE_FLOW_BACKEND_H_

#include <rte_byteorder.h>
#include <rte_ethdev.h>
#include <rte_flow.h>

#include <cerrno>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "offload/flow_rule_owner.h"

namespace bess::offload {

// The rte_flow backend for FlowRuleOwner (M20, D-070): the application's
// native rte_flow attributes, pattern and actions go to the device as they are
// -- BESS adds no flow IR. Two things are filled in for the owner: a MARK action
// whose `conf` is null gets the owner's MARK value, and a COUNT action is
// what Query reads. This backend uses the synchronous rte_flow_create and
// rte_flow_destroy (capability `async` false); their results are queued and
// delivered by Poll, so the owner's state machine is the same as for an
// asynchronous backend. The rte_flow template/async API is not used yet.
//
// Capabilities: `supported` is whether the port has rte_flow operations at
// all (rte_flow_validate answers something other than ENOSYS); the owner
// installs only there. The other fields are hints: the PMD's rte_flow_validate
// answer for one sample rule of that kind (match an IPv4 destination and a UDP
// port; then with a MARK, a COUNT, a VXLAN match, the transfer attribute),
// each ending in QUEUE 0 or else DROP. A PMD that refuses the sample may accept
// the application's rule, so the owner does not gate on them; the device's
// answer at install (or Validate beforehand) decides. `mark_bits` is a lower
// bound: the widest of 32, 24, 16 and 8 bits whose top bit the PMD accepts as a
// MARK value. A positive answer is kept per port (Reprobe after the port is
// reconfigured or reset); a negative one is asked again next time.
//
// Not tested against hardware here (no NIC on the CI runners): the unit tests
// cover the invalid-port path; a real-hardware certification matrix is
// separate (D-070).
class RteFlowBackend {
 public:
  struct Rule {
    rte_flow_attr attr{};
    std::vector<rte_flow_item> pattern;   // ends with RTE_FLOW_ITEM_TYPE_END
    std::vector<rte_flow_action> actions; // ends with RTE_FLOW_ACTION_TYPE_END
  };
  using HwHandle = rte_flow *;

  FlowCapabilities Capabilities(uint16_t port) const {
    if (!rte_eth_dev_is_valid_port(port)) {
      return {};  // not kept: the port may be probed (attached) later
    }
    auto it = caps_.find(port);
    if (it != caps_.end()) {
      return it->second;
    }
    const FlowCapabilities c = Probe(port);
    if (c.supported) {
      caps_.emplace(port, c);
    }
    return c;
  }

  // Forgets what a port reported, so the next Capabilities asks again.
  void Reprobe(uint16_t port) { caps_.erase(port); }

  // Asks the PMD whether it would accept `rule` (a null MARK conf is checked
  // with MARK value 1). The native escape hatch for an application compiler.
  bool Validate(uint16_t port, const Rule &rule, int &error) const {
    std::vector<rte_flow_action> actions = rule.actions;
    rte_flow_action_mark mark_conf{1};
    for (auto &a : actions) {
      if (a.type == RTE_FLOW_ACTION_TYPE_MARK && a.conf == nullptr) {
        a.conf = &mark_conf;
      }
    }
    rte_flow_error err{};
    const int rc = rte_flow_validate(port, &rule.attr, rule.pattern.data(), actions.data(), &err);
    error = rc == 0 ? 0 : -rc;
    return rc == 0;
  }

  bool Submit(uint16_t port, const Rule &rule, uint32_t mark, uint64_t tag, int &error) {
    std::vector<rte_flow_action> actions = rule.actions;
    rte_flow_action_mark mark_conf{mark};
    for (auto &a : actions) {
      if (a.type == RTE_FLOW_ACTION_TYPE_MARK && a.conf == nullptr) {
        a.conf = &mark_conf;
      }
    }
    // The completion record exists before the device call: once the device
    // holds the rule, nothing here can throw and leave it untracked (M22).
    done_.push_back({tag, true, nullptr, 0});
    rte_flow_error err{};
    rte_flow *flow = rte_flow_create(port, &rule.attr, rule.pattern.data(), actions.data(), &err);
    if (flow == nullptr) {
      done_.pop_back();
      error = rte_errno != 0 ? rte_errno : EINVAL;
      return false;
    }
    done_.back().hw = flow;
    return true;
  }

  bool Remove(uint16_t port, HwHandle hw, uint64_t tag, int &error) {
    done_.push_back({tag, true, hw, 0});  // before the device call, as in Submit
    rte_flow_error err{};
    if (rte_flow_destroy(port, hw, &err) != 0) {
      done_.pop_back();
      error = rte_errno != 0 ? rte_errno : EINVAL;
      return false;
    }
    return true;
  }

  template <typename Fn>
  size_t Poll(Fn &&fn) {
    size_t n = 0;
    while (!done_.empty()) {
      const Done d = done_.front();
      done_.pop_front();
      fn(d.tag, d.ok, d.hw, d.error);
      n++;
    }
    return n;
  }

  bool Query(uint16_t port, HwHandle hw, FlowRuleStats &out) {
    rte_flow_query_count count{};
    count.reset = 0;
    const rte_flow_action action{RTE_FLOW_ACTION_TYPE_COUNT, nullptr};
    rte_flow_error err{};
    if (rte_flow_query(port, hw, &action, &count, &err) != 0) {
      return false;
    }
    out.hits = count.hits_set ? count.hits : 0;
    out.bytes = count.bytes_set ? count.bytes : 0;
    return true;
  }

 private:
  // rte_flow_validate of a sample rule matching `pattern`, with `action` (or
  // none) and a fate, QUEUE 0 or else DROP: 0 if accepted, else the last
  // negative errno.
  static int TrySample(uint16_t port, const rte_flow_attr &attr, const rte_flow_item *pattern,
                       const rte_flow_action *action) {
    const rte_flow_action_queue queue{0};
    const rte_flow_action fates[2] = {{RTE_FLOW_ACTION_TYPE_QUEUE, &queue},
                                      {RTE_FLOW_ACTION_TYPE_DROP, nullptr}};
    int rc = -EINVAL;
    for (const rte_flow_action &fate : fates) {
      rte_flow_action actions[3];
      size_t n = 0;
      if (action != nullptr) {
        actions[n++] = *action;
      }
      actions[n++] = fate;
      actions[n] = {RTE_FLOW_ACTION_TYPE_END, nullptr};
      rte_flow_error err{};
      rc = rte_flow_validate(port, &attr, pattern, actions, &err);
      if (rc == 0) {
        return 0;
      }
    }
    return rc;
  }

  static FlowCapabilities Probe(uint16_t port) {
    FlowCapabilities c;
    // The sample: Ethernet / IPv4 to 192.0.2.1 / UDP to port 9 (fully masked
    // fields; PMDs refuse wildcard-only patterns), and the same over VXLAN.
    rte_flow_item_ipv4 ip_spec{}, ip_mask{};
    ip_spec.hdr.dst_addr = rte_cpu_to_be_32(0xc0000201);
    ip_mask.hdr.dst_addr = 0xffffffff;
    rte_flow_item_udp udp_spec{}, udp_mask{}, vxlan_udp_spec{};
    udp_spec.hdr.dst_port = rte_cpu_to_be_16(9);
    udp_mask.hdr.dst_port = 0xffff;
    vxlan_udp_spec.hdr.dst_port = rte_cpu_to_be_16(4789);
    const rte_flow_item sample[4] = {{RTE_FLOW_ITEM_TYPE_ETH, nullptr, nullptr, nullptr},
                                     {RTE_FLOW_ITEM_TYPE_IPV4, &ip_spec, nullptr, &ip_mask},
                                     {RTE_FLOW_ITEM_TYPE_UDP, &udp_spec, nullptr, &udp_mask},
                                     {RTE_FLOW_ITEM_TYPE_END, nullptr, nullptr, nullptr}};
    const rte_flow_item vxlan[5] = {{RTE_FLOW_ITEM_TYPE_ETH, nullptr, nullptr, nullptr},
                                    {RTE_FLOW_ITEM_TYPE_IPV4, &ip_spec, nullptr, &ip_mask},
                                    {RTE_FLOW_ITEM_TYPE_UDP, &vxlan_udp_spec, nullptr, &udp_mask},
                                    {RTE_FLOW_ITEM_TYPE_VXLAN, nullptr, nullptr, nullptr},
                                    {RTE_FLOW_ITEM_TYPE_END, nullptr, nullptr, nullptr}};
    rte_flow_attr ingress{};
    ingress.ingress = 1;
    const int base = TrySample(port, ingress, sample, nullptr);
    if (base == -ENOSYS || base == -ENODEV) {
      return c;  // no rte_flow operations on this port
    }
    c.supported = true;
    c.async = false;  // this backend completes at once
    for (uint32_t bits : {32u, 24u, 16u, 8u}) {
      const rte_flow_action_mark mark{uint32_t{1} << (bits - 1)};
      const rte_flow_action action{RTE_FLOW_ACTION_TYPE_MARK, &mark};
      if (TrySample(port, ingress, sample, &action) == 0) {
        c.mark_action = true;
        c.mark_bits = bits;
        break;
      }
    }
    const rte_flow_action_count count{};
    const rte_flow_action count_action{RTE_FLOW_ACTION_TYPE_COUNT, &count};
    c.count_action = TrySample(port, ingress, sample, &count_action) == 0;
    c.tunnel_match = TrySample(port, ingress, vxlan, nullptr) == 0;
    rte_flow_attr transfer{};
    transfer.transfer = 1;
    c.transfer = TrySample(port, transfer, sample, nullptr) == 0;
    // max_rules stays 0 (unknown): rte_flow_info_get, which reports template
    // limits, is still experimental DPDK API.
    return c;
  }

  struct Done {
    uint64_t tag;
    bool ok;
    HwHandle hw;
    int error;
  };
  std::deque<Done> done_;
  mutable std::map<uint16_t, FlowCapabilities> caps_;  // probed once per port
};

}  // namespace bess::offload

#endif  // BESS_OFFLOAD_RTE_FLOW_BACKEND_H_
