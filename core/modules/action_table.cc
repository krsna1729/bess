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

#include "framework/resource_bindings.h"
#include "dataplane/transaction_engine.h"
#include "utils/endian.h"
#include "utils/format.h"

namespace dataplane = bess::dataplane;

const Commands ActionTable::cmds = {};

const ResourceTables ActionTable::resource_tables = {
    ResourceTable::Map<bess::pb::ActionIdKey, bess::pb::ActionValue>("actions"),
};

CommandResponse ActionTable::Init(const bess::pb::ActionTableArg &arg) {
  if (arg.meters().empty() || arg.next_hops().empty()) {
    return CommandFailure(
        EINVAL,
        "'meters' and 'next_hops' are required: they are the resources an "
        "action's meter and next hop are declared against");
  }
  meters_resource_ = arg.meters();
  next_hops_resource_ = arg.next_hops();

  const size_t levels = arg.levels() == 0 ? 1 : arg.levels();
  if (levels > kMaxLevels) {
    return CommandFailure(EINVAL, "levels must be 1-%zu", kMaxLevels);
  }
  levels_ = levels;

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
        for (size_t i = 0; i < action.levels; i++) {
          if (action.meters[i].value() != 0) {
            refs.push_back(
                {meters_resource_, dataplane::EncodeKey(action.meters[i])});
          }
        }
        if (action.next_hop.value() != 0) {
          refs.push_back(
              {next_hops_resource_, dataplane::EncodeKey(action.next_hop)});
        }
        return refs;
      },
      std::vector<std::string>{meters_resource_, next_hops_resource_});
  // Typed keys and values over the RPC (D-025).
  binding_ = init_context().codecs().Bind(
      *resource_,
      std::make_shared<bess::framework::TypedCodec<bess::pb::ActionIdKey,
                                             bess::pb::ActionValue>>(
          [](const bess::pb::ActionIdKey &key)
              -> std::expected<dataplane::ResourceKey, std::string> {
            if (key.id() == 0) {
              return std::unexpected("action id 0 is invalid");
            }
            return dataplane::EncodeKey(ActionId(key.id()));
          },
          [levels](const bess::pb::ActionValue &value)
              -> std::expected<std::any, std::string> {
            Action action{};
            if (value.meter_ids_size() > 0) {
              // The list is the level count: it must match this table's, or
              // the attribute the Meter reads would disagree with the one
              // written here.
              if (static_cast<size_t>(value.meter_ids_size()) != levels) {
                return std::unexpected(
                    "meter_ids has " + std::to_string(value.meter_ids_size()) +
                    " ids, this table has " + std::to_string(levels) +
                    " levels");
              }
              for (int i = 0; i < value.meter_ids_size(); i++) {
                action.meters[i] =
                    bess::meter::MeterId(value.meter_ids(i));
              }
              action.levels = value.meter_ids_size();
            } else {
              // The single-id form names level 0 and leaves the other slots
              // at "no meter". The action still writes every slot this
              // table's level count covers: a Meter expecting more levels
              // must not read a neighbour slot this action never wrote.
              action.meters[0] = bess::meter::MeterId(value.meter_id());
              action.levels = levels;
            }
            action.next_hop = bess::route::NextHopId(value.next_hop_id());
            return std::any(action);
          },
          // Every live action, as its value names it (an erased one is gone
          // at once, though it stays readable until its removal stage).
          [this](const auto &visit) {
            for (size_t i = 1; i <= actions_->capacity(); i++) {
              const ActionId id(static_cast<ActionId::rep_type>(i));
              const Action *action = actions_->Current(id);
              if (action == nullptr) {
                continue;
              }
              bess::pb::ActionIdKey key;
              key.set_id(id.value());
              bess::pb::ActionValue value;
              value.set_meter_id(action->meters[0].value());
              // Only a multi-level table names the list: a one-level table
              // reads back exactly as it was written.
              if (levels_ > 1) {
                for (size_t l = 0; l < levels_; l++) {
                  value.add_meter_ids(action->meters[l].value());
                }
              }
              value.set_next_hop_id(action->next_hop.value());
              visit(key, value);
            }
          }));
  if (auto registered =
          init_context().resources().Register(resource_.get());
      !registered) {
    binding_.Reset();
    resource_.reset();
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }

  using AccessMode = bess::metadata::Attribute::AccessMode;
  action_attr_name_ = arg.action_attr().empty() ? "action_id" : arg.action_attr();
  metering_attr_name_ = arg.meter_attr().empty() ? "meter_id" : arg.meter_attr();
  next_hop_attr_name_ =
      arg.next_hop_attr().empty() ? "next_hop_id" : arg.next_hop_attr();
  action_id_attr_ = AddMetadataAttr(action_attr_name_, sizeof(bess::utils::be32_t),
                                    AccessMode::kRead);
  // One attribute, 4 bytes per level: every level's id is written together,
  // so the Meter downstream reads a packet's levels in one place.
  meter_id_attr_ = AddMetadataAttr(
      metering_attr_name_, levels * sizeof(bess::utils::be32_t),
      AccessMode::kWrite);
  next_hop_id_attr_ = AddMetadataAttr(next_hop_attr_name_,
                                      sizeof(bess::utils::be32_t),
                                      AccessMode::kWrite);
  if (action_id_attr_ < 0 || meter_id_attr_ < 0 || next_hop_id_attr_ < 0) {
    const std::string name = resource_->name();
    CHECK(init_context().resources().ReleaseForTeardown(
        std::span<const std::string>(&name, 1)));
    binding_.Reset();
    resource_.reset();
    return CommandFailure(EINVAL,
                          "add_metadata_attr() failed for '%s', '%s' or '%s'",
                          action_attr_name_.c_str(),
                          metering_attr_name_.c_str(),
                          next_hop_attr_name_.c_str());
  }
  return CommandSuccess();
}

void ActionTable::DeInit() {
  if (resource_ == nullptr) {
    return;
  }
  // A classifier in action mode references these actions (ExactMatch,
  // WildcardMatch), so release rather than unregister: the order modules are
  // destroyed in must not decide whether a pipeline can be torn down.
  const std::string name = resource_->name();
  auto released = init_context().resources().ReleaseForTeardown(
      std::span<const std::string>(&name, 1));
  CHECK(released) << released.error();
  binding_.Reset();
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
    LOG(ERROR) << "ActionTable '" << name() << "': '" << action_attr_name_
               << "', '" << metering_attr_name_ << "' or '" << next_hop_attr_name_
               << "' is unreadable (is something writing the action id, and are "
                  "a Meter and a Router downstream?); dropping packets";
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
    for (bess::PacketRef pkt : batch->packets()) {
      EmitPacket(ctx, pkt, DROP_GATE);
    }
    return;
  }

  int out_cnt = 0;
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    const ActionId id(
        get_attr_with_offset<bess::utils::be32_t>(action_offset, pkt).value());
    const Action *action = actions_->Lookup(id);
    if (unlikely(action == nullptr)) {
      EmitPacket(ctx, pkt, DROP_GATE);
      continue;
    }
    _set_attr_with_offset<bess::utils::be32_t>(
        hop_offset, pkt, bess::utils::be32_t(action->next_hop.value()));
    // Every slot this table's level count covers: the record may name fewer
    // levels (a typed Action with the historical constructor names one), and
    // the rest are 0, "no meter at this level". The Meter reads contiguous
    // 4-byte slots, so a slot left unwritten would be stale metadata.
    for (size_t l = 0; l < levels_; l++) {
      const bess::metadata::mt_offset_t slot =
          static_cast<bess::metadata::mt_offset_t>(
              meter_offset + static_cast<int>(l) *
                                 static_cast<int>(sizeof(bess::utils::be32_t)));
      const bess::meter::MeterId id =
          l < action->levels ? action->meters[l] : bess::meter::MeterId{};
      _set_attr_with_offset<bess::utils::be32_t>(
          slot, pkt, bess::utils::be32_t(id.value()));
    }
    if (out_cnt != i) {
      batch->handles()[out_cnt] = batch->handles()[i];
    }
    out_cnt++;
  }
  if (likely(out_cnt > 0)) {
    batch->set_cnt(out_cnt);
    RunNextModule(ctx, batch);
  }
}

std::string ActionTable::GetDesc() const {
  return bess::utils::Format("%zu actions", actions_->size());
}

ADD_MODULE(ActionTable, "action_table",
           "per-session actions: an action id resolves to a meter and a "
           "next hop")
