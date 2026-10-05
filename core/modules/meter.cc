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

#include "framework/resource_bindings.h"
#include "dataplane/transaction_engine.h"
#include "utils/endian.h"
#include "utils/format.h"

namespace dataplane = bess::dataplane;
namespace meter = bess::meter;

const Commands Meter::cmds = {};

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
          }));
  if (auto registered =
          init_context().resources().Register(resource_.get());
      !registered) {
    binding_.Reset();
    resource_.reset();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }

  meter_id_attr_ = AddMetadataAttr("meter_id", sizeof(bess::utils::be32_t),
                                   bess::metadata::Attribute::AccessMode::kRead);
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
    LOG(ERROR) << "Meter '" << name()
               << "': the 'meter_id' attribute is unreadable (is an "
                  "ActionTable writing it?); dropping packets";
  }
  return 0;
}

void Meter::BeginTransaction() {
  if (staged_ == nullptr) {
    staged_ = live_->Clone();
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
  later.Retire(published_.Exchange(live_->Build()));
}

void Meter::EndTransaction() noexcept {
  staged_.reset();
  published_staged_ = false;
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
  std::array<meter::MeterId, bess::PacketBatch::kMaxBurst> ids;
  std::array<uint32_t, bess::PacketBatch::kMaxBurst> bytes;
  std::array<meter::MeterColor, bess::PacketBatch::kMaxBurst> colors;
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    ids[i] = meter::MeterId(
        get_attr_with_offset<bess::utils::be32_t>(offset, pkt).value());
    bytes[i] = pkt.total_len();
  }

  const uint64_t resolved = set->CheckBatch(
      std::span(ids).first(static_cast<size_t>(cnt)),
      std::span(bytes).first(static_cast<size_t>(cnt)),
      std::span(colors).first(static_cast<size_t>(cnt)), now);
  for (int i = 0; i < cnt; i++) {
    if ((resolved & (uint64_t{1} << i)) == 0) {
      // Id 0 is "no meter": unmetered, so green. Anything else names a meter
      // that is not there (a dangling action), which is dropped.
      gates[i] = ids[i].value() == 0 ? kGreenGate : DROP_GATE;
      continue;
    }
    // MeterColor's values are the output gates: green 0, yellow 1, red 2.
    gates[i] = static_cast<gate_idx_t>(colors[i]);
  }
}

void Meter::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  std::array<gate_idx_t, bess::PacketBatch::kMaxBurst> gates;
  MeterBatch(batch, gates.data());
  for (int i = 0; i < batch->cnt(); i++) {
    EmitPacket(ctx, batch->packet(i), gates[i]);
  }
}

std::string Meter::GetDesc() const {
  return bess::utils::Format("%zu meters", live_->size());
}

ADD_MODULE(Meter, "meter",
           "per-session metering with a transactionally changed meter set")
