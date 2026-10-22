// SPDX-License-Identifier: BSD-3-Clause

// Transactions and actions while a worker forwards traffic through the
// module (G1.2 mode C), against a live bessd: they are accepted, take
// effect, and the daemon stays up. Was the live cases of
// bessctl/module_tests/l2forward.py, nat.py and session_pipeline.py; the
// rest of those files is in process (core/modules/*_test.cc).

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "live_daemon.h"
#include "packet_builder.h"

namespace {

using bess::live::LiveTest;
using bess::live::Types;
namespace v2 = bess::pb::v2;
using Clock = std::chrono::steady_clock;

constexpr auto kRunFor = std::chrono::seconds(2);

std::string Base64(const std::vector<uint8_t> &in) {
  static constexpr char kChars[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < in.size(); i += 3) {
    const uint32_t n = static_cast<uint32_t>(in[i]) << 16 |
                       (i + 1 < in.size() ? static_cast<uint32_t>(in[i + 1]) << 8 : 0) |
                       (i + 2 < in.size() ? in[i + 2] : 0);
    out.push_back(kChars[n >> 18 & 63]);
    out.push_back(kChars[n >> 12 & 63]);
    out.push_back(i + 1 < in.size() ? kChars[n >> 6 & 63] : '=');
    out.push_back(i + 2 < in.size() ? kChars[n & 63] : '=');
  }
  return out;
}

v2::TaskPlacement OnWorker0(const std::string &module) {
  v2::TaskPlacement task;
  task.set_module(module);
  task.set_wid(0);
  return task;
}

// Random IPv4 source and destination (offsets 26 and 30), as
// run_with_live_commands rewrote them.
constexpr char kRandomAddresses[] =
    R"({"fields": [{"offset": 26, "size": 4, "min": 1, "max": 4294967295},
                   {"offset": 30, "size": 4, "min": 1, "max": 4294967295}]})";

class StatefulModulesTest : public LiveTest {
 protected:
  void Apply(const bess::sdk::PipelineBuilder &builder) {
    auto pipeline = builder.Build();
    ASSERT_TRUE(pipeline) << pipeline.error().message;
    auto applied = client().ApplyPipeline(*pipeline);
    ASSERT_TRUE(applied) << applied.error().message;
  }

  // Commits `tx`, built by `what`; it must apply.
  void Commit(bess::sdk::Transaction &tx, const std::string &what) {
    auto done = tx.Commit();
    EXPECT_TRUE(done) << what << ": " << done.error().message;
  }

  // Packets `module` sent out of `gate` (its D-116 gate counter).
  double GatePackets(const std::string &module, int gate) {
    auto samples = client().Metrics();
    EXPECT_TRUE(samples) << samples.error().message;
    if (!samples) {
      return -1;
    }
    auto label = [](const v2::MetricSample &s, const char *key) {
      const auto it = s.labels().find(key);
      return it == s.labels().end() ? std::string() : it->second;
    };
    for (const v2::MetricSample &s : *samples) {
      if (s.name() == "bess_gate_packets_total" && label(s, "module") == module &&
          label(s, "gate") == std::to_string(gate) && label(s, "direction") == "out") {
        return s.value();
      }
    }
    ADD_FAILURE() << "no gate counter for " << module << ":" << gate;
    return -1;
  }

  // Runs the workers, calls `step(i)` (i = 0, 1, ...) for kRunFor, pauses;
  // the daemon must still answer. Returns the number of steps.
  template <typename Step>
  int RunWhileForwarding(Step &&step) {
    EXPECT_TRUE(client().ResumeWorkers());
    const auto deadline = Clock::now() + kRunFor;
    int i = 0;
    while (Clock::now() < deadline && !::testing::Test::HasFailure()) {
      step(i++);
    }
    EXPECT_TRUE(client().PauseWorkers());
    EXPECT_TRUE(daemon().Alive());
    return i;
  }

  Types types_{client()};
};

// The settings resource needs no pause: a transaction sets the default gate
// while a worker forwards traffic, and the worker reads the gate atomically.
// Packets from unknown MACs follow each new default gate: both gates forward.
TEST_F(StatefulModulesTest, L2ForwardDefaultGateFollowsUnderTraffic) {
  auto src_arg = types_.Make("bess.pb.SourceArg", R"({"pkt_size": 60})");
  auto l2_arg = types_.Make("bess.pb.L2ForwardArg", "{}");
  Apply(bess::sdk::PipelineBuilder()
            .Worker(0, 0, "")
            .Module("src", "Source", src_arg.get())
            .Module("l2", "L2Forward", l2_arg.get())
            .Module("sink0", "Sink", nullptr)
            .Module("sink1", "Sink", nullptr)
            .Chain({"src", "l2"})
            .Connect("l2", 0, "sink0", 0)
            .Connect("l2", 1, "sink1", 0)
            .Task(OnWorker0("src")));

  auto settings = client().Resource("l2/settings");
  ASSERT_TRUE(settings) << settings.error().message;
  const int steps = RunWhileForwarding([&](int i) {
    const std::string json = "{\"default_gate\": " + std::to_string(i % 2) + "}";
    auto value = types_.Make(settings->value_type, json);
    auto tx = client().NewTransaction();
    EXPECT_TRUE(tx.Set(*settings, *value));
    Commit(tx, "l2/settings = " + json);
  });
  EXPECT_GT(steps, 10) << "too few transactions ran";
  EXPECT_GT(GatePackets("l2", 0), 0);
  EXPECT_GT(GatePackets("l2", 1), 0);
}

// Transactions upsert and erase MACs in place while a worker forwards
// traffic (G1.2 mode C): every one applies, and traffic keeps flowing.
TEST_F(StatefulModulesTest, L2ForwardAddDeleteUnderTraffic) {
  auto src_arg = types_.Make("bess.pb.SourceArg", R"({"pkt_size": 60})");
  auto rnd_arg = types_.Make("bess.pb.RandomUpdateArg", kRandomAddresses);
  auto l2_arg = types_.Make("bess.pb.L2ForwardArg", R"({"size": 1024, "bucket": 4, "default_gate": 0})");
  Apply(bess::sdk::PipelineBuilder()
            .Worker(0, 0, "")
            .Module("src", "Source", src_arg.get())
            .Module("rnd", "RandomUpdate", rnd_arg.get())
            .Module("l2", "L2Forward", l2_arg.get())
            .Module("sink0", "Sink", nullptr)
            .Module("sink1", "Sink", nullptr)
            .Chain({"src", "rnd", "l2"})
            .Connect("l2", 0, "sink0", 0)
            .Connect("l2", 1, "sink1", 0)
            .Task(OnWorker0("src")));

  auto macs = client().Resource("l2/macs");
  ASSERT_TRUE(macs) << macs.error().message;
  auto gate1 = types_.Make(macs->value_type, R"({"gate": 1})");
  const int steps = RunWhileForwarding([&](int i) {
    char addr[18];
    std::snprintf(addr, sizeof(addr), "02:00:00:00:%02x:%02x", (i / 2) / 256 % 256,
                  (i / 2) % 256);
    auto key = types_.Make(macs->key_type, std::string(R"({"addr": ")") + addr + R"("})");
    auto tx = client().NewTransaction();
    if (i % 2 == 0) {
      EXPECT_TRUE(tx.Upsert(*macs, *key, *gate1));
      Commit(tx, std::string("upsert ") + addr);
    } else {
      EXPECT_TRUE(tx.Erase(*macs, *key));
      Commit(tx, std::string("erase ") + addr);
    }
  });
  EXPECT_GT(steps, 10) << "too few transactions ran";
  EXPECT_GT(GatePackets("l2", 0) + GatePackets("l2", 1), 0);
}

// The NAT's usage actions run while its worker translates (the log must be
// drainable under traffic, or a full log would stall expiry): a report
// requested under traffic completes and gives the flow's interim record.
TEST_F(StatefulModulesTest, NatUsageCommandsWhileRunning) {
  const auto udp = bess::testing::PacketBuilder()
                       .Ether("06:16:3e:1b:72:32", "02:1e:67:9f:4d:ae")
                       .IPv4("172.16.0.2", "8.8.8.8")
                       .Udp(33000, 53)
                       .Payload("y")
                       .Build();
  auto src_arg = types_.Make("bess.pb.SourceArg", R"({"pkt_size": 60})");
  auto rw_arg = types_.Make("bess.pb.RewriteArg", R"({"templates": [")" + Base64(udp) + R"("]})");
  auto nat_arg = types_.Make(
      "bess.pb.NATArg", R"({"ext_addrs": [{"ext_addr": "192.168.1.1"}], "capacity": 64, "usage": true})");
  Apply(bess::sdk::PipelineBuilder()
            .Worker(0, 0, "")
            .Module("src", "Source", src_arg.get())
            .Module("rw", "Rewrite", rw_arg.get())
            .Module("nat", "NAT", nat_arg.get())
            .Module("sink", "Sink", nullptr)
            .Chain({"src", "rw", "nat"})
            .Connect("nat", 1, "sink", 0)
            .Task(OnWorker0("src")));

  auto empty = types_.Make("bess.pb.EmptyArg", "{}");
  ASSERT_TRUE(client().ResumeWorkers());
  auto requested = client().InvokeAction("nat", "request_usage_report", *empty);
  EXPECT_TRUE(requested) << requested.error().message;
  // The report walks the table on the packet path: drain until it is done.
  std::string drained;
  const auto deadline = Clock::now() + std::chrono::seconds(10);
  while (Clock::now() < deadline) {
    auto resp = client().InvokeAction("nat", "drain_usage", *empty);
    ASSERT_TRUE(resp) << resp.error().message;
    const std::string json = types_.ToJson(*resp);
    drained += json;
    if (json.find(R"("reports_done":"1")") != std::string::npos) {
      break;
    }
  }
  EXPECT_TRUE(client().PauseWorkers());
  EXPECT_TRUE(daemon().Alive());
  EXPECT_NE(std::string::npos, drained.find(R"("reports_done":"1")")) << drained;
  EXPECT_NE(std::string::npos, drained.find(R"("internal_addr":"172.16.0.2","internal_port":33000)"))
      << drained;
  EXPECT_GT(GatePackets("nat", 1), 0);
}

// The ExactMatch's rules are written by transactions while a worker forwards
// traffic through it: rules and the meter and next hop their result names come
// and go under packets, every transaction applied. The referents exist for the
// whole run; each rule names one of them directly (D-172), so the meter and the
// router are on the packet path.
TEST_F(StatefulModulesTest, SessionTransactionsWhileTrafficRuns) {
  auto src_arg = types_.Make("bess.pb.SourceArg", R"({"pkt_size": 60})");
  auto rnd_arg = types_.Make(
      "bess.pb.RandomUpdateArg",
      R"({"fields": [{"offset": 34, "size": 2, "min": 1, "max": 64}]})");
  auto em_arg = types_.Make("bess.pb.ExactMatchArg",
                            R"({"fields": [{"offset": 34, "num_bytes": 2}],
                                "result_fields": [
                                  {"attr_name": "meter_id", "value_offset": 0, "num_bytes": 4,
                                   "reference": "mt/meters", "reference_key": "BE32"},
                                  {"attr_name": "next_hop_id", "value_offset": 4, "num_bytes": 4,
                                   "reference": "rt/next_hops", "reference_key": "BE32"}]})");
  auto mt_arg = types_.Make("bess.pb.MeterArg", R"({"capacity": 64})");
  auto rt_arg = types_.Make("bess.pb.RouterArg",
                            R"({"max_ipv4_routes": 64, "max_ipv4_tbl8s": 16, "max_next_hops": 64})");
  Apply(bess::sdk::PipelineBuilder()
            .Worker(0, 0, "")
            .Module("src", "Source", src_arg.get())
            .Module("rnd", "RandomUpdate", rnd_arg.get())
            .Module("em", "ExactMatch", em_arg.get())
            .Module("mt", "Meter", mt_arg.get())
            .Module("rt", "Router", rt_arg.get())
            .Module("sink", "Sink", nullptr)
            .Chain({"src", "rnd", "em", "mt", "rt", "sink"})
            .Task(OnWorker0("src")));

  auto meters = client().Resource("mt/meters");
  ASSERT_TRUE(meters) << meters.error().message;
  auto next_hops = client().Resource("rt/next_hops");
  ASSERT_TRUE(next_hops) << next_hops.error().message;
  auto rules = client().Resource("em/rules");
  ASSERT_TRUE(rules) << rules.error().message;
  // Source port 1..64, the one field the source and the rules share.
  auto rule_key = [&](int i) {
    const std::vector<uint8_t> port = {0, static_cast<uint8_t>(i + 1)};
    return types_.Make(rules->key_type,
                       R"({"fields": [{"value_bin": ")" + Base64(port) + R"("}]})");
  };
  // A rule's 8-byte result: the meter id then the next-hop id, big-endian.
  const auto result = [](uint32_t id) {
    const std::vector<uint8_t> bytes = {
        static_cast<uint8_t>(id >> 24U), static_cast<uint8_t>(id >> 16U),
        static_cast<uint8_t>(id >> 8U), static_cast<uint8_t>(id),
        static_cast<uint8_t>(id >> 24U), static_cast<uint8_t>(id >> 16U),
        static_cast<uint8_t>(id >> 8U), static_cast<uint8_t>(id)};
    return Base64(bytes);
  };

  // Every source port has a rule before traffic starts, and the four meters
  // and next hops exist for the whole run (each rule references one forever).
  {
    auto tx = client().NewTransaction();
    for (uint32_t id = 1; id <= 4; id++) {
      auto meter_key =
          types_.Make(meters->key_type, R"({"id": )" + std::to_string(id) + "}");
      auto meter_value =
          types_.Make(meters->value_type,
                      R"({"tr_tcm": {"committed_rate": "1000000000",
                                     "committed_burst": "1000000",
                                     "peak_rate": "1000000000",
                                     "peak_burst": "1000000"}})");
      ASSERT_TRUE(tx.Upsert(*meters, *meter_key, *meter_value));
      auto hop_key =
          types_.Make(next_hops->key_type, R"({"id": )" + std::to_string(id) + "}");
      auto hop_value =
          types_.Make(next_hops->value_type, R"({"egress_gate": 0})");
      ASSERT_TRUE(tx.Upsert(*next_hops, *hop_key, *hop_value));
    }
    for (int i = 0; i < 64; i++) {
      auto rule_value = types_.Make(
          rules->value_type,
          R"({"result": ")" + result(1 + i % 4) + R"("})");
      ASSERT_TRUE(tx.Upsert(*rules, *rule_key(i), *rule_value));
    }
    auto done = tx.Commit();
    ASSERT_TRUE(done) << "seed transaction: " << done.error().message;
  }

  int applied = 0;
  const int steps = RunWhileForwarding([&](int i) {
    const uint32_t id = 1 + i % 4;
    auto tx = client().NewTransaction();
    auto rule_value =
        types_.Make(rules->value_type, R"({"result": ")" + result(id) + R"("})");
    ASSERT_TRUE(tx.Upsert(*rules, *rule_key(i % 64), *rule_value));
    ASSERT_TRUE(tx.Erase(*rules, *rule_key((i + 32) % 64)));
    auto done = tx.Commit();
    ASSERT_TRUE(done) << "transaction " << i << ": " << done.error().message;
    applied++;
  });
  EXPECT_EQ(steps, applied);
  EXPECT_GT(applied, 10) << "too few transactions ran";
  EXPECT_GT(GatePackets("em", 0), 0);
}

}  // namespace
