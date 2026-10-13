// SPDX-License-Identifier: BSD-3-Clause

// ExactMatch and WildcardMatch on a running worker (the run_for and
// run_with_live_commands cases of bessctl/module_tests/exact_match.py and
// wildcard_match.py): Source -> RandomUpdate -> the module -> Sinks on worker
// 0. Rules installed with the pipeline, or changed by transactions while
// the worker forwards; the daemon stays up and every packet the module got
// left through one of its gates (D-116 gate counters). What the module does
// with each packet is core/modules/{exact,wildcard}_match_test.cc.

#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "live_daemon.h"

namespace {

using bess::live::LiveTest;
using bess::live::Types;
namespace v2 = bess::pb::v2;

std::string Base64(std::string_view bytes) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  size_t i = 0;
  for (; i + 2 < bytes.size(); i += 3) {
    const uint32_t v = (uint32_t{static_cast<uint8_t>(bytes[i])} << 16) |
                       (uint32_t{static_cast<uint8_t>(bytes[i + 1])} << 8) |
                       static_cast<uint8_t>(bytes[i + 2]);
    out += kAlphabet[v >> 18];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  if (i < bytes.size()) {
    uint32_t v = uint32_t{static_cast<uint8_t>(bytes[i])} << 16;
    if (i + 1 < bytes.size()) {
      v |= uint32_t{static_cast<uint8_t>(bytes[i + 1])} << 8;
    }
    out += kAlphabet[v >> 18];
    out += kAlphabet[(v >> 12) & 63];
    out += i + 1 < bytes.size() ? kAlphabet[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

// `bytes` as a FieldData JSON list element.
std::string Bin(const std::vector<uint8_t> &bytes) {
  const std::string raw(bytes.begin(), bytes.end());
  return R"({"value_bin": ")" + Base64(raw) + R"("})";
}

class MatchModulesTest : public LiveTest {
 protected:
  // Source -> RandomUpdate (random IPv4 addresses) -> `module` (input gate 0),
  // its gates `ogates` each to a Sink; the Source's task on worker 0;
  // `contents` adds the module's resource contents to the same pipeline.
  // Workers stay paused.
  void ApplyTraffic(Types &types, const std::string &mclass, const std::string &arg_json,
                    const std::vector<uint32_t> &ogates,
                    const std::function<void(bess::sdk::PipelineBuilder &)> &contents = {}) {
    auto rnd_arg = types.Make("bess.pb.RandomUpdateArg", R"({"fields": [
        {"offset": 26, "size": 4, "min": 1, "max": 4294967295},
        {"offset": 30, "size": 4, "min": 1, "max": 4294967295}]})");
    auto arg = types.Make("bess.pb." + mclass + "Arg", arg_json);
    v2::TaskPlacement task;
    task.set_module("src");
    task.set_wid(0);
    bess::sdk::PipelineBuilder b;
    b.Worker(0, 0, "")
        .Module("src", "Source", nullptr)
        .Module("rnd", "RandomUpdate", rnd_arg.get())
        .Module("m", mclass, arg.get())
        .Chain({"src", "rnd", "m"})
        .Task(task);
    for (uint32_t g : ogates) {
      const std::string sink = "sink" + std::to_string(g);
      b.Module(sink, "Sink", nullptr).Connect("m", g, sink, 0);
    }
    if (contents) {
      contents(b);
    }
    auto pipeline = b.Build();
    ASSERT_TRUE(pipeline) << pipeline.error().message;
    auto applied = client().ApplyPipeline(*pipeline);
    ASSERT_TRUE(applied) << applied.error().message;
  }

  // One transaction: `key_json` -> `value_json` upserted into the map `r`.
  void Upsert(Types &types, const bess::sdk::Resource &r, const std::string &key_json,
              const std::string &value_json) {
    auto tx = client().NewTransaction();
    auto key = types.Make(r.key_type, key_json);
    auto value = types.Make(r.value_type, value_json);
    ASSERT_TRUE(tx.Upsert(r, *key, *value));
    Commit(tx, "upsert " + key_json + " " + value_json);
  }
  // One transaction: `key_json` erased from the map `r`.
  void Erase(Types &types, const bess::sdk::Resource &r, const std::string &key_json) {
    auto tx = client().NewTransaction();
    auto key = types.Make(r.key_type, key_json);
    ASSERT_TRUE(tx.Erase(r, *key));
    Commit(tx, "erase " + key_json);
  }
  // One transaction: the singleton `r` set to `value_json`.
  void Set(Types &types, const bess::sdk::Resource &r, const std::string &value_json) {
    auto tx = client().NewTransaction();
    auto value = types.Make(r.value_type, value_json);
    ASSERT_TRUE(tx.Set(r, *value));
    Commit(tx, "set " + value_json);
  }
  void Commit(bess::sdk::Transaction &tx, const std::string &what) {
    auto done = tx.Commit();
    ASSERT_TRUE(done) << what << ": " << done.error().message;
  }

  // Packets out of `module`'s output gate `gate` (D-116), 0 without a sample.
  static double GateOut(const std::vector<v2::MetricSample> &samples, const std::string &module,
                        uint32_t gate) {
    for (const v2::MetricSample &s : samples) {
      if (s.name() != "bess_gate_packets_total") {
        continue;
      }
      const auto &l = s.labels();
      const auto m = l.find("module");
      const auto g = l.find("gate");
      const auto d = l.find("direction");
      if (m != l.end() && m->second == module && g != l.end() && g->second == std::to_string(gate) &&
          d != l.end() && d->second == "out") {
        return s.value();
      }
    }
    return 0;
  }

  // After the workers paused: the daemon answers, the module got traffic,
  // and every packet it got left through one of `ogates`.
  void ExpectEveryPacketForwarded(const std::vector<uint32_t> &ogates) {
    ASSERT_TRUE(daemon().Alive());
    auto samples = client().Metrics();
    ASSERT_TRUE(samples) << samples.error().message;
    const double in = GateOut(*samples, "rnd", 0);
    EXPECT_GT(in, 0) << "no traffic reached the module";
    double out = 0;
    for (uint32_t g : ogates) {
      out += GateOut(*samples, "m", g);
    }
    EXPECT_EQ(in, out);
  }
};

// Three fields of odd sizes and offsets, six rules listed in the pipeline, a
// default gate: traffic runs through the module on a worker.
// (The rules match by chance at most: Source packets carry whatever the
// buffers held; the default gate takes the rest.)
TEST_F(MatchModulesTest, ExactMatchForwardsTrafficWithRulesInstalled) {
  Types types(client());
  const std::vector<uint32_t> gates = {0, 1, 2, 3, 4, 5};
  const uint8_t rules[6][4] = {{0xff, 0x23, 0xba, 0x34}, {0xff, 0x34, 0xaa, 0x12},
                               {0xba, 0x33, 0xaa, 0x22}, {0xaa, 0x34, 0xba, 0x32},
                               {0x34, 0x34, 0x7a, 0x52}, {0x12, 0x34, 0x7a, 0x72}};
  ApplyTraffic(types, "ExactMatch",
               R"({"fields": [{"offset": 23, "num_bytes": 1}, {"offset": 2, "num_bytes": 2},
                              {"offset": 29, "num_bytes": 1}], "default_gate": 0})",
               gates, [&](bess::sdk::PipelineBuilder &b) {
                 for (uint32_t g = 0; g < 6; g++) {
                   const uint8_t *r = rules[g];
                   b.Entry("m/rules",
                           *types.Make("bess.pb.ExactMatchRuleKey",
                                       R"({"fields": [)" + Bin({r[0]}) + ", " +
                                           Bin({r[1], r[2]}) + ", " + Bin({r[3]}) + "]}"),
                           *types.Make("bess.pb.ExactMatchRuleValue",
                                       R"({"gate": )" + std::to_string(g) + "}"));
                 }
               });
  ASSERT_TRUE(client().ResumeWorkers());
  std::this_thread::sleep_for(std::chrono::seconds(1));
  ASSERT_TRUE(client().PauseWorkers());
  ExpectEveryPacketForwarded(gates);
}

TEST_F(MatchModulesTest, WildcardMatchForwardsTrafficWithRulesInstalled) {
  Types types(client());
  const std::vector<uint32_t> gates = {0, 1, 2, 3, 4, 5};
  const uint8_t rules[6][4] = {{0xff, 0x23, 0xba, 0x34}, {0xff, 0x34, 0xaa, 0x12},
                               {0xba, 0x33, 0xaa, 0x22}, {0xaa, 0x34, 0xba, 0x32},
                               {0x34, 0x34, 0x7a, 0x52}, {0x12, 0x34, 0x7a, 0x72}};
  const std::string masks = "[" + Bin({0xff}) + ", " + Bin({0xff, 0xff}) + ", " + Bin({0xff}) + "]";
  ApplyTraffic(types, "WildcardMatch",
               R"({"fields": [{"offset": 23, "num_bytes": 1}, {"offset": 2, "num_bytes": 2},
                              {"offset": 29, "num_bytes": 1}], "default_gate": 0})",
               gates, [&](bess::sdk::PipelineBuilder &b) {
                 for (uint32_t g = 0; g < 6; g++) {
                   const uint8_t *r = rules[g];
                   b.Entry("m/rules",
                           *types.Make("bess.pb.WildcardMatchRuleKey",
                                       R"({"masks": )" + masks + R"(, "values": [)" +
                                           Bin({r[0]}) + ", " + Bin({r[1], r[2]}) + ", " +
                                           Bin({r[3]}) + "]}"),
                           *types.Make("bess.pb.WildcardMatchRuleValue",
                                       R"({"gate": )" + std::to_string(g) + R"(, "priority": 0})"));
                 }
               });
  ASSERT_TRUE(client().ResumeWorkers());
  std::this_thread::sleep_for(std::chrono::seconds(1));
  ASSERT_TRUE(client().PauseWorkers());
  ExpectEveryPacketForwarded(gates);
}

// Rules come and go and the default gate flips while the worker forwards
// (G1.2 mode C): every transaction applies.
TEST_F(MatchModulesTest, ExactMatchCommandsWhileTheWorkerForwards) {
  Types types(client());
  const std::vector<uint32_t> gates = {0, 1};
  ApplyTraffic(types, "ExactMatch", R"({"fields": [{"offset": 26, "num_bytes": 4}]})", gates,
               [&](bess::sdk::PipelineBuilder &b) {
                 b.Setting("m/settings",
                           *types.Make("bess.pb.ExactMatchSettings", R"({"default_gate": 0})"));
               });
  ASSERT_TRUE(client().Resources(/*refresh=*/true));  // "m" may have been another class
  auto rules = client().Resource("m/rules");
  ASSERT_TRUE(rules) << rules.error().message;
  auto settings = client().Resource("m/settings");
  ASSERT_TRUE(settings) << settings.error().message;
  ASSERT_TRUE(client().ResumeWorkers());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  int i = 0;
  for (; std::chrono::steady_clock::now() < deadline; i++) {
    const std::string key =
        R"({"fields": [)" + Bin({10, 0, static_cast<uint8_t>(i / 2 % 64), 1}) + "]}";
    if (i % 2 == 0) {
      Upsert(types, *rules, key, R"({"gate": 1})");
    } else {
      Erase(types, *rules, key);
    }
    if (i % 50 == 0) {
      Set(types, *settings, R"({"default_gate": )" + std::to_string(i / 50 % 2) + "}");
    }
    if (HasFatalFailure()) {
      break;
    }
  }
  ASSERT_TRUE(client().PauseWorkers());
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_GT(i, 10) << "too few transactions ran";
  ExpectEveryPacketForwarded(gates);
}

// Rules come and go while the worker forwards, including masks that appear
// and disappear (the tuple list is republished): every transaction applies.
TEST_F(MatchModulesTest, WildcardMatchCommandsWhileTheWorkerForwards) {
  Types types(client());
  const std::vector<uint32_t> gates = {0, 1};
  ApplyTraffic(types, "WildcardMatch", R"({"fields": [{"offset": 26, "num_bytes": 4}]})", gates,
               [&](bess::sdk::PipelineBuilder &b) {
                 b.Setting("m/settings",
                           *types.Make("bess.pb.WildcardMatchSettings", R"({"default_gate": 0})"));
               });
  // "m/rules" belonged to another class in an earlier test: look it up again.
  ASSERT_TRUE(client().Resources(/*refresh=*/true));
  auto rules = client().Resource("m/rules");
  ASSERT_TRUE(rules) << rules.error().message;
  const std::vector<uint8_t> masks[3] = {
      {0xff, 0, 0, 0}, {0xff, 0xff, 0, 0}, {0xff, 0xff, 0xff, 0}};
  ASSERT_TRUE(client().ResumeWorkers());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  int i = 0;
  for (; std::chrono::steady_clock::now() < deadline; i++) {
    const std::vector<uint8_t> &m = masks[i / 2 % 3];
    const uint8_t raw[4] = {10, static_cast<uint8_t>(i / 2 % 256), static_cast<uint8_t>(i / 14 % 256),
                            0};
    std::vector<uint8_t> value(4);
    for (int b = 0; b < 4; b++) {
      value[b] = raw[b] & m[b];
    }
    const std::string key = R"({"masks": [)" + Bin(m) + R"(], "values": [)" + Bin(value) + "]}";
    if (i % 2 == 0) {
      Upsert(types, *rules, key, R"({"gate": 1, "priority": )" + std::to_string(i % 7) + "}");
    } else {
      Erase(types, *rules, key);
    }
    if (HasFatalFailure()) {
      break;
    }
  }
  ASSERT_TRUE(client().PauseWorkers());
  ASSERT_FALSE(HasFatalFailure());
  EXPECT_GT(i, 10) << "too few transactions ran";
  ExpectEveryPacketForwarded(gates);
}

}  // namespace
