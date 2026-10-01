// SPDX-License-Identifier: BSD-3-Clause

// Conformance plugin (M2): packet manipulation through the installed public
// headers only. Swaps the Ethernet addresses of packets whose payload is
// writable and drops the rest, using the checked packet mutation primitives.

#include "framework/plugin.h"
#include "module.h"
#include "packet_mutation.h"
#include "utils/ether.h"

class StandaloneMacSwap final : public Module {
 public:
  static const Commands cmds;

  CommandResponse Init(const bess::pb::EmptyArg &) { return CommandSuccess(); }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    using bess::utils::Ethernet;
    const int cnt = batch->cnt();
    for (int i = 0; i < cnt; i++) {
      bess::PacketRef pkt = batch->packet(i);
      if (pkt.head_len() < sizeof(Ethernet) ||
          bess::packet::PayloadWriteabilityOf(pkt) !=
              bess::packet::PayloadWriteability::kWritable) {
        DropPacket(ctx, pkt);
        continue;
      }
      auto *eth = pkt.head_data<Ethernet *>();
      const Ethernet::Address dst = eth->dst_addr;
      eth->dst_addr = eth->src_addr;
      eth->src_addr = dst;
      EmitPacket(ctx, pkt);
    }
    ProcessOGates(ctx);
  }
};

const Commands StandaloneMacSwap::cmds = {};

BESS_PLUGIN("standalone_macswap", "1.0.0");

ADD_MODULE(StandaloneMacSwap, "standalone_macswap",
           "Conformance plugin: swaps Ethernet addresses via public headers")
