// SPDX-License-Identifier: BSD-3-Clause

#include "stats/event_hub.h"

#include <bit>
#include <time.h>

namespace bess::stats {

namespace {

uint64_t MonotonicNs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);
}

}  // namespace

// One worker's ring: the worker produces, DrainWorkers consumes.
struct EventHub::Ring {
  explicit Ring(size_t capacity)
      : mask(std::bit_ceil(std::max<size_t>(capacity, 2)) - 1),
        slots(std::make_unique<WorkerEvent[]>(mask + 1)) {}
  const size_t mask;
  std::unique_ptr<WorkerEvent[]> slots;
  alignas(64) std::atomic<uint64_t> head{0};  // consumer
  alignas(64) std::atomic<uint64_t> tail{0};  // producer
  uint64_t refused_since = 0;                 // producer only
  std::atomic<uint64_t> refused{0};           // total, for metrics
};

EventHub::EventHub(size_t log_capacity, size_t ring_capacity) : log_capacity_(log_capacity) {
  for (auto &r : rings_) {
    r = std::make_unique<Ring>(ring_capacity);
  }
}

EventHub::~EventHub() = default;

uint32_t EventHub::RegisterType(const std::string &name,
                                const std::vector<std::string> &value_names,
                                const std::string &source_prefix) {
  std::lock_guard<std::mutex> guard(mutex_);
  for (size_t i = 0; i < types_.size(); i++) {
    if (types_[i].name == name) {
      return static_cast<uint32_t>(i);
    }
  }
  std::vector<std::string> names(value_names.begin(),
                                 value_names.begin() + std::min<size_t>(value_names.size(), 6));
  types_.push_back({name, std::move(names), source_prefix});
  return static_cast<uint32_t>(types_.size() - 1);
}

bool EventHub::Post(int wid, const WorkerEvent &event) noexcept {
  if (wid < 0 || wid >= kMaxWorkers) {
    return false;
  }
  Ring &r = *rings_[static_cast<size_t>(wid)];
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

uint64_t EventHub::AppendLocked(Event e) {
  e.sequence = next_sequence_++;
  if (e.time_ns == 0) {
    e.time_ns = MonotonicNs();
  }
  log_.push_back(std::move(e));
  while (log_.size() > log_capacity_) {
    log_.pop_front();
  }
  return log_.back().sequence;
}

uint64_t EventHub::Emit(std::string type, std::string source,
                        std::map<std::string, std::string> fields, uint64_t generation) {
  uint64_t seq;
  {
    std::lock_guard<std::mutex> guard(mutex_);
    seq = AppendLocked(Event{0, 0, generation, std::move(type), std::move(source), std::move(fields)});
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
          AppendLocked(Event{0, 0, 0, "bess.events_lost", "worker" + std::to_string(wid),
                             {{"count", std::to_string(w.lost_before)}}});
        }
        Event e;
        if (w.type < types_.size()) {
          const Type &t = types_[w.type];
          e.type = t.name;
          e.source = t.source_prefix + std::to_string(w.source);
          for (size_t i = 0; i < t.value_names.size(); i++) {
            e.fields[t.value_names[i]] = std::to_string(w.values[i]);
          }
        } else {
          e.type = "bess.unknown_event";
          e.source = std::to_string(w.source);
        }
        e.fields["worker"] = std::to_string(wid);
        AppendLocked(std::move(e));
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
  const uint64_t oldest = log_.empty() ? next_sequence_ : log_.front().sequence;
  if (from < oldest) {
    out.gap_from = from;
    out.gap_to = oldest;
    from = oldest;
  }
  for (size_t i = static_cast<size_t>(from - oldest); i < log_.size() && out.events.size() < max;
       i++) {
    out.events.push_back(log_[i]);
  }
  out.next = out.events.empty() ? from : out.events.back().sequence + 1;
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

}  // namespace bess::stats
