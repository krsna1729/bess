// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_METER_METER_RESOURCE_H_
#define BESS_METER_METER_RESOURCE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "dataplane/resource.h"
#include "meter/meter.h"
#include "meter/meter_set.h"
#include "rcu/rcu_ptr.h"
#include "utils/dpdk_memory.h"

namespace bess::meter {

// A set of shared meters as a transactional resource (D-199; the `Meter`
// module's "<module>/meters" until D-199 moved it here, D-021, G1.2b).
//
// Key: dataplane::EncodeKey(MeterId), ids 1..capacity. Value: a
// MeterProfileSpec in the op's std::any. Codec-free: the owner binds a codec
// (meter/meter_resource_codec.h has the one for bess.pb.MeterIdKey ->
// bess.pb.MeterPolicyValue).
//
// The desired state is a MeterSetBuilder; a transaction edits a private copy
// of it, Prepare builds one generation, and the first upsert publishes it
// through the owner's RcuPtr<MeterSet>. Nothing is visible until publication,
// and a refused transaction leaves the live builder, published generation
// and every meter's token state untouched. A meter that survives an
// update keeps its buckets (K5). The copy, the generation and the publish
// cost what the transaction changes, not what is installed: the copy and
// the generation share the index with the live builder, and an edit copies
// only its meter's path (MeterSetBuilder).
//
// An erase is SlotTable's two-step removal: the meter is absent to the control
// side at once (Contains, VisitPolicies) and its id cannot be upserted, the
// published generation keeps it readable until the removal cascade's stage
// runs (a referrer's old value may still hand a reader the id), and that stage
// publishes a generation without it. A transaction's erases share one removal
// step (BP-7): it erases every id from the builder, then builds and publishes
// one generation.
//
// Memory (D-198), one resource: the owner admits Bytes(capacity) (the first
// builder's index root) before constructing, and passes the admission's
// growth hook. Through that hook every index node an edit copies, state slab
// chunks, state control blocks and profiles are admitted before they are
// allocated and given back when freed, and so is every node of the policy
// and retiring containers (glibc, class C: D-206). An erase admits its
// removal stage's edit (MeterSet::Bytes(capacity), one id's path) at
// Reserve; a refusal there refuses the erase. The stage's DPDK allocation
// can still be refused (normal pages are checked again, D-198): it then
// changes nothing, gives the prepay back, and its retry prepays again.
// bytes() is what the meters hold now. The usual owner:
//
//   auto lease = memory().LeaseFor<MeterResource>("meters", capacity);
//   auto made = std::make_unique<MeterResource>(name, capacity, published_,
//                                               lease.growth());
//   resource_ = MemoryScope::Adopt(std::move(lease), std::move(made));
//
// The owner keeps the RcuPtr its packet path reads (one acquire load per
// batch, as before) and declares it after the resource, so the published
// generation is freed while the hook still counts it. The constructor
// publishes the first, empty generation; the RcuPtr must be uninitialized.
//
// Control path only: the transaction engine's lock serializes every call,
// except Shrink through the hook, which may come from an RCU free on any
// control thread.
class MeterResource final : public dataplane::Resource {
 public:
  // Throws what DpdkAllocate throws (utils::MemoryRefused) if the index root
  // cannot be allocated; nothing is published then. The first generation
  // admits nothing (it shares the builder's index).
  MeterResource(std::string name, size_t capacity,
                rcu::RcuPtr<MeterSet> &published,
                utils::MemoryGrowth *lease);
  ~MeterResource() override;

  // What the owner admits before constructing (MemoryCost<MeterResource>).
  static size_t Bytes(size_t capacity) {
    return MeterSetBuilder::Bytes(capacity);
  }
  // Held now: Bytes(capacity) plus every growth the hook admitted, less what
  // was given back (prepaid erase stages not yet built are not held).
  size_t bytes() const noexcept {
    return held_.load(std::memory_order_relaxed);
  }

  size_t capacity() const noexcept { return live_->capacity(); }
  // Meters in the committed builder, retiring ones included (they are still
  // published).
  size_t size() const noexcept { return live_->size(); }

  // Erased, and its removal stage has not run: absent to the control side,
  // still readable by packets, not upsertable.
  bool Retiring(MeterId id) const noexcept { return retiring_.contains(id); }
  // Generations published since construction, the first one included: one
  // per transaction that upserts, one per removal step.
  uint64_t generations() const noexcept { return generations_; }

  // Every committed, non-retiring meter with its policy as written, in id
  // order: the live builder's specifications (D-207: no copy of its own).
  // Control path.
  template <typename Visit>
  void VisitPolicies(Visit &&visit) const {
    live_->VisitSpecs([&](MeterId id, const MeterProfileSpec &policy) {
      if (!retiring_.contains(id)) {
        visit(id, policy);
      }
    });
  }

  // The same, from the first id at or above `first`, while `visit` returns
  // true: the walk enters the live builder's index at `first`, so a page
  // costs its length, not the meters before it.
  template <typename Visit>
  void VisitPoliciesFrom(MeterId first, Visit &&visit) const {
    live_->VisitSpecsFrom(first, [&](MeterId id, const MeterProfileSpec &policy) {
      return retiring_.contains(id) || visit(id, policy);
    });
  }

  // dataplane::Resource.
  size_t LiveCount() const override { return live_->size(); }
  bool Contains(const dataplane::ResourceKey &key) const override;
  // A reader may still hold an erased meter's id from a referrer's old value:
  // the erase is deferred to the removal cascade.
  bool DefersErase() const override { return true; }
  std::expected<Reservation, std::string> Reserve(
      const dataplane::Op &op) override;
  std::expected<void, std::string> Prepare() override;
  void EndTransaction() noexcept override;

 private:
  class UpsertOp;
  class EraseOp;

  // The hook the builders get (set on the first; clones and generations
  // share it): forwards to the owner's lease, except while a removal stage
  // edits the index (Unpublish), which draws on what the erase's Reserve
  // prepaid. Prepay/Refund and that edit run under the engine's lock; Shrink
  // may come from any thread.
  class Growth final : public utils::MemoryGrowth {
   public:
    Growth(utils::MemoryGrowth *lease, std::atomic<size_t> *held)
        : lease_(lease), held_(held) {}
    void Grow(size_t bytes, size_t glibc_bytes = 0) override;
    void Shrink(size_t bytes, size_t glibc_bytes = 0) noexcept override;
    void Prepay(size_t bytes);
    void Refund(size_t bytes) noexcept;

    // Admitted by erase reservations whose removal stage has not run yet
    // (index nodes: DPDK memory only).
    size_t prepaid_bytes = 0;
    bool paying = false;  // Unpublish's edit draws on the prepay

   private:
    utils::MemoryGrowth *lease_;
    std::atomic<size_t> *held_;
  };
  // The hook of the policy and retiring containers (class C, D-206): their
  // glibc nodes go to the owner's lease directly, never against an erase's
  // prepay (which is index nodes only).
  class ControlGrowth final : public utils::MemoryGrowth {
   public:
    ControlGrowth(utils::MemoryGrowth *lease, std::atomic<size_t> *held)
        : lease_(lease), held_(held) {}
    void Grow(size_t bytes, size_t glibc_bytes = 0) override;
    void Shrink(size_t bytes, size_t glibc_bytes = 0) noexcept override;

   private:
    utils::MemoryGrowth *lease_;
    std::atomic<size_t> *held_;
  };
  template <typename T>
  using ControlAllocator = utils::AdmittedGlibcAllocator<T>;

  // A transaction edits a private copy of the builder, so a refused one
  // leaves the live builder untouched.
  void BeginTransaction();
  // Swaps the edited copy in and publishes its prepared generation once.
  void PublishStaged(dataplane::Retirer &retirer);
  // A transaction's removal step, one cascade stage after its erases:
  // publishes one generation without `ids` (any re-added meanwhile is
  // skipped). `prepaid`: the step's prepay (one erase's per id) is held; a
  // refused attempt clears it and the retry admits it again.
  using ErasedIds = std::vector<MeterId, ControlAllocator<MeterId>>;
  void Unpublish(const ErasedIds &ids, bool &prepaid,
                 dataplane::Retirer &later);
  // Hands the transaction's erased ids to one removal step; the erases after
  // the first find none.
  void PublishErases(dataplane::Retirer &retirer) noexcept;
  // An erase's removal-stage edit, admitted by Reserve; given back when the
  // stage has made it, the erase aborts, or the stage has nothing to do.
  void PrepayErase();
  void RefundErase() noexcept;

  // Declared before every builder, generation holder and container: they
  // give bytes back through it from their destructors.
  std::atomic<size_t> held_;
  Growth growth_;
  ControlGrowth control_growth_;

  rcu::RcuPtr<MeterSet> &published_;
  std::unique_ptr<MeterSetBuilder> live_;
  // The transaction's edit copy while one is in flight.
  std::unique_ptr<MeterSetBuilder> staged_;
  std::unique_ptr<const MeterSet> staged_generation_;
  // The edit copy has become the live one (its first publish ran).
  bool published_staged_ = false;
  std::set<MeterId, std::less<MeterId>, ControlAllocator<MeterId>> retiring_;
  // The ids this transaction erases, for its one removal step.
  ErasedIds erasing_;
  uint64_t generations_ = 0;
};

}  // namespace bess::meter

#endif  // BESS_METER_METER_RESOURCE_H_
