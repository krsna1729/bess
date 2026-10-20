// SPDX-License-Identifier: BSD-3-Clause

#include "bridge.h"

#include <any>
#include <cstring>
#include <expected>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "framework/resource_codec.h"
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
l2::MacAddress ToL2(const Bridge::Mac &a) {
  l2::MacAddress m;
  m.bytes = a;
  return m;
}
Bridge::Mac ToKey(const Ethernet::Address &a) {
  Bridge::Mac m;
  std::memcpy(m.data(), a.bytes, m.size());
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

// The statics are the "<module>/statics" resource (D-129); `flush` is the
// class's one action. THREAD_UNSAFE: the FDB is worker-owned.
const Commands Bridge::cmds = {
    {"flush", "EmptyArg", MODULE_CMD_FUNC(&Bridge::CommandFlush),
     Command::THREAD_UNSAFE},
};

const ResourceTables Bridge::resource_tables = {
    ResourceTable::Map<bess::pb::BridgeStaticKey, bess::pb::BridgeStaticValue>(
        "statics", /*requires_paused_workers=*/true)};

// "<module>/statics": key EncodeKey(Mac), value the gate (gate_idx_t). The
// FDB is worker-owned, so this runs with the module's workers paused (the
// codec says so; Reserve refuses otherwise too). Reserve validates and
// accounts for FDB room; Publish programs the FDB and the control-side
// record, without allocating (the record's node is built in Reserve, and an
// erased one leaves with its operation).
class Bridge::StaticsResource final : public dataplane::Resource {
 public:
  explicit StaticsResource(Bridge &owner)
      : Resource(owner.name() + "/statics"), owner_(owner) {}

  size_t LiveCount() const override { return owner_.statics_.size(); }

  bool Contains(const dataplane::ResourceKey &key) const override {
    Mac mac;
    return dataplane::DecodeKey(key, &mac) && owner_.statics_.contains(mac);
  }

  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    if (owner_.HasRunningWorker()) {
      return std::unexpected("a worker runs the bridge (statics require paused workers)");
    }
    Mac mac;
    if (!dataplane::DecodeKey(op.key, &mac) || ToL2(mac).multicast()) {
      return std::unexpected("invalid static MAC key");
    }
    const bool existed = owner_.statics_.contains(mac);
    if (op.kind == dataplane::OpKind::kErase) {
      if (!existed) {
        return std::unexpected("not found");
      }
      Reservation reservation{std::make_unique<EraseOp>(owner_, mac), {}, {}};
      reservation.existed = true;
      return reservation;
    }

    const gate_idx_t *gate = std::any_cast<gate_idx_t>(&op.value);
    if (gate == nullptr) {
      return std::unexpected("wrong value type (want a gate)");
    }
    // The RPC codec refuses a gate outside [0, MAX_GATES) or DROP_GATE; the
    // typed path must too, and before a slot is reserved, or publication
    // would fail after Reserve and leave a placeholder nothing removes.
    if (!bess::IsValidGateValue(static_cast<uint64_t>(*gate))) {
      return std::unexpected("invalid static gate");
    }
    // Everything the reservation needs is allocated before the slot is
    // taken; from there to the return no allocation can fail and leave a
    // placeholder behind.
    std::map<Mac, gate_idx_t> node_source;
    node_source.emplace(mac, *gate);
    auto staged_op = std::make_unique<UpsertOp>(
        owner_, node_source.extract(node_source.begin()), false);
    // A MAC the FDB does not hold takes a slot (a learned one becomes
    // static in place). The slot is taken here, not at publication: the
    // table's cuckoo search can fail below capacity, and Publish must not.
    // Erases of this transaction may publish after its upserts, so they free
    // nothing here.
    if (!existed && owner_.fdb_->Lookup(kDomain, ToL2(mac)) ==
                        dataplane::kInvalidInterfaceId) {
      if (!owner_.fdb_->ReserveStatic(kDomain, ToL2(mac))) {
        return std::unexpected(bess::utils::Format(
            "FDB full (%zu entries)", owner_.fdb_->capacity()));
      }
      staged_op->MarkReserved();
    }
    Reservation reservation{std::move(staged_op), {}, {}};
    reservation.existed = existed;
    return reservation;
  }

  void EndTransaction() noexcept override {}

 private:
  using Node = std::map<Mac, gate_idx_t>::node_type;

  class UpsertOp final : public dataplane::StagedOp {
   public:
    UpsertOp(Bridge &owner, Node node, bool reserved)
        : owner_(owner), node_(std::move(node)), reserved_(reserved) {}
    void Publish(dataplane::Retirer &) noexcept override {
      const Mac mac = node_.key();
      const gate_idx_t gate = node_.mapped();
      const l2::ProgramResult result =
          owner_.fdb_->AddStatic(kDomain, ToL2(mac), InterfaceOfGate(gate));
      if (result != l2::ProgramResult::kAdded &&
          result != l2::ProgramResult::kReplaced) {
        LOG(ERROR) << "Bridge '" << owner_.name()
                   << "': no FDB slot for a static entry";
        return;
      }
      published_ = true;
      auto it = owner_.statics_.find(mac);
      if (it != owner_.statics_.end()) {
        it->second = gate;
      } else {
        owner_.statics_.insert(std::move(node_));
      }
    }

    // The reservation was taken in Reserve and this operation will not
    // publish: give the slot back.
    void Abort() noexcept override {
      if (reserved_ && !published_) {
        (void)owner_.fdb_->Remove(kDomain, ToL2(node_.key()));
      }
    }

    void MarkReserved() noexcept { reserved_ = true; }

   private:
    Bridge &owner_;
    Node node_;
    bool reserved_;
    bool published_ = false;
  };

  class EraseOp final : public dataplane::StagedOp {
   public:
    EraseOp(Bridge &owner, const Mac &mac) : owner_(owner), mac_(mac) {}
    void Publish(dataplane::Retirer &) noexcept override {
      (void)owner_.fdb_->Remove(kDomain, ToL2(mac_));
      erased_ = owner_.statics_.extract(mac_);  // freed with this operation
    }

   private:
    Bridge &owner_;
    Mac mac_;
    Node erased_;
  };

  Bridge &owner_;
};

CommandResponse Bridge::Init(const bess::pb::BridgeArg &arg) {
  max_entries_ = arg.size() ? arg.size() : 1024;
  const uint64_t aging_sec = arg.aging_time() ? arg.aging_time() : 300;
  Fdb::Config config;
  config.capacity = max_entries_ + kStaticReserve;
  config.learn_limit = max_entries_;
  config.aging = aging_sec * 1'000'000'000ull;
  config.granularity_shift = kGranularityShift;
  config.max_domains = 1;
  config.start = tsc_to_ns(rdtsc());
  auto fdb = Fdb::Create(config);
  if (!fdb) {
    return CommandFailure(ENOMEM, "cannot create the FDB for %u entries",
                          max_entries_);
  }
  fdb_ = std::move(*fdb);

  resource_ = std::make_unique<StaticsResource>(*this);
  binding_ = init_context().codecs().Bind(
      *resource_,
      std::make_shared<bess::framework::TypedCodec<bess::pb::BridgeStaticKey,
                                                   bess::pb::BridgeStaticValue>>(
          [](const bess::pb::BridgeStaticKey &key)
              -> std::expected<dataplane::ResourceKey, std::string> {
            Ethernet::Address mac;
            if (!mac.FromString(key.mac_addr())) {
              return std::unexpected("invalid MAC address: '" + key.mac_addr() + "'");
            }
            if (ToL2(ToKey(mac)).multicast()) {
              return std::unexpected("a multicast MAC cannot be a static entry");
            }
            return dataplane::EncodeKey(ToKey(mac));
          },
          [](const bess::pb::BridgeStaticValue &value)
              -> std::expected<std::any, std::string> {
            if (!bess::IsValidGateValue(value.gate())) {
              return std::unexpected("invalid output gate: " +
                                     std::to_string(value.gate()));
            }
            return std::any(static_cast<gate_idx_t>(value.gate()));
          },
          // Read back as a client sends a static: the MAC in lower case.
          [this](const auto &visit) {
            for (const auto &[mac, gate] : statics_) {
              Ethernet::Address addr;
              std::memcpy(addr.bytes, mac.data(), mac.size());
              bess::pb::BridgeStaticKey key;
              key.set_mac_addr(addr.ToString());
              bess::pb::BridgeStaticValue value;
              value.set_gate(gate);
              visit(key, value);
            }
          },
          [this] { return HasRunningWorker(); }));
  if (auto registered = init_context().resources().Register(resource_.get());
      !registered) {
    binding_.Reset();
    resource_.reset();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void Bridge::DeInit() {
  if (resource_ == nullptr) {
    return;
  }
  // The statics may still be live: the module leaves with them.
  const std::string name = resource_->name();
  auto released = init_context().resources().ReleaseForTeardown(
      std::span<const std::string>(&name, 1));
  CHECK(released) << released.error();
  binding_.Reset();
  resource_.reset();
}

CommandResponse Bridge::CommandFlush(const bess::pb::EmptyArg &) {
  fdb_->Flush(/*static_too=*/false);
  return CommandSuccess();
}

void Bridge::Flood(Context *ctx, bess::PacketRef pkt, gate_idx_t,
                   const std::vector<gate_idx_t> &flood_gates) {
  if (flood_gates.empty()) {
    DropPacket(ctx, pkt);
    return;
  }
  for (size_t g = 1; g < flood_gates.size(); g++) {
    // A flooded copy is the same frame on another port: the metadata an
    // upstream module wrote (tenant, meter id) is part of the packet.
    bess::PacketRef copy(bess::PacketCopyWithMetadata(pkt.handle()));
    if (copy.handle() != nullptr) {
      EmitPacket(ctx, copy, flood_gates[g]);
    }
  }
  EmitPacket(ctx, pkt, flood_gates[0]);
}

void Bridge::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const gate_idx_t igate = ctx->current_igate;
  // The scheduler's checkpoint for this task (the worker's current_tsc in a
  // worker); the context is what a non-worker caller sets too.
  const uint64_t now = tsc_to_ns(ctx->current_tsc);
  fdb_->Age(now, kAgeBudget);

  std::vector<gate_idx_t> flood_gates;
  const auto &out_gates = ogates();
  for (size_t g = 0; g < out_gates.size(); g++) {
    if (out_gates[g] != nullptr && static_cast<gate_idx_t>(g) != igate) {
      flood_gates.push_back(static_cast<gate_idx_t>(g));
    }
  }
  const dataplane::InterfaceId ingress = InterfaceOfGate(igate);
  for (bess::PacketRef pkt : batch->packets()) {
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
