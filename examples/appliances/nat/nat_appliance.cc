// SPDX-License-Identifier: BSD-3-Clause

// R2's graph adapter (roadmap M24): NatApp in a module. igate 0 is the inside
// (outbound, leaves on ogate 0), igate 1 the outside (inbound, leaves on ogate
// 1); the packet clock is the batch's. `self_test` drives NatApp directly on
// frames with its own clock: the reverse alias, the firewall's order, timed
// expiration.

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "framework/plugin.h"
#include "module.h"
#include "utils/time.h"
#include "nat/nat_app.h"

namespace {

using appliance::NatApp;

constexpr uint64_t kSecond = 1000000000ull;

// Ethernet + IPv4 + TCP (no options), 54 bytes.
std::vector<uint8_t> TcpFrame(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport,
                              uint8_t flags) {
  std::vector<uint8_t> f(54, 0);
  f[12] = 0x08;
  f[14] = 0x45, f[17] = 40, f[22] = 64, f[23] = 6;
  for (int i = 0; i < 4; i++) {
    f[26 + i] = static_cast<uint8_t>(src >> (24 - 8 * i));
    f[30 + i] = static_cast<uint8_t>(dst >> (24 - 8 * i));
  }
  f[34] = static_cast<uint8_t>(sport >> 8), f[35] = static_cast<uint8_t>(sport);
  f[36] = static_cast<uint8_t>(dport >> 8), f[37] = static_cast<uint8_t>(dport);
  f[46] = 5 << 4, f[47] = flags;
  return f;
}
uint32_t Ip(const std::vector<uint8_t> &f, size_t at) {
  return uint32_t{f[at]} << 24 | uint32_t{f[at + 1]} << 16 | uint32_t{f[at + 2]} << 8 | f[at + 3];
}
uint16_t PortAt(const std::vector<uint8_t> &f, size_t at) {
  return static_cast<uint16_t>(f[at] << 8 | f[at + 1]);
}

constexpr uint8_t kSyn = 0x02, kAck = 0x10;
constexpr uint32_t kInside = 0x0a010005, kRemote = 0x08080808, kStranger = 0x09090909;

}  // namespace

class NatAppliance final : public Module {
 public:
  static const gate_idx_t kNumIGates = 2;
  static const gate_idx_t kNumOGates = 2;
  static const Commands cmds;

  NatAppliance() : Module() { max_allowed_workers_ = 1; }  // worker-owned state

  CommandResponse Init(const bess::pb::EmptyArg &) {
    // The wheels start at the packet clock's now: a wheel far behind it would
    // refuse every deadline as beyond its horizon.
    auto made = NatApp::Create(300 * kSecond, tsc_to_ns(rdtsc()));
    if (!made) {
      return CommandFailure(ENOMEM, "%s", made.error().c_str());
    }
    app_ = std::move(*made);
    return CommandSuccess();
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    const bool outbound = ctx->current_igate == 0;
    app_->Expire(ctx->current_ns);
    for (int i = 0; i < batch->cnt(); i++) {
      bess::PacketRef pkt = batch->packet(i);
      const auto v = app_->Process(std::span<uint8_t>(pkt.head_data<uint8_t *>(), pkt.head_len()),
                                   outbound, ctx->current_ns);
      if (v == NatApp::Verdict::kForward) {
        EmitPacket(ctx, pkt, outbound ? 0 : 1);
      } else {
        DropPacket(ctx, pkt);
      }
    }
  }

  CommandResponse CommandSelfTest(const bess::pb::EmptyArg &) {
    if (std::string failed = SelfTest(); !failed.empty()) {
      return CommandFailure(EINVAL, "R2 direct path: %s", failed.c_str());
    }
    return CommandSuccess();
  }

 private:
  static std::string SelfTest() {
    using V = NatApp::Verdict;
    auto made = NatApp::Create(10 * kSecond, 0);  // its own instance and clock
    if (!made) {
      return "create";
    }
    NatApp &app = **made;
    // Outbound SYN: tracked, then mapped to the pool.
    auto syn = TcpFrame(kInside, 4000, kRemote, 80, kSyn);
    if (app.Process(syn, true, 1 * kSecond) != V::kForward) {
      return "outbound SYN";
    }
    const uint16_t mapped = PortAt(syn, 34);
    if (Ip(syn, 26) != NatApp::kPublic || mapped < 20000 || mapped >= 30000) {
      return "the source was not mapped into the pool";
    }
    // The reply comes back through the reverse alias to the inside host.
    auto synack = TcpFrame(kRemote, 80, NatApp::kPublic, mapped, kSyn | kAck);
    if (app.Process(synack, false, 2 * kSecond) != V::kForward || Ip(synack, 30) != kInside ||
        PortAt(synack, 36) != 4000) {
      return "the reply did not return through the reverse alias";
    }
    // A stranger reaching the mapped port is translated (endpoint-independent
    // mapping) but refused by the firewall: it started no connection inside.
    auto probe = TcpFrame(kStranger, 999, NatApp::kPublic, mapped, kSyn);
    if (app.Process(probe, false, 3 * kSecond) != V::kRefused) {
      return "an outside-started connection was admitted";
    }
    // No mapping: not translated.
    auto stray = TcpFrame(kRemote, 80, NatApp::kPublic, static_cast<uint16_t>(mapped + 1), kAck);
    if (app.Process(stray, false, 3 * kSecond) != V::kNotTranslated) {
      return "a packet to an unmapped port was translated";
    }
    if (app.mappings() != 1) {
      return "mapping count";
    }
    // Timed expiration: idle past the timeout, the mapping ends and its port
    // no longer leads inside.
    app.Expire(20 * kSecond);
    if (app.mappings() != 0) {
      return "the idle mapping did not expire";
    }
    auto late = TcpFrame(kRemote, 80, NatApp::kPublic, mapped, kAck);
    if (app.Process(late, false, 21 * kSecond) != V::kNotTranslated) {
      return "an expired mapping still translated";
    }
    return "";
  }

  std::unique_ptr<NatApp> app_;
};

const Commands NatAppliance::cmds = {
    {"self_test", "EmptyArg", MODULE_CMD_FUNC(&NatAppliance::CommandSelfTest),
     Command::THREAD_SAFE},
};

BESS_PLUGIN_REQUIRES("nat_appliance", "1.0.0", 0);

ADD_MODULE(NatAppliance, "nat_appliance",
           "Reference appliance R2: a stateful NAT gateway over BESS's conntrack and NAT")
