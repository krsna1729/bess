// SPDX-License-Identifier: BSD-3-Clause

// Operational events (M25, D-084, D-089) against a live bessd (was
// bessctl/module_tests/events.py): a full ConnTrack table is one throttled
// "bess.table_full" event with the count, not one per packet; transactions,
// applied or refused, are "bess.transaction" events with their outcome,
// request id and generation, in sequence; a reader from sequence 1 gets what
// the daemon holds or a gap first; a sequence this daemon never reached is a
// restart.

#include <chrono>
#include <condition_variable>
#include <format>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "live_daemon.h"
#include "steering.h"

namespace {

using bess::live::LiveTest;
using bess::live::Types;
namespace v2 = bess::pb::v2;

struct Watched {
  std::vector<v2::Event> events;
  bess::sdk::Result<void> result;
  bool ended = false;  // WatchEvents returned by itself (not stopped by us)
};

// Watches from `from` until `n` events other than gaps came (gaps are kept
// too), the stream ended, or 10 s passed.
Watched Watch(bess::sdk::Client &client, uint64_t from, uint64_t epoch,
              const std::vector<std::string> &types, size_t n) {
  Watched w;
  std::mutex mu;
  std::condition_variable cv;
  size_t counted = 0;
  std::stop_source stop;
  std::jthread watcher([&] {
    auto result = client.WatchEvents(
        from, epoch, types,
        [&](const v2::Event &e) {
          std::lock_guard lock(mu);
          w.events.push_back(e);
          counted += e.type() != "bess.gap";
          cv.notify_all();
          return counted < n;
        },
        stop.get_token());
    std::lock_guard lock(mu);
    w.result = result;
    w.ended = !stop.stop_requested();
    cv.notify_all();
  });
  {
    std::unique_lock lock(mu);
    cv.wait_for(lock, std::chrono::seconds(10), [&] { return w.ended || counted >= n; });
  }
  stop.request_stop();
  watcher.join();
  return w;
}

class EventsTest : public LiveTest {
 protected:
  // The sequence the next event will have.
  uint64_t NextSequence() {
    auto samples = client().Metrics();
    EXPECT_TRUE(samples) << samples.error().message;
    if (samples) {
      for (const v2::MetricSample &s : *samples) {
        if (s.name() == "bess_events_next_sequence") {
          return static_cast<uint64_t>(s.value());
        }
      }
    }
    ADD_FAILURE() << "no bess_events_next_sequence";
    return 0;
  }

  void ApplyExactMatch(Types &types) {
    auto arg = types.Make("bess.pb.ExactMatchArg",
                          R"({"fields": [{"offset": 26, "num_bytes": 4},
                                         {"offset": 30, "num_bytes": 4}]})");
    auto p = bess::sdk::PipelineBuilder().Module("em", "ExactMatch", arg.get()).Build();
    ASSERT_TRUE(p) << p.error().message;
    auto applied = client().ApplyPipeline(*p);
    ASSERT_TRUE(applied) << applied.error().message;
  }
};

// D-089: a ConnTrack whose table is full refuses new connections; it posts
// one bess.table_full per second per worker with the count, under its own
// name. UDP 10.0.0.x:1234 -> 10.1.0.1:5678 from 255 sources into 4 entries.
TEST_F(EventsTest, AFullTableIsAnEventNotOnePerPacket) {
  Types types(client());
  auto src = types.Make("bess.pb.SourceArg", R"({"pkt_size": 60})");
  // Ethernet type IPv4; IPv4 (IHL 5, length 46, TTL 64, UDP) to 10.1.0.1;
  // UDP 1234 -> 5678, length 26.
  auto upd = types.Make(
      "bess.pb.UpdateArg",
      std::format(R"({{"fields": [
          {{"offset": 12, "size": 2, "value": 2048}},
          {{"offset": 14, "size": 2, "value": 17664}},
          {{"offset": 16, "size": 2, "value": 46}},
          {{"offset": 20, "size": 2, "value": 0}},
          {{"offset": 22, "size": 2, "value": 16401}},
          {{"offset": 30, "size": 4, "value": 167837697}},
          {{"offset": 34, "size": 8, "value": "{}"}}]}})",
                  uint64_t{0x04D2162E001A0000}));
  auto rnd = types.Make(
      "bess.pb.RandomUpdateArg",
      R"({"fields": [{"offset": 26, "size": 4, "min": 167772161, "max": 167772415}]})");
  auto ct = types.Make("bess.pb.ConnTrackArg", R"({"capacity": 4})");
  bess::pb::v2::TaskPlacement task;
  task.set_module("src");
  task.set_wid(0);
  auto p = bess::sdk::PipelineBuilder()
               .Worker(0, 0, "")
               .Module("src", "Source", src.get())
               .Module("upd", "Update", upd.get())
               .Module("rnd", "RandomUpdate", rnd.get())
               .Module("ct", "ConnTrack", ct.get())
               .Module("pass", "Sink", nullptr)
               .Module("refused", "Sink", nullptr)
               .Chain({"src", "upd", "rnd", "ct", "pass"})
               .Connect("ct", 1, "refused", 0)
               .Task(task)
               .Build();
  ASSERT_TRUE(p) << p.error().message;
  auto applied = client().ApplyPipeline(*p);
  ASSERT_TRUE(applied) << applied.error().message;

  const uint64_t start = NextSequence();
  ASSERT_TRUE(client().ResumeWorkers());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  ASSERT_TRUE(client().PauseWorkers());

  const Watched w = Watch(client(), start, 0, {"bess.table_full"}, 1);
  ASSERT_TRUE(w.result) << w.result.error().message;
  const v2::Event *full = nullptr;
  for (const v2::Event &e : w.events) {
    if (e.type() == "bess.table_full") {
      full = &e;
      break;
    }
  }
  ASSERT_NE(nullptr, full) << "no bess.table_full event";
  EXPECT_EQ("ct", full->source());
  ASSERT_TRUE(full->fields().contains("count"));
  EXPECT_GT(std::stoull(full->fields().at("count")), 0u);
  EXPECT_TRUE(full->fields().contains("worker"));
}

TEST_F(EventsTest, TransactionsAreEvents) {
  Types types(client());
  ASSERT_NO_FATAL_FAILURE(ApplyExactMatch(types));
  auto rules = client().Resource("em/rules");
  ASSERT_TRUE(rules) << rules.error().message;
  auto key = types.Make(rules->key_type, bess::live::RuleKeyJson(0x01020304, 0x05060708));
  const uint64_t start = NextSequence();

  auto tx = client().NewTransaction();
  ASSERT_TRUE(tx.Upsert(*rules, *key, *types.Make(rules->value_type, R"({"gate": 1})")));
  auto done = tx.Commit();
  ASSERT_TRUE(done) << done.error().message;
  auto stale = client().NewTransaction({.expected_generation = done->generation - 1});
  ASSERT_TRUE(stale.Upsert(*rules, *key, *types.Make(rules->value_type, R"({"gate": 2})")));
  auto refused = stale.Commit();
  ASSERT_FALSE(refused);
  EXPECT_EQ(bess::sdk::ErrorKind::kConflict, refused.error().kind) << refused.error().message;

  // From the sequence before them: both are held, nothing raced.
  const Watched w = Watch(client(), start, 0, {"bess.transaction"}, 2);
  ASSERT_TRUE(w.result) << w.result.error().message;
  ASSERT_EQ(2u, w.events.size());
  const v2::Event &applied = w.events[0];
  const v2::Event &conflict = w.events[1];
  EXPECT_EQ("applied", applied.fields().at("outcome"));
  EXPECT_EQ("conflict", conflict.fields().at("outcome"));
  EXPECT_EQ(done->generation, applied.generation());
  EXPECT_EQ(done->request_id, applied.fields().at("request_id"));
  EXPECT_EQ(stale.request_id(), conflict.fields().at("request_id"));
  EXPECT_GT(conflict.sequence(), applied.sequence());
  EXPECT_EQ(client().DaemonEpoch(), applied.daemon_epoch());
}

TEST_F(EventsTest, AReaderFromTheStartGetsWhatIsHeld) {
  Types types(client());
  ASSERT_NO_FATAL_FAILURE(ApplyExactMatch(types));
  auto rules = client().Resource("em/rules");
  ASSERT_TRUE(rules) << rules.error().message;
  auto tx = client().NewTransaction();  // at least one event exists
  ASSERT_TRUE(tx.Upsert(*rules,
                        *types.Make(rules->key_type, bess::live::RuleKeyJson(0x09090909, 0x08080808)),
                        *types.Make(rules->value_type, R"({"gate": 1})")));
  ASSERT_TRUE(tx.Commit());

  const Watched w = Watch(client(), 1, 0, {}, 1);
  ASSERT_TRUE(w.result) << w.result.error().message;
  ASSERT_FALSE(w.events.empty());
  // Sequence 1 is held (this daemon has emitted few events), or a gap names
  // what is not.
  const v2::Event &first = w.events[0];
  EXPECT_TRUE(first.sequence() == 1 || first.type() == "bess.gap") << first.ShortDebugString();
}

// A controller resuming from a sequence it saved before a restart: told so,
// not left waiting for sequences that will mean other events.
TEST_F(EventsTest, ASequenceThisDaemonNeverReachedIsARestart) {
  ASSERT_TRUE(client().Resources(true));
  const auto epoch = client().DaemonEpoch();
  ASSERT_TRUE(epoch);
  const Watched w = Watch(client(), uint64_t{1'000'000'000'000}, *epoch, {}, 1);
  EXPECT_TRUE(w.ended) << "still waiting after 10 s";
  ASSERT_FALSE(w.result);
  EXPECT_EQ(bess::sdk::ErrorKind::kDaemonRestarted, w.result.error().kind)
      << w.result.error().message;
  EXPECT_TRUE(w.events.empty());
}

}  // namespace
