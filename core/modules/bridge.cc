// SPDX-License-Identifier: BSD-3-Clause

#include "bridge.h"

#include <cstring>
#include <vector>

#include "utils/ether.h"
#include "utils/format.h"
#include "utils/time.h"

namespace {

using bess::utils::Ethernet;
namespace l2 = bess::l2;
namespace dataplane = bess::dataplane;

const l2::BridgeDomainId kDomain(0);
// Aging in nanoseconds of the worker's TSC clock; wheel granularity 2^20 ns.
constexpr unsigned kGranularityShift = 20;
// Wheel work per batch; aging lags at most a few batches behind.
constexpr size_t kAgeBudget = 256;

l2::MacAddress ToMac(const Ethernet::Address &a) {
  l2::MacAddress m;
  std::memcpy(m.bytes.data(), a.bytes, 6);
  return m;
}
// Gate g <-> interface g + 1, DROP_GATE included.
dataplane::InterfaceId InterfaceOfGate(gate_idx_t g) {
  return dataplane::InterfaceId(static_cast<uint16_t>(g + 1));
}
gate_idx_t GateOfInterface(dataplane::InterfaceId i) {
  return static_cast<gate_idx_t>(i.value() - 1);
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
  const uint64_t aging_sec = arg.aging_time() ? arg.aging_time() : 300;
  l2::Fdb::Config config;
  config.capacity = max_entries_ + kStaticReserve;
  config.learn_limit = max_entries_;
  config.aging = aging_sec * 1'000'000'000ull;
  config.granularity_shift = kGranularityShift;
  config.max_domains = 1;
  config.start = tsc_to_ns(rdtsc());
  auto fdb = l2::Fdb::Create(config);
  if (!fdb) {
    return CommandFailure(ENOMEM, "cannot create the FDB for %u entries",
                          max_entries_);
  }
  fdb_ = std::move(*fdb);
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
  switch (fdb_->AddStatic(kDomain, ToMac(mac),
                          InterfaceOfGate(static_cast<gate_idx_t>(arg.gate())))) {
    case l2::ProgramResult::kAdded:
    case l2::ProgramResult::kReplaced:
      return CommandSuccess();
    case l2::ProgramResult::kInvalid:
      return CommandFailure(EINVAL, "a multicast MAC cannot be a static entry");
    case l2::ProgramResult::kFull:
      break;
  }
  return CommandFailure(ENOSPC, "FDB full (%zu entries)", fdb_->capacity());
}

CommandResponse Bridge::CommandDelete(
    const bess::pb::BridgeCommandDeleteArg &arg) {
  Ethernet::Address mac;
  if (!mac.FromString(arg.mac_addr())) {
    return CommandFailure(EINVAL, "invalid MAC address: '%s'",
                          arg.mac_addr().c_str());
  }
  if (!fdb_->Remove(kDomain, ToMac(mac))) {
    return CommandFailure(ENOENT, "MAC address not found in FDB");
  }
  return CommandSuccess();
}

CommandResponse Bridge::CommandClear(const bess::pb::BridgeCommandClearArg &) {
  fdb_->Flush(/*static_too=*/true);
  return CommandSuccess();
}

void Bridge::Flood(Context *ctx, bess::PacketRef pkt, gate_idx_t,
                   const std::vector<gate_idx_t> &flood_gates) {
  if (flood_gates.empty()) {
    DropPacket(ctx, pkt);
    return;
  }
  for (size_t g = 1; g < flood_gates.size(); g++) {
    bess::PacketRef copy(bess::PacketCopy(pkt.handle()));
    if (copy.handle() != nullptr) {
      EmitPacket(ctx, copy, flood_gates[g]);
    }
  }
  EmitPacket(ctx, pkt, flood_gates[0]);
}

void Bridge::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const int cnt = batch->cnt();
  const gate_idx_t igate = ctx->current_igate;
  const uint64_t now = tsc_to_ns(current_worker.current_tsc());
  fdb_->Age(now, kAgeBudget);

  std::vector<gate_idx_t> flood_gates;
  const auto &out_gates = ogates();
  for (size_t g = 0; g < out_gates.size(); g++) {
    if (out_gates[g] != nullptr && static_cast<gate_idx_t>(g) != igate) {
      flood_gates.push_back(static_cast<gate_idx_t>(g));
    }
  }
  const dataplane::InterfaceId ingress = InterfaceOfGate(igate);
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    if (unlikely(pkt.head_len() < sizeof(Ethernet))) {
      DropPacket(ctx, pkt);
      continue;
    }
    const Ethernet *eth = pkt.head_data<const Ethernet *>();
    // Learning ignores multicast sources and keeps static entries.
    (void)fdb_->Learn(kDomain, ToMac(eth->src_addr), ingress, now);

    if (unlikely((eth->dst_addr.bytes[0] & 1) != 0)) {
      Flood(ctx, pkt, igate, flood_gates);
      continue;
    }
    const dataplane::InterfaceId out = fdb_->Lookup(kDomain, ToMac(eth->dst_addr));
    if (out == dataplane::kInvalidInterfaceId) {
      Flood(ctx, pkt, igate, flood_gates);
    } else if (out == ingress) {
      DropPacket(ctx, pkt);  // hairpin
    } else {
      EmitPacket(ctx, pkt, GateOfInterface(out));
    }
  }
}

std::string Bridge::GetDesc() const {
  return bess::utils::Format("%zu FDB entries, max %u", fdb_->size(),
                             max_entries_);
}

ADD_MODULE(Bridge, "bridge",
           "Ethernet L2 Learning Bridge with MAC aging and flooding")
