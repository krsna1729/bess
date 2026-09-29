// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_INLINE_FUNCTION_H_
#define BESS_UTILS_INLINE_FUNCTION_H_

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace bess::utils {

// A move-only callable stored in `kBytes` of inline storage, never on the
// heap: constructing, moving and destroying one does not allocate. For code
// that must not allocate at a given point (the transaction engine's
// publication window, D-021) but wants to accept lambdas. A callable that
// does not fit is a compile error, not a silent allocation -- what
// std::move_only_function would do (its inline buffer is small and
// library-specific).
//
// Allocation in the callable's own construction (a lambda copying a
// std::string into its capture) is the caller's, as anywhere else.
template <typename Signature, size_t kBytes>
class InlineFunction;

template <typename R, typename... Args, size_t kBytes>
class InlineFunction<R(Args...), kBytes> {
 public:
  static constexpr size_t kCapacity = kBytes;

  InlineFunction() noexcept = default;

  template <typename F>
    requires(!std::is_same_v<std::remove_cvref_t<F>, InlineFunction> &&
             std::is_invocable_r_v<R, std::remove_cvref_t<F> &, Args...>)
  InlineFunction(F &&f) {  // NOLINT: implicit, like std::function
    using T = std::remove_cvref_t<F>;
    static_assert(sizeof(T) <= kBytes,
                  "callable too large for its inline storage: prepare the "
                  "state before the no-allocation point and capture a "
                  "pointer or reference to it");
    static_assert(alignof(T) <= alignof(std::max_align_t),
                  "over-aligned callable");
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "the callable must be nothrow move constructible");
    ::new (static_cast<void *>(storage_)) T(std::forward<F>(f));
    ops_ = &kOps<T>;
  }

  InlineFunction(InlineFunction &&other) noexcept { Take(other); }
  InlineFunction &operator=(InlineFunction &&other) noexcept {
    if (this != &other) {
      Reset();
      Take(other);
    }
    return *this;
  }
  InlineFunction(const InlineFunction &) = delete;
  InlineFunction &operator=(const InlineFunction &) = delete;
  ~InlineFunction() { Reset(); }

  explicit operator bool() const noexcept { return ops_ != nullptr; }

  R operator()(Args... args) {
    return ops_->call(storage_, std::forward<Args>(args)...);
  }

 private:
  struct Ops {
    R (*call)(void *, Args &&...);
    void (*move)(void *to, void *from) noexcept;  // and destroys `from`
    void (*destroy)(void *) noexcept;
  };

  template <typename T>
  static constexpr Ops kOps = {
      [](void *f, Args &&...args) -> R {
        return (*static_cast<T *>(f))(std::forward<Args>(args)...);
      },
      [](void *to, void *from) noexcept {
        ::new (to) T(std::move(*static_cast<T *>(from)));
        static_cast<T *>(from)->~T();
      },
      [](void *f) noexcept { static_cast<T *>(f)->~T(); },
  };

  void Take(InlineFunction &other) noexcept {
    if (other.ops_ != nullptr) {
      other.ops_->move(storage_, other.storage_);
      ops_ = other.ops_;
      other.ops_ = nullptr;
    }
  }
  void Reset() noexcept {
    if (ops_ != nullptr) {
      ops_->destroy(storage_);
      ops_ = nullptr;
    }
  }

  alignas(std::max_align_t) std::byte storage_[kBytes];
  const Ops *ops_ = nullptr;
};

}  // namespace bess::utils

#endif  // BESS_UTILS_INLINE_FUNCTION_H_
