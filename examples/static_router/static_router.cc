// SPDX-License-Identifier: BSD-3-Clause

// R1's application without bessd (D-095, Appendix J "direct appliance build
// does not require framework"): RouterApp (examples/appliances/router) over
// BESS's router, linked from the installed static archives (bess-dev-static)
// and DPDK. No daemon, no Module, no framework: a program owns its RCU domain
// and calls the application on frames. The EAL comes up on the first table's
// creation, without hugepages (512 MB of normal pages).
//
// One thread reads and writes here, so the domain needs no reader: every
// grace period is complete at once. A program with packet threads registers
// each as a reader and reports its quiescent states (rcu/rcu_domain.h).

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "rcu/rcu_domain.h"
#include "router/router_app.h"

namespace {

using appliance::RouterApp;

// Ethernet + IPv4 + UDP, 60 bytes, `src` -> `dst` (host order).
std::vector<uint8_t> UdpFrame(uint32_t src, uint32_t dst, uint8_t ttl = 64) {
  std::vector<uint8_t> f(60, 0);
  f[12] = 0x08;
  f[14] = 0x45, f[17] = 46, f[22] = ttl, f[23] = 17;
  for (int i = 0; i < 4; i++) {
    f[26 + i] = static_cast<uint8_t>(src >> (24 - 8 * i));
    f[30 + i] = static_cast<uint8_t>(dst >> (24 - 8 * i));
  }
  f[34] = 0x03, f[35] = 0xe8, f[37] = 53, f[39] = 26;
  return f;
}

bess::dataplane::InterfaceId If(uint16_t i) { return bess::dataplane::InterfaceId{i}; }

int Fail(const char *what) {
  std::fprintf(stderr, "static_router: FAIL: %s\n", what);
  return 1;
}

}  // namespace

int main() {
  bess::rcu::RcuDomain rcu(/*max_readers=*/1);
  {
    auto made = RouterApp::Create(rcu);
    if (!made) {
      std::fprintf(stderr, "static_router: %s\n", made.error().c_str());
      return 1;
    }
    RouterApp &app = **made;
    using V = RouterApp::Verdict;

    // 172.16.1.1 arriving on interface 1 (VRF 1) leaves by interface 1, to
    // gateway A's MAC, from interface 1's, with the TTL decremented.
    auto f = UdpFrame(0xc0a80001, 0xac100101);
    const RouterApp::Decision d = app.Process(If(1), f);
    const uint8_t gateway_a[6] = {0x02, 0, 0, 0, 0, 0xa1};
    const uint8_t if1[6] = {0x02, 0, 0, 0, 0, 0x01};
    if (d.verdict != V::kForward || d.egress != If(1)) {
      return Fail("172.16.1.1 was not forwarded by interface 1");
    }
    if (std::memcmp(f.data(), gateway_a, 6) != 0 || std::memcmp(f.data() + 6, if1, 6) != 0 ||
        f[22] != 63) {
      return Fail("the forwarded frame's MACs or TTL are wrong");
    }
    // The same destination in VRF 2 (interface 3) uses VRF 2's route.
    auto g = UdpFrame(0xc0a80001, 0x0a010203);
    if (app.Process(If(3), g).egress != If(3)) {
      return Fail("VRF 2 did not use its own route");
    }
    // A miss and an expired TTL are not forwarded, and the frame is unchanged.
    auto miss = UdpFrame(0xc0a80001, 0x08080808);
    const auto before = miss;
    auto expired = UdpFrame(0xc0a80001, 0xac100101, 1);
    if (app.Process(If(1), miss).verdict != V::kNoRoute || miss != before ||
        app.Process(If(1), expired).verdict != V::kTtlExpired) {
      return Fail("a miss or an expired TTL was forwarded");
    }
    std::printf("static_router: OK: 172.16.1.1 forwarded by interface 1 (TTL 64 -> 63, "
                "gateway A's MAC); VRF 2 routed apart; miss and TTL expiry dropped "
                "(%zu routes, no bessd)\n",
                app.router().route_count());
  }
  rcu.Drain();  // the router's retired tables
  return 0;
}
