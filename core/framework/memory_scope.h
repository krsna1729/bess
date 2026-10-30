// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_MEMORY_SCOPE_H_
#define BESS_FRAMEWORK_MEMORY_SCOPE_H_

#include <concepts>
#include <cstddef>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "utils/dpdk_memory.h"

// Decision D-198 ("Dataplane memory"): how a module sizes its tables. A
// module allocates every table, pool, ring or other primitive whose size it
// takes from its arguments through one entry point, init_context().memory():
//
//  - each primitive knows its byte cost from the arguments that size it
//    (MemoryCost<T>::Bytes, equal to what its constructor allocates -- each
//    primitive's unit test checks Bytes() == bytes());
//  - the module admits those bytes before it allocates them, under the memory
//    policy (utils::AdmitTable: normal pages against the limit, hugepages
//    against --mem_hugepage_mb, past it normal pages unless
//    --mem_fallback=false), so a table that does not fit is refused whole
//    (utils::MemoryRefused -> RESOURCE_EXHAUSTED at the control boundary),
//    never half-built and never an OOM kill;
//  - the framework, not the module, decides where the bytes go (D-206):
//    an admission arms the calling thread's placement (utils::ArmPlacement),
//    so the allocations that follow land where it admitted them, and bytes
//    that land elsewhere (hugepages ran out before the cap) move with them;
//  - what each module admitted is accounted per module and resource, by
//    where it landed (hugepages, normal pages, glibc), and reported
//    (GetResourceStatus.memory.modules).
//
// Module argument names stay the module's own: modules size different things
// (entries, tuples, levels, queue depth). What is uniform is the semantics.
// Control path only: nothing here is reachable from the packet path, and an
// object a module creates here is read by workers exactly as before.

namespace bess::framework {

class MemoryScope;
class MemoryAccounts;

// What a primitive costs, in bytes, from the arguments that size it: what its
// constructor (or static Create factory) allocates -- DPDK heap, glibc, DPDK
// libraries' internal tables -- taking the same arguments. The generic
// definition forwards to a static T::Bytes(args...) next to the constructor
// (the dataplane libraries cannot include the framework, so that is where
// primitives declare it), and to T::GlibcBytes(args...), the part of Bytes
// taken from glibc rather than DPDK's heap, when T has one (else 0).
// Specialize MemoryCost<T> for a type that cannot have them.
template <typename T>
struct MemoryCost {
  template <typename... Args>
  static size_t Bytes(const Args &...args) {
    return T::Bytes(args...);
  }
  template <typename... Args>
  static size_t GlibcBytes(const Args &...args) {
    if constexpr (requires { T::GlibcBytes(args...); }) {
      return T::GlibcBytes(args...);
    } else {
      return 0;
    }
  }
};

namespace detail {
// The definition (memory_scope.cc) is final over utils::MemoryGrowth, whose
// destructor is protected; clang-tidy 19 checks this declaration, which
// cannot say `final`.
// NOLINTNEXTLINE(cppcoreguidelines-virtual-class-destructor)
struct LeaseNode;
}  // namespace detail

// Bytes admitted in a scope for one resource, released when the lease is
// destroyed or reset. Move-only. A primitive that grows after construction
// takes growth() (utils::MemoryGrowth) and admits each growth through it;
// Track() makes the reported current bytes the object's own bytes().
class MemoryLease {
 public:
  MemoryLease() = default;
  MemoryLease(MemoryLease &&o) noexcept : node_(std::exchange(o.node_, nullptr)) {}
  MemoryLease &operator=(MemoryLease &&o) noexcept {
    if (this != &o) {
      reset();
      node_ = std::exchange(o.node_, nullptr);
    }
    return *this;
  }
  MemoryLease(const MemoryLease &) = delete;
  MemoryLease &operator=(const MemoryLease &) = delete;
  ~MemoryLease() { reset(); }

  explicit operator bool() const noexcept { return node_ != nullptr; }
  // What is admitted now: the initial bytes plus every growth, less shrinks.
  size_t bytes() const noexcept;
  // Stable for the lease's lifetime, including across moves; nullptr when
  // empty.
  utils::MemoryGrowth *growth() const noexcept;

  // Reports `object.bytes()` as the resource's current bytes (read under
  // the control-plane lock by GetResourceStatus, which also orders the
  // object's destruction). Declare a lease member BEFORE the object it
  // admits, so the object is destroyed first: a primitive may give bytes
  // back through growth() from its destructor or from an RCU free of what
  // it retired, so the hook must outlive it.
  template <typename T>
  void Track(const T &object) noexcept {
    Track(&object, [](const void *p) -> size_t {
      return static_cast<const T *>(p)->bytes();
    });
  }
  void Untrack() noexcept { Track(nullptr, nullptr); }

  // Releases the bytes. No-op when empty.
  void reset() noexcept;

 private:
  friend class MemoryScope;
  explicit MemoryLease(detail::LeaseNode *node) noexcept : node_(node) {}
  void Track(const void *object, size_t (*read)(const void *)) noexcept;

  detail::LeaseNode *node_ = nullptr;
};

// The owner MemoryScope::Create returns: the object, constructed after its
// bytes were admitted, and the admission, released after the object is
// destroyed. Move-only; the object's address does not change when the owner
// moves. Workers reach the object through get() or a pointer cached from it.
template <typename T>
class Sized {
 public:
  Sized() = default;
  Sized(Sized &&) noexcept = default;
  Sized &operator=(Sized &&o) noexcept {
    if (this != &o) {
      reset();
      lease_ = std::move(o.lease_);
      object_ = std::move(o.object_);
    }
    return *this;
  }
  ~Sized() { reset(); }

  T *get() const noexcept { return object_.get(); }
  T *operator->() const noexcept { return object_.get(); }
  T &operator*() const noexcept { return *object_; }
  explicit operator bool() const noexcept { return object_ != nullptr; }
  size_t admitted_bytes() const noexcept { return lease_.bytes(); }

  // Destroys the object, then releases its bytes.
  void reset() noexcept {
    lease_.Untrack();
    object_.reset();
    lease_.reset();
  }

 private:
  friend class MemoryScope;
  using Object = std::unique_ptr<T, void (*)(T *)>;
  MemoryLease lease_;
  Object object_{nullptr, [](T *p) { delete p; }};
};

// A primitive that reports what it holds now (for current_bytes).
template <typename T>
concept HasMemoryBytes = requires(const T &t) {
  { t.bytes() } -> std::convertible_to<size_t>;
};
// A primitive that grows after construction and admits each growth.
template <typename T>
concept HasMemoryGrowth = requires(T &t, utils::MemoryGrowth *g) {
  t.set_memory_growth(g);
};

// One module's memory (ModuleInitContext::memory()): every byte the module
// admitted, by resource. Destroyed with its module, after the module's own
// members, releasing whatever is still admitted. Not copyable or movable.
class MemoryScope {
 public:
  // `owner` names the scope in the accounts and is read when they are
  // listed (a module's name is set after its scope exists), so it must
  // outlive the scope.
  explicit MemoryScope(const std::string *owner);
  ~MemoryScope();
  MemoryScope(const MemoryScope &) = delete;
  MemoryScope &operator=(const MemoryScope &) = delete;

  // The memory node's DPDK socket id (D-123; utils::MemoryNode()), for the
  // DPDK APIs a module calls itself (rte_ring_create, rte_hash_create, ...).
  // Primitives allocate there by default.
  int socket() const;

  // Admits `bytes` for `resource` (a name inside the module, e.g.
  // "fteids"; reported as "<module>/<resource>") before they are allocated,
  // `glibc_bytes` of them from glibc and the rest from DPDK's heap, under
  // the memory policy (utils::AdmitTable): normal backing ->
  // AdmitNormalMemory; hugepage backing within the hugepage cap -> admitted;
  // past the cap -> AdmitNormalMemory, or MemoryRefused(kHugepagesExhausted)
  // with --mem_fallback=false. Arms this thread's placement with the DPDK
  // part (D-206): allocate it next, on this thread. Throws
  // utils::MemoryRefused and records nothing on refusal. Bytes admitted here
  // are released by Release or with the scope.
  void Admit(std::string_view resource, size_t bytes, size_t glibc_bytes = 0);
  // Gives back bytes Admit admitted for `resource` (at most what it holds),
  // `glibc_bytes` of them glibc's.
  void Release(std::string_view resource, size_t bytes,
               size_t glibc_bytes = 0) noexcept;
  // Admit, as a lease that releases the bytes when it is destroyed.
  MemoryLease Lease(std::string_view resource, size_t bytes,
                    size_t glibc_bytes = 0);
  // A lease of what T costs built from `args` (MemoryCost<T>), for a T made
  // by a factory that can fail with an error the module reports (one that
  // returns std::expected): lease first, make second, then Adopt.
  template <typename T, typename... Args>
  MemoryLease LeaseFor(std::string_view resource, const Args &...args) {
    return Lease(resource, MemoryCost<T>::Bytes(args...),
                 MemoryCost<T>::GlibcBytes(args...));
  }

  // bytes = MemoryCost<T>::Bytes(args...); admits them for `resource`;
  // constructs T(args...), or, for a T built by a factory, T::Create(
  // args...) returning a std::unique_ptr<T, D> (D stateless; null is
  // std::bad_alloc). Pass socket() where T takes a socket; the primitives
  // default to it. The owner releases the admission when it is destroyed.
  // A T with bytes() reports it as current; a T with set_memory_growth gets
  // the lease's growth hook, so it admits what it grows by.
  template <typename T, typename... Args>
  Sized<T> Create(std::string_view resource, Args &&...args) {
    MemoryLease lease = LeaseFor<T>(resource, std::as_const(args)...);
    if constexpr (std::is_constructible_v<T, Args &&...>) {
      return Adopt(std::move(lease),
                   std::unique_ptr<T>(new T(std::forward<Args>(args)...)));
    } else {
      auto made = T::Create(std::forward<Args>(args)...);
      if (made == nullptr) {
        throw std::bad_alloc();
      }
      return Adopt(std::move(lease), std::move(made));
    }
  }

  // The owner of `object`, made after `lease` admitted it (LeaseFor): as
  // Create, tracks its bytes() and hands it the lease's growth hook. D must
  // be stateless.
  template <typename T, typename D>
  static Sized<T> Adopt(MemoryLease lease, std::unique_ptr<T, D> object) {
    static_assert(std::is_empty_v<D> && std::is_default_constructible_v<D>,
                  "Adopt takes a std::unique_ptr<T, D> with a stateless D");
    Sized<T> owner;
    owner.lease_ = std::move(lease);
    owner.object_ =
        typename Sized<T>::Object(object.release(), [](T *p) { D{}(p); });
    if (owner.object_ != nullptr) {
      if constexpr (HasMemoryBytes<T>) {
        owner.lease_.Track(*owner.object_);
      }
      if constexpr (HasMemoryGrowth<T>) {
        owner.object_->set_memory_growth(owner.lease_.growth());
      }
    }
    return owner;
  }

  // Everything admitted in this scope now.
  size_t admitted_bytes() const;

 private:
  friend class MemoryAccounts;
  friend class MemoryLease;
  friend struct detail::LeaseNode;
  struct Entry;

  Entry &EntryLocked(std::string_view resource);

  const std::string *owner_;
  // Ordered, so listings are stable; nodes are never erased while the scope
  // lives (leases point into them).
  std::map<std::string, std::unique_ptr<Entry>, std::less<>> entries_;
};

// What one scope admitted for one resource.
struct ModuleMemoryUse {
  std::string module;
  std::string resource;
  size_t admitted_bytes = 0;
  size_t current_bytes = 0;  // tracked objects' bytes(), else admitted
  // admitted_bytes by where they landed (D-206): DPDK memory on hugepages,
  // DPDK memory on normal pages (the fallback heap, or a normal-page
  // backing), and glibc.
  size_t hugepage_bytes = 0;
  size_t normal_bytes = 0;
  size_t glibc_bytes = 0;
};

// The process's memory accounts: every live scope's admitted bytes by module
// and resource (control path; one mutex orders admission, release and
// listing). GetResourceStatus reads them under the control-plane lock, which
// keeps tracked objects alive while their bytes() are read.
class MemoryAccounts {
 public:
  // The daemon's own scope, owner "bessd" (the packet pool: resource
  // "packet_pool").
  static MemoryScope &Process();
  // Rows with admitted bytes, ordered by module, then resource.
  static std::vector<ModuleMemoryUse> List();
};

// A growth hook that charges the daemon's own scope (MemoryAccounts::
// Process(), module "bessd") under `resource`: for process-wide control-plane
// state no module owns -- the transaction engine's reference ledger
// ("transaction_refs"), the transaction outcome records
// ("transaction_records") -- through utils::AdmittedGlibcAllocator (D-206).
// Must outlive every container charging it.
class ProcessGrowth final : public utils::MemoryGrowth {
 public:
  explicit ProcessGrowth(std::string resource) : resource_(std::move(resource)) {}
  void Grow(size_t bytes, size_t glibc_bytes) override {
    MemoryAccounts::Process().Admit(resource_, bytes, glibc_bytes);
  }
  void Shrink(size_t bytes, size_t glibc_bytes) noexcept override {
    MemoryAccounts::Process().Release(resource_, bytes, glibc_bytes);
  }

 private:
  std::string resource_;
};

}  // namespace bess::framework

#endif  // BESS_FRAMEWORK_MEMORY_SCOPE_H_
