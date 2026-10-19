// SPDX-License-Identifier: BSD-3-Clause

#include "meter.h"

#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <variant>

#include "framework/resource_bindings.h"
#include "dataplane/transaction_engine.h"
#include "utils/endian.h"
#include "utils/format.h"
#include "utils/ip.h"

namespace dataplane = bess::dataplane;
namespace meter = bess::meter;

const Commands Meter::cmds = {};

const ResourceTables Meter::resource_tables = {
    ResourceTable::Map<bess::pb::MeterIdKey, bess::pb::MeterPolicyValue>(
        "meters"),
};

namespace {

// The RPC's policy message as K5's profile specification (D-025).
std::expected<meter::MeterProfileSpec, std::string> PolicyFromPb(
    const bess::pb::MeterPolicyValue &value) {
  switch (value.profile_case()) {
    case bess::pb::MeterPolicyValue::kSrTcm:
      return meter::MeterProfileSpec{meter::SrTcmSpec{
          value.sr_tcm().committed_rate(), value.sr_tcm().committed_burst(),
          value.sr_tcm().excess_burst()}};
    case bess::pb::MeterPolicyValue::kTrTcm:
      return meter::MeterProfileSpec{meter::TrTcmSpec{
          value.tr_tcm().committed_rate(), value.tr_tcm().committed_burst(),
          value.tr_tcm().peak_rate(), value.tr_tcm().peak_burst()}};
    case bess::pb::MeterPolicyValue::kTrTcmRfc4115:
      return meter::MeterProfileSpec{meter::TrTcmRfc4115Spec{
          value.tr_tcm_rfc4115().committed_rate(),
          value.tr_tcm_rfc4115().committed_burst(),
          value.tr_tcm_rfc4115().excess_rate(),
          value.tr_tcm_rfc4115().excess_burst()}};
    case bess::pb::MeterPolicyValue::PROFILE_NOT_SET:
      break;
  }
  return std::unexpected("meter policy has no profile set");
}

// PolicyFromPb's inverse: the message a client sends for `spec` (D-129's
// read-back; every field is a scalar, so the form is unique).
bess::pb::MeterPolicyValue PolicyToPb(const meter::MeterProfileSpec &spec) {
  bess::pb::MeterPolicyValue value;
  if (const auto *sr = std::get_if<meter::SrTcmSpec>(&spec)) {
    auto *pb = value.mutable_sr_tcm();
    pb->set_committed_rate(sr->committed_rate);
    pb->set_committed_burst(sr->committed_burst);
    pb->set_excess_burst(sr->excess_burst);
  } else if (const auto *tr = std::get_if<meter::TrTcmSpec>(&spec)) {
    auto *pb = value.mutable_tr_tcm();
    pb->set_committed_rate(tr->committed_rate);
    pb->set_committed_burst(tr->committed_burst);
    pb->set_peak_rate(tr->peak_rate);
    pb->set_peak_burst(tr->peak_burst);
  } else if (const auto *rfc = std::get_if<meter::TrTcmRfc4115Spec>(&spec)) {
    auto *pb = value.mutable_tr_tcm_rfc4115();
    pb->set_committed_rate(rfc->committed_rate);
    pb->set_committed_burst(rfc->committed_burst);
    pb->set_excess_rate(rfc->excess_rate);
    pb->set_excess_burst(rfc->excess_burst);
  }
  return value;
}

}  // namespace

// "<module>/meters" (D-021): key = EncodeKey(MeterId), value =
// MeterProfileSpec. The desired state is a MeterSetBuilder; a transaction
// edits a private copy of it (BeginTransaction) and every upsert publishes a
// generation built from that copy -- so nothing is visible until the
// transaction publishes, and a rejected transaction leaves the live builder,
// the published generation and every meter's token state untouched.
//
// An erase is the two-step removal SlotTable defines: it is absent to the
// control side at once (and the id is unpublishable), the published
// generation keeps the meter readable until the removal cascade's stage runs,
// and that stage publishes a generation without it and retires the one it
// replaced.
class Meter::MetersResource final : public dataplane::Resource {
 public:
  explicit MetersResource(Meter &owner)
      : Resource(owner.name() + "/meters"), owner_(owner) {}

  size_t LiveCount() const override { return owner_.live_->size(); }

  bool Contains(const dataplane::ResourceKey &key) const override {
    meter::MeterId id;
    if (!dataplane::DecodeKey(key, &id) || !owner_.live_->Contains(id)) {
      return false;
    }
    return !owner_.retiring_.contains(id);
  }


  // A reader may still hold an erased meter's id from an action's old value:
  // the erase is deferred to the removal cascade.
  bool DefersErase() const override { return true; }

  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override {
    meter::MeterId id;
    if (!dataplane::DecodeKey(op.key, &id) || id.value() == 0 ||
        id.value() > owner_.live_->capacity()) {
      return std::unexpected("invalid meter id");
    }
    if (op.kind == dataplane::OpKind::kErase) {
      if (!Contains(op.key)) {
        return std::unexpected("not found");
      }
      // The key stops accepting writes now, but stays in every published
      // generation until the removal cascade: an action's old value can
      // still hand a reader this meter id. A simultaneous upsert of another
      // meter must not make this id disappear prematurely.
      auto erase = std::make_unique<EraseOp>(owner_, id);
      owner_.retiring_.insert(id);
      Reservation erase_reservation{std::move(erase), {},
                                    Footprint{.removals = 1}};
      erase_reservation.existed = true;
      return erase_reservation;
    }

    const Policy *policy = std::any_cast<Policy>(&op.value);
    if (policy == nullptr) {
      return std::unexpected("wrong value type (want MeterProfileSpec)");
    }
    if (auto valid = meter::ValidateMeterProfileSpec(*policy); !valid) {
      return std::unexpected(std::string("invalid profile: ") +
                             meter::MeterErrorName(valid.error()));
    }
    if (owner_.retiring_.contains(id)) {
      return std::unexpected(
          "the meter is still retiring (removed less than a grace period "
          "ago)");
    }

    // Everything fallible -- profile compilation, state allocation, building
    // the generation -- happens here, on the transaction's private copy.
    owner_.BeginTransaction();
    const bool existed = owner_.staged_->Contains(id);
    auto applied =
        existed ? owner_.staged_->Reconfigure(id, *policy)
                : owner_.staged_->Add(id, *policy, meter::MeterSharing::kShared);
    if (!applied) {
      return std::unexpected(meter::MeterErrorName(applied.error()));
    }
    owner_.staged_policies_.insert_or_assign(id, *policy);
    Reservation reservation{
        std::make_unique<UpsertOp>(owner_, owner_.staged_->Build()), {},
        // Publishing replaces the generation that is live now.
        Footprint{.retires = 1}};
    reservation.existed = owner_.live_->Contains(id);
    return reservation;
  }

  void EndTransaction() noexcept override { owner_.EndTransaction(); }

 private:
  class UpsertOp final : public dataplane::StagedOp {
   public:
    UpsertOp(Meter &owner,
             std::unique_ptr<const meter::MeterSet> generation)
        : owner_(owner), generation_(std::move(generation)) {}
    void Publish(dataplane::Retirer &retirer) noexcept override {
      owner_.PublishStaged(std::move(generation_), retirer);
    }

   private:
    Meter &owner_;
    std::unique_ptr<const meter::MeterSet> generation_;
  };

  class EraseOp final : public dataplane::StagedOp {
   public:
    EraseOp(Meter &owner, meter::MeterId id) : owner_(owner), id_(id) {}
    void Publish(dataplane::Retirer &retirer) noexcept override {
      // The stage runs one grace period after this publish: by then no reader
      // can still obtain the id from a referrer's old value.
      retirer.RemoveLater(
          [&owner = owner_, id = id_](dataplane::Retirer &later) {
            owner.Unpublish(id, later);
          });
    }
    // The reservation marked the meter as retiring; a transaction that will
    // not publish must put it back.
    void Abort() noexcept override { owner_.retiring_.erase(id_); }

   private:
    Meter &owner_;
    meter::MeterId id_;
  };

  Meter &owner_;
};

CommandResponse Meter::Init(const bess::pb::MeterArg &arg) {
  // The level count sizes the attribute both this module and the ActionTable
  // declare, and the framework refuses a size mismatch between them. Validate
  // it before the resource is registered, so a refused module leaves nothing.
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
  live_ = std::make_unique<meter::MeterSetBuilder>(capacity);
  published_.Initialize(live_->Build());

  resource_ = std::make_unique<MetersResource>(*this);
  binding_ = init_context().codecs().Bind(
      *resource_,
      std::make_shared<bess::framework::TypedCodec<bess::pb::MeterIdKey,
                                             bess::pb::MeterPolicyValue>>(
          [](const bess::pb::MeterIdKey &key)
              -> std::expected<dataplane::ResourceKey, std::string> {
            if (key.id() == 0) {
              return std::unexpected("meter id 0 is invalid");
            }
            return dataplane::EncodeKey(meter::MeterId(key.id()));
          },
          [](const bess::pb::MeterPolicyValue &value)
              -> std::expected<std::any, std::string> {
            auto policy = PolicyFromPb(value);
            if (!policy) {
              return std::unexpected(policy.error());
            }
            return std::any(*policy);
          },
          // Every meter the control side holds (an erased one is gone at
          // once, though it stays readable until its removal stage).
          [this](const auto &visit) {
            for (const auto &[id, policy] : policies_) {
              if (retiring_.contains(id)) {
                continue;
              }
              bess::pb::MeterIdKey key;
              key.set_id(id.value());
              visit(key, PolicyToPb(policy));
            }
          }));
  if (auto registered =
          init_context().resources().Register(resource_.get());
      !registered) {
    binding_.Reset();
    resource_.reset();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }

  // A graph with two meters -- a flow maximum and a session aggregate from one
  // classifier result -- sets the name so each reads its own identity.
  id_attr_name_ = arg.id_attr().empty() ? "meter_id" : arg.id_attr();
  // One attribute, 4 bytes per level: every level's id is read from the same
  // place the ActionTable wrote the list.
  meter_id_attr_ = AddMetadataAttr(
      id_attr_name_, levels_ * sizeof(bess::utils::be32_t),
      bess::metadata::Attribute::AccessMode::kRead);
  // The profile's rates and bursts are in the chosen unit: bytes/s for a
  // bit-rate, packets/s for a packet-rate (8.2.63), the same buckets either
  // way. The library is unit-agnostic -- it is handed one integer per packet.
  packets_ = arg.cost() == bess::pb::MeterArg::METER_COST_PACKETS;
  if (meter_id_attr_ < 0) {
    const std::string name = resource_->name();
    CHECK(init_context().resources().ReleaseForTeardown(
        std::span<const std::string>(&name, 1)));
    binding_.Reset();
    resource_.reset();
    return CommandFailure(-meter_id_attr_, "add_metadata_attr() failed");
  }
  return CommandSuccess();
}

void Meter::DeInit() {
  if (resource_ == nullptr) {
    return;
  }
  // Meters are referenced by actions, so a meter may leave with live keys and
  // with its keys still named (D-021's teardown release): the order modules
  // are destroyed in must not decide whether a pipeline can be torn down.
  const std::string name = resource_->name();
  auto released = init_context().resources().ReleaseForTeardown(
      std::span<const std::string>(&name, 1));
  CHECK(released) << released.error();
  binding_.Reset();
  resource_.reset();
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
               << "' attribute is unreadable (is an ActionTable writing it?); "
                  "dropping packets";
  }
  return 0;
}

void Meter::BeginTransaction() {
  if (staged_ == nullptr) {
    staged_ = live_->Clone();
    staged_policies_ = policies_;
    published_staged_ = false;
  }
}

void Meter::PublishStaged(std::unique_ptr<const meter::MeterSet> generation,
                          dataplane::Retirer &retirer) {
  // Every operation of the transaction reserved against the same private
  // copy, so the swap happens once: the desired state becomes what that copy
  // holds, and the published generation is this operation's view of it.
  if (!published_staged_) {
    live_.swap(staged_);
    policies_.swap(staged_policies_);
    published_staged_ = true;
  }
  retirer.Retire(published_.Exchange(std::move(generation)));
}

void Meter::Unpublish(meter::MeterId id, dataplane::Retirer &later) {
  if (retiring_.erase(id) == 0) {
    return;  // re-added: the published generation is already the new meter
  }
  auto erased = live_->Erase(id);
  CHECK(erased) << meter::MeterErrorName(erased.error());
  policies_.erase(id);
  later.Retire(published_.Exchange(live_->Build()));
}

void Meter::EndTransaction() noexcept {
  staged_.reset();
  staged_policies_.clear();
  published_staged_ = false;
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
  for (size_t l = 0; l < levels; l++) {
    const bess::metadata::mt_offset_t slot =
        static_cast<bess::metadata::mt_offset_t>(
            offset + static_cast<int>(l) *
                         static_cast<int>(sizeof(bess::utils::be32_t)));
    for (int i = 0; i < cnt; i++) {
      if (!alive[i]) {
        continue;
      }
      bess::PacketRef pkt = batch->packet(i);
      const meter::MeterId id(
          get_attr_with_offset<bess::utils::be32_t>(slot, pkt).value());
      if (id.value() == 0) {
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
      meter::MeterState *state = set->Lookup(id);
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
  return bess::utils::Format("%zu meters", live_->size());
}

ADD_MODULE(Meter, "meter",
           "per-session metering with a transactionally changed meter set")
