// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_PACKET_STORE_H_
#define BESS_MODULES_PACKET_STORE_H_

#include <deque>
#include <map>
#include <string>
#include <vector>

#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../utils/endian.h"

// PacketStore: Bounded, ownership-safe packet buffer (K9).
// Buffers packets indexed by flow ID (from metadata 'store_id' or 'vif_id').
// Supports bounded capacity, per-flow limits, aging eviction, and on-demand
// release/discard via commands.
class PacketStore final : public Module {
 public:
  static const gate_idx_t kNumOGates = 2;  // 0: released, 1: evicted
  static const Commands cmds;

  struct StoredPacket {
    bess::PacketRef pkt;
    uint64_t timestamp_ns = 0;
  };

  struct ReleasedPacket {
    bess::PacketRef pkt;
    gate_idx_t gate = 0;
  };
  // One worker: the store, the release queue and the packet counters are
  // unsynchronised std::map / std::deque state mutated on the packet path
  // (which also allocates). Contain it until it is rebuilt over a bounded
  // store with explicit packet ownership (M10/M11 substrate).
  PacketStore() : Module() { max_allowed_workers_ = 1; }
  ~PacketStore() override { DeInit(); }

  CommandResponse Init(const bess::pb::PacketStoreArg &arg);
  void DeInit() override;

  CommandResponse CommandRelease(
      const bess::pb::PacketStoreCommandReleaseArg &arg);
  CommandResponse CommandDrop(const bess::pb::PacketStoreCommandDropArg &arg);
  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch,
                             void *arg) override;
  CommandResponse CommandClear(const bess::pb::PacketStoreCommandClearArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;
  std::string GetDesc() const override;

 private:
  void EvictExpired(uint64_t now_ns);

  uint32_t max_packets_ = 4096;
  uint32_t max_packets_per_flow_ = 64;
  uint64_t timeout_ns_ = 5'000'000'000ULL;  // 5 seconds
  size_t total_packets_ = 0;

  int store_id_attr_ = -1;
  int vif_id_attr_ = -1;

  // flow_id -> FIFO queue of stored packets
  std::map<uint32_t, std::deque<StoredPacket>> store_;
  std::deque<ReleasedPacket> release_queue_;
};

#endif  // BESS_MODULES_PACKET_STORE_H_
