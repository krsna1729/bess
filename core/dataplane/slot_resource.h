// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_DATAPLANE_SLOT_RESOURCE_H_
#define BESS_DATAPLANE_SLOT_RESOURCE_H_

#include <any>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "dataplane/resource.h"
#include "dataplane/slot_table.h"

namespace bess {
namespace dataplane {

// A SlotTable exposed as a transactional resource (G1.2b): key = the id
// (EncodeKey(Id)), value = T. Upsert publishes a new immutable object at the
// id (the old one is retired after the transaction's grace period); erase
// removes it, keeping it readable -- and the id unusable -- until the
// transaction's removal cascade reaches it (SlotTable::Retire, then
// Unpublish from Retirer::RemoveLater). For objects that other resources reference by id: actions, next
// hops, meters' policies. Decision D-021.
//
// `references` names the keys a value refers to (for example, an action
// object naming a meter), so the engine can order and check them.
template <typename Id, typename T>
class SlotResource final : public Resource {
 public:
  using ReferencesFn = std::function<std::vector<Reference>(const T &)>;

  // `may_reference`: the resources `references` can name (declared, see
  // Resource).
  SlotResource(std::string name, SlotTable<Id, T> &table,
               ReferencesFn references = {},
               std::vector<std::string> may_reference = {})
      : Resource(std::move(name), std::move(may_reference)),
        table_(table),
        references_(std::move(references)) {}

  size_t LiveCount() const override { return table_.size(); }

  bool Contains(const ResourceKey &key) const override {
    Id id;
    return DecodeKey(key, &id) && table_.Contains(id);
  }

  bool DefersErase() const override { return true; }

  std::vector<Reference> ReferencesOf(const ResourceKey &key) const override {
    Id id;
    if (!references_ || !DecodeKey(key, &id)) {
      return {};
    }
    const T *current = table_.Current(id);
    return current ? references_(*current) : std::vector<Reference>{};
  }

  std::expected<Reservation, std::string> Reserve(const Op &op) override {
    Id id;
    if (!DecodeKey(op.key, &id) || !table_.ValidId(id)) {
      return std::unexpected("invalid id");
    }
    if (op.kind == OpKind::kErase) {
      if (!table_.Contains(id)) {
        return std::unexpected("not found");
      }
      Reservation erase{std::make_unique<UnpublishOp>(table_, id), {},
                        Footprint{.removals = 1}};
      erase.existed = true;
      if (references_) {
        erase.previous_references = references_(*table_.Current(id));
      }
      return erase;
    }
    if (!table_.CanPublish(id)) {
      return std::unexpected(
          "id is still retiring (removed less than a grace period ago)");
    }
    const T *value = std::any_cast<T>(&op.value);
    if (value == nullptr) {
      return std::unexpected("wrong value type");
    }
    const T *previous = table_.Current(id);
    Reservation reservation{
        std::make_unique<PublishOp>(table_, id, std::make_unique<const T>(*value)),
        {},
        // The object it replaces, if any, is retired.
        Footprint{.retires = previous != nullptr ? 1u : 0u}};
    reservation.existed = previous != nullptr;
    if (references_) {
      reservation.references = references_(*value);
      if (previous != nullptr) {
        reservation.previous_references = references_(*previous);
      }
    }
    return reservation;
  }

 private:
  class PublishOp final : public StagedOp {
   public:
    PublishOp(SlotTable<Id, T> &table, Id id, std::unique_ptr<const T> object)
        : table_(table), id_(id), object_(std::move(object)) {}
    void Publish(Retirer &retirer) noexcept override {
      retirer.Retire(table_.Publish(id_, std::move(object_)));
    }

   private:
    SlotTable<Id, T> &table_;
    Id id_;
    std::unique_ptr<const T> object_;
  };

  class UnpublishOp final : public StagedOp {
   public:
    UnpublishOp(SlotTable<Id, T> &table, Id id) : table_(table), id_(id) {}
    void Publish(Retirer &retirer) noexcept override {
      table_.Retire(id_);  // absent now, readable until the cascade
      retirer.RemoveLater([&table = table_, id = id_](Retirer &later) {
        later.Retire(table.Unpublish(id));
      });
    }

   private:
    SlotTable<Id, T> &table_;
    Id id_;
  };

  SlotTable<Id, T> &table_;
  ReferencesFn references_;
};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_SLOT_RESOURCE_H_
