// SPDX-License-Identifier: BSD-3-Clause

#include "meter/meter_set.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <type_traits>
#include <utility>
#include <variant>

namespace bess::meter {

namespace detail {

// The growth hook a builder, its clones, and the generations and states they
// make share (D-198). Generations and states are freed after a grace period,
// on whatever control thread reclaims, possibly after the builder that made
// them: each holds this, not the hook, and gives its bytes back through it.
// The hook stays attached while any builder sharing it lives (the owner's
// admission lives at least that long); once the last one is gone the owner
// has released everything, and what is freed later gives nothing back.
class MeterMemory final : public utils::MemoryGrowth {
 public:
  void Grow(size_t bytes, size_t glibc_bytes = 0) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hook_ != nullptr) {
      hook_->Grow(bytes, glibc_bytes);
    }
  }
  void Shrink(size_t bytes, size_t glibc_bytes = 0) noexcept override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (hook_ != nullptr) {
      hook_->Shrink(bytes, glibc_bytes);
    }
  }
  void Attach(utils::MemoryGrowth *hook) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    hook_ = hook;
  }
  // A builder sharing this appeared / went away; the last one detaches.
  void Join() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    builders_++;
  }
  void Leave() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (--builders_ == 0) {
      hook_ = nullptr;
    }
  }

  // Glibc bytes AdmittedAllocator holds now (state control blocks,
  // profiles).
  std::atomic<size_t> glibc_held{0};

 private:
  std::mutex mutex_;
  utils::MemoryGrowth *hook_ = nullptr;  // guarded by mutex_
  size_t builders_ = 0;
};

namespace {

// Allocates the standard library's part of a state's or profile's shared
// ownership (a control block per meter state; a profile with its block),
// glibc and one per meter, admitted through `memory` first and given back
// when it is freed (D-198).
template <typename T>
struct AdmittedAllocator {
  using value_type = T;

  explicit AdmittedAllocator(std::shared_ptr<MeterMemory> m) noexcept
      : memory(std::move(m)) {}
  template <typename U>
  AdmittedAllocator(const AdmittedAllocator<U> &o) noexcept
      : memory(o.memory) {}

  T *allocate(size_t n) {
    const size_t bytes = n * sizeof(T);
    memory->Grow(bytes, bytes);
    T *p;
    try {
      p = std::allocator<T>().allocate(n);
    } catch (...) {
      memory->Shrink(bytes, bytes);
      throw;
    }
    memory->glibc_held.fetch_add(bytes, std::memory_order_relaxed);
    return p;
  }
  void deallocate(T *p, size_t n) noexcept {
    std::allocator<T>().deallocate(p, n);
    const size_t bytes = n * sizeof(T);
    memory->glibc_held.fetch_sub(bytes, std::memory_order_relaxed);
    memory->Shrink(bytes, bytes);
  }
  template <typename U>
  bool operator==(const AdmittedAllocator<U> &o) const noexcept {
    return memory == o.memory;
  }

  std::shared_ptr<MeterMemory> memory;
};

}  // namespace

}  // namespace detail

MeterSet::~MeterSet() {
  if (memory_ != nullptr) {
    memory_->Shrink(bytes(), bytes() - states_.capacity() * sizeof(MeterState *));
  }
}

size_t MeterSet::bytes() const noexcept {
  return states_.capacity() * sizeof(MeterState *) +
         owners_.capacity() * sizeof(owners_[0]) +
         profiles_.capacity() * sizeof(profiles_[0]);
}

MeterSetBuilder::MeterSetBuilder(size_t capacity)
    : entries_(capacity + 1),
      memory_(std::make_shared<detail::MeterMemory>()) {
  memory_->Join();
}

MeterSetBuilder::~MeterSetBuilder() {
  // This builder's entries go now (a clone's were admitted by Clone, the
  // first builder's by its owner); the hook stays with any clone left.
  memory_->Shrink(entries_.capacity() * sizeof(std::optional<Entry>));
  memory_->Leave();
}

MeterSetBuilder::MeterSetBuilder(CloneTag, const MeterSetBuilder &other)
    : entries_(other.entries_),
      size_(other.size_),
      profiles_(other.profiles_),
      slabs_(other.slabs_),
      memory_(other.memory_) {
  memory_->Join();
}

size_t MeterSetBuilder::Bytes(size_t capacity) {
  return (capacity + 1) * sizeof(std::optional<Entry>);
}

size_t MeterSetBuilder::bytes() const noexcept {
  size_t total = entries_.capacity() * sizeof(std::optional<Entry>) +
                 memory_->glibc_held.load(std::memory_order_relaxed);
  for (const auto &[socket, slab] : slabs_) {
    total += slab->bytes();
  }
  return total;
}

void MeterSetBuilder::set_memory_growth(
    bess::utils::MemoryGrowth *growth) noexcept {
  memory_->Attach(growth);
}

std::unique_ptr<MeterSetBuilder> MeterSetBuilder::Clone() const {
  // The copy's entries are admitted first; its destructor gives them back.
  const size_t bytes = entries_.size() * sizeof(std::optional<Entry>);
  memory_->Grow(bytes);
  try {
    return std::unique_ptr<MeterSetBuilder>(
        new MeterSetBuilder(CloneTag{}, *this));
  } catch (...) {
    memory_->Shrink(bytes);
    throw;
  }
}

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
  auto profile = std::allocate_shared<const MeterProfile>(
      detail::AdmittedAllocator<MeterProfile>(memory_),
      std::move(compiled).value());

  // Drop interned entries whose profile is gone, so the map tracks live
  // profiles rather than every specification ever seen.
  std::erase_if(profiles_, [](const auto &p) { return p.second.expired(); });
  profiles_[key] = profile;
  return profile;
}

std::expected<std::shared_ptr<MeterState>, MeterError>
MeterSetBuilder::NewState(const std::shared_ptr<const MeterProfile> &profile,
                          MeterSharing sharing, int socket) {
  auto &slab = slabs_[socket];
  if (slab == nullptr) {
    slab = std::shared_ptr<bess::utils::ObjectSlab<MeterState>>(
        bess::utils::ObjectSlab<MeterState>::Create(socket));
    // Each chunk admitted through the shared hook (D-198).
    slab->set_memory_growth(memory_.get());
  }
  void *slot = slab->Allocate();
  auto state = MeterState::ConstructAt(slot, *profile, sharing);
  if (!state) {
    slab->Free(slot);
    return std::unexpected(state.error());
  }
  // Run wherever the last generation holding the state is destroyed --
  // normally the control thread that reclaims RCU retirements; the slab
  // locks for that. The control block is admitted (AdmittedAllocator); a
  // refusal runs the deleter, so the slot goes back.
  return std::shared_ptr<MeterState>(
      *state,
      [slab](MeterState *s) {
        s->~MeterState();  // private: this builder is a friend, the slab is not
        slab->Free(s);
      },
      detail::AdmittedAllocator<MeterState>(memory_));
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
  // The generation is admitted before it is allocated: the state index
  // (DPDK) and one owner and up to one profile reference per meter (glibc,
  // reserved exactly). It gives them back when it is destroyed (D-198).
  // MeterSet::GlibcBytes(size_) is the references of size_ meters; the
  // index is capacity + 1 pointers whatever the occupancy.
  const size_t glibc = MeterSet::GlibcBytes(size_);
  const size_t bytes = entries_.size() * sizeof(MeterState *) + glibc;
  memory_->Grow(bytes, glibc);
  try {
    MeterSet::States states(entries_.size(), nullptr);
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

    return std::unique_ptr<const MeterSet>(new MeterSet(
        std::move(states), std::move(owners), std::move(profiles), memory_));
  } catch (...) {
    memory_->Shrink(bytes, glibc);
    throw;
  }
}

}  // namespace bess::meter
