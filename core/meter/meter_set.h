// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_METER_METER_SET_H_
#define BESS_METER_METER_SET_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <span>

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
// Interned profiles, shared by a builder and its clones (meter_set.cc).
struct MeterProfiles;

// The id index (D-199): a radix tree, 64 ids per leaf and 256 children per
// inner node, as deep as the capacity needs (MeterLevels). Its nodes are
// immutable once a generation or a second builder can reach them, and shared:
// a change copies the nodes on its id's path and nothing else, so a new
// generation costs O(depth) nodes per changed id, not O(capacity). Nodes are
// packet-path memory (DPDK, D-029), each admitted through the family's hook
// (D-198) and given back when its last holder -- a builder, a generation or a
// parent node -- lets go.
//
// The fanout is bounded on purpose: copying a node takes a reference to each
// of its children, one atomic on each child's header line, and every edit of
// a shared path (each erase's removal stage, the first edit of a
// transaction) copies the root. A capacity-sized root would make that
// O(capacity / 64) scattered cache misses per edit. The packet path hides the
// depth instead (MeterSet::Resolve).
inline constexpr unsigned kMeterLeafBits = 6;
inline constexpr unsigned kMeterInnerBits = 8;
inline constexpr size_t kMeterLeafSlots = size_t{1} << kMeterLeafBits;
inline constexpr size_t kMeterInnerSlots = size_t{1} << kMeterInnerBits;
// MeterId is 32 bits: a leaf and three inner levels cover 2^30 ids, four 2^38.
inline constexpr unsigned kMeterMaxLevels = 4;

// Inner levels above the leaves for ids 0..capacity.
constexpr unsigned MeterLevels(size_t capacity) noexcept {
  unsigned levels = 0;
  for (size_t span = kMeterLeafSlots;
       span <= capacity && levels < kMeterMaxLevels; span <<= kMeterInnerBits) {
    levels++;
  }
  return levels;
}

// The child slot of `index` in an inner node `level` levels above the leaves.
constexpr size_t MeterDigit(size_t index, unsigned level) noexcept {
  return (index >> (kMeterLeafBits + kMeterInnerBits * (level - 1))) &
         (kMeterInnerSlots - 1);
}

struct MeterNode {
  std::atomic<uint32_t> refs{1};  // builders, generations and parents
  uint32_t used = 0;              // non-empty slots
  uint64_t owner = 0;  // the builder edit token that may write it in place
};

struct MeterInner : MeterNode {
  MeterNode *children[kMeterInnerSlots] = {};
};

// One meter as the builder wants it; empty when `state` is null.
struct MeterEntry {
  std::shared_ptr<const MeterProfile> profile;
  std::shared_ptr<MeterState> state;
  MeterSharing sharing = MeterSharing::kShared;
  int socket = 0;
};

// One leaf allocation holds the header line and the 64 state pointers on
// their own eight cache lines, so the packet path's slots are dense (576 B per
// 64 ids); the control-side entries are a separate allocation.
struct alignas(64) MeterLeaf : MeterNode {
  MeterEntry *entries = nullptr;  // kMeterLeafSlots of them
  // The packet path reads only these: states[i] is entries[i].state.get().
  alignas(64) MeterState *states[kMeterLeafSlots] = {};
};

// What one leaf and one inner node allocate (DPDK).
inline constexpr size_t kMeterLeafBytes =
    sizeof(MeterLeaf) + kMeterLeafSlots * sizeof(MeterEntry);
inline constexpr size_t kMeterInnerBytes = sizeof(MeterInner);
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
// an update keeps the same MeterState, and the index nodes that map it hold an
// owning reference to it (and its profile). A state is released only once no
// generation or builder reaches it -- after the last generation that did has
// been retired past its grace period and destroyed on the control thread that
// reclaims it. That is the whole lifetime rule; the packet path sees raw
// pointers and never touches a reference count.
//
// The packet-facing lookup is a radix tree of state pointers indexed by id
// (detail::MeterLeaf), with nullptr for an empty id (one-based ids, as in
// K2's ObjectTable). Its levels are a few dependent loads of small, hot
// nodes, then one pointer load in a leaf; a MeterState carries its profile,
// so that pointer is the last load before the state's own cache line. A batch
// resolves stage-major (Resolve), so its walks overlap rather than queue
// behind each meter's check. The tree is what keeps a generation's cost to
// what changed (D-199): a new generation shares every node its predecessor
// had off the changed paths.
class MeterSet {
 public:
  // Largest batch CheckBatch() accepts: the result is a 64-bit mask.
  static constexpr size_t kMaxBatch = 64;

  MeterSet(const MeterSet &) = delete;
  MeterSet &operator=(const MeterSet &) = delete;

  // The meter for `id`, or nullptr for an invalid, out-of-range or erased id.
  // Valid for as long as this generation is.
  MeterState *Lookup(MeterId id) const noexcept {
    const detail::MeterLeaf *leaf = LeafOf(id);
    return leaf != nullptr ? leaf->states[SlotOf(id)] : nullptr;
  }

  // Lookup() for a batch, stage-major: every id's walk down to its leaf,
  // prefetching the leaf's slot; then every slot, write-prefetching the
  // meter's state line. The walks of a batch overlap one another, and its
  // slot and state misses overlap too, where a check per lookup would queue
  // each walk behind the previous check's lock (K4.6). Writes meters[i] =
  // Lookup(ids[i]). Spans of equal size, at most kMaxBatch (caller
  // preconditions).
  void Resolve(std::span<const MeterId> ids,
               std::span<MeterState *> meters) const noexcept {
    promise(ids.size() == meters.size());
    promise(ids.size() <= kMaxBatch);
    std::array<const detail::MeterLeaf *, kMaxBatch> leaves;
    dataplane::RunStages(
        ids.size(),
        [&](size_t i) {
          leaves[i] = LeafOf(ids[i]);
          if (leaves[i] != nullptr) {
            dataplane::Prefetch(static_cast<const void *>(
                &leaves[i]->states[SlotOf(ids[i])]));
          }
        },
        [&](size_t i) {
          MeterState *meter = leaves[i] != nullptr
                                  ? leaves[i]->states[SlotOf(ids[i])]
                                  : nullptr;
          meters[i] = meter;
          if (meter != nullptr) {
            dataplane::Prefetch<dataplane::PrefetchIntent::kWrite>(meter);
          }
        });
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
    // Stage-major: resolve the batch (its misses overlap), then check.
    std::array<MeterState *, kMaxBatch> meters;
    Resolve(ids, std::span<MeterState *>(meters.data(), ids.size()));
    uint64_t resolved = 0;
    for (size_t i = 0; i < ids.size(); i++) {
      if (MeterState *meter = meters[i]) {
        colors[i] = meter->Check(now, bytes[i]);
        resolved |= uint64_t{1} << i;
      }
    }
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
    std::array<MeterState *, kMaxBatch> meters;
    Resolve(ids, std::span<MeterState *>(meters.data(), ids.size()));
    uint64_t resolved = 0;
    for (size_t i = 0; i < ids.size(); i++) {
      if (MeterState *meter = meters[i]) {
        colors[i] = meter->CheckColorAware(now, bytes[i], colors[i]);
        resolved |= uint64_t{1} << i;
      }
    }
    return resolved;
  }

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return capacity_; }

  // Memory (D-198): a generation holds index nodes, shared with the builder
  // and the generations before and after it; each node is admitted once,
  // when an edit makes it, and given back when its last holder goes. Build
  // itself admits nothing. Bytes(capacity) is the most one edit of one meter
  // (Add, Reconfigure, Reset, Erase) of a MeterSetBuilder(capacity) admits
  // for the index: a copy of every node on the id's path, the next
  // generation's cost over the one before it. Bytes is all DPDK memory.
  static size_t Bytes(size_t capacity) {
    return detail::MeterLevels(capacity) * detail::kMeterInnerBytes +
           detail::kMeterLeafBytes;
  }
  ~MeterSet();

 private:
  friend class MeterSetBuilder;

  static size_t SlotOf(MeterId id) noexcept {
    return static_cast<size_t>(id.value()) & (detail::kMeterLeafSlots - 1);
  }
  // The leaf mapping `id`, or nullptr for an out-of-range id or an empty
  // subtree.
  const detail::MeterLeaf *LeafOf(MeterId id) const noexcept {
    const size_t index = static_cast<size_t>(id.value());
    if (index > capacity_) {
      return nullptr;
    }
    const detail::MeterNode *node = root_;
    for (unsigned level = levels_; level != 0; level--) {
      node = static_cast<const detail::MeterInner *>(node)
                 ->children[detail::MeterDigit(index, level)];
      if (node == nullptr) {
        return nullptr;
      }
    }
    return static_cast<const detail::MeterLeaf *>(node);
  }

  // Takes a reference to `root`.
  MeterSet(detail::MeterNode *root, size_t capacity, size_t meters,
           unsigned levels,
           std::shared_ptr<detail::MeterMemory> memory) noexcept;

  // Never null; slot 0 of the first leaf is the invalid id and always empty.
  detail::MeterNode *root_;
  size_t capacity_;
  size_t size_;
  unsigned levels_;
  std::shared_ptr<detail::MeterMemory> memory_;  // gives the nodes back
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

  // Every edit copies the index nodes on `id`'s path that a generation or
  // another builder can still reach, so Erase, like the others, may throw
  // what the growth hook or DpdkAllocate throws (utils::MemoryRefused).
  std::expected<void, MeterError> Erase(MeterId id);

  bool Contains(MeterId id) const noexcept { return Find(id) != nullptr; }
  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return capacity_; }

  // Snapshots the current desired state as an immutable generation: O(1),
  // the generation shares the index with this builder, and the next edit of
  // a meter copies only that meter's path (MeterSet::Bytes).
  std::unique_ptr<const MeterSet> Build() const;
  // A transaction-private edit copy: O(1), sharing the index, the meter
  // states, profiles and slabs with this builder and its generations. Either
  // side's later edits copy what they change, so neither sees the other's.
  // Only the control plane may call this.
  std::unique_ptr<MeterSetBuilder> Clone() const;

  // Memory (D-198). The constructor allocates the index's root node on the
  // DPDK heap: Bytes(capacity). What grows on the control path afterwards
  // is admitted through set_memory_growth's hook before it is allocated and
  // given back when it is freed: index nodes an edit makes (DPDK; at most
  // MeterSet::Bytes(capacity) per edit), meter-state slab chunks (DPDK;
  // utils::ObjectSlab), each profile with its shared_ptr control block
  // (DPDK: workers read it, D-206), and each state's shared_ptr control
  // block and interned-profile map node (glibc, as malloc's chunks:
  // utils::MallocChunkBytes). Clones and generations share the hook; it
  // stays attached while any builder sharing it lives, so the hook must
  // outlive the builder and its clones (a Sized<MeterSetBuilder> does).
  static size_t Bytes(size_t capacity) {
    return detail::MeterLevels(capacity) == 0 ? detail::kMeterLeafBytes
                                               : detail::kMeterInnerBytes;
  }
  // Held now by this builder, its clones and their generations together
  // (they share index nodes): the index nodes, this builder's slabs' chunks,
  // the profiles, control blocks and interned map nodes still live.
  size_t bytes() const noexcept;
  void set_memory_growth(bess::utils::MemoryGrowth *growth) noexcept;

 private:
  struct CloneTag {};
  MeterSetBuilder(CloneTag, const MeterSetBuilder &other);

  using Entry = detail::MeterEntry;
  using ProfileKey = std::array<uint64_t, 5>;

  static ProfileKey KeyOf(const MeterProfileSpec &spec);

  const Entry *Find(MeterId id) const noexcept;
  // The leaf holding `index`, which this builder may write: the nodes on the
  // path it does not own are copied (and missing ones created), all
  // allocated before any is linked, so a refusal leaves the index as it was.
  detail::MeterLeaf *WritableLeaf(size_t index);
  // Unlinks the now-empty nodes on `index`'s path (below the root), which
  // WritableLeaf has just made this builder's own.
  void Prune(size_t index) noexcept;

  // Returns the shared profile for `spec`, compiling it on first use.
  std::expected<std::shared_ptr<const MeterProfile>, MeterError> InternProfile(
      const MeterProfileSpec &spec);

  std::expected<std::shared_ptr<MeterState>, MeterError> NewState(
      const std::shared_ptr<const MeterProfile> &profile, MeterSharing sharing,
      int socket);

  size_t capacity_;
  unsigned levels_;
  size_t size_ = 0;
  // Nodes stamped with token_ were made by this builder since it last shared
  // its index (Build, Clone) and are written in place; any other node is
  // copied first. Build and Clone are const but share the index, hence
  // mutable: a fresh token freezes every node either side can now reach.
  mutable uint64_t token_;

  // Shared with clones, generations, slabs and states (their allocator).
  std::shared_ptr<detail::MeterMemory> memory_;

  // Profiles are interned by specification so identical meters share one
  // compiled profile, across this builder and its clones. Weak: a profile
  // lives as long as some entry uses it.
  std::shared_ptr<detail::MeterProfiles> profiles_;

  // One slab per requested socket. Shared with every state allocated from it,
  // so a slab outlives the builder while any generation still holds a state.
  std::map<int, std::shared_ptr<bess::utils::ObjectSlab<MeterState>>> slabs_;

  // Made last in the constructor, so nothing after it can throw and leak it.
  detail::MeterNode *root_;  // one reference; never null
};

}  // namespace bess::meter

#endif  // BESS_METER_METER_SET_H_
