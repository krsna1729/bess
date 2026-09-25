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

#include "meter/meter_set.h"

#include <algorithm>
#include <mutex>
#include <type_traits>
#include <utility>
#include <variant>

#include <rte_malloc.h>

namespace bess::meter {

// A per-socket pool of cache-line MeterState slots. Chunks are allocated on
// demand and never freed or moved until the slab itself goes, which happens
// only after every state carved from it has been released (each state's
// deleter holds a reference to its slab).
//
// Release is called from wherever the last generation holding a state is
// destroyed -- normally the control thread that reclaims RCU retirements -- so
// the free list is locked. Nothing here runs on the packet path.
class MeterStateSlab {
 public:
  explicit MeterStateSlab(int socket) : socket_(socket) {}

  ~MeterStateSlab() {
    for (void *chunk : chunks_) {
      rte_free(chunk);
    }
  }

  MeterStateSlab(const MeterStateSlab &) = delete;
  MeterStateSlab &operator=(const MeterStateSlab &) = delete;

  void *Acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (free_.empty() && !Grow()) {
      return nullptr;
    }
    void *slot = free_.back();
    free_.pop_back();
    return slot;
  }

  void Release(void *slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    free_.push_back(slot);
  }

 private:
  // 64 KiB per chunk: large enough that a big set is a handful of
  // allocations, small enough that a set of a few meters wastes little.
  static constexpr size_t kSlotsPerChunk = 1024;

  bool Grow() {
    const size_t bytes = kSlotsPerChunk * sizeof(MeterState);
    void *chunk = rte_malloc_socket("bess_meter_slab", bytes,
                                    alignof(MeterState), socket_);
    if (chunk == nullptr && socket_ != SOCKET_ID_ANY) {
      // Placement is a preference; see MeterState::Create().
      chunk = rte_malloc_socket("bess_meter_slab", bytes, alignof(MeterState),
                                SOCKET_ID_ANY);
    }
    if (chunk == nullptr) {
      return false;
    }
    chunks_.push_back(chunk);
    // Pushed in reverse so slots are handed out in address order: meters
    // added together sit next to each other.
    auto *slots = static_cast<MeterState *>(chunk);
    for (size_t i = kSlotsPerChunk; i-- > 0;) {
      free_.push_back(&slots[i]);
    }
    return true;
  }

  const int socket_;
  std::mutex mutex_;
  std::vector<void *> chunks_;
  std::vector<void *> free_;
};

MeterSetBuilder::MeterSetBuilder(size_t capacity) : entries_(capacity + 1) {}

MeterSetBuilder::~MeterSetBuilder() = default;

MeterSetBuilder::ProfileKey MeterSetBuilder::KeyOf(
    const MeterProfileSpec &spec) {
  const auto tag = static_cast<uint64_t>(AlgorithmOf(spec));
  return std::visit(
      [tag](const auto &s) -> ProfileKey {
        using S = std::decay_t<decltype(s)>;
        if constexpr (std::is_same_v<S, SrTcmSpec>) {
          return {tag, s.committed_rate, s.committed_burst, 0, s.excess_burst};
        } else if constexpr (std::is_same_v<S, TrTcmSpec>) {
          return {tag, s.committed_rate, s.committed_burst, s.peak_rate,
                  s.peak_burst};
        } else {
          return {tag, s.committed_rate, s.committed_burst, s.excess_rate,
                  s.excess_burst};
        }
      },
      spec);
}

const MeterSetBuilder::Entry *MeterSetBuilder::Find(
    MeterId id) const noexcept {
  const size_t index = static_cast<size_t>(id.value());
  if (index == 0 || index >= entries_.size() || !entries_[index].has_value()) {
    return nullptr;
  }
  return &*entries_[index];
}

MeterSetBuilder::Entry *MeterSetBuilder::Find(MeterId id) noexcept {
  return const_cast<Entry *>(std::as_const(*this).Find(id));
}

std::expected<std::shared_ptr<const MeterProfile>, MeterError>
MeterSetBuilder::InternProfile(const MeterProfileSpec &spec) {
  const ProfileKey key = KeyOf(spec);
  if (auto it = profiles_.find(key); it != profiles_.end()) {
    if (std::shared_ptr<const MeterProfile> live = it->second.lock()) {
      return live;
    }
  }

  auto compiled = MeterProfile::Create(spec);
  if (!compiled) {
    return std::unexpected(compiled.error());
  }
  auto profile =
      std::make_shared<const MeterProfile>(std::move(compiled).value());

  // Drop interned entries whose profile is gone, so the map tracks live
  // profiles rather than every specification ever seen.
  std::erase_if(profiles_, [](const auto &p) { return p.second.expired(); });
  profiles_[key] = profile;
  return profile;
}

std::expected<std::shared_ptr<MeterState>, MeterError>
MeterSetBuilder::NewState(const std::shared_ptr<const MeterProfile> &profile,
                          MeterSharing sharing, int socket) {
  std::shared_ptr<MeterStateSlab> &slab = slabs_[socket];
  if (slab == nullptr) {
    slab = std::make_shared<MeterStateSlab>(socket);
  }
  void *slot = slab->Acquire();
  if (slot == nullptr) {
    return std::unexpected(MeterError::kOutOfMemory);
  }
  auto state = MeterState::ConstructAt(slot, *profile, sharing);
  if (!state) {
    slab->Release(slot);
    return std::unexpected(state.error());
  }
  return std::shared_ptr<MeterState>(*state, [slab](MeterState *s) {
    s->~MeterState();
    slab->Release(s);
  });
}

std::expected<void, MeterError> MeterSetBuilder::Add(
    MeterId id, const MeterProfileSpec &spec, MeterSharing sharing,
    int socket) {
  const size_t index = static_cast<size_t>(id.value());
  if (index == 0 || index >= entries_.size()) {
    return std::unexpected(MeterError::kInvalidId);
  }
  if (entries_[index].has_value()) {
    return std::unexpected(MeterError::kDuplicateId);
  }

  auto profile = InternProfile(spec);
  if (!profile) {
    return std::unexpected(profile.error());
  }
  auto state = NewState(*profile, sharing, socket);
  if (!state) {
    return std::unexpected(state.error());
  }
  entries_[index].emplace(Entry{std::move(profile).value(),
                                std::move(state).value(), sharing, socket});
  size_++;
  return {};
}

std::expected<void, MeterError> MeterSetBuilder::Reconfigure(
    MeterId id, const MeterProfileSpec &spec) {
  Entry *entry = Find(id);
  if (entry == nullptr) {
    return std::unexpected(MeterError::kUnknownId);
  }
  if (entry->profile->spec() == spec) {
    return {};
  }

  auto profile = InternProfile(spec);
  if (!profile) {
    return std::unexpected(profile.error());
  }
  auto state = NewState(*profile, entry->sharing, entry->socket);
  if (!state) {
    return std::unexpected(state.error());
  }
  // Commit both only once both exist, so a failure leaves the meter as it was.
  entry->profile = std::move(profile).value();
  entry->state = std::move(state).value();
  return {};
}

std::expected<void, MeterError> MeterSetBuilder::Reset(MeterId id) {
  Entry *entry = Find(id);
  if (entry == nullptr) {
    return std::unexpected(MeterError::kUnknownId);
  }
  auto state = NewState(entry->profile, entry->sharing, entry->socket);
  if (!state) {
    return std::unexpected(state.error());
  }
  entry->state = std::move(state).value();
  return {};
}

std::expected<void, MeterError> MeterSetBuilder::Erase(MeterId id) {
  const size_t index = static_cast<size_t>(id.value());
  if (index == 0 || index >= entries_.size()) {
    return std::unexpected(MeterError::kInvalidId);
  }
  if (!entries_[index].has_value()) {
    return std::unexpected(MeterError::kUnknownId);
  }
  entries_[index].reset();
  size_--;
  return {};
}

std::unique_ptr<const MeterSet> MeterSetBuilder::Build() const {
  std::vector<MeterState *> states(entries_.size(), nullptr);
  std::vector<std::shared_ptr<MeterState>> owners;
  std::vector<std::shared_ptr<const MeterProfile>> profiles;
  owners.reserve(size_);
  profiles.reserve(size_);

  for (size_t index = 1; index < entries_.size(); index++) {
    const std::optional<Entry> &entry = entries_[index];
    if (!entry.has_value()) {
      continue;
    }
    states[index] = entry->state.get();
    owners.push_back(entry->state);
    profiles.push_back(entry->profile);
  }

  // One owning reference per distinct profile is enough.
  std::sort(profiles.begin(), profiles.end());
  profiles.erase(std::unique(profiles.begin(), profiles.end()),
                 profiles.end());

  return std::unique_ptr<const MeterSet>(
      new MeterSet(std::move(states), std::move(owners), std::move(profiles)));
}

}  // namespace bess::meter
