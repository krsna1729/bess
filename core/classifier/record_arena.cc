// SPDX-License-Identifier: BSD-3-Clause

#include "classifier/record_arena.h"

#include <new>
#include <utility>

#include "utils/logging.h"

namespace bess::classifier {

namespace {
constexpr size_t kAlignment = alignof(std::max_align_t);

size_t AlignUp(size_t value) {
  return (value + kAlignment - 1) / kAlignment * kAlignment;
}
}  // namespace

void RecordArena::ChunkDeleter::operator()(std::byte *p) const noexcept {
  ::operator delete[](p, std::align_val_t{kAlignment});
}

RecordArena::RecordArena(size_t record_size, rcu::RcuDomain &domain)
    : record_size_(record_size),
      stride_(AlignUp(record_size)),
      domain_(domain),
      published_(new std::atomic<const std::byte *>[kMaxChunks]) {
  CHECK(record_size > 0 && record_size <= kMaxRecordSize);
  for (uint32_t c = 0; c < kMaxChunks; c++) {
    published_[c].store(nullptr, std::memory_order_relaxed);
  }
}

RecordArena::~RecordArena() = default;

RecordArena::Id RecordArena::Allocate() {
  Reclaim();
  if (free_head_ != 0) {
    const Id id = free_head_;
    free_head_ = *reinterpret_cast<const Id *>(Base(id));
    free_count_--;
    return id;
  }
  const Id id = next_id_;
  const uint32_t chunk = id >> kChunkBits;
  if (chunk >= kMaxChunks) {
    return 0;
  }
  if (chunk == chunks_.size()) {
    chunks_.push_back(Chunk(static_cast<std::byte *>(
        ::operator new[](stride_ << kChunkBits, std::align_val_t{kAlignment}))));
    // Published before any id in it is: a reader reaches a chunk only through
    // an id a table loaded, and the table releases its reference later.
    published_[chunk].store(chunks_.back().get(), std::memory_order_release);
  }
  next_id_++;
  return id;
}

std::byte *RecordArena::Base(Id id) const noexcept {
  const uint32_t chunk = id >> kChunkBits;
  // The reader path touches only the fixed published_ array and the acquired
  // chunk pointer: chunks_ belongs to the writer. A chunk is published before
  // any id in it can name a record, and its pointer only becomes non-null
  // once the bytes exist, so a null pointer is the not-allocated case.
  if (id == 0 || chunk >= kMaxChunks) {
    return nullptr;
  }
  const std::byte *base = published_[chunk].load(std::memory_order_acquire);
  if (base == nullptr) {
    return nullptr;
  }
  const uint32_t offset = id & ((1u << kChunkBits) - 1);
  return const_cast<std::byte *>(base) + static_cast<size_t>(offset) * stride_;
}

void RecordArena::Free(Id id) noexcept {
  std::byte *slot = Base(id);
  *reinterpret_cast<Id *>(slot) = free_head_;
  free_head_ = id;
  free_count_++;
}

MutableBytes RecordArena::Record(Id id) noexcept {
  std::byte *base = Base(id);
  if (base == nullptr) {
    return {};
  }
  return MutableBytes(base, record_size_);
}

ConstBytes RecordArena::Lookup(Id id) const noexcept {
  const std::byte *base = Base(id);
  if (base == nullptr) {
    return {};
  }
  return ConstBytes(base, record_size_);
}

void RecordArena::Abort(Id id) noexcept {
  if (id != 0) {
    Free(id);
  }
}

void RecordArena::Retire(Id id) {
  if (id != 0) {
    retiring_.push_back({id, domain_.StartGracePeriod()});
  }
}

void RecordArena::Reclaim() {
  while (!retiring_.empty() && domain_.IsComplete(retiring_.front().token)) {
    Free(retiring_.front().id);
    retiring_.pop_front();
  }
}

}  // namespace bess::classifier
