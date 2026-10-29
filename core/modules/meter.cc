// SPDX-License-Identifier: BSD-3-Clause

#include "meter.h"

#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include "dataplane/resource_registry.h"
#include "framework/resource_bindings.h"
#include "meter/meter_resource_codec.h"
#include "utils/endian.h"
#include "utils/format.h"
#include "utils/ip.h"

namespace meter = bess::meter;

const Commands Meter::cmds = {
    {"get_status", "MeterCommandGetStatusArg",
     MODULE_CMD_FUNC(&Meter::CommandGetStatus), Command::THREAD_SAFE,
     "MeterCommandGetStatusResponse"},
};

const ResourceTables Meter::resource_tables = {
    ResourceTable::Map<bess::pb::MeterIdKey, bess::pb::MeterPolicyValue>("meters")
        .Rebuilds(),
};

namespace {

// MeterStateSnapshot's bucket kinds as the wire enum.
bess::pb::MeterBucketKind PbKind(meter::MeterBucketKind kind) {
  switch (kind) {
    case meter::MeterBucketKind::kCommitted:
      return bess::pb::METER_BUCKET_COMMITTED;
    case meter::MeterBucketKind::kExcess:
      return bess::pb::METER_BUCKET_EXCESS;
    case meter::MeterBucketKind::kPeak:
      return bess::pb::METER_BUCKET_PEAK;
  }
  return bess::pb::METER_BUCKET_COMMITTED;
}

}  // namespace

CommandResponse Meter::Init(const bess::pb::MeterArg &arg) {
  // The level count sizes the attribute this module declares, and the
  // framework refuses a size mismatch with its writer. Validate it before the
  // resource is registered, so a refused module leaves nothing.
  levels_ = arg.levels() == 0 ? 1 : arg.levels();
  if (levels_ * sizeof(bess::utils::be32_t) >
      bess::metadata::kMetadataAttrMaxSize) {
    return CommandFailure(EINVAL, "levels must be 1-%zu",
                          bess::metadata::kMetadataAttrMaxSize /
                              sizeof(bess::utils::be32_t));
  }
  switch (arg.byte_basis()) {
    case bess::pb::METER_BYTE_BASIS_L2:
      byte_basis_ = ByteBasis::kL2;
      break;
    case bess::pb::METER_BYTE_BASIS_L3:
      byte_basis_ = ByteBasis::kL3;
      break;
    case bess::pb::METER_BYTE_BASIS_L4:
      byte_basis_ = ByteBasis::kL4;
      break;
    case bess::pb::METER_BYTE_BASIS_CUSTOM:
      byte_basis_ = ByteBasis::kCustom;
      break;
    default:
      return CommandFailure(EINVAL, "unknown byte_basis");
  }
  byte_offset_ = arg.byte_offset();
  if (byte_offset_ > 65535) {
    return CommandFailure(EINVAL, "byte_offset must be 0-65535");
  }
  if (byte_basis_ == ByteBasis::kL2 && byte_offset_ != 0) {
    return CommandFailure(EINVAL, "byte_offset must be 0 for the L2 basis");
  }
  if (arg.capacity() > std::numeric_limits<meter::MeterId::rep_type>::max()) {
    return CommandFailure(EINVAL, "meter capacity exceeds valid meter ids");
  }
  const size_t capacity =
      arg.capacity() == 0 ? 1024 : static_cast<size_t>(arg.capacity());
  // Admitted before it is built (D-198); everything the meters grow by
  // afterwards, the first generation included, is admitted through the
  // lease's hook (meter/meter_resource.h).
  {
    bess::framework::MemoryLease lease =
        init_context().memory().LeaseFor<meter::MeterResource>("meters",
                                                               capacity);
    auto made = std::make_unique<meter::MeterResource>(
        name() + "/meters", capacity, published_, lease.growth());
    meters_ = bess::framework::MemoryScope::Adopt(std::move(lease),
                                                  std::move(made));
  }
  binding_ = init_context().codecs().Bind(
      *meters_, meter::MakeMeterResourceCodec(*meters_));
  if (auto registered = init_context().resources().Register(meters_.get());
      !registered) {
    binding_.Reset();
    meters_.reset();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }

  // A graph with two meters -- a flow maximum and a session aggregate from one
  // classifier result -- sets the name so each reads its own identity.
  id_attr_name_ = arg.id_attr().empty() ? "meter_id" : arg.id_attr();
  // One attribute, 4 bytes per level: every level's id is read from the same
  // place the writer placed the list.
  meter_id_attr_ = AddMetadataAttr(
      id_attr_name_, levels_ * sizeof(bess::utils::be32_t),
      bess::metadata::Attribute::AccessMode::kRead);
  // The profile's rates and bursts are in the chosen unit: bytes/s for a
  // bit-rate, packets/s for a packet-rate (8.2.63), the same buckets either
  // way. The library is unit-agnostic -- it is handed one integer per packet.
  packets_ = arg.cost() == bess::pb::MeterArg::METER_COST_PACKETS;
  if (meter_id_attr_ < 0) {
    const std::string name = meters_->name();
    CHECK(init_context().resources().ReleaseForTeardown(
        std::span<const std::string>(&name, 1)));
    binding_.Reset();
    meters_.reset();
    return CommandFailure(-meter_id_attr_, "add_metadata_attr() failed");
  }
  return CommandSuccess();
}

void Meter::DeInit() {
  if (!meters_) {
    return;
  }
  // Meters are referenced by classifier rules, so a meter may leave with live
  // keys and with its keys still named (D-021's teardown release): the order
  // modules are destroyed in must not decide whether a pipeline can be torn
  // down.
  const std::string name = meters_->name();
  auto released = init_context().resources().ReleaseForTeardown(
      std::span<const std::string>(&name, 1));
  CHECK(released) << released.error();
  binding_.Reset();
  meters_.reset();
}

int Meter::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  // Metadata offsets are assigned just before this: an attribute with no
  // valid offset means the packet path will drop everything, which is worth
  // saying once, here, rather than never.
  if (!bess::metadata::IsValidOffset(attr_offset(meter_id_attr_))) {
    LOG(ERROR) << "Meter '" << name() << "': the '" << id_attr_name_
               << "' attribute is unreadable (is anything writing it?); "
                  "dropping packets";
  }
  return 0;
}

std::optional<uint32_t> Meter::AccountedBytes(bess::PacketRef pkt) const {
  const uint32_t frame_len = pkt.total_len();
  if (byte_basis_ == ByteBasis::kL2) {
    return frame_len;
  }
  if (byte_basis_ == ByteBasis::kCustom) {
    if (frame_len < byte_offset_) {
      return std::nullopt;
    }
    return frame_len - byte_offset_;
  }

  // L3 and L4 parse the IP header at byte_offset_: its version, IHL and
  // lengths live in the first six bytes (options may be in a later segment).
  if (pkt.head_len() < byte_offset_ + 4) {
    return std::nullopt;
  }
  const uint8_t *ip = pkt.head_data<const uint8_t *>() + byte_offset_;
  const uint8_t version = static_cast<unsigned>(ip[0]) >> 4u;
  if (version == 4) {
    const uint32_t ihl = (ip[0] & 0x0fu) * 4;
    const uint32_t total = (static_cast<uint32_t>(ip[2]) << 8u) | ip[3];
    if (ihl < sizeof(bess::utils::Ipv4) || total < ihl ||
        frame_len < byte_offset_ + total) {
      return std::nullopt;
    }
    return byte_basis_ == ByteBasis::kL4 ? total - ihl : total;
  }
  if (version == 6) {
    // The payload length is at offset 4. Zero is either an empty packet (a
    // valid IPv6 datagram, for example Next Header 59) or a jumbogram: RFC
    // 2675 gives a jumbogram length 0 and puts the real length in a Jumbo
    // Payload option inside a Hop-by-Hop header, which is the next header.
    if (pkt.head_len() < byte_offset_ + 6) {
      return std::nullopt;
    }
    const uint32_t payload = (static_cast<uint32_t>(ip[4]) << 8u) | ip[5];
    if (payload == 0) {
      if (pkt.head_len() < byte_offset_ + 7) {
        return std::nullopt;  // cannot see the next header
      }
      if (ip[6] == 0) {
        return std::nullopt;  // a Hop-by-Hop header at length 0: jumbogram
                              // (or malformed), which these bases cannot size
      }
      if (frame_len < byte_offset_ + 40) {
        return std::nullopt;
      }
      return byte_basis_ == ByteBasis::kL4 ? 0 : 40;
    }
    const uint32_t total = 40 + payload;  // the fixed IPv6 header
    if (frame_len < byte_offset_ + total) {
      return std::nullopt;
    }
    return byte_basis_ == ByteBasis::kL4 ? payload : total;
  }
  return std::nullopt;  // not an IP packet
}

void Meter::MeterBatch(bess::PacketBatch *batch, gate_idx_t *gates) const {
  const int cnt = batch->cnt();
  const bess::metadata::mt_offset_t offset = attr_offset(meter_id_attr_);
  if (!bess::metadata::IsValidOffset(offset)) {
    // Fail closed: forwarding a packet whose meter cannot be resolved would
    // pass traffic the policy never saw. OnEvent(PreResume) is where this is
    // reported.
    for (int i = 0; i < cnt; i++) {
      gates[i] = DROP_GATE;
    }
    return;
  }

  const auto id_at = [&](size_t l, int i) {
    const bess::metadata::mt_offset_t slot =
        static_cast<bess::metadata::mt_offset_t>(
            offset + static_cast<int>(l) *
                         static_cast<int>(sizeof(bess::utils::be32_t)));
    return get_attr_with_offset<bess::utils::be32_t>(slot, batch->packet(i)).value();
  };
  const meter::MeterSet *set = published_.Read();
  const uint64_t now = meter::MeterNow();
  const size_t levels = levels_;
  std::array<bool, bess::PacketBatch::kMaxBurst> alive;
  std::array<std::optional<uint32_t>, bess::PacketBatch::kMaxBurst> costs;
  for (int i = 0; i < cnt; i++) {
    gates[i] = kGreenGate;
    alive[i] = true;
    // The packet's charge is level-independent; a packet the basis cannot
    // measure is dropped at its first non-zero meter id. A packet-rate meter
    // charges one unit whatever the frame, so it does not consult the basis.
    costs[i] = packets_ ? std::optional<uint32_t>(1)
                        : AccountedBytes(batch->packet(i));
  }
  // One pass per level, over the packets the levels before it let live: a
  // packet a level marks red is not charged to the levels after it, so the
  // session's bucket is not billed for what the flow's dropped. Green and
  // yellow continue and keep charging; the packet's colour is the worst seen.
  // A level's ids are resolved for the whole batch first (MeterSet::Resolve:
  // the walks and their misses overlap), then checked in packet order.
  static_assert(bess::PacketBatch::kMaxBurst <= meter::MeterSet::kMaxBatch);
  std::array<meter::MeterId, bess::PacketBatch::kMaxBurst> ids;
  std::array<meter::MeterState *, bess::PacketBatch::kMaxBurst> states;
  const auto n = static_cast<size_t>(cnt);
  for (size_t l = 0; l < levels; l++) {
    for (int i = 0; i < cnt; i++) {
      ids[i] = alive[i] ? meter::MeterId(id_at(l, i)) : meter::kInvalidMeterId;
    }
    set->Resolve(std::span<const meter::MeterId>(ids.data(), n),
                 std::span<meter::MeterState *>(states.data(), n));
    for (int i = 0; i < cnt; i++) {
      if (!alive[i]) {
        continue;
      }
      if (ids[i].value() == 0) {
        continue;  // no meter at this level
      }
      if (!costs[i]) {
        // The configured accounting cannot measure this packet (a short
        // frame, a non-IP one, a malformed header): dropping it is the only
        // answer that does not forward it unmetered.
        gates[i] = DROP_GATE;
        alive[i] = false;
        continue;
      }
      meter::MeterState *state = states[i];
      if (state == nullptr) {
        // A dangling id is dropped rather than forwarded unmetered, as with
        // a single level.
        gates[i] = DROP_GATE;
        alive[i] = false;
        continue;
      }
      const uint32_t cost = costs[i].value_or(0);
      const meter::MeterColor color = state->CheckColorAware(
          now, cost, static_cast<meter::MeterColor>(gates[i]));
      // MeterColor's values are the output gates: green 0, yellow 1, red 2.
      gates[i] = static_cast<gate_idx_t>(color);
      if (color == meter::MeterColor::kRed) {
        alive[i] = false;
      }
    }
  }
}

void Meter::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  std::array<gate_idx_t, bess::PacketBatch::kMaxBurst> gates;
  MeterBatch(batch, gates.data());
  EmitBatch(ctx, batch, gates.data());
}

std::string Meter::GetDesc() const {
  return bess::utils::Format("%zu meters", meters_->size());
}

CommandResponse Meter::CommandGetStatus(
    const bess::pb::MeterCommandGetStatusArg &arg) {
  // The published generation is what packets see. Reading it here is the same
  // RCU read a worker does, so a concurrent transaction cannot invalidate the
  // pointer; each meter's Inspect() takes that meter's own check lock when the
  // meter is shared.
  const meter::MeterSet *set = published_.Read();
  bess::pb::MeterCommandGetStatusResponse response;

  auto append = [&](meter::MeterId id) {
    meter::MeterState *state = set->Lookup(id);
    if (state == nullptr) {
      return false;
    }
    const meter::MeterStateSnapshot snapshot = state->Inspect();
    bess::pb::MeterStatus *status = response.add_meters();
    status->set_meter_id(id.value());
    *status->mutable_policy() = meter::MeterPolicyToPb(snapshot.spec);
    status->set_shared(snapshot.sharing == meter::MeterSharing::kShared);
    status->set_last_update_tsc(snapshot.last_update_tsc);
    for (const meter::MeterBucketState &bucket : snapshot.buckets) {
      bess::pb::MeterBucketState *out = status->add_buckets();
      out->set_kind(PbKind(bucket.kind));
      out->set_tokens(bucket.tokens);
      out->set_limit(bucket.limit);
    }
    return true;
  };

  if (arg.meter_ids().empty()) {
    // The control side's committed set, which is what the resource read-back
    // (GetPipeline) shows. A retiring meter is already absent there even
    // though packets may still resolve its id until the removal cascade.
    meters_->VisitPolicies(
        [&](meter::MeterId id, const meter::MeterProfileSpec &) { append(id); });
  } else {
    for (uint32_t raw : arg.meter_ids()) {
      if (raw == 0) {
        return CommandFailure(EINVAL, "meter id 0 is invalid");
      }
      const meter::MeterId id(raw);
      // An erased meter stays in the published generation until the removal
      // cascade, so packets may still resolve it, but the control side is
      // already treating it as absent (as the resource read-back does).
      if (meters_->Retiring(id) || !append(id)) {
        return CommandFailure(ENOENT, "meter %u is not configured", raw);
      }
    }
  }
  return CommandSuccess(response);
}

ADD_MODULE(Meter, "meter",
           "per-session metering with a transactionally changed meter set")
