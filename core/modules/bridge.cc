// SPDX-License-Identifier: BSD-3-Clause

#include "bridge.h"

#include <cstring>
#include <vector>

#include "utils/format.h"
#include "utils/time.h"

namespace {

using bess::utils::Ethernet;

uint64_t MacToUint64(const Ethernet::Address &addr) {
  uint64_t v = 0;
  std::memcpy(&v, addr.bytes, Ethernet::Address::kSize);
  return v;
}

}  // namespace

const Commands Bridge::cmds = {
    {"add", "BridgeCommandAddArg", MODULE_CMD_FUNC(&Bridge::CommandAdd),
     Command::THREAD_UNSAFE},
    {"delete", "BridgeCommandDeleteArg", MODULE_CMD_FUNC(&Bridge::CommandDelete),
     Command::THREAD_UNSAFE},
    {"clear", "BridgeCommandClearArg", MODULE_CMD_FUNC(&Bridge::CommandClear),
     Command::THREAD_UNSAFE},
};

CommandResponse Bridge::Init(const bess::pb::BridgeArg &arg) {
  max_entries_ = arg.size() ? arg.size() : 1024;
  aging_time_sec_ = arg.aging_time() ? arg.aging_time() : 300;
  return CommandSuccess();
}

CommandResponse Bridge::CommandAdd(const bess::pb::BridgeCommandAddArg &arg) {
  Ethernet::Address mac;
  if (!mac.FromString(arg.mac_addr())) {
    return CommandFailure(EINVAL, "invalid MAC address: '%s'",
                          arg.mac_addr().c_str());
  }
  if (!bess::IsValidGateValue(arg.gate())) {
    return CommandFailure(EINVAL, "invalid output gate: %u", arg.gate());
  }

  uint64_t key = MacToUint64(mac);
  fdb_[key] = Entry{
      .gate = static_cast<gate_idx_t>(arg.gate()),
      .last_seen_sec = 0,
      .is_static = true,
  };
  return CommandSuccess();
}

CommandResponse Bridge::CommandDelete(
    const bess::pb::BridgeCommandDeleteArg &arg) {
  Ethernet::Address mac;
  if (!mac.FromString(arg.mac_addr())) {
    return CommandFailure(EINVAL, "invalid MAC address: '%s'",
                          arg.mac_addr().c_str());
  }

  uint64_t key = MacToUint64(mac);
  if (fdb_.erase(key) == 0) {
    return CommandFailure(ENOENT, "MAC address not found in FDB");
  }
  return CommandSuccess();
}

CommandResponse Bridge::CommandClear(const bess::pb::BridgeCommandClearArg &) {
  fdb_.clear();
  return CommandSuccess();
}

void Bridge::ExpireEntries(uint64_t now_sec) {
  for (auto it = fdb_.begin(); it != fdb_.end();) {
    if (!it->second.is_static &&
        (now_sec - it->second.last_seen_sec > aging_time_sec_)) {
      it = fdb_.erase(it);
    } else {
      ++it;
    }
  }
}

void Bridge::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const int cnt = batch->cnt();
  const gate_idx_t igate = ctx->current_igate;
  const uint64_t now_sec =
      tsc_to_ns(current_worker.current_tsc()) / 1'000'000'000ULL;

  // Periodically clean expired entries if table grows
  if (fdb_.size() >= max_entries_) {
    ExpireEntries(now_sec);
  }

  // Pre-collect active flooding gates (all output gates except ingress gate)
  std::vector<gate_idx_t> flood_gates;
  const auto &out_gates = ogates();
  for (size_t g = 0; g < out_gates.size(); g++) {
    if (out_gates[g] != nullptr && static_cast<gate_idx_t>(g) != igate) {
      flood_gates.push_back(static_cast<gate_idx_t>(g));
    }
  }
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    if (unlikely(pkt.head_len() < sizeof(Ethernet))) {
      DropPacket(ctx, pkt);
      continue;
    }

    const Ethernet *eth = pkt.head_data<const Ethernet *>();
    const bool src_multicast = (eth->src_addr.bytes[0] & 1) != 0;
    const bool dst_multicast = (eth->dst_addr.bytes[0] & 1) != 0;

    // 1. MAC Learning: learn arrival gate for unicast source MAC
    if (likely(!src_multicast)) {
      const uint64_t src_key = MacToUint64(eth->src_addr);
      auto it = fdb_.find(src_key);
      if (it != fdb_.end()) {
        if (!it->second.is_static) {
          it->second.gate = igate;
          it->second.last_seen_sec = now_sec;
        }
      } else if (fdb_.size() < max_entries_) {
        fdb_[src_key] = Entry{
            .gate = igate,
            .last_seen_sec = now_sec,
            .is_static = false,
        };
      }
    }

    // 2. MAC Forwarding
    if (unlikely(dst_multicast)) {
      // Broadcast/Multicast -> Flood
      if (flood_gates.empty()) {
        DropPacket(ctx, pkt);
      } else {
        for (size_t g = 1; g < flood_gates.size(); g++) {
          bess::PacketRef copy(bess::PacketCopy(pkt.handle()));
          if (copy.handle() != nullptr) {
            EmitPacket(ctx, copy, flood_gates[g]);
          }
        }
        EmitPacket(ctx, pkt, flood_gates[0]);
      }
      continue;
    }

    // Unicast lookup
    const uint64_t dst_key = MacToUint64(eth->dst_addr);
    auto it = fdb_.find(dst_key);
    if (it != fdb_.end() &&
        (it->second.is_static ||
         (now_sec - it->second.last_seen_sec <= aging_time_sec_))) {
      const gate_idx_t out_gate = it->second.gate;
      if (out_gate == igate) {
        // Hairpin filter: packet destined to same segment -> drop
        DropPacket(ctx, pkt);
      } else {
        // Forward to learned gate
        EmitPacket(ctx, pkt, out_gate);
      }
    } else {
      // Unknown unicast -> Flood
      if (flood_gates.empty()) {
        DropPacket(ctx, pkt);
      } else {
        for (size_t g = 1; g < flood_gates.size(); g++) {
          bess::PacketRef copy(bess::PacketCopy(pkt.handle()));
          if (copy.handle() != nullptr) {
            EmitPacket(ctx, copy, flood_gates[g]);
          }
        }
        EmitPacket(ctx, pkt, flood_gates[0]);
      }
    }
  }
}

std::string Bridge::GetDesc() const {
  return bess::utils::Format("%zu FDB entries, max %u", fdb_.size(),
                             max_entries_);
}

ADD_MODULE(Bridge, "bridge",
           "Ethernet L2 Learning Bridge with MAC aging and flooding")
