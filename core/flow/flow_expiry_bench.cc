// SPDX-License-Identifier: BSD-3-Clause

// What expiry costs a packet loop (roadmap M10, Decision D-053): batches of 32
// lookups in a WorkerFlowTable, each hit followed by the per-packet work of one
// of these ways of keeping a flow alive:
//
//   none           the loop with no timer at all: packets++ on the State
//   owner_store    State::last_seen = now, one store into the line the lookup
//                  already read; the engine is not called (the engine's record
//                  is only a lower bound and the poll callback re-arms it)
//   engine_lazy    ExpiryWheel::Refresh(handle, now + timeout): a store into the
//                  engine's node, a second array
//   engine_eager   Cancel + Schedule: the unlink/relink refresh of a plain
//                  timing wheel
//   *_poll         the same, plus one Poll(now, 16) per batch (the call a worker
//                  makes between batches), delivering nothing: no flow is idle
//
// 32 bytes of State, an 8-byte key, uniform random packets over the keys; time
// advances 33 ns per packet (a 30 Mpps stream). A packet whose flow is gone
// (expired, or never created) creates it, so the table stays near its size and
// the *_poll modes include the cost of erasing idle flows and of creating them
// again. The idle timeout is the second argument, in milliseconds: 100 ms is a
// stress case (the wheel visits every flow's record every 100 ms of simulated
// time, and with 1M flows a few percent are idle for that long at any moment);
// 10 s is a typical one. "ns_per_packet" is the whole loop per packet;
// `visited` and `erased` count the poll callbacks and the flows they erased.

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <vector>

#include "dataplane/expiry_wheel.h"
#include "flow/worker_flow_table.h"

namespace {

using bess::dataplane::ExpiryHandle;
using bess::dataplane::ExpiryWheel;
using bess::dataplane::kNoExpiry;
using bess::flow::FlowHandle;

constexpr uint64_t kPerPacket = 33;
constexpr unsigned kShift = 20;

enum Mode : int64_t {
  kNone = 0,
  kOwnerStore,
  kEngineLazy,
  kEngineEager,
  kOwnerStorePoll,
  kEngineLazyPoll,
  kEngineEagerPoll,
};

const char* ModeName(int64_t m) {
  static const char* names[] = {"none",        "owner_store",     "engine_lazy",
                                "engine_eager", "owner_store_poll", "engine_lazy_poll",
                                "engine_eager_poll"};
  return names[m];
}

struct State {
  uint64_t last_seen = 0;
  ExpiryHandle timer = kNoExpiry;
  uint64_t packets = 0;
  FlowHandle self{};  // what a re-armed timer delivers
};
static_assert(sizeof(State) == 32);

using Wheel = ExpiryWheel<FlowHandle>;

struct TimerObserver {
  Wheel* wheel = nullptr;
  const uint64_t* now = nullptr;
  uint64_t timeout = 0;
  void OnCreate(FlowHandle h, State& s) noexcept {
    s.last_seen = *now;
    s.self = h;
    s.timer = wheel->Schedule(*now + timeout, h);
  }
  void OnErase(FlowHandle, State& s) noexcept { wheel->Cancel(s.timer); }
  void OnFull() noexcept {}
};
struct TimerTraits : bess::flow::DefaultFlowTableTraits {
  using Observer = TimerObserver;
  using Owner = bess::flow::UncheckedOwner;
};
struct PlainTraits : bess::flow::DefaultFlowTableTraits {
  using Owner = bess::flow::UncheckedOwner;
};

template <typename Traits>
using Table = bess::flow::WorkerFlowTable<uint64_t, State, bess::flow::DefaultFlowHash<uint64_t>,
                                          bess::flow::DefaultFlowEqual<uint64_t>, Traits>;

// Mode-specific per-hit work.
template <int64_t kMode>
inline void Touch(State* s, Wheel* wheel, uint64_t now, uint64_t timeout) {
  s->packets++;
  if constexpr (kMode == kOwnerStore || kMode == kOwnerStorePoll) {
    s->last_seen = now;
  } else if constexpr (kMode == kEngineLazy || kMode == kEngineLazyPoll) {
    wheel->Refresh(s->timer, now + timeout);
  } else if constexpr (kMode == kEngineEager || kMode == kEngineEagerPoll) {
    wheel->Cancel(s->timer);
    s->timer = wheel->Schedule(now + timeout, s->self);
  }
}

template <int64_t kMode>
void BM_PacketLoop(benchmark::State& state) {
  const size_t n = static_cast<size_t>(state.range(0));
  const uint64_t timeout = static_cast<uint64_t>(state.range(1)) * 1'000'000;  // ms to ns
  uint64_t now = 1'000'000'000;
  std::unique_ptr<Wheel> wheel;
  using T = std::conditional_t<(kMode == kNone), Table<PlainTraits>, Table<TimerTraits>>;
  std::unique_ptr<T> table;
  if constexpr (kMode == kNone) {
    table = std::move(*T::Create(n));
  } else {
    wheel = std::move(Wheel::Create(n, now, kShift)).value();
    TimerObserver obs;
    obs.wheel = wheel.get();
    obs.now = &now;
    obs.timeout = timeout;
    table = std::move(*T::Create(n, {}, {}, obs));
  }
  for (uint64_t k = 1; k <= n; k++) {
    table->Emplace(k);
  }
  // Query stream: 4M uniform random keys, replayed (4 per flow at 1M, so every
  // flow is touched; a shorter stream would leave most of a large table idle).
  std::mt19937_64 rng(5);
  std::vector<uint64_t> keys(1 << 22);
  for (auto& k : keys) k = 1 + rng() % n;
  constexpr size_t kBatch = 32;
  State* out[kBatch];
  size_t pos = 0;
  uint64_t visited = 0, moved = 0, created = 0;
  for (auto _ : state) {
    for (int rep = 0; rep < 32; rep++) {
      std::span<const uint64_t> batch(&keys[pos], kBatch);
      pos = (pos + kBatch) & (keys.size() - 1);
      uint64_t hits = table->FindBatch(batch, std::span<State*>(out, kBatch));
      now += kPerPacket * kBatch;
      for (size_t i = 0; i < kBatch; i++) {
        if (!((hits >> i) & 1)) {  // no flow: create it
          out[i] = table->Emplace(batch[i]).state;
          created++;
        }
        if (out[i] != nullptr) Touch<kMode>(out[i], wheel.get(), now, timeout);
      }
      if constexpr (kMode == kOwnerStorePoll) {
        const auto r = wheel->Poll(
            now, 16, [&](const FlowHandle& h) noexcept -> std::optional<uint64_t> {
              const State* s = table->Lookup(h);
              if (s == nullptr) return std::nullopt;
              if (s->last_seen + timeout > now) return s->last_seen + timeout;  // touched since
              table->Erase(h);
              return std::nullopt;
            });
        visited += r.fired;
        moved += r.moved;
      } else if constexpr (kMode == kEngineLazyPoll || kMode == kEngineEagerPoll) {
        const auto r =
            wheel->Poll(now, 16, [&](const FlowHandle& h) noexcept { table->Erase(h); });
        visited += r.fired;
        moved += r.moved;
      }
    }
  }
  benchmark::DoNotOptimize(visited);
  const double packets = 32.0 * kBatch * static_cast<double>(state.iterations());
  state.SetItemsProcessed(static_cast<int64_t>(packets));
  state.counters["ns_per_packet"] =
      benchmark::Counter(packets, benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["visited_per_kpkt"] = 1000.0 * static_cast<double>(visited) / packets;
  state.counters["moved_per_kpkt"] = 1000.0 * static_cast<double>(moved) / packets;
  state.counters["created_per_kpkt"] = 1000.0 * static_cast<double>(created) / packets;
  state.counters["flows_at_end"] = static_cast<double>(table->size());
}

// The periodic scan of the flow table itself: ForEach over every slot, reading
// State::last_seen, counting flows idle past the timeout. One cache line of
// slot record per flow is touched whatever the number of flows that expire.
void BM_TableSweep(benchmark::State& state) {
  const size_t n = static_cast<size_t>(state.range(0));
  auto table = std::move(*Table<PlainTraits>::Create(n));
  for (uint64_t k = 1; k <= n; k++) {
    table->Emplace(k).state->last_seen = k;
  }
  uint64_t now = 1'000'000;
  for (auto _ : state) {
    uint64_t idle = 0;
    table->ForEach([&](FlowHandle, const uint64_t&, State& s) {
      idle += (s.last_seen + 100'000'000 < now) ? 1 : 0;
    });
    benchmark::DoNotOptimize(idle);
    now++;
  }
  state.counters["ns_per_flow"] = benchmark::Counter(
      static_cast<double>(n) * static_cast<double>(state.iterations()),
      benchmark::Counter::kIsRate | benchmark::Counter::kInvert);
  state.counters["bytes_per_flow"] =
      static_cast<double>(table->memory_bytes()) / static_cast<double>(n);
}

template <int64_t kMode>
void Reg() {
  constexpr bool kPoll = kMode == kOwnerStorePoll || kMode == kEngineLazyPoll ||
                         kMode == kEngineEagerPoll;
  for (int64_t n : {int64_t{1} << 10, int64_t{1} << 16, int64_t{1} << 20}) {
    // Without a poll nothing ever fires, so the timeout does not matter.
    for (int64_t ms : kPoll ? std::vector<int64_t>{100, 10'000} : std::vector<int64_t>{10'000}) {
      benchmark::RegisterBenchmark((std::string("BM_PacketLoop/") + ModeName(kMode)).c_str(),
                                   BM_PacketLoop<kMode>)
          ->Args({n, ms})
          ->Unit(benchmark::kNanosecond)
          ->MinTime(0.5);
    }
  }
}

[[maybe_unused]] const bool kRegistered = [] {
  for (int64_t n : {int64_t{1} << 10, int64_t{1} << 16, int64_t{1} << 20}) {
    benchmark::RegisterBenchmark("BM_TableSweep", BM_TableSweep)
        ->Args({n})
        ->Unit(benchmark::kMicrosecond)
        ->MinTime(0.3);
  }
  Reg<kNone>();
  Reg<kOwnerStore>();
  Reg<kEngineLazy>();
  Reg<kEngineEager>();
  Reg<kOwnerStorePoll>();
  Reg<kEngineLazyPoll>();
  Reg<kEngineEagerPoll>();
  return true;
}();

}  // namespace
