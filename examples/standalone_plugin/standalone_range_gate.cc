// SPDX-License-Identifier: BSD-3-Clause

// Conformance plugin (M2): a classifier built from the installed public
// classifier headers. Packets whose UDP/IPv4 destination port is in
// [1000, 2000] leave on gate 1; everything else leaves on gate 0.

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "classifier/range_backend.h"
#include "framework/plugin.h"
#include "module.h"

namespace {

constexpr size_t kUdpDstPortOffset = 14 + 20 + 2;  // Ethernet + IPv4 + UDP src

}  // namespace

class StandaloneRangeGate final : public Module {
 public:
  static const gate_idx_t kNumOGates = 2;
  static const Commands cmds;

  CommandResponse Init(const bess::pb::EmptyArg &) {
    using bess::classifier::RangeRule;
    RangeRule<uint16_t> rule;
    rule.value.assign(2, std::byte{0});
    rule.mask.assign(2, std::byte{0});  // no exact-match bits: ranges only
    rule.dst_range = {1000, 2000};
    rule.dst_port_offset = 0;
    rule.priority = 1;
    rule.result = 1;
    classifier_ = bess::classifier::RangeClassifier<uint16_t>(
        std::vector<RangeRule<uint16_t>>{rule});
    return CommandSuccess();
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    const int cnt = batch->cnt();
    for (int i = 0; i < cnt; i++) {
      bess::PacketRef pkt = batch->packet(i);
      gate_idx_t gate = 0;
      if (pkt.head_len() > kUdpDstPortOffset + 1) {
        const auto *bytes = pkt.head_data<const uint8_t *>();
        // The key is the destination port in network byte order, as the
        // classifier's offsets are byte offsets into the key.
        const std::array<std::byte, 2> key = {
            std::byte{bytes[kUdpDstPortOffset]},
            std::byte{bytes[kUdpDstPortOffset + 1]}};
        uint16_t result = 0;
        if (classifier_.LookupBatch(bess::classifier::ConstBytes(key.data(), 2),
                                    /*key_stride=*/2, /*key_size=*/2,
                                    std::span<uint16_t>(&result, 1)) != 0) {
          gate = result;
        }
      }
      EmitPacket(ctx, pkt, gate);
    }
    ProcessOGates(ctx);
  }

 private:
  bess::classifier::RangeClassifier<uint16_t> classifier_;
};

const Commands StandaloneRangeGate::cmds = {};

BESS_PLUGIN("standalone_range_gate", "1.0.0");

ADD_MODULE(StandaloneRangeGate, "standalone_range_gate",
           "Conformance plugin: routes UDP port ranges via the public classifier")
