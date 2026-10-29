// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_METER_METER_SET_H_
#define BESS_METER_METER_SET_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <vector>

#include "dataplane/batch_stages.h"
#include "meter/meter.h"
#include "utils/common.h"
#include "utils/dpdk_memory.h"
#include "utils/dpdk_slab.h"

namespace bess::meter {

class MeterSetBuilder;

namespace detail {
// The growth hook a builder, its clones, and the generations and states they
// make share (D-198; meter_set.cc). The definition is final over
// utils::MemoryGrowth, whose destructor is protected; clang-tidy 19 checks
// this declaration, which cannot say `final`.
// NOLINTNEXTLINE(cppcoreguidelines-virtual-class-destructor)
class MeterMemory;
}  // namespace detail

// One published generation of meters (K5):
//
//   MeterId -> MeterState *
//
// A generation is immutable and is published like any other: RcuPtr<MeterSet>
// on the runtime's RcuDomain. What makes meters different from other table
// objects is that the *state* behind an id is mutable and must outlive any one
// generation -- publishing a new set because some other meter changed must not
// refill every bucket.
//
// So state is shared between generations, not copied: a meter that survives
// an update keeps the same MeterState, and each generation holds an owning
// reference to every state (and profile) it maps. A state is released only
// once no generation references it -- after the last generation that did has
// been retired past its grace period and destroyed on the control thread that
// reclaims it. That is the whole lifetime rule; the packet path sees raw
// pointers and never touches a reference count.
//
// The packet-facing lookup is a dense array of state pointers indexed by id,
// with nullptr for an empty id (one-based ids, as in K2's ObjectTable; a
// nullable pointer needs no separate validity flag). A MeterState carries its
// profile, so resolving a meter is one pointer load before the state's own
// cache line.
class MeterSet {
 public:
  // Largest batch CheckBatch() accepts: the result is a 64-bit mask.
  static constexpr size_t kMaxBatch = 64;

  MeterSet(const MeterSet &) = delete;
  MeterSet &operator=(const MeterSet &) = delete;

  // The meter for `id`, or nullptr for an invalid, out-of-range or erased id.
  // Valid for as long as this generation is.
  MeterState *Lookup(MeterId id) const noexcept {
    const size_t index = static_cast<size_t>(id.value());
    return index < states_.size() ? states_[index] : nullptr;
  }

  // Colour-blind checks for a batch at one TSC time. Position i meters
  // `bytes[i]` against `ids[i]` and writes `colors[i]`. Returns a mask with bit
  // i set where `ids[i]` resolved; unresolved positions leave `colors[i]`
  // untouched, and what an unmetered packet means is the caller's decision.
  // Spans have equal sizes of at most kMaxBatch (caller preconditions).
  uint64_t CheckBatch(std::span<const MeterId> ids,
                      std::span<const uint32_t> bytes,
                      std::span<MeterColor> colors,
                      uint64_t now) const noexcept {
    promise(ids.size() == bytes.size() && ids.size() == colors.size());
    promise(ids.size() <= kMaxBatch);
    // Stage-major: resolve every id and write-prefetch its state line, then
    // run the checks, so a batch's state misses overlap (K5/K4.6).
    std::array<MeterState *, kMaxBatch> meters;
    uint64_t resolved = 0;
    dataplane::RunStages(
        ids.size(),
        [&](size_t i) {
          meters[i] = Lookup(ids[i]);
          if (meters[i] != nullptr) {
            dataplane::Prefetch<dataplane::PrefetchIntent::kWrite>(meters[i]);
          }
        },
        [&](size_t i) {
          if (MeterState *meter = meters[i]) {
            colors[i] = meter->Check(now, bytes[i]);
            resolved |= uint64_t{1} << i;
          }
        });
    return resolved;
  }

  // Colour-aware variant: `colors` carries the input colours in and the
  // results out. Same mask and precondition rules as CheckBatch().
  uint64_t CheckBatchColorAware(std::span<const MeterId> ids,
                                std::span<const uint32_t> bytes,
                                std::span<MeterColor> colors,
                                uint64_t now) const noexcept {
    promise(ids.size() == bytes.size() && ids.size() == colors.size());
    promise(ids.size() <= kMaxBatch);
    // Stage-major: resolve every id and write-prefetch its state line, then
    // run the checks, so a batch's state misses overlap (K5/K4.6).
    std::array<MeterState *, kMaxBatch> meters;
    uint64_t resolved = 0;
    dataplane::RunStages(
        ids.size(),
        [&](size_t i) {
          meters[i] = Lookup(ids[i]);
          if (meters[i] != nullptr) {
            dataplane::Prefetch<dataplane::PrefetchIntent::kWrite>(meters[i]);
          }
        },
        [&](size_t i) {
          if (MeterState *meter = meters[i]) {
            colors[i] = meter->CheckColorAware(now, bytes[i], colors[i]);
            resolved |= uint64_t{1} << i;
          }
        });
    return resolved;
  }

  size_t size() const noexcept { return owners_.size(); }
  size_t capacity() const noexcept { return states_.size() - 1; }

  // Distinct profiles referenced by this generation (control-side
  // introspection: identical specifications share one profile).
  size_t profile_count() const noexcept { return profiles_.size(); }

  // Memory (D-198): the state index (DPDK) and the owning references
  // (glibc), admitted by Build through the builder's growth hook and given
  // back when the generation is destroyed (after its grace period).
  // Bytes(capacity) is the most one generation of a MeterSetBuilder
  // (capacity) admits: the index (capacity + 1 pointers) and, with every id
  // in use, one owner and one profile reference per meter (GlibcBytes); a
  // generation of fewer meters admits less.
  static size_t Bytes(size_t capacity) {
    return (capacity + 1) * sizeof(MeterState *) + GlibcBytes(capacity);
  }
  static size_t GlibcBytes(size_t capacity) {
    return capacity * (sizeof(std::shared_ptr<MeterState>) +
                       sizeof(std::shared_ptr<const MeterProfile>));
  }
  size_t bytes() const noexcept;
  ~MeterSet();

 private:
  friend class MeterSetBuilder;

  // Packet-path memory: DPDK's heap on the memory node (D-029, D-123).
  using States =
      std::vector<MeterState *, bess::utils::DpdkAllocator<MeterState *>>;

  MeterSet(States states, std::vector<std::shared_ptr<MeterState>> owners,
           std::vector<std::shared_ptr<const MeterProfile>> profiles,
           std::shared_ptr<detail::MeterMemory> memory)
      : states_(std::move(states)),
        owners_(std::move(owners)),
        profiles_(std::move(profiles)),
        memory_(std::move(memory)) {}

  // Slot 0 is the invalid id and always nullptr.
  States states_;

  // Ownership only; never read on the packet path.
  std::vector<std::shared_ptr<MeterState>> owners_;
  std::vector<std::shared_ptr<const MeterProfile>> profiles_;
  std::shared_ptr<detail::MeterMemory> memory_;  // gives bytes() back
};

// The control-side desired state of a set of meters. Unlike an
// ObjectTableBuilder it is long-lived: it is edited, a generation is built
// from it, and editing continues. Building never consumes it.
//
// Ids are chosen by the caller, as with every K2 table. Erasing an id and
// adding it again creates a new meter with fresh state; a reader still on an
// older generation keeps updating the old state, which stays alive until that
// generation is reclaimed.
//
// States are carved from per-socket slabs of cache-line slots
// (utils::ObjectSlab) rather than allocated one by one, so a set's states are
// dense in memory. A slot returns to its slab only when the last generation
// referencing its state is destroyed, so RCU reclamation -- not the builder --
// decides when a slot can be reused. Slabs grow in fixed chunks and never
// move.
//
// Writers are the caller's to serialize (one control thread, or the owning
// module's command path), as with any builder. Generation destruction may run
// on another control thread; the slabs lock internally for that.
class MeterSetBuilder {
 public:
  explicit MeterSetBuilder(size_t capacity);
  ~MeterSetBuilder();

  MeterSetBuilder(const MeterSetBuilder &) = delete;
  MeterSetBuilder &operator=(const MeterSetBuilder &) = delete;

  // Creates meter `id` with full buckets. `socket` places its state (a DPDK
  // socket id, by default the memory node).
  std::expected<void, MeterError> Add(MeterId id, const MeterProfileSpec &spec,
                                      MeterSharing sharing,
                                      int socket = bess::utils::MemoryNode());

  // Changes meter `id`'s profile. An identical specification is a no-op that
  // keeps the running state. A different one starts fresh state with full
  // buckets: rte_meter has no rebinding operation, and carrying token counts
  // across would mean reading a state a worker may be updating right now.
  // Sharing and placement are kept.
  std::expected<void, MeterError> Reconfigure(MeterId id,
                                              const MeterProfileSpec &spec);

  // Replaces meter `id`'s state with fresh, full buckets under the same
  // profile.
  std::expected<void, MeterError> Reset(MeterId id);

  std::expected<void, MeterError> Erase(MeterId id);

  bool Contains(MeterId id) const noexcept { return Find(id) != nullptr; }
  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return entries_.size() - 1; }

  // Snapshots the current desired state as an immutable generation. No state
  // is allocated or copied, only references.
  std::unique_ptr<const MeterSet> Build() const;
  // A transaction-private edit copy: desired entries are copied, while
  // unchanged meter states, profiles, and slabs remain shared with published
  // generations. Only the control plane may call this.
  std::unique_ptr<MeterSetBuilder> Clone() const;

  // Memory (D-198). The constructor allocates the desired entries, one per
  // id plus the invalid slot 0, on the DPDK heap: Bytes(capacity). What
  // grows on the control path afterwards is admitted through
  // set_memory_growth's hook before it is allocated and given back when it
  // is freed: meter-state slab chunks (DPDK; utils::ObjectSlab), each
  // state's shared_ptr control block and each profile (glibc), every
  // generation Build makes (its index and references) and every Clone's
  // entries. Clones and generations share the hook; it stays attached while
  // any builder sharing it lives, so the hook must outlive the builder and
  // its clones (a Sized<MeterSetBuilder> does).
  static size_t Bytes(size_t capacity);
  // Held now: this builder's entries, the slabs' chunks and the control
  // blocks and profiles still live (shared with clones and generations).
  size_t bytes() const noexcept;
  void set_memory_growth(bess::utils::MemoryGrowth *growth) noexcept;

 private:
  struct CloneTag {};
  MeterSetBuilder(CloneTag, const MeterSetBuilder &other);

  struct Entry {
    std::shared_ptr<const MeterProfile> profile;
    std::shared_ptr<MeterState> state;
    MeterSharing sharing;
    int socket;
  };

  using ProfileKey = std::array<uint64_t, 5>;

  static ProfileKey KeyOf(const MeterProfileSpec &spec);

  const Entry *Find(MeterId id) const noexcept;
  Entry *Find(MeterId id) noexcept;

  // Returns the shared profile for `spec`, compiling it on first use.
  std::expected<std::shared_ptr<const MeterProfile>, MeterError> InternProfile(
      const MeterProfileSpec &spec);

  std::expected<std::shared_ptr<MeterState>, MeterError> NewState(
      const std::shared_ptr<const MeterProfile> &profile, MeterSharing sharing,
      int socket);

  // Desired entries, one per id: N-scaled, so in DPDK memory too (D-029).
  std::vector<std::optional<Entry>,
              bess::utils::DpdkAllocator<std::optional<Entry>>>
      entries_;
  size_t size_ = 0;

  // Profiles are interned by specification so identical meters share one
  // compiled profile. Weak: a profile lives as long as some entry or
  // generation uses it.
  std::map<ProfileKey, std::weak_ptr<const MeterProfile>> profiles_;

  // One slab per requested socket. Shared with every state allocated from it,
  // so a slab outlives the builder while any generation still holds a state.
  std::map<int, std::shared_ptr<bess::utils::ObjectSlab<MeterState>>> slabs_;

  // Shared with clones, generations, slabs and states (their allocator).
  std::shared_ptr<detail::MeterMemory> memory_;
};

}  // namespace bess::meter

#endif  // BESS_METER_METER_SET_H_
