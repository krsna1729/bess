// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_STATS_EVENT_HUB_H_
#define BESS_STATS_EVENT_HUB_H_

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace bess::stats {

// Operational events (M25 phase 2, D-084): control-plane observations, never
// per-packet logs. Each has a sequence number (per daemon epoch, gapless while
// the log holds it), a type, a source, a monotonic time, the transaction
// generation and string fields; the application gives types and fields their
// meaning.
struct Event {
  uint64_t sequence = 0;
  uint64_t time_ns = 0;
  uint64_t generation = 0;
  std::string type;
  std::string source;
  std::map<std::string, std::string> fields;
};

// What a worker posts: a registered type (its field names fixed at
// registration), a source id the poster chose, and up to six values. 64
// bytes, copied into a per-worker ring; the control side turns it into an
// Event.
struct WorkerEvent {
  uint32_t type = 0;    // from RegisterType
  uint32_t source = 0;  // the poster's (a module instance's id, a table's)
  uint64_t values[6] = {};
  uint64_t lost_before = 0;  // set by Post: events this worker's ring refused before this one
};
static_assert(sizeof(WorkerEvent) == 64);

// The event log and the per-worker rings that feed it.
//
// Workers: Post(wid, event) is a bounded single-producer single-consumer ring
// per worker; it never blocks and never allocates. A full ring refuses the
// event and counts it; the next accepted event carries the count, so loss is
// visible in the log as a "bess.events_lost" event.
//
// Control side: Emit() appends directly; DrainWorkers() (the maintenance loop,
// every tick) moves what workers posted into the log. The log holds the last
// `log_capacity` events; a reader that asks for older ones gets a gap.
//
// Readers (any thread, e.g. a WatchEvents stream): Read() waits up to a
// timeout for events at or after a sequence. Close() wakes every reader for
// good (daemon shutdown).
class EventHub {
 public:
  static constexpr int kMaxWorkers = 64;

  // The log's events are heap objects (strings and a field map): about
  // 0.5 KiB each, so the default 16384 holds about 8 MiB when full. Worker
  // events reach the log only through DrainWorkers (the maintenance loop):
  // with the loop off, a worker's 257th event is refused and counted.
  explicit EventHub(size_t log_capacity = 16384, size_t ring_capacity = 256);
  ~EventHub();
  EventHub(const EventHub &) = delete;
  EventHub &operator=(const EventHub &) = delete;

  // Control side, before workers post it: a worker event type and the names
  // of its values (at most 6). Returns its id. Idempotent per name.
  uint32_t RegisterType(const std::string &name, const std::vector<std::string> &value_names,
                        const std::string &source_prefix = "");

  // Worker `wid`: false when its ring is full (counted).
  bool Post(int wid, const WorkerEvent &event) noexcept;

  // Control side.
  uint64_t Emit(std::string type, std::string source, std::map<std::string, std::string> fields,
                uint64_t generation = 0);
  size_t DrainWorkers();

  struct ReadResult {
    std::vector<Event> events;
    // When `from` had aged out of the log: [gap_from, gap_to) were lost to
    // this reader; events start at gap_to.
    uint64_t gap_from = 0;
    uint64_t gap_to = 0;
    uint64_t next = 0;  // the sequence to ask for next
    bool closed = false;
  };
  // Events with sequence >= `from` (0: the next one emitted), at most `max`,
  // waiting up to `wait` for the first.
  ReadResult Read(uint64_t from, size_t max, std::chrono::milliseconds wait);
  void Close();

  uint64_t next_sequence() const;
  uint64_t lost() const noexcept;  // worker events refused by full rings, all workers

 private:
  struct Ring;
  struct Type {
    std::string name;
    std::vector<std::string> value_names;
    std::string source_prefix;
  };

  uint64_t AppendLocked(Event e);

  const size_t log_capacity_;
  std::array<std::unique_ptr<Ring>, kMaxWorkers> rings_;
  std::vector<Type> types_;  // control side (RegisterType, DrainWorkers)
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Event> log_;
  uint64_t next_sequence_ = 1;
  bool closed_ = false;
};

}  // namespace bess::stats

#endif  // BESS_STATS_EVENT_HUB_H_
