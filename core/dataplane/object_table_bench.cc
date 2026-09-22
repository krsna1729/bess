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

// K2.4: what the object table costs, and whether the shipped representation is
// the right one.
//
// Three things are measured, because they pull in different directions:
//
//   lookup   -- must be indexed-array cost plus a bounds/validity check;
//   build    -- a replacement generation is built from scratch, so flat storage
//               copies every object while indirect storage copies pointers;
//   memory   -- flat pays sizeof(T) per slot, indirect pays a pointer plus an
//               allocation per object.
//
// The sweep is the full cross product, because the interesting behaviour is
// *where the table stops being cache-resident* and that boundary moves with both
// axes:
//
//   payload bytes   4  16  32  64 128 256      (object size)
//   capacity       1K  8K 64K 512K [1M]        (table size)
//   representation raw  bare-flat  ObjectTable  indirect
//
// Every row reports `storage_bytes`, so it is readable which side of L1d (48 KiB
// here), L2 (1280 KiB), L3 (24 MiB) or DRAM it sits on. Flat storage keeps the
// object inline, so the whole footprint is walked by a lookup; indirect storage
// keeps an 8-byte-per-slot pointer array (cache-resident far longer) and pushes
// the objects into a separate region.
//
// The raw indexed array is the baseline: if `ObjectTable::Lookup()` is not
// effectively that, the abstraction is the problem. The bare-flat variant is the
// same storage as ObjectTable with no wrapper, which separates "flat costs X"
// from "the wrapper costs Y". The indirect variant exists only here, as a
// benchmark alternative -- K2 ships one representation, and alternative storage
// layouts are not public types.
//
// Methodology: these are random-access loops, so they are dominated by cache
// behaviour and are extremely sensitive to anything else running on the machine.
// Recorded numbers come from runs pinned to CPU 2 with its SMT sibling (CPU 3)
// offlined, `--benchmark_min_time=0.1s --benchmark_repetitions=3
// --benchmark_report_aggregates_only`, taking the best of three such runs. The
// sibling has to go: leaving it online costs 20-35% run-to-run spread on the
// rows at or beyond L3, against 21 of 170 rows exceeding 15% with it offlined.
// Unpinned runs on a loaded machine disagree by more than the differences being
// measured, and an earlier version of this file sampled a fixed 4096 ids, which
// kept the touched lines L2-resident no matter how large the table was -- the
// table-size axis then measured nothing. Walking every slot is what makes the
// footprint of a row equal to the footprint of its table.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include "dataplane/action_id.h"
#include "dataplane/object_table.h"

namespace {

using bess::dataplane::ActionId;
using bess::dataplane::ObjectTable;
using bess::dataplane::ObjectTableBuilder;

// A payload of a given size: the point of an object table is values that are
// too large to keep in a classifier's result slot, so the size sweep is the
// interesting axis.
template <size_t Bytes>
struct Payload {
  std::array<uint8_t, Bytes> data;
  uint32_t tag;

  explicit Payload(uint32_t t = 0) : data{}, tag(t) {}
};

enum class Access {
  kHot,      // a handful of ids, all valid
  kUniform,  // uniform random over the whole table, all valid
  kMixed,    // uniform random, a quarter of them invalid or out of range
};

// The ids a lookup loop walks, precomputed so the timed loop is only the
// lookup itself.
//
// The uniform and mixed patterns walk the *whole* table once per iteration
// (every slot exactly once, in random order). That is deliberate: sampling a
// fixed 4096 ids out of a large table touches a fixed 4096 cache lines no matter
// how big the table is, so the working set stays L2-resident and the table-size
// axis measures nothing. Walking every slot makes the footprint of the row equal
// to the footprint of the table, which is the point of the sweep. `kHot` is the
// opposite case on purpose: a handful of ids, always resident.
template <typename Id>
std::vector<Id> MakeIds(size_t capacity, Access access) {
  std::vector<Id> ids;
  std::mt19937 rng(1234);

  switch (access) {
    case Access::kHot: {
      constexpr size_t kHotIds = 8;
      ids.reserve(kHotIds);
      for (size_t i = 0; i < kHotIds; i++) {
        ids.push_back(Id(static_cast<uint32_t>(1 + (i % kHotIds))));
      }
      break;
    }
    case Access::kUniform: {
      ids.reserve(capacity);
      for (size_t i = 0; i < capacity; i++) {
        ids.push_back(Id(static_cast<uint32_t>(i + 1)));
      }
      std::shuffle(ids.begin(), ids.end(), rng);
      break;
    }
    case Access::kMixed: {
      // Three quarters valid, a quarter invalid (id zero and out of range), so
      // the validity path is exercised without changing the valid working set.
      const size_t valid = capacity - capacity / 4;
      ids.reserve(capacity);
      for (size_t i = 0; i < valid; i++) {
        ids.push_back(Id(static_cast<uint32_t>(i + 1)));
      }
      for (size_t i = valid; i < capacity; i++) {
        ids.push_back(i % 2 == 0
                          ? Id(0)
                          : Id(static_cast<uint32_t>(capacity + 1 + i % 1024)));
      }
      std::shuffle(ids.begin(), ids.end(), rng);
      break;
    }
  }
  return ids;
}

// The working set a row actually walks: the point of the sweep is which side of
// L1d/L2/L3 this lands on.
double TouchedBytes(size_t valid_ids, size_t bytes_per_slot) {
  return static_cast<double>(valid_ids) * static_cast<double>(bytes_per_slot);
}

// The benchmark-only alternative representation: a slot array of pointers to
// objects that live in a pool outside the table. The table owns slots, not
// objects, so a new generation copies pointers -- which is exactly the ownership
// question the representation raises, and why the pool's lifetime has to be
// managed separately.
template <typename T>
class IndirectSlots {
 public:
  explicit IndirectSlots(std::vector<const T *> slots) : slots_(std::move(slots)) {}

  const T *Lookup(uint32_t id) const noexcept {
    return id == 0 || id >= slots_.size() ? nullptr : slots_[id];
  }

  size_t storage_bytes() const { return slots_.size() * sizeof(const T *); }

 private:
  std::vector<const T *> slots_;
};

// One-based slot array over a pool: slot 0 reserved, so invalid handling matches
// ObjectTable's.
template <typename T>
std::vector<const T *> MakeIndirectSlots(
    const std::vector<std::unique_ptr<const T>> &objects) {
  std::vector<const T *> slots(objects.size() + 1, nullptr);
  for (size_t i = 0; i < objects.size(); i++) {
    slots[i + 1] = objects[i].get();
  }
  return slots;
}

template <typename T>
std::vector<std::unique_ptr<const T>> MakeIndirectPool(size_t capacity) {
  std::vector<std::unique_ptr<const T>> objects;
  objects.reserve(capacity);
  for (size_t i = 0; i < capacity; i++) {
    objects.push_back(std::make_unique<const T>(static_cast<uint32_t>(i)));
  }
  return objects;
}

// The source of truth a generation is built from.
template <typename T>
std::vector<T> MakeObjects(size_t capacity) {
  std::vector<T> objects;
  objects.reserve(capacity);
  for (size_t i = 0; i < capacity; i++) {
    objects.emplace_back(static_cast<uint32_t>(i));
  }
  return objects;
}

// -- baseline: raw indexed array ---------------------------------------------

template <size_t Bytes>
void BM_RawArrayLookup(benchmark::State &state) {
  const size_t capacity = state.range(0);
  const Access access = static_cast<Access>(state.range(1));
  using P = Payload<Bytes>;

  const std::vector<P> values = MakeObjects<P>(capacity);
  const std::vector<ActionId> ids = MakeIds<ActionId>(capacity, access);

  uint32_t sink = 0;
  for (auto _ : state) {
    for (ActionId id : ids) {
      // The irreducible cost: index, load, use. No validity check, because a
      // raw array cannot express one.
      sink ^= values[id.value() - 1].tag;
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * ids.size());
  state.counters["bytes_per_object"] = static_cast<double>(sizeof(P));
  state.counters["storage_bytes"] =
      static_cast<double>(values.size() * sizeof(P));
  state.counters["touched_bytes"] =
      TouchedBytes(access == Access::kHot ? 8 : values.size(), sizeof(P));
}

// -- flat storage without the wrapper ----------------------------------------

// The same storage as ObjectTable and the same lookup body, with no class and no
// indirection: the row that answers "is ObjectTable::Lookup() as cheap as the
// array it wraps?".
template <size_t Bytes>
void BM_BareFlatLookup(benchmark::State &state) {
  const size_t capacity = state.range(0);
  const Access access = static_cast<Access>(state.range(1));
  using P = Payload<Bytes>;

  std::vector<std::optional<P>> slots(capacity + 1);
  for (size_t i = 0; i < capacity; i++) {
    slots[i + 1].emplace(static_cast<uint32_t>(i));
  }

  const std::vector<ActionId> ids = MakeIds<ActionId>(capacity, access);

  uint32_t sink = 0;
  for (auto _ : state) {
    for (ActionId id : ids) {
      const size_t index = id.value();
      const P *object = (index == 0 || index >= slots.size() || !slots[index])
                            ? nullptr
                            : &slots[index].value();
      sink ^= object == nullptr ? 0u : object->tag;
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * ids.size());
  state.counters["bytes_per_object"] = static_cast<double>(sizeof(P));
  state.counters["storage_bytes"] =
      static_cast<double>(slots.size() * sizeof(std::optional<P>));
  state.counters["touched_bytes"] =
      TouchedBytes(access == Access::kHot ? 8 : slots.size() - 1,
                   sizeof(std::optional<P>));
}

// -- shipped representation: flat, inline slots ------------------------------

template <size_t Bytes>
void BM_ObjectTableLookup(benchmark::State &state) {
  const size_t capacity = state.range(0);
  const Access access = static_cast<Access>(state.range(1));
  using P = Payload<Bytes>;

  ObjectTableBuilder<ActionId, P> builder(capacity);
  for (size_t i = 0; i < capacity; i++) {
    builder.Emplace(ActionId(static_cast<uint32_t>(i + 1)),
                    static_cast<uint32_t>(i));
  }
  const std::unique_ptr<const ObjectTable<ActionId, P>> table =
      std::move(builder).Build();

  const std::vector<ActionId> ids = MakeIds<ActionId>(capacity, access);

  uint32_t sink = 0;
  for (auto _ : state) {
    for (ActionId id : ids) {
      const P *object = table->Lookup(id);
      // The payload is consumed, not just addressed: a lookup that only
      // materializes a pointer would measure address arithmetic, not the memory
      // the table is made of. The sink is a data dependency on the loaded value,
      // which is what stops the compiler from sinking the load out of the loop
      // (a `DoNotOptimize` on a local does not: the load then feeds nothing the
      // compiler must keep inside the loop). The XOR chain is one cycle wide and
      // does not serialize the independent loads.
      sink ^= object == nullptr ? 0u : object->tag;
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * ids.size());
  state.counters["bytes_per_object"] = static_cast<double>(sizeof(P));
  state.counters["storage_bytes"] = static_cast<double>(table->storage_bytes());
  state.counters["touched_bytes"] =
      TouchedBytes(access == Access::kHot ? 8 : table->capacity(),
                   table->storage_bytes() / (table->capacity() + 1));
}

// -- alternative representation: indirect slots ------------------------------

template <size_t Bytes>
void BM_IndirectLookup(benchmark::State &state) {
  const size_t capacity = state.range(0);
  const Access access = static_cast<Access>(state.range(1));
  using P = Payload<Bytes>;

  const std::vector<std::unique_ptr<const P>> pool = MakeIndirectPool<P>(capacity);
  const IndirectSlots<P> table(MakeIndirectSlots(pool));

  const std::vector<ActionId> ids = MakeIds<ActionId>(capacity, access);

  uint32_t sink = 0;
  for (auto _ : state) {
    for (ActionId id : ids) {
      const P *object = table.Lookup(id.value());
      sink ^= object == nullptr ? 0u : object->tag;
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * ids.size());
  state.counters["bytes_per_object"] = static_cast<double>(sizeof(P));
  state.counters["storage_bytes"] =
      static_cast<double>(table.storage_bytes() + capacity * sizeof(P));
  // The pointer array is what a lookup walks; the objects are reached through
  // it. Both are reported so the row's residency is readable.
  state.counters["touched_bytes"] =
      TouchedBytes(access == Access::kHot ? 8 : capacity, sizeof(const P *));
  state.counters["touched_object_bytes"] =
      TouchedBytes(access == Access::kHot ? 8 : capacity, sizeof(P));
}

// -- batch lookup ------------------------------------------------------------

template <size_t Bytes>
void BM_ObjectTableBatchLookup(benchmark::State &state) {
  const size_t capacity = 4096;
  const size_t batch = state.range(0);
  using P = Payload<Bytes>;

  ObjectTableBuilder<ActionId, P> builder(capacity);
  for (size_t i = 0; i < capacity; i++) {
    builder.Emplace(ActionId(static_cast<uint32_t>(i + 1)),
                    static_cast<uint32_t>(i));
  }
  const std::unique_ptr<const ObjectTable<ActionId, P>> table =
      std::move(builder).Build();

  const std::vector<ActionId> ids = MakeIds<ActionId>(capacity, Access::kMixed);
  std::vector<const P *> results(ids.size(), nullptr);
  const std::span<const ActionId> id_span(ids);
  const std::span<const P *> result_span(results);

  uint32_t sink = 0;
  for (auto _ : state) {
    for (size_t start = 0; start + batch <= ids.size(); start += batch) {
      table->LookupBatch(id_span.subspan(start, batch),
                         result_span.subspan(start, batch));
      for (size_t i = 0; i < batch; i++) {
        const P *object = results[start + i];
        sink ^= object == nullptr ? 0u : object->tag;
      }
    }
    benchmark::DoNotOptimize(sink);
  }
  state.SetItemsProcessed(state.iterations() * ids.size());
  state.counters["batch"] = static_cast<double>(batch);
}

// -- generation build and replacement cost -----------------------------------

// Building a generation from scratch is what an update costs, and flat storage
// copies every object while indirect storage copies pointers.
template <size_t Bytes>
void BM_ObjectTableBuild(benchmark::State &state) {
  const size_t capacity = state.range(0);
  using P = Payload<Bytes>;

  const std::vector<P> source = MakeObjects<P>(capacity);

  for (auto _ : state) {
    ObjectTableBuilder<ActionId, P> builder(capacity);
    for (size_t i = 0; i < capacity; i++) {
      builder.Emplace(ActionId(static_cast<uint32_t>(i + 1)), source[i]);
    }
    std::unique_ptr<const ObjectTable<ActionId, P>> table =
        std::move(builder).Build();
    benchmark::DoNotOptimize(table);
  }
  state.SetItemsProcessed(state.iterations() * capacity);
  state.counters["objects"] = static_cast<double>(capacity);
  state.counters["bytes_per_object"] = static_cast<double>(sizeof(P));
  state.counters["storage_bytes"] =
      static_cast<double>((capacity + 1) * sizeof(std::optional<P>));
}

template <size_t Bytes>
void BM_IndirectBuild(benchmark::State &state) {
  const size_t capacity = state.range(0);
  using P = Payload<Bytes>;

  const std::vector<P> source = MakeObjects<P>(capacity);

  for (auto _ : state) {
    std::vector<std::unique_ptr<const P>> objects;
    objects.reserve(capacity);
    for (size_t i = 0; i < capacity; i++) {
      objects.push_back(std::make_unique<const P>(source[i]));
    }
    IndirectSlots<P> table(MakeIndirectSlots(objects));
    benchmark::DoNotOptimize(table);
  }
  state.SetItemsProcessed(state.iterations() * capacity);
  state.counters["objects"] = static_cast<double>(capacity);
  state.counters["bytes_per_object"] = static_cast<double>(sizeof(P));
  state.counters["storage_bytes"] =
      static_cast<double>((capacity + 1) * sizeof(const P *) +
                          capacity * sizeof(P));
}

// Replacing one object in a published generation is the update a control-plane
// transaction performs. Flat storage rebuilds by copying every object; indirect
// storage rebuilds the pointer array and reuses all but one object.
template <size_t Bytes>
void BM_ObjectTableReplace(benchmark::State &state) {
  const size_t capacity = state.range(0);
  using P = Payload<Bytes>;

  const std::vector<P> source = MakeObjects<P>(capacity);
  uint32_t gen = 0;

  for (auto _ : state) {
    ObjectTableBuilder<ActionId, P> builder(capacity);
    for (size_t i = 0; i < capacity; i++) {
      // One changed object, everything else carried over unchanged.
      builder.Emplace(ActionId(static_cast<uint32_t>(i + 1)),
                      i == 0 ? P(gen++) : source[i]);
    }
    std::unique_ptr<const ObjectTable<ActionId, P>> table =
        std::move(builder).Build();
    benchmark::DoNotOptimize(table);
  }
  state.SetItemsProcessed(state.iterations() * capacity);
  state.counters["objects"] = static_cast<double>(capacity);
  state.counters["bytes_per_object"] = static_cast<double>(sizeof(P));
  state.counters["changed_objects"] = 1.0;
}

template <size_t Bytes>
void BM_IndirectReplace(benchmark::State &state) {
  const size_t capacity = state.range(0);
  using P = Payload<Bytes>;

  // The pool outlives every generation, so unchanged objects are shared rather
  // than copied. That is the representation's whole advantage here -- and the
  // reason its lifetime has to be managed outside the table.
  std::vector<std::unique_ptr<const P>> pool = MakeIndirectPool<P>(capacity);
  const std::vector<const P *> pointers = MakeIndirectSlots(pool);
  uint32_t gen = 0;

  for (auto _ : state) {
    std::vector<const P *> next = pointers;
    pool[0] = std::make_unique<const P>(gen++);
    next[1] = pool[0].get();
    IndirectSlots<P> table(std::move(next));
    benchmark::DoNotOptimize(table);
  }
  state.SetItemsProcessed(state.iterations() * capacity);
  state.counters["objects"] = static_cast<double>(capacity);
  state.counters["bytes_per_object"] = static_cast<double>(sizeof(P));
  state.counters["changed_objects"] = 1.0;
  state.counters["storage_bytes"] =
      static_cast<double>((capacity + 1) * sizeof(const P *) +
                          capacity * sizeof(P));
}

// -- the sweep ---------------------------------------------------------------

// Full cross product of representation x payload size x table size. 1M entries
// is included only where the footprint stays sane (4 and 16 byte payloads).
#define K24_CAPACITIES                                      \
  ->Args({1024, (int)Access::kUniform})                     \
  ->Args({10240, (int)Access::kUniform})                    \
  ->Args({65536, (int)Access::kUniform})                    \
  ->Args({102400, (int)Access::kUniform})                   \
  ->Args({524288, (int)Access::kUniform})

#define K24_LOOKUP_SWEEP(fn)         \
  BENCHMARK_TEMPLATE(fn, 4)          \
      K24_CAPACITIES                 \
      ->Args({1048576, (int)Access::kUniform}); \
  BENCHMARK_TEMPLATE(fn, 16)         \
      K24_CAPACITIES                 \
      ->Args({1048576, (int)Access::kUniform}); \
  BENCHMARK_TEMPLATE(fn, 32)         \
      K24_CAPACITIES                 \
      ->Args({1048576, (int)Access::kUniform}); \
  BENCHMARK_TEMPLATE(fn, 64)         \
      K24_CAPACITIES                 \
      ->Args({1048576, (int)Access::kUniform}); \
  BENCHMARK_TEMPLATE(fn, 128)        \
      K24_CAPACITIES;                \
  BENCHMARK_TEMPLATE(fn, 256)        \
      K24_CAPACITIES;

K24_LOOKUP_SWEEP(BM_RawArrayLookup)
K24_LOOKUP_SWEEP(BM_BareFlatLookup)
K24_LOOKUP_SWEEP(BM_ObjectTableLookup)
K24_LOOKUP_SWEEP(BM_IndirectLookup)

// Access-distribution rows, on the shipped representation only: how much of the
// lookup cost is the index/validity work and how much is the miss.
BENCHMARK_TEMPLATE(BM_ObjectTableLookup, 64)
    ->Args({10240, (int)Access::kHot})
    ->Args({10240, (int)Access::kMixed})
    ->Args({524288, (int)Access::kHot})
    ->Args({524288, (int)Access::kMixed});
BENCHMARK_TEMPLATE(BM_ObjectTableLookup, 256)
    ->Args({65536, (int)Access::kHot})
    ->Args({65536, (int)Access::kMixed});

BENCHMARK_TEMPLATE(BM_ObjectTableBatchLookup, 16)->Arg(1)->Arg(8)->Arg(16)->Arg(32);

#define K24_BUILD_SWEEP(fn)                \
  BENCHMARK_TEMPLATE(fn, 4)->Arg(10240);   \
  BENCHMARK_TEMPLATE(fn, 4)->Arg(102400);  \
  BENCHMARK_TEMPLATE(fn, 64)->Arg(10240);  \
  BENCHMARK_TEMPLATE(fn, 64)->Arg(102400); \
  BENCHMARK_TEMPLATE(fn, 256)->Arg(10240); \
  BENCHMARK_TEMPLATE(fn, 256)->Arg(102400);

K24_BUILD_SWEEP(BM_ObjectTableBuild)
K24_BUILD_SWEEP(BM_IndirectBuild)
K24_BUILD_SWEEP(BM_ObjectTableReplace)
K24_BUILD_SWEEP(BM_IndirectReplace)

}  // namespace
