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
#include <string_view>
#include <vector>

#include "dataplane/worker_id.h"
#include "utils/dpdk_memory.h"

namespace bess::stats {

// Operational events (M25 phase 2, D-084): control-plane observations, never
// per-packet logs. Each has a sequence number (per daemon epoch; memory
// refusals leave explicit gaps), a type, a source, a monotonic time, the transaction
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
// every tick) moves what workers posted into the log. The log holds up to
// `log_capacity` events. Memory pressure can refuse an observation: its sequence
// is skipped and memory_lost() counts it, without affecting the observed command.
// Readers receive explicit gaps for both refused and aged-out observations.
//
// Readers (any thread, e.g. a WatchEvents stream): Read() waits up to a
// timeout for events at or after a sequence. Close() wakes every reader for
// good (daemon shutdown).
class EventHub {
 public:
  static constexpr int kMaxWorkers = 64;
  static constexpr size_t kDefaultLogCapacity = 16384;
  static constexpr size_t kDefaultRingCapacity = 256;

  // The log's events are heap objects (strings and a field map): about
  // 0.5 KiB each, so the default 16384 holds about 8 MiB when full. Worker
  // events reach the log only through DrainWorkers (the maintenance loop):
  // with the loop off, a worker's 257th event is refused and counted.
  // `growth`, when supplied by the owner, admits all retained storage and
  // must outlive this hub. Read() returns independent, caller-owned copies.
  explicit EventHub(size_t log_capacity = kDefaultLogCapacity,
                    size_t ring_capacity = kDefaultRingCapacity,
                    utils::MemoryGrowth *growth = nullptr);
  ~EventHub();
  EventHub(const EventHub &) = delete;
  EventHub &operator=(const EventHub &) = delete;

  // Control side, before workers post it: a worker event type and the names
  // of its values (at most 6). Returns its id. Idempotent per name.
  uint32_t RegisterType(const std::string &name, const std::vector<std::string> &value_names,
                        const std::string &source_prefix = "");

  // Control side: a source id that the log shows as `name` (a module
  // instance's name, say) instead of the type's prefix and a number.
  // Idempotent per name. Ids carry kNamedSource; plugins need
  // BESS_CAP_EVENT_SOURCES (an older bessd lacks it).
  static constexpr uint32_t kNamedSource = UINT32_C(1) << 31;
  uint32_t NamedSource(const std::string &name);

  // Worker `wid`: false when its ring is full (counted). A poster's own
  // source ids stay below kNamedSource.
  bool Post(dataplane::WorkerId wid, const WorkerEvent &event) noexcept;
  // Worker `wid`: whether its ring is full now (nothing counted), for a
  // poster that keeps what it would post for later (EventThrottle).
  bool Full(dataplane::WorkerId wid) const noexcept;

  // Control side. A memory-refused observation still consumes its returned
  // sequence and wakes readers. Retention allocation failures never propagate.
  uint64_t Emit(std::string type, std::string source,
                const std::map<std::string, std::string> &fields,
                uint64_t generation = 0);
  size_t DrainWorkers();

  struct ReadResult {
    std::vector<Event> events;
    // [gap_from, gap_to) were refused or aged out; events start at gap_to.
    // Each batch is contiguous: a later interior gap starts the next Read().
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
  uint64_t memory_lost() const noexcept;  // observations refused by log memory

 private:
  template <typename T>
  using Allocator = utils::AdmittedGlibcAllocator<T>;
  using Text = std::basic_string<char, std::char_traits<char>, Allocator<char>>;
  using ValueNames = std::vector<Text, Allocator<Text>>;
  struct TextLess {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
      return a < b;
    }
  };
  using Fields = std::map<Text, Text, TextLess,
                          Allocator<std::pair<const Text, Text>>>;
  struct StoredEvent {
    explicit StoredEvent(utils::MemoryGrowth *growth);
    void SetField(std::string_view key, std::string_view value);
    void SetField(std::string_view key, uint64_t value);
    uint64_t sequence = 0;
    uint64_t time_ns = 0;
    uint64_t generation = 0;
    Text type;
    Text source;
    Fields fields;
  };
  struct Ring;
  struct RingDelete {
    void operator()(Ring *ring) const noexcept;
  };
  struct Type {
    Text name;
    ValueNames value_names;
    Text source_prefix;
  };

  uint64_t AppendLocked(StoredEvent event);
  uint64_t LoseLocked() noexcept;

  const size_t log_capacity_;
  // The injected hook outlives all retained storage. Post and Full never use it.
  utils::MemoryGrowth *const growth_;
  std::array<std::unique_ptr<Ring, RingDelete>, kMaxWorkers> rings_;
  std::vector<Type, Allocator<Type>> types_;  // RegisterType, DrainWorkers
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<StoredEvent, Allocator<StoredEvent>> log_;
  uint64_t next_sequence_ = 1;
  std::atomic<uint64_t> memory_lost_{0};
  bool closed_ = false;
  std::vector<Text, Allocator<Text>> source_names_;  // ids without kNamedSource
};

}  // namespace bess::stats

#endif  // BESS_STATS_EVENT_HUB_H_
