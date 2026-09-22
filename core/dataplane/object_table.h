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

#ifndef BESS_DATAPLANE_OBJECT_TABLE_H_
#define BESS_DATAPLANE_OBJECT_TABLE_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace bess {
namespace dataplane {

template <typename Id, typename T>
class ObjectTableBuilder;

// An immutable table mapping a strongly typed id to an object (K2).
//
//   Id -> const T *
//
// The packet-facing interface is deliberately tiny: extract the id, bounds
// check, validity check, address calculation. No hashing, no search, no
// allocation, no lock, no reference counting. `T` may be any size, which is the
// whole point: this is the mechanism for when a lookup result is better carried
// as a compact id than as an inline value.
//
// Ids are one-based: `Id{0}` is reserved as invalid, so slot 0 is never
// populated and no subtract-one arithmetic is needed. Configured capacity is
// independent of the id width -- a 32-bit id space does not mean a 32-bit-sized
// allocation.
//
// The table is a plain immutable value: it owns no RCU domain and knows nothing
// about publication. Publishing it is `RcuPtr<ObjectTable<...>>` plus the
// runtime's `RcuDomain` (K1), which is also what keeps a retired generation
// alive until readers are done and destroys it on a control thread.
//
// Ids are stable: a surviving object keeps its id across generations, nothing
// is compacted or renumbered, and erasing an id leaves a hole that is never
// automatically filled. Automatic reuse would turn a stale id into a different
// object (an ABA problem) -- RCU protects object *memory* lifetime, not
// semantic id reuse, so that decision belongs to whoever allocates ids.
template <typename Id, typename T>
class ObjectTable {
 public:
  using id_type = Id;
  using object_type = T;

  // Returns the object for `id`, or nullptr for an invalid id (zero), an
  // out-of-range id, or an empty slot. The pointer is valid for as long as the
  // table generation is; no ownership is transferred and nothing is allocated.
  const T *Lookup(Id id) const noexcept {
    const size_t index = static_cast<size_t>(id.value());
    if (index == 0 || index >= slots_.size()) {
      return nullptr;
    }
    const std::optional<T> &slot = slots_[index];
    return slot.has_value() ? std::addressof(*slot) : nullptr;
  }

  // Resolves a batch. Results line up with `ids`; the same rules as Lookup()
  // apply per element. No allocation or synchronization.
  void LookupBatch(std::span<const Id> ids,
                   std::span<const T *> results) const noexcept {
    const size_t count = ids.size() < results.size() ? ids.size()
                                                     : results.size();
    for (size_t i = 0; i < count; i++) {
      results[i] = Lookup(ids[i]);
    }
  }

  bool Contains(Id id) const noexcept { return Lookup(id) != nullptr; }

  // Number of populated slots.
  size_t size() const noexcept { return size_; }

  // Configured number of usable slots (id 1 .. capacity).
  size_t capacity() const noexcept { return slots_.size() - 1; }

  // Control-side introspection for benchmarks and tests; not a packet-path
  // statistic, and deliberately not maintained per lookup.
  size_t storage_bytes() const noexcept {
    return slots_.size() * sizeof(std::optional<T>);
  }

 private:
  friend class ObjectTableBuilder<Id, T>;

  ObjectTable(std::vector<std::optional<T>> slots, size_t size)
      : slots_(std::move(slots)), size_(size) {}

  std::vector<std::optional<T>> slots_;
  size_t size_ = 0;
};

// The mutable side of an ObjectTable: it changes *mappings*, the built table
// never changes at all. All updates construct a replacement generation.
//
// It owns objects that have not been published yet, so replacing or erasing a
// mapping may destroy the previous value immediately -- no reader can be
// holding it. That is why publication, not the builder, is what needs RCU.
template <typename Id, typename T>
class ObjectTableBuilder {
 public:
  explicit ObjectTableBuilder(size_t capacity) : slots_(capacity + 1) {}

  ObjectTableBuilder(const ObjectTableBuilder &) = delete;
  ObjectTableBuilder &operator=(const ObjectTableBuilder &) = delete;

  // Installs `value` at `id`. Ids are chosen by the caller, not allocated here:
  // id lifetime may need to be coordinated with classifier generations,
  // hardware rules or other producers, which this class cannot know about.
  // Returns false for an invalid or out-of-range id.
  bool Set(Id id, T value) { return Emplace(id, std::move(value)); }

  template <typename... Args>
  bool Emplace(Id id, Args &&...args) {
    const size_t index = static_cast<size_t>(id.value());
    if (index == 0 || index >= slots_.size()) {
      return false;  // invalid id or beyond the configured capacity
    }
    if (!slots_[index].has_value()) {
      size_++;
    }
    slots_[index].emplace(std::forward<Args>(args)...);
    return true;
  }

  // Empties a slot. The id stays reserved as a hole: it is not handed to any
  // later insertion. Returns false if the id was invalid, out of range or
  // already empty.
  bool Erase(Id id) {
    const size_t index = static_cast<size_t>(id.value());
    if (index == 0 || index >= slots_.size() || !slots_[index].has_value()) {
      return false;
    }
    slots_[index].reset();
    size_--;
    return true;
  }

  bool Contains(Id id) const {
    const size_t index = static_cast<size_t>(id.value());
    return index != 0 && index < slots_.size() && slots_[index].has_value();
  }

  size_t size() const noexcept { return size_; }
  size_t capacity() const noexcept { return slots_.size() - 1; }

  // Freezes the table. The builder is consumed: it must not be used afterwards.
  std::unique_ptr<const ObjectTable<Id, T>> Build() && {
    return std::unique_ptr<const ObjectTable<Id, T>>(
        new ObjectTable<Id, T>(std::move(slots_), size_));
  }

 private:
  std::vector<std::optional<T>> slots_;
  size_t size_ = 0;
};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_OBJECT_TABLE_H_
