// SPDX-License-Identifier: BSD-3-Clause

#include "action_table.h"

#include <any>
#include <cstdint>
#include <limits>
#include <expected>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "runtime/runtime_state.h"
#include "framework/resource_codec.h"
#include "../dataplane/transaction_engine.h"
#include "../utils/endian.h"
#include "../utils/format.h"

namespace dataplane = bess::dataplane;

const Commands ActionTable::cmds = {};

CommandResponse ActionTable::Init(const bess::pb::ActionTableArg &arg) {
  if (arg.meters().empty() || arg.next_hops().empty()) {
    return CommandFailure(
        EINVAL,
        "'meters' and 'next_hops' are required: they are the resources an "
        "action's meter and next hop are declared against");
  }
  meters_resource_ = arg.meters();
  next_hops_resource_ = arg.next_hops();

  if (arg.capacity() > std::numeric_limits<ActionId::rep_type>::max()) {
    return CommandFailure(EINVAL, "action capacity exceeds valid action ids");
  }

  const size_t capacity =
      arg.capacity() == 0 ? 1024 : static_cast<size_t>(arg.capacity());
  actions_ = std::make_unique<dataplane::SlotTable<ActionId, Action>>(capacity);
  resource_ = std::make_unique<dataplane::SlotResource<ActionId, Action>>(
      name() + "/actions", *actions_,
      [this](const Action &action) {
        std::vector<dataplane::Reference> refs;
        if (action.meter.value() != 0) {
          refs.push_back(
              {meters_resource_, dataplane::EncodeKey(action.meter)});
        }
        if (action.next_hop.value() != 0) {
          refs.push_back(
              {next_hops_resource_, dataplane::EncodeKey(action.next_hop)});
        }
        return refs;
      },
      std::vector<std::string>{meters_resource_, next_hops_resource_});
  // Typed keys and values over the RPC (D-025).
  resource_->SetCodec(
      std::make_shared<dataplane::TypedCodec<bess::pb::ActionIdKey,
                                             bess::pb::ActionValue>>(
          [](const bess::pb::ActionIdKey &key)
              -> std::expected<dataplane::ResourceKey, std::string> {
            if (key.id() == 0) {
              return std::unexpected("action id 0 is invalid");
            }
            return dataplane::EncodeKey(ActionId(key.id()));
          },
          [](const bess::pb::ActionValue &value)
              -> std::expected<std::any, std::string> {
            return std::any(
                Action{bess::meter::MeterId(value.meter_id()),
                       bess::route::NextHopId(value.next_hop_id())});
          }));
  if (auto registered =
          bess::runtime::runtime().transactions().Register(resource_.get());
      !registered) {
    resource_.reset();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }

  using AccessMode = bess::metadata::Attribute::AccessMode;
  action_id_attr_ =
      AddMetadataAttr("action_id", sizeof(bess::utils::be32_t), AccessMode::kRead);
  meter_id_attr_ =
      AddMetadataAttr("meter_id", sizeof(bess::utils::be32_t), AccessMode::kWrite);
  next_hop_id_attr_ = AddMetadataAttr("next_hop_id",
                                      sizeof(bess::utils::be32_t),
                                      AccessMode::kWrite);
  if (action_id_attr_ < 0 || meter_id_attr_ < 0 || next_hop_id_attr_ < 0) {
    const std::string name = resource_->name();
    CHECK(bess::runtime::runtime().transactions().ReleaseForTeardown(
        std::span<const std::string>(&name, 1)));
    resource_.reset();
    return CommandFailure(EINVAL, "add_metadata_attr() failed");
  }
  return CommandSuccess();
}

void ActionTable::DeInit() {
  if (resource_ == nullptr) {
    return;
  }
  // An ExactMatch in action mode references these actions, so release rather
  // than unregister: the order modules are destroyed in must not decide
  // whether a pipeline can be torn down.
  const std::string name = resource_->name();
  auto released = bess::runtime::runtime().transactions().ReleaseForTeardown(
      std::span<const std::string>(&name, 1));
  CHECK(released) << released.error();
  resource_.reset();
}

int ActionTable::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  // Metadata offsets are assigned just before this: an attribute with no
  // valid offset means the packet path will drop everything, which is worth
  // saying once, here, rather than never.
  if (!bess::metadata::IsValidOffset(attr_offset(action_id_attr_)) ||
      !bess::metadata::IsValidOffset(attr_offset(meter_id_attr_)) ||
      !bess::metadata::IsValidOffset(attr_offset(next_hop_id_attr_))) {
    LOG(ERROR) << "ActionTable '" << name()
               << "': 'action_id', 'meter_id' or 'next_hop_id' is unreadable "
                  "(is an ExactMatch writing the action id, and are a Meter "
                  "and a Router downstream?); dropping packets";
  }
  return 0;
}

void ActionTable::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const int cnt = batch->cnt();
  const bess::metadata::mt_offset_t action_offset = attr_offset(action_id_attr_);
  const bess::metadata::mt_offset_t meter_offset = attr_offset(meter_id_attr_);
  const bess::metadata::mt_offset_t hop_offset = attr_offset(next_hop_id_attr_);
  if (!bess::metadata::IsValidOffset(action_offset) ||
      !bess::metadata::IsValidOffset(meter_offset) ||
      !bess::metadata::IsValidOffset(hop_offset)) {
    // Fail closed: without the action id nothing can be resolved, and without
    // a place to put the action's meter and next hop the modules downstream
    // would see stale ids. OnEvent(PreResume) is where this is reported.
    for (int i = 0; i < cnt; i++) {
      EmitPacket(ctx, batch->packet(i), DROP_GATE);
    }
    return;
  }

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    const ActionId id(
        get_attr_with_offset<bess::utils::be32_t>(action_offset, pkt).value());
    const Action *action = actions_->Lookup(id);
    if (action == nullptr) {
      EmitPacket(ctx, pkt, DROP_GATE);
      continue;
    }
    _set_attr_with_offset<bess::utils::be32_t>(
        meter_offset, pkt, bess::utils::be32_t(action->meter.value()));
    _set_attr_with_offset<bess::utils::be32_t>(
        hop_offset, pkt, bess::utils::be32_t(action->next_hop.value()));
    EmitPacket(ctx, pkt, 0);
  }
}

std::string ActionTable::GetDesc() const {
  return bess::utils::Format("%zu actions", actions_->size());
}

ADD_MODULE(ActionTable, "action_table",
           "per-session actions: an action id resolves to a meter and a "
           "next hop")
