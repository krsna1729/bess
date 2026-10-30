// SPDX-License-Identifier: BSD-3-Clause

#include "meter/meter_set.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <mutex>
#include <new>
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

  // Glibc bytes GlibcAllocator holds now (state control blocks, interned
  // map nodes), as malloc's chunks.
  std::atomic<size_t> glibc_held{0};
  // DPDK bytes ProfileAllocator holds now (profiles with their blocks).
  std::atomic<size_t> profile_bytes{0};
  // Index nodes held now by every builder and generation sharing this.
  std::atomic<size_t> node_bytes{0};

 private:
  std::mutex mutex_;
  utils::MemoryGrowth *hook_ = nullptr;  // guarded by mutex_
  size_t builders_ = 0;
};

// Allocates control-plane state no worker reads (D-206, class C): a meter
// state's shared_ptr control block and the interned map's nodes. Glibc, as
// before, charged exactly (utils::MallocBytes, measured on the block) to `memory`
// and given back when it is freed (D-198). Holds `memory`, which a block
// may outlive the builder that made it with.
template <typename T>
struct GlibcAllocator {
  using value_type = T;

  explicit GlibcAllocator(std::shared_ptr<MeterMemory> m) noexcept
      : memory(std::move(m)) {}
  template <typename U>
  GlibcAllocator(const GlibcAllocator<U> &o) noexcept : memory(o.memory) {}

  T *allocate(size_t n) {
    T *p = std::allocator<T>().allocate(n);
    const size_t bytes = utils::MallocBytes(p);
    try {
      memory->Grow(bytes, bytes);
    } catch (...) {
      std::allocator<T>().deallocate(p, n);
      throw;
    }
    memory->glibc_held.fetch_add(bytes, std::memory_order_relaxed);
    return p;
  }
  void deallocate(T *p, size_t n) noexcept {
    const size_t bytes = utils::MallocBytes(p);
    std::allocator<T>().deallocate(p, n);
    memory->glibc_held.fetch_sub(bytes, std::memory_order_relaxed);
    memory->Shrink(bytes, bytes);
  }
  template <typename U>
  bool operator==(const GlibcAllocator<U> &o) const noexcept {
    return memory == o.memory;
  }

  std::shared_ptr<MeterMemory> memory;
};

// Allocates a profile together with its shared_ptr control block: packet-
// path state (D-206, class P: MeterState::RunLocked reads the profile per
// packet), so DPDK memory the framework places, admitted through `memory`
// first (no glibc part) and given back when it is freed.
template <typename T>
struct ProfileAllocator {
  using value_type = T;

  explicit ProfileAllocator(std::shared_ptr<MeterMemory> m) noexcept
      : memory(std::move(m)) {}
  template <typename U>
  ProfileAllocator(const ProfileAllocator<U> &o) noexcept
      : memory(o.memory) {}

  T *allocate(size_t n) {
    const size_t bytes = n * sizeof(T);
    memory->Grow(bytes);
    void *p;
    try {
      p = utils::DpdkAllocate(
          bytes, std::max<size_t>(alignof(T), RTE_CACHE_LINE_SIZE));
    } catch (...) {
      memory->Shrink(bytes);
      throw;
    }
    memory->profile_bytes.fetch_add(bytes, std::memory_order_relaxed);
    return static_cast<T *>(p);
  }
  void deallocate(T *p, size_t n) noexcept {
    utils::DpdkFree(p);
    const size_t bytes = n * sizeof(T);
    memory->profile_bytes.fetch_sub(bytes, std::memory_order_relaxed);
    memory->Shrink(bytes);
  }
  template <typename U>
  bool operator==(const ProfileAllocator<U> &o) const noexcept {
    return memory == o.memory;
  }

  std::shared_ptr<MeterMemory> memory;
};

// Builders only (the caller serializes them), never generations.
struct MeterProfiles {
  // The map size of the first sweep.
  static constexpr size_t kFirstSweep = 64;

  using Key = std::array<uint64_t, 5>;
  using Interned = std::weak_ptr<const MeterProfile>;

  explicit MeterProfiles(std::shared_ptr<MeterMemory> memory)
      : interned(GlibcAllocator<std::pair<const Key, Interned>>(
            std::move(memory))) {}

  // Its nodes admitted (class C, D-206).
  std::map<Key, Interned, std::less<Key>,
           GlibcAllocator<std::pair<const Key, Interned>>>
      interned;
  // Expired entries are dropped when the map reaches this size, which then
  // becomes twice what is left: amortized O(1) per new profile.
  size_t sweep_at = kFirstSweep;
};

namespace {

// Unique per process, so no two builders ever stamp nodes alike.
uint64_t NewToken() noexcept {
  static std::atomic<uint64_t> next{1};
  return next.fetch_add(1, std::memory_order_relaxed);
}

MeterInner *AsInner(MeterNode *node) noexcept {
  return static_cast<MeterInner *>(node);
}
const MeterInner *AsInner(const MeterNode *node) noexcept {
  return static_cast<const MeterInner *>(node);
}
MeterLeaf *AsLeaf(MeterNode *node) noexcept {
  return static_cast<MeterLeaf *>(node);
}
const MeterLeaf *AsLeaf(const MeterNode *node) noexcept {
  return static_cast<const MeterLeaf *>(node);
}

size_t NodeBytes(unsigned level) noexcept {
  return level == 0 ? kMeterLeafBytes : kMeterInnerBytes;
}

// A node `level` levels above the leaves (0: a leaf), stamped `owner`: a copy
// of `from`, whose children or entries it references too, or empty when
// `from` is null. Admitted before it is allocated (D-198); a leaf is two
// allocations, its packet-path slots and its entries.
MeterNode *NewNode(MeterMemory &memory, unsigned level, const MeterNode *from,
                   uint64_t owner) {
  const size_t bytes = NodeBytes(level);
  memory.Grow(bytes);
  void *p = nullptr;
  void *entries = nullptr;
  try {
    const size_t node_bytes =
        level == 0 ? sizeof(MeterLeaf) : sizeof(MeterInner);
    p = utils::DpdkAllocate(node_bytes, RTE_CACHE_LINE_SIZE);
    if (level == 0) {
      entries = utils::DpdkAllocate(kMeterLeafSlots * sizeof(MeterEntry),
                                    RTE_CACHE_LINE_SIZE);
    }
  } catch (...) {
    if (p != nullptr) {
      utils::DpdkFree(p);
    }
    memory.Shrink(bytes);
    throw;
  }
  memory.node_bytes.fetch_add(bytes, std::memory_order_relaxed);
  MeterNode *node;
  if (level == 0) {
    MeterLeaf *leaf = new (p) MeterLeaf();
    auto *first = static_cast<MeterEntry *>(entries);
    if (from != nullptr) {
      const MeterLeaf *source = AsLeaf(from);
      std::copy(std::begin(source->states), std::end(source->states),
                leaf->states);
      std::uninitialized_copy_n(source->entries, kMeterLeafSlots, first);
    } else {
      std::uninitialized_value_construct_n(first, kMeterLeafSlots);
    }
    leaf->entries = first;
    node = leaf;
  } else {
    MeterInner *inner = new (p) MeterInner();
    if (from != nullptr) {
      const MeterInner *source = AsInner(from);
      for (size_t i = 0; i < kMeterInnerSlots; i++) {
        if (MeterNode *child = source->children[i]) {
          child->refs.fetch_add(1, std::memory_order_relaxed);
          inner->children[i] = child;
        }
      }
    }
    node = inner;
  }
  node->used = from != nullptr ? from->used : 0;
  node->owner = owner;
  return node;
}

// Drops one reference to `node`, if any; the last one frees it, its
// references to its children or meters, and its admission.
void Unref(MeterNode *node, unsigned level, MeterMemory &memory) noexcept {
  if (node == nullptr ||
      node->refs.fetch_sub(1, std::memory_order_acq_rel) != 1) {
    return;
  }
  if (level == 0) {
    MeterLeaf *leaf = AsLeaf(node);
    std::destroy_n(leaf->entries, kMeterLeafSlots);
    utils::DpdkFree(leaf->entries);
    leaf->~MeterLeaf();
  } else {
    MeterInner *inner = AsInner(node);
    for (MeterNode *child : inner->children) {
      if (child != nullptr) {
        Unref(child, level - 1, memory);
      }
    }
    inner->~MeterInner();
  }
  utils::DpdkFree(node);
  const size_t bytes = NodeBytes(level);
  memory.node_bytes.fetch_sub(bytes, std::memory_order_relaxed);
  memory.Shrink(bytes);
}

}  // namespace

}  // namespace detail

MeterSet::MeterSet(detail::MeterNode *root, size_t capacity, size_t meters,
                   unsigned levels,
                   std::shared_ptr<detail::MeterMemory> memory) noexcept
    : root_(root),
      capacity_(capacity),
      size_(meters),
      levels_(levels),
      memory_(std::move(memory)) {
  root_->refs.fetch_add(1, std::memory_order_relaxed);
}

MeterSet::~MeterSet() { detail::Unref(root_, levels_, *memory_); }

MeterSetBuilder::MeterSetBuilder(size_t capacity)
    : capacity_(capacity),
      levels_(detail::MeterLevels(capacity)),
      token_(detail::NewToken()),
      memory_(std::make_shared<detail::MeterMemory>()),
      profiles_(std::make_shared<detail::MeterProfiles>(memory_)),
      // No hook yet: the owner admitted Bytes(capacity) for it.
      root_(detail::NewNode(*memory_, levels_, nullptr, token_)) {
  memory_->Join();
}

MeterSetBuilder::~MeterSetBuilder() {
  // While the hook is still attached: what only this builder held goes now.
  detail::Unref(root_, levels_, *memory_);
  memory_->Leave();
}

MeterSetBuilder::MeterSetBuilder(CloneTag, const MeterSetBuilder &other)
    : capacity_(other.capacity_),
      levels_(other.levels_),
      size_(other.size_),
      token_(detail::NewToken()),
      memory_(other.memory_),
      profiles_(other.profiles_),
      slabs_(other.slabs_),
      root_(other.root_) {
  root_->refs.fetch_add(1, std::memory_order_relaxed);
  memory_->Join();
}

size_t MeterSetBuilder::bytes() const noexcept {
  size_t total = memory_->node_bytes.load(std::memory_order_relaxed) +
                 memory_->glibc_held.load(std::memory_order_relaxed) +
                 memory_->profile_bytes.load(std::memory_order_relaxed);
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
  auto clone = std::unique_ptr<MeterSetBuilder>(
      new MeterSetBuilder(CloneTag{}, *this));
  token_ = detail::NewToken();  // the index is shared now
  return clone;
}

std::unique_ptr<const MeterSet> MeterSetBuilder::Build() const {
  auto set = std::unique_ptr<const MeterSet>(
      new MeterSet(root_, capacity_, size_, levels_, memory_));
  token_ = detail::NewToken();  // the index is published now
  return set;
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
  if (index == 0 || index > capacity_) {
    return nullptr;
  }
  const detail::MeterNode *node = root_;
  for (unsigned level = levels_; level != 0; level--) {
    node = detail::AsInner(node)->children[detail::MeterDigit(index, level)];
    if (node == nullptr) {
      return nullptr;
    }
  }
  const Entry &entry =
      detail::AsLeaf(node)->entries[index & (detail::kMeterLeafSlots - 1)];
  return entry.state != nullptr ? &entry : nullptr;
}

detail::MeterLeaf *MeterSetBuilder::WritableLeaf(size_t index) {
  // Depth d: 0 is the root, levels_ the leaf; a node's level is levels_ - d.
  // fresh[d] replaces depth d's node, or is null when this builder writes
  // that node in place.
  std::array<detail::MeterNode *, detail::kMeterMaxLevels + 1> fresh{};
  try {
    detail::MeterNode *node = root_;
    for (unsigned d = 0; d <= levels_; d++) {
      const unsigned level = levels_ - d;
      if (node == nullptr || node->owner != token_) {
        fresh[d] = detail::NewNode(*memory_, level, node, token_);
      }
      if (level != 0 && node != nullptr) {
        node = detail::AsInner(node)
                   ->children[detail::MeterDigit(index, level)];
      }
    }
  } catch (...) {
    for (unsigned d = 0; d <= levels_; d++) {
      if (fresh[d] != nullptr) {
        detail::Unref(fresh[d], levels_ - d, *memory_);
      }
    }
    throw;
  }

  // Link the replacements top-down; nothing below can fail. A replaced
  // node's parent (or root_) gives up its reference, while the copy keeps
  // the node's children alive. The root is never missing.
  if (fresh[0] != nullptr) {
    detail::Unref(root_, levels_, *memory_);
    root_ = fresh[0];
  }
  detail::MeterNode *node = root_;
  for (unsigned level = levels_; level != 0; level--) {
    detail::MeterInner *parent = detail::AsInner(node);
    detail::MeterNode *&child =
        parent->children[detail::MeterDigit(index, level)];
    if (detail::MeterNode *copy = fresh[levels_ - level + 1]) {
      if (child != nullptr) {
        detail::Unref(child, level - 1, *memory_);
      } else {
        parent->used++;
      }
      child = copy;
    }
    node = child;
  }
  return detail::AsLeaf(node);
}

void MeterSetBuilder::Prune(size_t index) noexcept {
  std::array<detail::MeterNode *, detail::kMeterMaxLevels + 1> path{};
  detail::MeterNode *node = root_;
  for (unsigned d = 0; d <= levels_; d++) {
    path[d] = node;
    if (d < levels_) {
      node = detail::AsInner(node)
                 ->children[detail::MeterDigit(index, levels_ - d)];
    }
  }
  for (unsigned d = levels_; d != 0 && path[d]->used == 0; d--) {
    detail::MeterInner *parent = detail::AsInner(path[d - 1]);
    parent->children[detail::MeterDigit(index, levels_ - d + 1)] = nullptr;
    parent->used--;
    detail::Unref(path[d], levels_ - d, *memory_);
  }
}

std::expected<std::shared_ptr<const MeterProfile>, MeterError>
MeterSetBuilder::InternProfile(const MeterProfileSpec &spec) {
  const ProfileKey key = KeyOf(spec);
  auto &interned = profiles_->interned;
  if (auto it = interned.find(key); it != interned.end()) {
    if (std::shared_ptr<const MeterProfile> live = it->second.lock()) {
      return live;
    }
  }

  auto compiled = MeterProfile::Create(spec);
  if (!compiled) {
    return std::unexpected(compiled.error());
  }
  auto profile = std::allocate_shared<const MeterProfile>(
      detail::ProfileAllocator<MeterProfile>(memory_),
      std::move(compiled).value());

  // Drop interned entries whose profile is gone, so the map tracks live
  // profiles rather than every specification ever seen -- once it has
  // doubled, not on every new profile, which made N profiles cost O(N^2).
  if (interned.size() >= profiles_->sweep_at) {
    std::erase_if(interned, [](const auto &p) { return p.second.expired(); });
    profiles_->sweep_at =
        std::max(detail::MeterProfiles::kFirstSweep, 2 * interned.size());
  }
  interned[key] = profile;
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
  // locks for that. The control block is admitted (GlibcAllocator); a
  // refusal runs the deleter, so the slot goes back.
  return std::shared_ptr<MeterState>(
      *state,
      [slab](MeterState *s) {
        s->~MeterState();
        slab->Free(s);
      },
      detail::GlibcAllocator<MeterState>(memory_));
}

std::expected<void, MeterError> MeterSetBuilder::Add(
    MeterId id, const MeterProfileSpec &spec, MeterSharing sharing,
    int socket) {
  const size_t index = static_cast<size_t>(id.value());
  if (index == 0 || index > capacity_) {
    return std::unexpected(MeterError::kInvalidId);
  }
  if (Find(id) != nullptr) {
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
  detail::MeterLeaf *leaf = WritableLeaf(index);
  const size_t slot = index & (detail::kMeterLeafSlots - 1);
  leaf->states[slot] = state->get();
  leaf->entries[slot] = Entry{std::move(profile).value(),
                              std::move(state).value(), sharing, socket};
  leaf->used++;
  size_++;
  return {};
}

std::expected<void, MeterError> MeterSetBuilder::Reconfigure(
    MeterId id, const MeterProfileSpec &spec) {
  const Entry *entry = Find(id);
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
  // Commit both only once both exist, so a failure leaves the meter as it
  // was. `entry` may be in a node the copy below replaces: not used after.
  const size_t index = static_cast<size_t>(id.value());
  detail::MeterLeaf *leaf = WritableLeaf(index);
  const size_t slot = index & (detail::kMeterLeafSlots - 1);
  leaf->states[slot] = state->get();
  leaf->entries[slot].profile = std::move(profile).value();
  leaf->entries[slot].state = std::move(state).value();
  return {};
}

std::expected<void, MeterError> MeterSetBuilder::Reset(MeterId id) {
  const Entry *entry = Find(id);
  if (entry == nullptr) {
    return std::unexpected(MeterError::kUnknownId);
  }
  auto state = NewState(entry->profile, entry->sharing, entry->socket);
  if (!state) {
    return std::unexpected(state.error());
  }
  const size_t index = static_cast<size_t>(id.value());
  detail::MeterLeaf *leaf = WritableLeaf(index);
  const size_t slot = index & (detail::kMeterLeafSlots - 1);
  leaf->states[slot] = state->get();
  leaf->entries[slot].state = std::move(state).value();
  return {};
}

std::expected<void, MeterError> MeterSetBuilder::Erase(MeterId id) {
  const size_t index = static_cast<size_t>(id.value());
  if (index == 0 || index > capacity_) {
    return std::unexpected(MeterError::kInvalidId);
  }
  if (Find(id) == nullptr) {
    return std::unexpected(MeterError::kUnknownId);
  }
  detail::MeterLeaf *leaf = WritableLeaf(index);
  const size_t slot = index & (detail::kMeterLeafSlots - 1);
  leaf->states[slot] = nullptr;
  leaf->entries[slot] = Entry{};
  leaf->used--;
  size_--;
  Prune(index);
  return {};
}

}  // namespace bess::meter
