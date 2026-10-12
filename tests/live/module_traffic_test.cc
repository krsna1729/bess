// SPDX-License-Identifier: BSD-3-Clause

// Modules under traffic on a running worker (the run_for cases of
// bessctl/module_tests/{buffer,generic_decap,update_ttl,replicate,vxlan,vlan}.py,
// and timestamp.py): Source -> Rewrite -> RandomUpdate -> module -> a Sink
// per output gate; the daemon stays up and each gate forwards packets (gate
// counters, D-116). Packet-level behaviour is tested in process
// (core/modules/*_test.cc).

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "live_daemon.h"
#include "packet_builder.h"

namespace {

using bess::live::LiveTest;
using bess::live::Types;
using bess::testing::Bytes;
using bess::testing::PacketBuilder;
using std::chrono::milliseconds;
namespace gp = google::protobuf;
namespace v2 = bess::pb::v2;

Bytes Tcp() {
  return PacketBuilder()
      .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
      .IPv4("10.0.0.1", "10.0.0.2")
      .Tcp(1234, 80)
      .PadTo(60)
      .Build();
}

Bytes TaggedTcp(uint16_t vid) {
  return PacketBuilder()
      .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
      .Dot1Q(vid)
      .IPv4("10.0.0.1", "10.0.0.2")
      .Tcp(1234, 80)
      .PadTo(64)
      .Build();
}

Bytes Udp() {
  return PacketBuilder()
      .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
      .IPv4("172.12.0.3", "127.12.0.4")
      .Udp(10001, 10002)
      .PadTo(60)
      .Build();
}

std::optional<double> Value(const std::vector<v2::MetricSample> &samples, const std::string &name,
                            const std::map<std::string, std::string> &labels) {
  for (const v2::MetricSample &s : samples) {
    if (s.name() != name) {
      continue;
    }
    bool match = true;
    for (const auto &[k, v] : labels) {
      const auto it = s.labels().find(k);
      match = match && it != s.labels().end() && it->second == v;
    }
    if (match) {
      return s.value();
    }
  }
  return std::nullopt;
}

// A uint64 field of a command's answer as JSON (proto3 JSON writes uint64 as
// a string and leaves zero out).
uint64_t U64(const gp::Struct &s, const std::string &field) {
  const auto it = s.fields().find(field);
  if (it == s.fields().end()) {
    return 0;
  }
  return it->second.has_string_value() ? std::stoull(it->second.string_value())
                                       : static_cast<uint64_t>(it->second.number_value());
}

class ModuleTrafficTest : public LiveTest {
 protected:
  std::unique_ptr<gp::Message> Rewrite(Types &types, const std::vector<Bytes> &templates) {
    auto arg = types.Make("bess.pb.RewriteArg", "{}");
    const gp::FieldDescriptor *f = arg->GetDescriptor()->FindFieldByName("templates");
    for (const Bytes &t : templates) {
      arg->GetReflection()->AddString(arg.get(), f, std::string(t.begin(), t.end()));
    }
    return arg;
  }

  // `mclass`(`arg_json` of `arg_type`) as "mut" under traffic of `templates`
  // (source and destination IPv4 addresses randomized, at offsets 26 and 30)
  // for `duration`, a Sink on each of `ogates`, from an empty pipeline.
  // Returns the packets each gate forwarded, after checking the daemon is
  // still up.
  std::map<int, double> RunFor(const std::string &mclass, const std::string &arg_type,
                               const std::string &arg_json, const std::vector<int> &ogates,
                               const std::vector<Bytes> &templates = {Tcp()},
                               milliseconds duration = milliseconds(300)) {
    std::map<int, double> out;
    daemon().Reset();
    Types types(client());
    auto rewrite = Rewrite(types, templates);
    auto random = types.Make("bess.pb.RandomUpdateArg",
                             R"({"fields": [{"offset": 26, "size": 4, "min": 1, "max": 4294967295},
                                            {"offset": 30, "size": 4, "min": 1, "max": 4294967295}]})");
    // No argument type: the module takes none.
    auto arg = arg_type.empty() ? nullptr : types.Make(arg_type, arg_json);
    v2::TaskPlacement task;
    task.set_module("src");
    task.set_wid(0);
    bess::sdk::PipelineBuilder b;
    b.Worker(0, 0, "")
        .Module("src", "Source", nullptr)
        .Module("rewrite", "Rewrite", rewrite.get())
        .Module("random", "RandomUpdate", random.get())
        .Module("mut", mclass, arg.get())
        .Chain({"src", "rewrite", "random", "mut"})
        .Task(task);
    for (int g : ogates) {
      const std::string sink = "sink" + std::to_string(g);
      b.Module(sink, "Sink", nullptr).Connect("mut", static_cast<uint32_t>(g), sink, 0);
    }
    auto pipeline = b.Build();
    EXPECT_TRUE(pipeline) << pipeline.error().message;
    if (!pipeline) {
      return out;
    }
    auto applied = client().ApplyPipeline(*pipeline);
    EXPECT_TRUE(applied) << applied.error().message;
    if (!applied) {
      return out;
    }
    EXPECT_TRUE(client().ResumeWorkers());
    std::this_thread::sleep_for(duration);
    EXPECT_TRUE(client().PauseWorkers());
    EXPECT_TRUE(daemon().Alive());

    auto samples = client().Metrics();
    EXPECT_TRUE(samples) << samples.error().message;
    if (!samples) {
      return out;
    }
    for (int g : ogates) {
      const auto n = Value(*samples, "bess_gate_packets_total",
                           {{"module", "mut"}, {"gate", std::to_string(g)}, {"direction", "out"}});
      EXPECT_TRUE(n) << mclass << " gate " << g << " has no counter";
      out[g] = n.value_or(0);
    }
    return out;
  }
};

TEST_F(ModuleTrafficTest, Buffer) {
  EXPECT_GT(RunFor("Buffer", "bess.pb.BufferArg", "{}", {0})[0], 0);
}

TEST_F(ModuleTrafficTest, GenericDecap) {
  EXPECT_GT(RunFor("GenericDecap", "bess.pb.GenericDecapArg", R"({"bytes": 0})", {0})[0], 0);
  EXPECT_GT(RunFor("GenericDecap", "bess.pb.GenericDecapArg", R"({"bytes": 23})", {0})[0], 0);
}

TEST_F(ModuleTrafficTest, UpdateTTL) {
  EXPECT_GT(RunFor("UpdateTTL", "", "", {0})[0], 0);
}

// Every gate gets the copies; a copy can only be missing (allocation
// failure), never extra.
TEST_F(ModuleTrafficTest, Replicate) {
  for (int n : {1, 4, 10}) {
    std::vector<int> gates;
    std::string json = R"({"gates": [)";
    for (int g = 0; g < n; g++) {
      gates.push_back(g);
      json += (g ? ", " : "") + std::to_string(g);
    }
    json += "]}";
    auto out = RunFor("Replicate", "bess.pb.ReplicateArg", json, gates);
    ASSERT_GT(out[0], 0) << n << " gates";
    for (int g = 1; g < n; g++) {
      EXPECT_GT(out[g], 0) << n << " gates, gate " << g;
      EXPECT_LE(out[g], out[0]) << n << " gates, gate " << g;
    }
  }
}

TEST_F(ModuleTrafficTest, VXLANEncap) {
  EXPECT_GT(RunFor("VXLANEncap", "bess.pb.VXLANEncapArg", R"({"dstport": 4789})", {0})[0], 0);
}

// Untagged packets on gate 0, VID 7 on gate 7.
TEST_F(ModuleTrafficTest, VLANSplit) {
  auto out = RunFor("VLANSplit", "bess.pb.VLANSplitArg", "{}", {0, 7}, {Tcp(), TaggedTcp(7)});
  EXPECT_GT(out[0], 0);
  EXPECT_GT(out[7], 0);
}

// Timestamp and Measure together (neither can be tested alone): after a
// warm-up and a clear, Measure has latency samples whose average, total and
// count agree (integer division truncates the average by less than one
// nanosecond per sample).
TEST_F(ModuleTrafficTest, TimestampedAndMeasured) {
  Types types(client());
  auto rewrite = Rewrite(types, {Udp()});
  auto bypass = types.Make("bess.pb.BypassArg", R"({"cycles_per_batch": 100})");
  auto measure =
      types.Make("bess.pb.MeasureArg", R"({"latency_ns_resolution": 1, "latency_ns_max": 100000})");
  v2::TaskPlacement task;
  task.set_module("src");
  task.set_wid(0);
  auto pipeline = bess::sdk::PipelineBuilder()
                      .Worker(0, 0, "")
                      .Module("src", "Source", nullptr)
                      .Module("rewrite", "Rewrite", rewrite.get())
                      .Module("ts", "Timestamp", nullptr)
                      .Module("bypass", "Bypass", bypass.get())
                      .Module("measure", "Measure", measure.get())
                      .Module("sink", "Sink", nullptr)
                      .Chain({"src", "rewrite", "ts", "bypass", "measure", "sink"})
                      .Task(task)
                      .Build();
  ASSERT_TRUE(pipeline) << pipeline.error().message;
  auto applied = client().ApplyPipeline(*pipeline);
  ASSERT_TRUE(applied) << applied.error().message;

  ASSERT_TRUE(client().ResumeWorkers());
  std::this_thread::sleep_for(milliseconds(200));  // warm up ...
  auto empty = types.Make("bess.pb.EmptyArg", "{}");
  auto cleared = client().RunModuleCommand("measure", "clear", *empty);
  EXPECT_TRUE(cleared) << cleared.error().message;
  std::this_thread::sleep_for(milliseconds(800));  // ... then measure
  ASSERT_TRUE(client().PauseWorkers());
  ASSERT_TRUE(daemon().Alive());

  auto summary_arg = types.Make("bess.pb.MeasureCommandGetSummaryArg",
                                R"({"latency_percentiles": [0, 25, 50, 99, 100]})");
  auto summary = client().RunModuleCommand("measure", "get_summary", *summary_arg);
  ASSERT_TRUE(summary) << summary.error().message;
  const std::string json = types.ToJson(*summary);
  gp::Struct s;
  ASSERT_TRUE(gp::util::JsonStringToMessage(json, &s).ok()) << json;
  ASSERT_EQ(1u, s.fields().count("latency")) << json;
  const gp::Struct &latency = s.fields().at("latency").struct_value();
  const uint64_t count = U64(latency, "count");
  const uint64_t avg_ns = U64(latency, "avg_ns");
  const uint64_t total_ns = U64(latency, "total_ns");
  EXPECT_GT(count, 0u) << json;
  EXPECT_GT(U64(s, "packets"), 0u) << json;
  EXPECT_LE(avg_ns * count, total_ns) << json;
  EXPECT_LT(total_ns, (avg_ns + 1) * count) << json;
  EXPECT_EQ(5, latency.fields().at("percentile_values_ns").list_value().values_size()) << json;
}

}  // namespace
