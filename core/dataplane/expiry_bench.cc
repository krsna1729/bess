// SPDX-License-Identifier: BSD-3-Clause

// Expiry mechanisms compared (roadmap M10, Decision D-053).
//
// The mechanism was chosen by running the same operations through each
// candidate with the same interface, not by preference:
//
//   wheel         ExpiryWheel (dataplane/expiry_wheel.h): hierarchical timing
//                 wheel over a fixed node array, refresh to a later deadline
//                 is a store into the node ("lazy"), granularity 2^20 ticks
//   wheel-eager   the same wheel, but a refresh cancels and re-arms (unlink +
//                 relink; the refresh a plain timing wheel does)
//   hashed        a single-level hashed wheel (8192 slots of 2^20 ticks): an
//                 entry sits in slot (deadline / 2^20) mod 8192 and is skipped
//                 on each revolution until due. A performance baseline: it is
//                 not a complete implementation (an entry armed into a slot the
//                 poll has already passed waits for the next revolution)
//   heap          a binary min-heap of (deadline, id) with lazy deletion and
//                 lazy refresh (a later deadline is a store; the record is
//                 re-pushed when popped early)
//   scan          the periodic-scan baseline: one deadline per timer in a flat
//                 array and a poll examines the next `budget` entries from a
//                 cursor (what bridge.cc and packet_store.cc do, with a budget)
//   scan-embedded the same with each deadline 64 bytes from the next, as when
//                 the scan walks cache-line-sized State records
//   rte_timer     DPDK's rte_timer (per-lcore skip list, no per-poll budget,
//                 driven by the real TSC, so its distributions are scaled: see
//                 RteTimer)
//
// Every candidate keeps one timer per index 0..N-1 (the owner's flow slot) and
// offers Schedule/Refresh/Cancel/Poll, so the measured differences are the
// mechanisms'. A tick is a nanosecond.
//
//  * BM_Lifecycle   arm all N timers, then cancel all N (ns per operation)
//  * BM_Refresh     refresh a random timer to now + 30 s (uniform over all, or a
//                   hot 1% subset), now advancing 100 ns per refresh
//  * BM_PollNone    a poll with nothing due, N timers armed 1-2 hours out
//  * BM_Drain       everything armed expires and is drained by budgeted polls:
//                   ns per expiry, and the longest and 99.9th percentile single
//                   poll (the "worst budgeted poll"); distributions: all the
//                   same instant, uniform over 1 s, over 5 min, over 1-2 hours
//
// Bytes per timer are reported as `bytes_per_timer` (engine memory only) and
// `owner_bytes` (what the owner keeps per timer: the engine handle).

// rte_timer is stable in DPDK 25.11; rte_hash is not used here.
#include <benchmark/benchmark.h>

#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_timer.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "dataplane/expiry_wheel.h"
#include "dpdk.h"

namespace {

using Clock = std::chrono::steady_clock;

// The EAL is needed only for rte_timer (its clock, lcore id and a memzone); it
// is brought up when the first benchmark runs, after flags and logging are
// initialized, in the no-hugepage mode (a 512 MB heap, a small address space).
void EnsureDpdk() {
  if (!bess::IsDpdkInitialized()) bess::InitDpdk(0);
}

constexpr uint64_t kNs = 1;
constexpr uint64_t kUs = 1000 * kNs;
constexpr uint64_t kMs = 1000 * kUs;
constexpr uint64_t kSec = 1000 * kMs;
constexpr unsigned kShift = 20;  // 2^20 ns = 1.05 ms wheel granularity

enum Dist : int64_t { kSame = 0, kOneSec = 1, kFiveMin = 2, kLongIdle = 3 };

const char* DistName(int64_t d) {
  switch (d) {
    case kSame: return "same";
    case kOneSec: return "uniform_1s";
    case kFiveMin: return "uniform_5min";
    default: return "long_idle_1h_2h";
  }
}

// Offsets from the start time at which timer i expires, and the time by which
// all are due.
struct Deadlines {
  std::vector<uint64_t> offset;
  uint64_t last = 0;
};

Deadlines MakeDeadlines(size_t n, int64_t dist, uint64_t seed = 42) {
  std::mt19937_64 rng(seed);
  Deadlines out;
  out.offset.resize(n);
  for (size_t i = 0; i < n; i++) {
    uint64_t o = 0;
    switch (dist) {
      case kSame: o = 500 * kMs; break;
      case kOneSec: o = 1 + rng() % kSec; break;
      case kFiveMin: o = 1 + rng() % (300 * kSec); break;
      default: o = 3600 * kSec + rng() % (3600 * kSec); break;
    }
    out.offset[i] = o;
    out.last = std::max(out.last, o);
  }
  return out;
}

// -- candidates ----------------------------------------------------------------------

// The engine under test.
template <bool kLazy>
class WheelAdapter {
 public:
  static constexpr bool kBudgeted = true;
  static constexpr bool kRealClock = false;
  explicit WheelAdapter(size_t n, uint64_t start)
      : wheel_(std::move(bess::dataplane::ExpiryWheel<uint64_t>::Create(n, start, kShift)).value()),
        handle_(n) {}
  void SetNow(uint64_t) {}
  void Schedule(size_t i, uint64_t deadline) { handle_[i] = wheel_->Schedule(deadline, i); }
  void Refresh(size_t i, uint64_t deadline) {
    if constexpr (kLazy) {
      wheel_->Refresh(handle_[i], deadline);
    } else {
      wheel_->Cancel(handle_[i]);
      handle_[i] = wheel_->Schedule(deadline, i);
    }
  }
  void Cancel(size_t i) { wheel_->Cancel(handle_[i]); }
  size_t Poll(uint64_t now, size_t budget) {
    const auto r = wheel_->Poll(now, budget, [&](const uint64_t&) noexcept {});
    work_ = r.work();
    return r.fired;
  }
  size_t work() const { return work_; }
  size_t bytes(size_t n) const { return wheel_->memory_bytes() / std::max<size_t>(n, 1); }
  static constexpr size_t owner_bytes() { return sizeof(bess::dataplane::ExpiryHandle); }
  void PrepareDrain(uint64_t) {}

 private:
  std::unique_ptr<bess::dataplane::ExpiryWheel<uint64_t>> wheel_;
  std::vector<bess::dataplane::ExpiryHandle> handle_;
  size_t work_ = 0;
};

class HashedWheel {
 public:
  static constexpr bool kBudgeted = true;
  static constexpr bool kRealClock = false;
  static constexpr uint32_t kSlots = 8192;
  explicit HashedWheel(size_t n, uint64_t start)
      : nodes_(kSlots + n), tick_(start >> kShift), cursor_(0) {
    for (uint32_t s = 0; s < kSlots; s++) nodes_[s].prev = nodes_[s].next = s;
    cursor_ = nodes_[SlotOf(tick_)].next;
  }
  void SetNow(uint64_t) {}
  void Schedule(size_t i, uint64_t deadline) {
    const uint32_t node = kSlots + static_cast<uint32_t>(i);
    nodes_[node].deadline = deadline;
    Link(SlotOf((deadline + (uint64_t{1} << kShift) - 1) >> kShift), node);
  }
  void Refresh(size_t i, uint64_t deadline) {  // eager: relink into the new slot
    const uint32_t node = kSlots + static_cast<uint32_t>(i);
    if (cursor_ == node) cursor_ = nodes_[node].next;
    Unlink(node);
    Schedule(i, deadline);
  }
  void Cancel(size_t i) {
    const uint32_t node = kSlots + static_cast<uint32_t>(i);
    if (cursor_ == node) cursor_ = nodes_[node].next;
    Unlink(node);
  }
  size_t Poll(uint64_t now, size_t budget) {
    const uint64_t target = now >> kShift;
    size_t fired = 0, work = 0;
    while (work < budget) {
      const uint32_t head = SlotOf(tick_);
      if (cursor_ == head) {  // slot finished
        if (tick_ >= target) break;
        tick_++;
        work++;  // passing a slot costs a unit, empty or not
        cursor_ = nodes_[SlotOf(tick_)].next;
        continue;
      }
      const uint32_t node = cursor_;
      cursor_ = nodes_[node].next;
      work++;
      if (nodes_[node].deadline <= now) {
        Unlink(node);
        fired++;
      }
    }
    work_ = work;
    return fired;
  }
  size_t work() const { return work_; }
  size_t bytes(size_t n) const { return (nodes_.size() * sizeof(Node)) / std::max<size_t>(n, 1); }
  static constexpr size_t owner_bytes() { return 0; }
  void PrepareDrain(uint64_t) {}

 private:
  struct Node {
    uint32_t prev = 0, next = 0;
    uint64_t deadline = 0;
  };
  static uint32_t SlotOf(uint64_t tick) { return static_cast<uint32_t>(tick & (kSlots - 1)); }
  void Link(uint32_t head, uint32_t node) {
    const uint32_t last = nodes_[head].prev;
    nodes_[node].prev = last;
    nodes_[node].next = head;
    nodes_[last].next = node;
    nodes_[head].prev = node;
  }
  void Unlink(uint32_t node) {
    nodes_[nodes_[node].prev].next = nodes_[node].next;
    nodes_[nodes_[node].next].prev = nodes_[node].prev;
  }
  std::vector<Node> nodes_;
  uint64_t tick_;
  uint32_t cursor_;
  size_t work_ = 0;
};

class HeapTimers {
 public:
  static constexpr bool kBudgeted = true;
  static constexpr bool kRealClock = false;
  explicit HeapTimers(size_t n, uint64_t) : deadline_(n, 0), gen_(n, 0), live_(n, 0) {
    heap_.reserve(n + n / 4);
  }
  void SetNow(uint64_t) {}
  void Schedule(size_t i, uint64_t deadline) {
    deadline_[i] = deadline;
    gen_[i]++;
    live_[i] = 1;
    Push({deadline, static_cast<uint32_t>(i), gen_[i]});
  }
  void Refresh(size_t i, uint64_t deadline) {
    const bool later = deadline >= deadline_[i];
    deadline_[i] = deadline;
    if (!later) Push({deadline, static_cast<uint32_t>(i), gen_[i]});
  }
  void Cancel(size_t i) {
    live_[i] = 0;
    gen_[i]++;
  }
  size_t Poll(uint64_t now, size_t budget) {
    size_t fired = 0, work = 0;
    while (work < budget && !heap_.empty() && heap_.front().deadline <= now) {
      std::pop_heap(heap_.begin(), heap_.end(), Later);
      const Rec r = heap_.back();
      heap_.pop_back();
      work++;
      if (r.gen != gen_[r.id] || !live_[r.id]) continue;  // stale
      if (deadline_[r.id] > r.deadline) {                  // refreshed since
        Push({deadline_[r.id], r.id, r.gen});
        continue;
      }
      live_[r.id] = 0;
      gen_[r.id]++;
      fired++;
    }
    work_ = work;
    return fired;
  }
  size_t work() const { return work_; }
  size_t bytes(size_t n) const {
    return (heap_.capacity() * sizeof(Rec) + deadline_.size() * 8 + gen_.size() * 4 +
            live_.size()) / std::max<size_t>(n, 1);
  }
  static constexpr size_t owner_bytes() { return 0; }
  void PrepareDrain(uint64_t) {}

 private:
  struct Rec {
    uint64_t deadline;
    uint32_t id, gen;
  };
  static bool Later(const Rec& a, const Rec& b) { return a.deadline > b.deadline; }
  void Push(const Rec& r) {
    heap_.push_back(r);
    std::push_heap(heap_.begin(), heap_.end(), Later);
  }
  std::vector<Rec> heap_;
  std::vector<uint64_t> deadline_;
  std::vector<uint32_t> gen_;
  std::vector<uint8_t> live_;
  size_t work_ = 0;
};

// kStride is the distance in bytes between one timer's deadline and the next:
// 8 is the best case for a scan (a compact array of deadlines of its own); 64
// is a deadline embedded in a cache-line-sized State, as it is when the scan
// walks the flow table's own records.
template <size_t kStride>
class ScanTimers {
 public:
  static constexpr bool kBudgeted = true;
  static constexpr bool kRealClock = false;
  static constexpr size_t kWords = kStride / 8;
  explicit ScanTimers(size_t n, uint64_t) : deadline_(n * kWords, kIdle), n_(n) {}
  void SetNow(uint64_t) {}
  void Schedule(size_t i, uint64_t deadline) { deadline_[i * kWords] = deadline; }
  void Refresh(size_t i, uint64_t deadline) { deadline_[i * kWords] = deadline; }
  void Cancel(size_t i) { deadline_[i * kWords] = kIdle; }
  size_t Poll(uint64_t now, size_t budget) {
    size_t fired = 0;
    for (size_t k = 0; k < budget; k++) {
      uint64_t& d = deadline_[cursor_ * kWords];
      if (d <= now) {
        d = kIdle;
        fired++;
      }
      if (++cursor_ == n_) cursor_ = 0;
    }
    work_ = budget;
    return fired;
  }
  size_t work() const { return work_; }
  size_t bytes(size_t n) const { return deadline_.size() * 8 / std::max<size_t>(n, 1); }
  static constexpr size_t owner_bytes() { return 0; }
  void PrepareDrain(uint64_t) {}

 private:
  static constexpr uint64_t kIdle = UINT64_MAX;
  std::vector<uint64_t> deadline_;
  size_t n_;
  size_t cursor_ = 0;
  size_t work_ = 0;
};

// DPDK's rte_timer. It reads the real TSC and takes no budget, so `now` and
// `budget` are ignored by Poll, and the distributions that would take seconds or
// hours of real time cannot be replayed in virtual time. For the drain benchmark
// the offsets are compressed into a spread of real time that is long compared
// with the time the fill takes (about 3 microseconds per timer, capped at 4 s;
// 1 ms at least): rte_timer takes its deadlines relative to the TSC at the call,
// so with a shorter spread the timers would expire in the order they were armed
// (sequential memory, an unrealistically kind drain). The offsets keep their
// order and proportions, and the benchmark waits out the spread before the
// drain, untimed. Timers that must stay pending use their real distance.
class RteTimer {
 public:
  static constexpr bool kBudgeted = false;
  static constexpr bool kRealClock = true;
  explicit RteTimer(size_t n, uint64_t /*start*/)
      : timers_(n),
        per_ns_(static_cast<double>(rte_get_tsc_hz()) / 1e9),
        spread_(std::clamp<uint64_t>(n * 9000, 3'000'000, 12'000'000'000ull)) {
    rte_timer_subsystem_init();  // EEXIST on repeat is fine
    for (auto& t : timers_) rte_timer_init(&t);
    count_ = 0;
  }
  ~RteTimer() {
    for (auto& t : timers_) rte_timer_stop(&t);
  }
  void SetNow(uint64_t now) { now_ = now; }
  void Schedule(size_t i, uint64_t deadline) {
    const uint64_t d = deadline > now_ ? deadline - now_ : 1;
    rte_timer_reset(&timers_[i], static_cast<uint64_t>(d * per_ns_) + 1, SINGLE,
                    rte_lcore_id(), &Fire, nullptr);
  }
  // Used by the drain benchmark: all timers are due within `spread_` cycles.
  void ScheduleCompressed(size_t i, uint64_t offset, uint64_t span) {
    const uint64_t cycles =
        1 + (span == 0 ? 0
                       : static_cast<uint64_t>(static_cast<double>(offset) /
                                               static_cast<double>(span) *
                                               static_cast<double>(spread_)));
    last_insert_ = rte_get_timer_cycles();
    rte_timer_reset(&timers_[i], cycles, SINGLE, rte_lcore_id(), &Fire, nullptr);
  }
  void Refresh(size_t i, uint64_t deadline) { Schedule(i, deadline); }
  void Cancel(size_t i) { rte_timer_stop(&timers_[i]); }
  size_t Poll(uint64_t, size_t) {
    count_ = 0;
    rte_timer_manage();
    work_ = count_;
    return count_;
  }
  size_t work() const { return work_; }
  size_t bytes(size_t) const { return sizeof(rte_timer); }
  static constexpr size_t owner_bytes() { return 0; }
  void PrepareDrain(uint64_t) {
    const uint64_t until = last_insert_ + spread_ + 3'000'000;
    while (rte_get_timer_cycles() < until) {
    }
  }

 private:
  static void Fire(rte_timer*, void*) { count_++; }
  std::vector<rte_timer> timers_;
  double per_ns_;
  uint64_t spread_;
  uint64_t last_insert_ = 0;
  uint64_t now_ = 0;
  size_t work_ = 0;
  static inline size_t count_ = 0;
};

// -- benchmarks ------------------------------------------------------------------------

template <typename A>
void Fill(A& a, const Deadlines& d, uint64_t start, bool compress = false) {
  a.SetNow(start);
  for (size_t i = 0; i < d.offset.size(); i++) {
    if constexpr (A::kRealClock) {
      if (compress) {
        a.ScheduleCompressed(i, d.offset[i], d.last);
        continue;
      }
    }
    a.Schedule(i, start + d.offset[i]);
  }
}

void ReportBytes(benchmark::State& state, size_t bytes, size_t owner) {
  state.counters["bytes_per_timer"] = static_cast<double>(bytes);
  state.counters["owner_bytes"] = static_cast<double>(owner);
}

template <typename A>
void BM_Lifecycle(benchmark::State& state) {
  EnsureDpdk();
  const size_t n = static_cast<size_t>(state.range(0));
  const int64_t dist = state.range(1);
  const Deadlines d = MakeDeadlines(n, dist);
  constexpr uint64_t kStart = 1'000'000'000;
  A a(n, kStart);
  double sched = 0, cancel = 0;
  size_t iters = 0;
  for (auto _ : state) {
    a.SetNow(kStart);
    const auto t0 = Clock::now();
    for (size_t i = 0; i < n; i++) a.Schedule(i, kStart + d.offset[i]);
    const auto t1 = Clock::now();
    for (size_t i = 0; i < n; i++) a.Cancel(i);
    const auto t2 = Clock::now();
    sched += std::chrono::duration<double, std::nano>(t1 - t0).count();
    cancel += std::chrono::duration<double, std::nano>(t2 - t1).count();
    iters++;
    state.SetIterationTime(std::chrono::duration<double>(t2 - t0).count());
  }
  state.counters["ns_schedule"] = sched / (iters * n);
  state.counters["ns_cancel"] = cancel / (iters * n);
  ReportBytes(state, a.bytes(n), a.owner_bytes());
}

template <typename A>
void BM_Refresh(benchmark::State& state) {
  EnsureDpdk();
  const size_t n = static_cast<size_t>(state.range(0));
  const bool hot = state.range(1) != 0;
  const Deadlines d = MakeDeadlines(n, kFiveMin);
  constexpr uint64_t kStart = 1'000'000'000;
  constexpr uint64_t kTimeout = 30 * kSec;
  A a(n, kStart);
  Fill(a, d, kStart);
  // 262,144 ids, replayed: uniform over all timers, or over the first 1%.
  std::mt19937_64 rng(7);
  std::vector<uint32_t> ids(1 << 18);
  const size_t span = hot ? std::max<size_t>(n / 100, 1) : n;
  for (auto& id : ids) id = static_cast<uint32_t>(rng() % span);
  uint64_t now = kStart;
  size_t k = 0;
  for (auto _ : state) {
    for (int b = 0; b < 1024; b++) {
      now += 100;
      a.SetNow(now);
      a.Refresh(ids[k++ & (ids.size() - 1)], now + kTimeout);
    }
  }
  state.SetItemsProcessed(state.iterations() * 1024);
  state.counters["ns_per_refresh"] =
      benchmark::Counter(1024.0 * state.iterations(),
                         benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  ReportBytes(state, a.bytes(n), a.owner_bytes());
}

template <typename A>
void BM_PollNone(benchmark::State& state) {
  EnsureDpdk();
  const size_t n = static_cast<size_t>(state.range(0));
  const size_t budget = 64;
  const Deadlines d = MakeDeadlines(n, kLongIdle);
  constexpr uint64_t kStart = 1'000'000'000;
  A a(n, kStart);
  Fill(a, d, kStart);
  uint64_t now = kStart;
  for (auto _ : state) {
    now += 250 * kUs;
    a.SetNow(now);
    benchmark::DoNotOptimize(a.Poll(now, budget));
  }
  ReportBytes(state, a.bytes(n), a.owner_bytes());
}

template <typename A>
void BM_Drain(benchmark::State& state) {
  EnsureDpdk();
  const size_t n = static_cast<size_t>(state.range(0));
  const int64_t dist = state.range(1);
  const size_t budget = static_cast<size_t>(state.range(2));
  const Deadlines d = MakeDeadlines(n, dist);
  constexpr uint64_t kStart = 1'000'000'000;
  A a(n, kStart);
  const double tsc_per_ns = static_cast<double>(rte_get_tsc_hz()) / 1e9;
  std::vector<uint64_t> poll_ticks;
  double total_ns = 0, worst_ns = 0, p999_ns = 0, polls = 0, worst_work = 0;
  size_t iters = 0;
  uint64_t base = kStart;  // time only moves forward, across iterations too
  for (auto _ : state) {
    Fill(a, d, base, /*compress=*/true);
    const uint64_t end = base + d.last + 4 * (uint64_t{1} << kShift);
    base = end + 1;
    a.SetNow(end);
    a.PrepareDrain(end);
    poll_ticks.clear();
    size_t fired = 0;
    const auto t0 = Clock::now();
    while (fired < n) {
      // rte_rdtsc: the counter rte_get_tsc_hz() (tsc_per_ns above) measures.
      const uint64_t p0 = rte_rdtsc();
      fired += a.Poll(end, budget);
      poll_ticks.push_back(rte_rdtsc() - p0);
      worst_work = std::max<double>(worst_work, static_cast<double>(a.work()));
      if (poll_ticks.size() > 50'000'000) break;  // a candidate that cannot finish
    }
    const auto t1 = Clock::now();
    state.SetIterationTime(std::chrono::duration<double>(t1 - t0).count());
    total_ns += std::chrono::duration<double, std::nano>(t1 - t0).count();
    std::sort(poll_ticks.begin(), poll_ticks.end());
    worst_ns = std::max(worst_ns, poll_ticks.back() / tsc_per_ns);
    p999_ns = std::max(p999_ns,
                       poll_ticks[std::min(poll_ticks.size() - 1,
                                           static_cast<size_t>(poll_ticks.size() * 0.999))] /
                           tsc_per_ns);
    polls += static_cast<double>(poll_ticks.size());
    iters++;
  }
  state.counters["ns_per_expiry"] = total_ns / (iters * n);
  state.counters["worst_poll_ns"] = worst_ns;
  state.counters["p999_poll_ns"] = p999_ns;
  state.counters["polls"] = polls / iters;
  state.counters["max_work_per_poll"] = worst_work;
  ReportBytes(state, a.bytes(n), a.owner_bytes());
}

// -- registration ----------------------------------------------------------------------

template <typename A>
void RegisterAll(const std::string& name, const std::vector<int64_t>& sizes,
                 int64_t max_long_idle, bool lifecycle_all_dists) {
  auto unit = benchmark::kNanosecond;
  for (int64_t n : sizes) {
    for (int64_t dist : {int64_t{kSame}, int64_t{kOneSec}, int64_t{kFiveMin}, int64_t{kLongIdle}}) {
      if (!lifecycle_all_dists && dist != kFiveMin) continue;
      benchmark::RegisterBenchmark(("BM_Lifecycle/" + name + "/" + DistName(dist)).c_str(),
                                   BM_Lifecycle<A>)
          ->Args({n, dist})->UseManualTime()->Unit(unit)->Iterations(3);
    }
    for (int64_t hot : {0, 1}) {
      benchmark::RegisterBenchmark(
          ("BM_Refresh/" + name + (hot ? "/hot_1pct" : "/uniform")).c_str(), BM_Refresh<A>)
          ->Args({n, hot})->Unit(unit)->MinTime(0.2);
    }
    benchmark::RegisterBenchmark(("BM_PollNone/" + name).c_str(), BM_PollNone<A>)
        ->Args({n})->Unit(unit)->MinTime(0.2);
    for (int64_t dist : {int64_t{kSame}, int64_t{kOneSec}, int64_t{kFiveMin}, int64_t{kLongIdle}}) {
      if (dist == kLongIdle && n > max_long_idle) continue;
      for (int64_t budget : A::kBudgeted ? std::vector<int64_t>{64, 1024} : std::vector<int64_t>{0}) {
        benchmark::RegisterBenchmark(("BM_Drain/" + name + "/" + DistName(dist)).c_str(),
                                     BM_Drain<A>)
            ->Args({n, dist, budget})->UseManualTime()->Unit(unit)->Iterations(3);
      }
    }
  }
}

[[maybe_unused]] const bool kRegistered = [] {
  const std::vector<int64_t> sizes = {1 << 10, 1 << 16, 1 << 20};
  RegisterAll<WheelAdapter<true>>("wheel", sizes, INT64_MAX, true);
  RegisterAll<WheelAdapter<false>>("wheel-eager", sizes, INT64_MAX, false);
  // The hashed wheel visits every timer once per revolution (8,192 slots of
  // about 1 ms): over an hour that is 440 revolutions, so 1M timers need
  // 440M visits. Not run at that size.
  RegisterAll<HashedWheel>("hashed", sizes, 1 << 16, true);
  RegisterAll<HeapTimers>("heap", sizes, INT64_MAX, true);
  RegisterAll<ScanTimers<8>>("scan", sizes, INT64_MAX, true);
  RegisterAll<ScanTimers<64>>("scan-embedded", sizes, INT64_MAX, false);
  RegisterAll<RteTimer>("rte_timer", sizes, INT64_MAX, true);
  if (std::getenv("EXPIRY_BENCH_LARGE") != nullptr) {
    // 10M timers: the two candidates that fit comfortably (320 MB and 80 MB).
    const std::vector<int64_t> large = {10'000'000};
    RegisterAll<WheelAdapter<true>>("wheel", large, INT64_MAX, false);
    RegisterAll<ScanTimers<8>>("scan", large, INT64_MAX, false);
  }
  return true;
}();

}  // namespace
