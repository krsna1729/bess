// SPDX-License-Identifier: BSD-3-Clause

#include "meter/meter_resource.h"

#include <any>
#include <utility>

#include "utils/logging.h"

namespace bess::meter {

class MeterResource::UpsertOp final : public dataplane::StagedOp {
 public:
  explicit UpsertOp(MeterResource &owner) : owner_(owner) {}
  void Publish(dataplane::Retirer &retirer) noexcept override {
    owner_.PublishStaged(retirer);
  }

 private:
  MeterResource &owner_;
};

class MeterResource::EraseOp final : public dataplane::StagedOp {
 public:
  EraseOp(MeterResource &owner, MeterId id) : owner_(owner), id_(id) {}
  void Publish(dataplane::Retirer &retirer) noexcept override {
    // The stage runs one grace period after this publish: by then no reader
    // can still obtain the id from a referrer's old value.
    retirer.RemoveLater([&owner = owner_, id = id_, prepaid = true](
                            dataplane::Retirer &later) mutable {
      owner.Unpublish(id, prepaid, later);
    });
  }
  // The reservation marked the meter as retiring and prepaid its removal
  // stage; a transaction that will not publish puts both back.
  void Abort() noexcept override {
    owner_.retiring_.erase(id_);
    owner_.RefundErase();
  }

 private:
  MeterResource &owner_;
  MeterId id_;
};

MeterResource::MeterResource(std::string name, size_t capacity,
                             rcu::RcuPtr<MeterSet> &published,
                             utils::MemoryGrowth *lease)
    : Resource(std::move(name)),
      held_(Bytes(capacity)),
      growth_(lease, &held_),
      control_growth_(lease, &held_),
      published_(published),
      live_(std::make_unique<MeterSetBuilder>(capacity)),
      retiring_(ControlAllocator<MeterId>(&control_growth_)),
      policies_(Policies::allocator_type(&control_growth_)),
      staged_policies_(Policies::allocator_type(&control_growth_)) {
  live_->set_memory_growth(&growth_);
  published_.Initialize(live_->Build());
}

MeterResource::~MeterResource() = default;

bool MeterResource::Contains(const dataplane::ResourceKey &key) const {
  MeterId id;
  if (!dataplane::DecodeKey(key, &id) || !live_->Contains(id)) {
    return false;
  }
  return !retiring_.contains(id);
}

std::expected<dataplane::Resource::Reservation, std::string>
MeterResource::Reserve(const dataplane::Op &op) {
  MeterId id;
  if (!dataplane::DecodeKey(op.key, &id) || id.value() == 0 ||
      id.value() > live_->capacity()) {
    return std::unexpected("invalid meter id");
  }
  if (op.kind == dataplane::OpKind::kErase) {
    if (!Contains(op.key)) {
      return std::unexpected("not found");
    }
    // The key stops accepting writes now, but stays in every published
    // generation until the removal cascade: a referrer's old value can still
    // hand a reader this id, and a simultaneous upsert of another meter must
    // not make it disappear early. The cascade's edit without it (one id's
    // path, MeterSet::Bytes) is admitted now: a removal stage cannot refuse,
    // so a refusal refuses this erase.
    auto erase = std::make_unique<EraseOp>(*this, id);
    PrepayErase();
    try {
      retiring_.insert(id);
    } catch (...) {
      RefundErase();
      throw;
    }
    Reservation reservation{std::move(erase), {}, Footprint{.removals = 1},
                            true, {}};
    return reservation;
  }

  const MeterProfileSpec *policy = std::any_cast<MeterProfileSpec>(&op.value);
  if (policy == nullptr) {
    return std::unexpected("wrong value type (want MeterProfileSpec)");
  }
  if (auto valid = ValidateMeterProfileSpec(*policy); !valid) {
    return std::unexpected(std::string("invalid profile: ") +
                           MeterErrorName(valid.error()));
  }
  if (retiring_.contains(id)) {
    return std::unexpected(
        "the meter is still retiring (removed less than a grace period ago)");
  }

  // Everything fallible happens on the private builder. Prepare builds one
  // generation for all operations after their validation has succeeded.
  // staged_policies_ holds this transaction's upserts only.
  BeginTransaction();
  const bool existed = staged_->Contains(id);
  auto applied = existed ? staged_->Reconfigure(id, *policy)
                         : staged_->Add(id, *policy, MeterSharing::kShared);
  if (!applied) {
    return std::unexpected(MeterErrorName(applied.error()));
  }
  staged_policies_.insert_or_assign(id, *policy);
  // Publishing replaces the generation that is live now.
  Reservation reservation{std::make_unique<UpsertOp>(*this),
                          {}, Footprint{.retires = 1}, live_->Contains(id), {}};
  return reservation;
}

void MeterResource::BeginTransaction() {
  if (staged_ == nullptr) {
    staged_ = live_->Clone();  // O(1): shares the index with live_
    published_staged_ = false;
  }
}

std::expected<void, std::string> MeterResource::Prepare() {
  if (staged_ != nullptr) {
    staged_generation_ = staged_->Build();
  }
  return {};
}

void MeterResource::EndTransaction() noexcept {
  staged_generation_.reset();
  staged_.reset();
  staged_policies_.clear();
  published_staged_ = false;
}

void MeterResource::PublishStaged(dataplane::Retirer &retirer) {
  if (!published_staged_) {
    live_.swap(staged_);
    // The upserts' nodes move over; the policies they replace are
    // overwritten in place. Neither allocates.
    policies_.merge(staged_policies_);
    for (auto &[id, policy] : staged_policies_) {
      policies_.find(id)->second = policy;
    }
    retirer.Retire(published_.Exchange(std::move(staged_generation_)));
    published_staged_ = true;
  }
}

void MeterResource::Unpublish(MeterId id, bool &prepaid,
                              dataplane::Retirer &later) {
  if (!prepaid) {
    PrepayErase();  // a retry: the refused attempt gave its prepay back
    prepaid = true;
  }
  if (!retiring_.contains(id)) {
    RefundErase();
    return;  // re-added: the published generation is already the new meter
  }

  // The edit is paid from this erase's prepay (Growth::Grow), and what it did
  // not use -- or gave back, if it failed -- is released after. Its DPDK
  // memory can still be refused (on normal pages each allocation is checked
  // against the limit, D-198): then nothing changed but the prepay, given
  // back, and the engine runs this stage again.
  const size_t had = growth_.prepaid_bytes;
  auto settle = [&]() noexcept {
    growth_.paying = false;
    growth_.Refund(MeterSet::Bytes(live_->capacity()) -
                   (had - growth_.prepaid_bytes));
  };
  growth_.paying = true;
  std::unique_ptr<const MeterSet> generation;
  try {
    if (live_->Contains(id)) {  // a retry after Build was refused: erased
      auto erased = live_->Erase(id);
      CHECK(erased) << MeterErrorName(erased.error());
    }
    generation = live_->Build();
  } catch (...) {
    settle();
    prepaid = false;
    throw;
  }
  settle();
  retiring_.erase(id);
  policies_.erase(id);
  later.Retire(published_.Exchange(std::move(generation)));
}

void MeterResource::PrepayErase() {
  growth_.Prepay(MeterSet::Bytes(live_->capacity()));
}

void MeterResource::RefundErase() noexcept {
  growth_.Refund(MeterSet::Bytes(live_->capacity()));
}

void MeterResource::Growth::Grow(size_t bytes, size_t glibc_bytes) {
  if (!paying) {
    lease_->Grow(bytes, glibc_bytes);
  } else {
    // Already admitted by the erase's Reserve; MeterSet::Bytes bounds what
    // one edit of the builder admits, all of it index nodes.
    CHECK(bytes <= prepaid_bytes && glibc_bytes == 0)
        << "meter removal exceeds its prepaid admission";
    prepaid_bytes -= bytes;
  }
  held_->fetch_add(bytes, std::memory_order_relaxed);
}

void MeterResource::Growth::Shrink(size_t bytes, size_t glibc_bytes) noexcept {
  held_->fetch_sub(bytes, std::memory_order_relaxed);
  lease_->Shrink(bytes, glibc_bytes);
}

void MeterResource::Growth::Prepay(size_t bytes) {
  lease_->Grow(bytes, 0);
  prepaid_bytes += bytes;
}

void MeterResource::Growth::Refund(size_t bytes) noexcept {
  prepaid_bytes -= bytes;
  lease_->Shrink(bytes, 0);
}

void MeterResource::ControlGrowth::Grow(size_t bytes, size_t glibc_bytes) {
  lease_->Grow(bytes, glibc_bytes);
  held_->fetch_add(bytes, std::memory_order_relaxed);
}

void MeterResource::ControlGrowth::Shrink(size_t bytes,
                                          size_t glibc_bytes) noexcept {
  held_->fetch_sub(bytes, std::memory_order_relaxed);
  lease_->Shrink(bytes, glibc_bytes);
}

}  // namespace bess::meter
