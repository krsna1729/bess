// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_BRIDGE_H_
#define BESS_MODULES_BRIDGE_H_

#include <string>
#include <unordered_map>
#include <vector>

#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../utils/ether.h"

// Bridge: Ethernet L2 Learning Bridge with MAC aging and flooding.
// Learns source MAC to ingress gate mappings dynamically, forwards known
// unicast traffic, and floods unknown unicast and broadcast traffic.
class Bridge final : public Module {
 public:
  static const gate_idx_t kNumIGates = MAX_GATES;
  static const gate_idx_t kNumOGates = MAX_GATES;
  static const Commands cmds;

  struct Entry {
    gate_idx_t gate = 0;
    uint64_t last_seen_sec = 0;
    bool is_static = false;
  };

  Bridge() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::BridgeArg &arg);
  CommandResponse CommandAdd(const bess::pb::BridgeCommandAddArg &arg);
  CommandResponse CommandDelete(const bess::pb::BridgeCommandDeleteArg &arg);
  CommandResponse CommandClear(const bess::pb::BridgeCommandClearArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  std::string GetDesc() const override;

 private:
  void ExpireEntries(uint64_t now_sec);

  uint32_t max_entries_ = 1024;
  uint32_t aging_time_sec_ = 300;

  // MAC as uint64 -> Entry
  std::unordered_map<uint64_t, Entry> fdb_;
};

#endif  // BESS_MODULES_BRIDGE_H_
