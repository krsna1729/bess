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

#include "meter/meter.h"
#include "utils/common.h"

namespace bess::meter {

class MeterSetBuilder;
class MeterStateSlab;

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
    std::array<MeterState *, kMaxBatch> meters;
    Resolve(ids, meters);
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
    Resolve(ids, meters);
    uint64_t resolved = 0;
    for (size_t i = 0; i < ids.size(); i++) {
      if (MeterState *meter = meters[i]) {
        colors[i] = meter->CheckColorAware(now, bytes[i], colors[i]);
        resolved |= uint64_t{1} << i;
      }
    }
    return resolved;
  }

  size_t size() const noexcept { return owners_.size(); }
  size_t capacity() const noexcept { return states_.size() - 1; }

  // Distinct profiles referenced by this generation (control-side
  // introspection: identical specifications share one profile).
  size_t profile_count() const noexcept { return profiles_.size(); }

 private:
  friend class MeterSetBuilder;

  // First pass of a batch: resolve every id and prefetch every state line, so
  // the checks that follow overlap their cache misses instead of taking them
  // one at a time. Meter state is read-modify-write, hence the write
  // prefetch.
  void Resolve(std::span<const MeterId> ids,
               std::array<MeterState *, kMaxBatch> &meters) const noexcept {
    for (size_t i = 0; i < ids.size(); i++) {
      meters[i] = Lookup(ids[i]);
      if (meters[i] != nullptr) {
        __builtin_prefetch(meters[i], 1);
      }
    }
  }

  MeterSet(std::vector<MeterState *> states,
           std::vector<std::shared_ptr<MeterState>> owners,
           std::vector<std::shared_ptr<const MeterProfile>> profiles)
      : states_(std::move(states)),
        owners_(std::move(owners)),
        profiles_(std::move(profiles)) {}

  // Slot 0 is the invalid id and always nullptr.
  std::vector<MeterState *> states_;

  // Ownership only; never read on the packet path.
  std::vector<std::shared_ptr<MeterState>> owners_;
  std::vector<std::shared_ptr<const MeterProfile>> profiles_;
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
// States are carved from per-socket slabs of cache-line slots rather than
// allocated one by one, so a set's states are dense in memory. A slot returns
// to its slab only when the last generation referencing its state is
// destroyed, so RCU reclamation -- not the builder -- decides when a slot can
// be reused. Slabs grow in fixed chunks and never move.
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

  // Creates meter `id` with full buckets. `socket` places its state; an
  // exclusive meter belongs on the socket of the worker that will run it.
  std::expected<void, MeterError> Add(MeterId id, const MeterProfileSpec &spec,
                                      MeterSharing sharing,
                                      int socket = SOCKET_ID_ANY);

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

 private:
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

  std::vector<std::optional<Entry>> entries_;
  size_t size_ = 0;

  // Profiles are interned by specification so identical meters share one
  // compiled profile. Weak: a profile lives as long as some entry or
  // generation uses it.
  std::map<ProfileKey, std::weak_ptr<const MeterProfile>> profiles_;

  // One slab per requested socket. Shared with every state allocated from it,
  // so a slab outlives the builder while any generation still holds a state.
  std::map<int, std::shared_ptr<MeterStateSlab>> slabs_;
};

}  // namespace bess::meter

#endif  // BESS_METER_METER_SET_H_
