// SPDX-License-Identifier: BSD-3-Clause

// Conformance plugin (M23, persona C: a specialised module on a public
// library). Per-flow state lives in the installed `flow::WorkerFlowTable`: the
// first packet of a flow (IPv4 source, destination, protocol and ports) leaves
// on gate 1, every later packet of a known flow on gate 0. A table that never
// found a flow would send nothing to gate 0.

#include <cstdint>
#include <cstring>
#include <memory>

#include "flow/worker_flow_table.h"
#include "framework/plugin.h"
#include "module.h"

namespace {

// The key's bytes are hashed and compared directly: no padding, and the
// reserved bytes are always zero.
struct FlowKey {
  uint32_t src_ip;
  uint32_t dst_ip;
  uint16_t src_port;
  uint16_t dst_port;
  uint8_t protocol;
  uint8_t reserved[3];
};
static_assert(sizeof(FlowKey) == 16);

struct FlowCount {
  uint64_t packets = 0;
};

constexpr size_t kIpv4 = 14;               // after the Ethernet header
constexpr size_t kPorts = kIpv4 + 20;      // an IPv4 header without options
constexpr size_t kNeeded = kPorts + 4;

using Table = bess::flow::WorkerFlowTable<FlowKey, FlowCount>;

}  // namespace

class StandaloneFlowCount final : public Module {
 public:
  static const gate_idx_t kNumOGates = 2;
  static const Commands cmds;

  // One worker owns the table; the module may not run on two.
  StandaloneFlowCount() { max_allowed_workers_ = 1; }

  CommandResponse Init(const bess::pb::EmptyArg &) {
    auto table = Table::Create(4096);
    if (!table) {
      return CommandFailure(ENOMEM, "flow table");
    }
    table_ = std::move(*table);
    return CommandSuccess();
  }

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override {
    const int cnt = batch->cnt();
    for (int i = 0; i < cnt; i++) {
      bess::PacketRef pkt = batch->packet(i);
      FlowKey key{};
      if (pkt.head_len() >= kNeeded) {
        const auto *bytes = pkt.head_data<const uint8_t *>();
        std::memcpy(&key.src_ip, bytes + kIpv4 + 12, 4);
        std::memcpy(&key.dst_ip, bytes + kIpv4 + 16, 4);
        std::memcpy(&key.src_port, bytes + kPorts, 2);
        std::memcpy(&key.dst_port, bytes + kPorts + 2, 2);
        key.protocol = bytes[kIpv4 + 9];
      }
      gate_idx_t gate = 0;
      if (FlowCount *known = table_->Find(key)) {
        known->packets++;
      } else if (auto made = table_->Emplace(key); made.created()) {
        made.state->packets = 1;
        gate = 1;
      }
      EmitPacket(ctx, pkt, gate);
    }
    ProcessOGates(ctx);
  }

 private:
  std::unique_ptr<Table> table_;
};

const Commands StandaloneFlowCount::cmds = {};

BESS_PLUGIN("standalone_flow_count", "1.0.0");

ADD_MODULE(StandaloneFlowCount, "standalone_flow_count",
           "Conformance plugin: per-flow counters in the public WorkerFlowTable")
