// SPDX-License-Identifier: BSD-3-Clause

// R1's graph adapter (roadmap M24): RouterApp in a module. The application's
// interface-to-device mapping: input gate g is interface 2g+1 (packets
// arriving on igate 0 are routed in VRF 1, on igate 1 in VRF 2); interface i
// leaves on output gate i-1. Everything else is RouterApp's, the same code
// the direct path runs.
//
// `self_test` runs the direct path -- RouterApp::Process on frames in memory,
// no module graph -- and checks what R1 must demonstrate: overlapping VRFs,
// ECMP that spreads flows and keeps each on one path, and a neighbor update
// that changes forwarding without writing a route.

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "framework/plugin.h"
#include "module.h"
#include "router/router_app.h"

namespace {

using appliance::RouterApp;

// Ethernet + IPv4 + UDP, 60 bytes, `src` -> `dst` (host order).
std::vector<uint8_t> UdpFrame(uint32_t src, uint32_t dst, uint16_t sport, uint8_t ttl = 64) {
  std::vector<uint8_t> f(60, 0);
  f[12] = 0x08;
  f[14] = 0x45, f[17] = 28, f[22] = ttl, f[23] = 17;
  for (int i = 0; i < 4; i++) {
    f[26 + i] = static_cast<uint8_t>(src >> (24 - 8 * i));
    f[30 + i] = static_cast<uint8_t>(dst >> (24 - 8 * i));
  }
  f[34] = static_cast<uint8_t>(sport >> 8), f[35] = static_cast<uint8_t>(sport);
  f[37] = 53, f[39] = 8;
  return f;
}

bess::dataplane::InterfaceId If(uint16_t i) { return bess::dataplane::InterfaceId{i}; }

}  // namespace

class RouterAppliance final : public Module {
 public:
  static const gate_idx_t kNumIGates = 2;
  static const gate_idx_t kNumOGates = 4;
  static const Commands cmds;

  CommandResponse Init(const bess::pb::EmptyArg &) {
    auto made = RouterApp::Create(init_context().rcu());
    if (!made) {
      return CommandFailure(EINVAL, "%s", made.error().c_str());
    }
    app_ = std::move(*made);
    return CommandSuccess();
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    const auto ingress = If(static_cast<uint16_t>(2 * ctx->current_igate + 1));
    for (int i = 0; i < batch->cnt(); i++) {
      bess::PacketRef pkt = batch->packet(i);
      const RouterApp::Decision d =
          app_->Process(ingress, std::span<uint8_t>(pkt.head_data<uint8_t *>(), pkt.head_len()));
      if (d.verdict == RouterApp::Verdict::kForward) {
        EmitPacket(ctx, pkt, static_cast<gate_idx_t>(d.egress.value() - 1));
      } else {
        DropPacket(ctx, pkt);
      }
    }
  }

  CommandResponse CommandSelfTest(const bess::pb::EmptyArg &) {
    if (std::string failed = SelfTest(); !failed.empty()) {
      return CommandFailure(EINVAL, "R1 direct path: %s", failed.c_str());
    }
    return CommandSuccess();
  }

 private:
  // The direct path, no graph. Returns "" or what failed.
  std::string SelfTest() {
    using V = RouterApp::Verdict;
    const uint32_t host = 0x0a010203;  // 10.1.2.3: routed in both VRFs
    // Overlapping VRFs: the same destination leaves VRF 1 by the ECMP pair
    // (if1/if2) and VRF 2 by if3.
    auto f1 = UdpFrame(0xc0a80001, host, 1000);
    auto f2 = f1;
    const RouterApp::Decision in1 = app_->Process(If(1), f1);
    const RouterApp::Decision in2 = app_->Process(If(3), f2);
    if (in1.verdict != V::kForward || (in1.egress.value() != 1 && in1.egress.value() != 2)) {
      return "VRF 1 did not use its ECMP pair";
    }
    if (in2.verdict != V::kForward || in2.egress.value() != 3) {
      return "VRF 2 did not use its own route";
    }
    if (f1[22] != 63 || f2[22] != 63) {
      return "TTL not decremented";
    }
    // ECMP: 256 flows use both members; a flow repeated keeps its member.
    std::set<uint16_t> used;
    for (uint16_t port = 1; port <= 256; port++) {
      auto a = UdpFrame(0xc0a80001, host, port);
      auto b = a;
      const auto da = app_->Process(If(1), a);
      const auto db = app_->Process(If(1), b);
      if (da.egress != db.egress) {
        return "a flow changed path";
      }
      used.insert(da.egress.value());
    }
    if (used != std::set<uint16_t>{1, 2}) {
      return "ECMP did not spread over both members";
    }
    // A neighbor update: if3's gateway moves to another MAC. Forwarding follows
    // at once and no route is written.
    const size_t routes = app_->router().route_count();
    const size_t writes = app_->route_writes();
    bess::utils::Ethernet::Address moved;
    const uint8_t mac[6] = {0x02, 0, 0, 0, 0, 0xb3};
    std::memcpy(&moved, mac, 6);
    if (!app_->LearnNeighbor(If(3), RouterApp::kGatewayC, moved)) {
      return "neighbor update";
    }
    auto f3 = UdpFrame(0xc0a80001, host, 1000);
    if (app_->Process(If(3), f3).verdict != V::kForward || std::memcmp(f3.data(), mac, 6) != 0) {
      return "the new MAC is not used";
    }
    if (app_->router().route_count() != routes || app_->route_writes() != writes) {
      return "a route was written for a neighbor change";
    }
    // Unresolved neighbor: the application drops.
    if (!app_->LearnNeighbor(If(3), RouterApp::kGatewayC, moved,
                             bess::route::NeighborState::kIncomplete)) {
      return "neighbor state";
    }
    auto f4 = UdpFrame(0xc0a80001, host, 1000);
    const bool unresolved = app_->Process(If(3), f4).verdict == V::kUnresolved;
    (void)app_->LearnNeighbor(If(3), RouterApp::kGatewayC, moved);  // restore
    if (!unresolved) {
      return "an unresolved neighbor was forwarded to";
    }
    // No route, TTL expiry.
    auto f5 = UdpFrame(0xc0a80001, 0x08080808, 1000);
    auto f6 = UdpFrame(0xc0a80001, host, 1000, 1);
    if (app_->Process(If(1), f5).verdict != V::kNoRoute ||
        app_->Process(If(3), f6).verdict != V::kTtlExpired) {
      return "a miss or an expired TTL was forwarded";
    }
    return "";
  }

  std::unique_ptr<RouterApp> app_;
};

const Commands RouterAppliance::cmds = {
    {"self_test", "EmptyArg", MODULE_CMD_FUNC(&RouterAppliance::CommandSelfTest),
     Command::THREAD_UNSAFE},
};

BESS_PLUGIN_REQUIRES("router_appliance", "1.0.0", BESS_CAP_INIT_CONTEXT);

ADD_MODULE(RouterAppliance, "router_appliance",
           "Reference appliance R1: VRFs, ECMP and neighbors over BESS's route library")
