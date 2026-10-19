// SPDX-License-Identifier: BSD-3-Clause

// The worked example of the counter-watch pattern (D-150, D-151, D-152, D-154)
// against a live bessd: the sample plugin's UsageReport meters a key that the
// graph writes into metadata, a controller arms a quota through the module's
// own "<name>/quotas" resource, and the crossing arrives as the shared
// bess.counter_threshold event.
//
// The three ways are each exercised: the packet path's compare reports while
// the workers run (USAGE_REPORT_EVENTS), the tick sweeps the armed keys on the
// control side while the packet path does not compare at all
// (USAGE_REPORT_SWEEP), and both together (USAGE_REPORT_BOTH, the default).
//
// BESS_PLUGIN_DIR: the build's sample_plugin/, as the plugin's own live test.

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "bess/sdk/client.h"
#include "live_daemon.h"

#include "pb/usage_msg.pb.h"

namespace {

using bess::live::Daemon;
using bess::live::Env;
using bess::live::OnWorker;
using bess::live::Types;
namespace v2 = bess::pb::v2;
namespace sample_pb = sample::usage::pb;

// The module's name in the pipeline; its quota resource is "usage/quotas".
constexpr const char *kModule = "usage";
constexpr const char *kKeyAttr = "usage_key";
// The key space the module is built for, and the key every packet carries.
constexpr uint64_t kKeys = 4;
constexpr uint64_t kKey = 1;
// The frame the source repeats: the sample plugin's manifest template, a
// 60-byte UDP packet, so a small octet quota crosses within a short run.
constexpr const char *kFrame =
    "BhY+G3IyAh5nn02uCABFAAAmAAEAAEARrxzAqAEBCgAAAScRJxIAEsXcaGVsbG93b3JsZA==";

struct Watched {
  std::vector<v2::Event> events;
  bess::sdk::Result<void> result;
  bool ended = false;  // WatchEvents returned by itself (not stopped by us)
};

// Watches `types` from sequence `from` until `n` events other than gaps came,
// the stream ended, or 10 s passed. The daemon holds what already happened, so
// a watch started after the traffic still delivers a crossing that fired
// before it began.
Watched Watch(bess::sdk::Client &client, uint64_t from,
              const std::vector<std::string> &types, size_t n) {
  Watched w;
  std::mutex mu;
  std::condition_variable cv;
  size_t counted = 0;
  std::stop_source stop;
  std::jthread watcher([&] {
    const bess::sdk::Result<void> result = client.WatchEvents(
        from, 0, types,
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

// The packets that left `module` on its output gates.
double GatePackets(const std::vector<v2::MetricSample> &samples, const std::string &module) {
  double n = 0;
  for (const v2::MetricSample &s : samples) {
    const auto &l = s.labels();
    if (s.name() == "bess_gate_packets_total" && l.contains("module") &&
        l.at("module") == module && l.contains("direction") && l.at("direction") == "out") {
      n += s.value();
    }
  }
  return n;
}

class UsageReportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    daemon_ = Daemon::Launch({"-modules=" + Env("BESS_PLUGIN_DIR")});
    ASSERT_TRUE(daemon_->Alive()) << daemon_->Log();
  }

  void TearDown() override {
    if (daemon_ != nullptr && !daemon_->Alive()) {
      ADD_FAILURE() << "bessd died:\n" << daemon_->Log();
    }
  }

  bess::sdk::Client &client() { return daemon_->client(); }

  // The sequence the next event will have: a watch from it sees only what
  // happens after it.
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

  // Source -> Rewrite (the frame) -> SetMetadata (the key) -> UsageReport ->
  // Sink, on one worker. `quota` octets are armed on kKey through the module's
  // own resource; 0 leaves the resource unlisted, so nothing is armed.
  void Apply(sample_pb::UsageReportMode mode, uint64_t quota) {
    Types types(client());
    auto rewrite = types.Make(
        "bess.pb.RewriteArg", std::string("{\"templates\": [\"") + kFrame + "\"]}");
    auto key = types.Make("bess.pb.SetMetadataArg",
                          std::string("{\"attrs\": [{\"name\": \"") + kKeyAttr +
                              "\", \"size\": 8, \"value_int\": " + std::to_string(kKey) +
                              "}]}");
    sample_pb::UsageReportArg arg;
    arg.set_keys(kKeys);
    arg.set_key_attr(kKeyAttr);
    arg.set_mode(mode);
    arg.set_tick_period_ms(50);
    arg.set_workers(1);

    bess::sdk::PipelineBuilder b;
    b.Worker(0, 0, "")
        .Module("src", "Source", nullptr)
        .Module("frame", "Rewrite", rewrite.get())
        .Module("key", "SetMetadata", key.get())
        .Module(kModule, "UsageReport", &arg)
        .Module("sink", "Sink", nullptr)
        .Chain({"src", "frame", "key", kModule, "sink"})
        .Task(OnWorker("src", 0));
    if (quota != 0) {
      sample_pb::UsageQuotas quotas;
      sample_pb::UsageQuota *q = quotas.add_quotas();
      q->set_key(kKey);
      q->set_octets(quota);
      const std::string name = std::string(kModule) + "/quotas";
      b.Resource(name).Setting(name, quotas);
    }
    auto pipeline = b.Build();
    ASSERT_TRUE(pipeline) << pipeline.error().message;
    auto applied = client().ApplyPipeline(*pipeline);
    ASSERT_TRUE(applied) << applied.error().message;
  }

  // Runs traffic for `duration`, returning the packets that left the module.
  double Run(std::chrono::milliseconds duration) {
    EXPECT_TRUE(client().ResumeWorkers());
    std::this_thread::sleep_for(duration);
    EXPECT_TRUE(client().PauseWorkers());
    auto samples = client().Metrics();
    EXPECT_TRUE(samples) << samples.error().message;
    return samples ? GatePackets(*samples, kModule) : 0;
  }

  std::unique_ptr<Daemon> daemon_;
};

// The packet path's compare: the worker that saw the crossing posts it.
TEST_F(UsageReportTest, ThePacketPathReportsACrossing) {
  Apply(sample_pb::USAGE_REPORT_EVENTS, 4000);
  const uint64_t start = NextSequence();
  const double left = Run(std::chrono::milliseconds(400));
  EXPECT_GT(left, 0) << "no packet left " << kModule;
  const Watched w = Watch(client(), start, {"bess.counter_threshold"}, 1);
  ASSERT_FALSE(w.events.empty()) << "no crossing reported:\n" << daemon_->Log();
  const v2::Event &e = w.events.front();
  EXPECT_EQ(kModule, e.source());
  EXPECT_EQ(std::to_string(kKey), e.fields().at("key"));
  EXPECT_EQ("4000", e.fields().at("limit"));
  EXPECT_GE(std::stoull(e.fields().at("value")), 4000u);
}

// The tick's sweep: the packet path only meters, and the control side reads
// every plane's total for the armed key.
TEST_F(UsageReportTest, TheSweepReportsACrossing) {
  Apply(sample_pb::USAGE_REPORT_SWEEP, 4000);
  const uint64_t start = NextSequence();
  const double left = Run(std::chrono::milliseconds(600));
  EXPECT_GT(left, 0) << "no packet left " << kModule;
  const Watched w = Watch(client(), start, {"bess.counter_threshold"}, 1);
  ASSERT_FALSE(w.events.empty()) << "no crossing reported:\n" << daemon_->Log();
  const v2::Event &e = w.events.front();
  EXPECT_EQ(kModule, e.source());
  EXPECT_EQ(std::to_string(kKey), e.fields().at("key"));
  EXPECT_GE(std::stoull(e.fields().at("value")), 4000u);
}

// Nothing armed, nothing reported: the module meters a key with no quota and
// stays silent about it.
TEST_F(UsageReportTest, AnUnarmedKeyIsNotReported) {
  Apply(sample_pb::USAGE_REPORT_BOTH, 0);
  const uint64_t start = NextSequence();
  const double left = Run(std::chrono::milliseconds(400));
  EXPECT_GT(left, 0) << "no packet left " << kModule;
  const Watched w = Watch(client(), start, {"bess.counter_threshold"}, 1);
  EXPECT_TRUE(w.events.empty()) << "an unarmed key reported a crossing:\n" << daemon_->Log();
}

}  // namespace
