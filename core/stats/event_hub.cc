// SPDX-License-Identifier: BSD-3-Clause

#include "stats/event_hub.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <string_view>

namespace bess::stats {

namespace {

uint64_t MonotonicNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

template <typename Text>
void AppendNumber(Text &text, uint64_t value) {
  char digits[20];
  const auto converted = std::to_chars(digits, digits + sizeof(digits), value);
  text.append(digits, static_cast<size_t>(converted.ptr - digits));
}

}  // namespace

// One worker's ring: the worker produces, DrainWorkers consumes.
struct EventHub::Ring {
  Ring(size_t capacity, utils::MemoryGrowth *growth)
      : mask(std::bit_ceil(std::max<size_t>(capacity, 2)) - 1),
        slots(mask + 1, Allocator<WorkerEvent>(growth)) {}
  const size_t mask;
  std::vector<WorkerEvent, Allocator<WorkerEvent>> slots;
  alignas(64) std::atomic<uint64_t> head{0};  // consumer
  alignas(64) std::atomic<uint64_t> tail{0};  // producer
  uint64_t refused_since = 0;                 // producer only
  std::atomic<uint64_t> refused{0};           // total, for metrics
};

void EventHub::RingDelete::operator()(Ring *ring) const noexcept {
  Allocator<Ring> allocator(ring->slots.get_allocator().growth);
  std::destroy_at(ring);
  allocator.deallocate(ring, 1);
}

EventHub::StoredEvent::StoredEvent(utils::MemoryGrowth *growth)
    : type(Allocator<char>(growth)),
      source(Allocator<char>(growth)),
      fields(TextLess{}, Allocator<std::pair<const Text, Text>>(growth)) {}

void EventHub::StoredEvent::SetField(std::string_view key, std::string_view value) {
  auto at = fields.lower_bound(key);
  if (at != fields.end() && !fields.key_comp()(key, at->first)) {
    at->second.assign(value.data(), value.size());
    return;
  }
  const auto allocator = type.get_allocator();
  fields.emplace_hint(at, Text(key.data(), key.size(), allocator),
                      Text(value.data(), value.size(), allocator));
}

void EventHub::StoredEvent::SetField(std::string_view key, uint64_t value) {
  char digits[20];
  const auto converted = std::to_chars(digits, digits + sizeof(digits), value);
  SetField(key, std::string_view(digits, static_cast<size_t>(converted.ptr - digits)));
}

EventHub::EventHub(size_t log_capacity, size_t ring_capacity,
                   utils::MemoryGrowth *growth)
    : log_capacity_(log_capacity),
      growth_(growth),
      types_(Allocator<Type>(growth)),
      log_(Allocator<StoredEvent>(growth)),
      source_names_(Allocator<Text>(growth)) {
  Allocator<Ring> allocator(growth);
  for (auto &r : rings_) {
    Ring *ring = allocator.allocate(1);
    try {
      std::construct_at(ring, ring_capacity, growth);
    } catch (...) {
      allocator.deallocate(ring, 1);
      throw;
    }
    r.reset(ring);
  }
}

EventHub::~EventHub() = default;

uint32_t EventHub::RegisterType(const std::string &name,
                                const std::vector<std::string> &value_names,
                                const std::string &source_prefix) {
  std::lock_guard<std::mutex> guard(mutex_);
  for (size_t i = 0; i < types_.size(); i++) {
    if (std::string_view(types_[i].name) == name) {
      return static_cast<uint32_t>(i);
    }
  }
  const Allocator<char> text_allocator(growth_);
  ValueNames names{Allocator<Text>(growth_)};
  names.reserve(std::min<size_t>(value_names.size(), 6));
  for (size_t i = 0; i < std::min<size_t>(value_names.size(), 6); i++) {
    names.emplace_back(value_names[i].data(), value_names[i].size(), text_allocator);
  }
  types_.push_back({Text(name.data(), name.size(), text_allocator), std::move(names),
                   Text(source_prefix.data(), source_prefix.size(), text_allocator)});
  return static_cast<uint32_t>(types_.size() - 1);
}

uint32_t EventHub::NamedSource(const std::string &name) {
  std::lock_guard<std::mutex> guard(mutex_);
  for (size_t i = 0; i < source_names_.size(); i++) {
    if (std::string_view(source_names_[i]) == name) {
      return kNamedSource | static_cast<uint32_t>(i);
    }
  }
  source_names_.emplace_back(name.data(), name.size(), Allocator<char>(growth_));
  return kNamedSource | static_cast<uint32_t>(source_names_.size() - 1);
}

bool EventHub::Full(dataplane::WorkerId wid) const noexcept {
  if (wid.value() >= kMaxWorkers) {
    return true;
  }
  const Ring &r = *rings_[wid.value()];
  return r.tail.load(std::memory_order_relaxed) - r.head.load(std::memory_order_acquire) > r.mask;
}

bool EventHub::Post(dataplane::WorkerId wid, const WorkerEvent &event) noexcept {
  if (wid.value() >= kMaxWorkers) {
    return false;
  }
  Ring &r = *rings_[wid.value()];
  const uint64_t tail = r.tail.load(std::memory_order_relaxed);
  if (tail - r.head.load(std::memory_order_acquire) > r.mask) {
    r.refused_since++;
    r.refused.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  WorkerEvent &slot = r.slots[tail & r.mask];
  slot = event;
  slot.lost_before = r.refused_since;
  r.refused_since = 0;
  r.tail.store(tail + 1, std::memory_order_release);
  return true;
}

uint64_t EventHub::AppendLocked(StoredEvent event) {
  const uint64_t sequence = next_sequence_;
  event.sequence = sequence;
  event.time_ns = MonotonicNs();
  // No sequence is published until deque insertion succeeds. A failed
  // insertion leaves the retained log intact; the caller records its gap.
  log_.push_back(std::move(event));
  next_sequence_++;
  while (log_.size() > log_capacity_) {
    log_.pop_front();
  }
  return sequence;
}

uint64_t EventHub::LoseLocked() noexcept {
  memory_lost_.fetch_add(1, std::memory_order_relaxed);
  return next_sequence_++;
}

uint64_t EventHub::Emit(std::string type, std::string source,
                        std::map<std::string, std::string> fields, uint64_t generation) {
  uint64_t seq;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    try {
      StoredEvent event(growth_);
      event.type.assign(type.data(), type.size());
      event.source.assign(source.data(), source.size());
      event.generation = generation;
      for (const auto &[key, value] : fields) {
        event.SetField(key, value);
      }
      seq = AppendLocked(std::move(event));
    } catch (const std::bad_alloc &) {
      // Operational telemetry is best effort, never a failed committed RPC.
      seq = LoseLocked();
    }
  }
  cv_.notify_all();
  return seq;
}

size_t EventHub::DrainWorkers() {
  size_t moved = 0;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    for (int wid = 0; wid < kMaxWorkers; wid++) {
      Ring &r = *rings_[static_cast<size_t>(wid)];
      uint64_t head = r.head.load(std::memory_order_relaxed);
      const uint64_t tail = r.tail.load(std::memory_order_acquire);
      for (; head != tail; head++) {
        const WorkerEvent &w = r.slots[head & r.mask];
        if (w.lost_before != 0) {
          try {
            StoredEvent loss(growth_);
            loss.type = "bess.events_lost";
            loss.source = "worker";
            AppendNumber(loss.source, static_cast<uint64_t>(wid));
            loss.SetField("count", w.lost_before);
            AppendLocked(std::move(loss));
          } catch (const std::bad_alloc &) {
            LoseLocked();
          }
        }
        try {
          StoredEvent event(growth_);
          if (w.type < types_.size()) {
            const Type &type = types_[w.type];
            event.type = type.name;
            const uint32_t named = w.source & ~kNamedSource;
            if ((w.source & kNamedSource) != 0 && named < source_names_.size()) {
              event.source = source_names_[named];
            } else {
              event.source = type.source_prefix;
              AppendNumber(event.source, w.source);
            }
            for (size_t i = 0; i < type.value_names.size(); i++) {
              event.SetField(type.value_names[i], w.values[i]);
            }
          } else {
            event.type = "bess.unknown_event";
            AppendNumber(event.source, w.source);
          }
          event.SetField("worker", static_cast<uint64_t>(wid));
          AppendLocked(std::move(event));
        } catch (const std::bad_alloc &) {
          LoseLocked();
        }
        moved++;
      }
      r.head.store(head, std::memory_order_release);
    }
  }
  if (moved != 0) {
    cv_.notify_all();
  }
  return moved;
}

EventHub::ReadResult EventHub::Read(uint64_t from, size_t max, std::chrono::milliseconds wait) {
  ReadResult out;
  std::unique_lock<std::mutex> lock(mutex_);
  if (from == 0) {
    from = next_sequence_;
  }
  cv_.wait_for(lock, wait, [&] { return closed_ || next_sequence_ > from; });
  out.closed = closed_;
  auto at = std::lower_bound(
      log_.begin(), log_.end(), from,
      [](const StoredEvent &event, uint64_t sequence) { return event.sequence < sequence; });
  const uint64_t first = at == log_.end() ? next_sequence_ : at->sequence;
  if (from < first) {
    out.gap_from = from;
    out.gap_to = first;
    from = first;
  }
  out.next = from;
  for (; at != log_.end() && out.events.size() < max && at->sequence == out.next; ++at) {
    Event &event = out.events.emplace_back();
    event.sequence = at->sequence;
    event.time_ns = at->time_ns;
    event.generation = at->generation;
    event.type.assign(at->type.data(), at->type.size());
    event.source.assign(at->source.data(), at->source.size());
    for (const auto &[key, value] : at->fields) {
      event.fields.emplace(std::string(key.data(), key.size()),
                           std::string(value.data(), value.size()));
    }
    out.next++;
  }
  return out;
}

void EventHub::Close() {
  {
    std::lock_guard<std::mutex> guard(mutex_);
    closed_ = true;
  }
  cv_.notify_all();
}

uint64_t EventHub::next_sequence() const {
  std::lock_guard<std::mutex> guard(mutex_);
  return next_sequence_;
}

uint64_t EventHub::lost() const noexcept {
  uint64_t n = 0;
  for (const auto &r : rings_) {
    n += r->refused.load(std::memory_order_relaxed);
  }
  return n;
}

uint64_t EventHub::memory_lost() const noexcept {
  return memory_lost_.load(std::memory_order_relaxed);
}

}  // namespace bess::stats
