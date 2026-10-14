// SPDX-License-Identifier: BSD-3-Clause

// Plugins built from the installed tree (bess-dev) in the installed bessd
// (M23, M24): ci_profile verify-install runs this binary with BESSD naming
// the staged bessd and BESS_PLUGIN_DIR the plugins' build directory.
// Each plugin's symbols resolve against that bessd, its classes register, and
// packets cross it: Source [-> Rewrite] -> <module> -> Sink on one worker, its
// output gate must have carried packets. The reference appliances also pass
// self_test, and R4's resources are driven over control_v2 (D-094): a
// transaction installs a session across the router and the application before
// the traffic, and the engine's checks across owners hold over the wire.
// Not a meson test: the plugins exist only in verify-install. Replaces
// tools/check_standalone_plugins.py.
//
//   StandalonePlugins.ConformancePlugins   examples/standalone_plugin
//   StandalonePlugins.Appliances           examples/appliances

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "live_daemon.h"

namespace {

using bess::live::Daemon;
using bess::live::Types;
namespace v2 = bess::pb::v2;
namespace gp = google::protobuf;

using Frame = std::array<uint8_t, 60>;

// 60 bytes: Ethernet, IPv4 (10.0.0.1 -> 192.0.2.1), UDP 1000 -> `dst_port`.
Frame UdpFrame(uint16_t dst_port) {
  Frame f{};
  f[12] = 0x08;  // IPv4
  f[14] = 0x45;
  f[17] = 28;
  f[22] = 64;
  f[23] = 17;
  f[26] = 10;
  f[29] = 1;
  f[30] = 192;
  f[32] = 2;
  f[33] = 1;
  f[34] = 0x03;
  f[35] = 0xe8;  // source port 1000
  f[36] = dst_port >> 8;
  f[37] = dst_port & 0xff;
  f[39] = 8;
  return f;
}

// 60 bytes: Ethernet, IPv4 (192.168.0.1 -> `dst`, with its checksum), UDP
// from `sport` to 53.
Frame Ipv4UdpFrame(std::array<uint8_t, 4> dst, uint16_t sport = 1000) {
  Frame f = UdpFrame(53);
  f[34] = sport >> 8;
  f[35] = sport & 0xff;
  f[26] = 192;
  f[27] = 168;
  f[28] = 0;
  f[29] = 1;
  for (int i = 0; i < 4; i++) {
    f[30 + i] = dst[i];
  }
  f[24] = f[25] = 0;
  uint32_t s = 0;
  for (int i = 14; i < 34; i += 2) {
    s += (f[i] << 8) | f[i + 1];
  }
  s = (s & 0xffff) + (s >> 16);
  s = (s & 0xffff) + (s >> 16);
  f[24] = (~s >> 8) & 0xff;
  f[25] = ~s & 0xff;
  return f;
}

std::string Base64(const Frame &f) {
  static constexpr char kDigits[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < f.size(); i += 3) {
    const uint32_t n = (f[i] << 16) | ((i + 1 < f.size() ? f[i + 1] : 0) << 8) |
                       (i + 2 < f.size() ? f[i + 2] : 0);
    out += kDigits[(n >> 18) & 63];
    out += kDigits[(n >> 12) & 63];
    out += i + 1 < f.size() ? kDigits[(n >> 6) & 63] : '=';
    out += i + 2 < f.size() ? kDigits[n & 63] : '=';
  }
  return out;
}

struct Instance {
  std::string mclass;
  std::string name;
  std::optional<Frame> frame;  // nullopt: Source's own packets
  uint32_t gate;               // the output gate those packets must leave by
};

class StandalonePlugins : public ::testing::Test {
 protected:
  void SetUp() override {
    daemon_ = Daemon::Launch({"-modules=" + bess::live::Env("BESS_PLUGIN_DIR")});
    ASSERT_TRUE(daemon_->Alive()) << daemon_->Log();
    types_ = std::make_unique<Types>(client());
  }
  void TearDown() override {
    if (HasFailure()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      std::string log = daemon_->Log();
      ADD_FAILURE() << "bessd log (tail):\n"
                    << (log.size() > 4000 ? log.substr(log.size() - 4000) : log);
    }
  }

  bess::sdk::Client &client() { return daemon_->client(); }

  double GatePackets(const std::string &module, uint32_t gate) {
    auto samples = client().Metrics();
    EXPECT_TRUE(samples) << samples.error().message;
    if (!samples) {
      return 0;
    }
    for (const v2::MetricSample &s : *samples) {
      const auto &l = s.labels();
      if (s.name() == "bess_gate_packets_total" && l.contains("module") &&
          l.at("module") == module && l.contains("gate") &&
          l.at("gate") == std::to_string(gate) && l.contains("direction") &&
          l.at("direction") == "out") {
        return s.value();
      }
    }
    return 0;
  }

  // Applies every instance's chain on worker 0; `before_traffic` runs after
  // the apply, then the workers run 0.3 s; every instance's gate must have
  // carried packets.
  void Run(const std::vector<Instance> &instances, const std::function<void()> &before_traffic) {
    auto caps = client().Capabilities();
    ASSERT_TRUE(caps) << caps.error().message;
    for (const Instance &i : instances) {
      EXPECT_NE(std::find(caps->module_classes().begin(), caps->module_classes().end(),
                          i.mclass),
                caps->module_classes().end())
          << i.mclass << " is not registered";
    }
    std::unique_ptr<gp::Message> empty = types_->Make("bess.pb.EmptyArg", "{}");
    bess::sdk::PipelineBuilder b;
    b.Worker(0, std::nullopt, "");
    std::vector<std::unique_ptr<gp::Message>> rewrites;
    for (const Instance &i : instances) {
      b.Module(i.name + "_src", "Source", nullptr);
      b.Module(i.name, i.mclass, empty.get());
      b.Module(i.name + "_sink", "Sink", nullptr);
      if (i.frame) {
        rewrites.push_back(types_->Make("bess.pb.RewriteArg",
                                        R"({"templates": [")" + Base64(*i.frame) + R"("]})"));
        b.Module(i.name + "_rw", "Rewrite", rewrites.back().get());
        b.Chain({i.name + "_src", i.name + "_rw", i.name});
      } else {
        b.Chain({i.name + "_src", i.name});
      }
      b.Connect(i.name, i.gate, i.name + "_sink", 0);
      v2::TaskPlacement task;
      task.set_module(i.name + "_src");
      task.set_wid(0);
      b.Task(task);
    }
    auto pipeline = b.Build();
    ASSERT_TRUE(pipeline) << pipeline.error().message;
    auto applied = client().ApplyPipeline(*pipeline);
    ASSERT_TRUE(applied) << applied.error().message;
    if (before_traffic) {
      before_traffic();
      ASSERT_FALSE(HasFatalFailure());
    }
    ASSERT_TRUE(client().ResumeWorkers());
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    ASSERT_TRUE(client().PauseWorkers());
    for (const Instance &i : instances) {
      EXPECT_GT(GatePackets(i.name, i.gate), 0)
          << i.mclass << " (" << i.name << "): no packet left by gate " << i.gate;
    }
  }

  std::unique_ptr<Daemon> daemon_;
  std::unique_ptr<Types> types_;
};

TEST_F(StandalonePlugins, ConformancePlugins) {
  Run({
          {"StandalonePass", "pass0", std::nullopt, 0},
          {"StandaloneMacSwap", "macswap0", std::nullopt, 0},
          {"StandaloneRangeGate", "rangegate0", UdpFrame(1500), 1},  // in [1000, 2000]
          {"StandaloneFlowCount", "flowcount0", std::nullopt, 0},    // flows it knows
          // The second appliance looks up the first's policy graph: one sees a
          // classifier hit (gate 1), the other the default action (gate 0).
          {"StandaloneAppliance", "appliance0", UdpFrame(1500), 1},
          {"StandaloneAppliance", "appliance1", UdpFrame(53), 0},
          // Gate 1 only after a request round trip through the maintenance loop.
          {"StandaloneRequests", "requests0", std::nullopt, 1},
      },
      nullptr);
}

// R4's five resources over control_v2 (D-094): the application's QoS
// policies, actions and rules and its router's next hops and routes.
class SessionWire {
 public:
  static constexpr const char *kEnb = "192.0.2.100";

  SessionWire(bess::sdk::Client &client, Types &types) : client_(client), types_(types) {}

  struct Op {
    std::string table;
    std::string key;
    std::optional<std::string> value;  // nullopt: erase
  };

  // Session n: (with `router`) next hop n and the eNodeB's /32 route, then
  // QoS policy n, action n (naming next hop `next_hop`) and rule n.
  static std::vector<Op> SessionOps(int n, const std::string &ue, int next_hop, bool router) {
    const std::string id = R"({"id": )" + std::to_string(n) + "}";
    std::vector<Op> ops;
    if (router) {
      ops.push_back({"next_hops", id,
                     R"({"egress_gate": 0, "neighbor": "ROUTER_NEIGHBOR_STATE_RESOLVED"})"});
      ops.push_back({"routes", std::string(R"({"ipv4": ")") + kEnb + R"(", "prefix_length": 32})",
                     R"({"next_hop_id": )" + std::to_string(n) + "}"});
    }
    ops.push_back({"qos", id,
                   R"({"committed_rate": 1000000000, "committed_burst": 1500,)"
                   R"( "excess_burst": 1500})"});
    ops.push_back({"actions", id,
                   R"({"qos_id": )" + std::to_string(n) + R"(, "next_hop_id": )" +
                       std::to_string(next_hop) + R"(, "enb": ")" + kEnb + R"(", "teid": )" +
                       std::to_string(0x1000 + n) + "}"});
    ops.push_back({"rules", id,
                   R"({"ue": ")" + ue + R"(", "action_id": )" + std::to_string(n) + "}"});
    return ops;
  }

  bess::sdk::Result<bess::sdk::Applied> Apply(const std::vector<Op> &ops) {
    auto tx = client_.NewTransaction();
    for (const Op &op : ops) {
      const bess::sdk::Resource &r = resources_.at(op.table);
      auto key = types_.Make(r.key_type, op.key);
      auto staged = op.value ? tx.Upsert(r, *key, *types_.Make(r.value_type, *op.value))
                             : tx.Erase(r, *key);
      EXPECT_TRUE(staged) << op.table << " " << op.key << ": " << staged.error().message;
    }
    return tx.Commit();
  }

  // Before the traffic: the resources are listed with their types, and one
  // transaction installs session 1 across the router and the application.
  void Install() {
    auto listed = client_.Resources(true);
    ASSERT_TRUE(listed) << listed.error().message;
    const std::vector<std::array<std::string, 3>> expected = {
        {"qos", "session_appliance.QosKey", "session_appliance.QosValue"},
        {"actions", "session_appliance.ActionKey", "session_appliance.ActionValue"},
        {"rules", "session_appliance.RuleKey", "session_appliance.RuleValue"},
        {"next_hops", "bess.pb.RouterNextHopIdKey", "bess.pb.RouterNextHopValue"},
        {"routes", "bess.pb.RouterRouteKey", "bess.pb.RouterRouteValue"},
    };
    for (const auto &[table, key, value] : expected) {
      auto it = listed->find("session0/" + table);
      ASSERT_NE(it, listed->end()) << "session0/" << table << " is not listed";
      EXPECT_EQ(key, it->second.key_type) << table;
      EXPECT_EQ(value, it->second.value_type) << table;
      resources_.emplace(table, it->second);
    }
    auto applied = Apply(SessionOps(1, "10.45.0.1", 1, true));
    ASSERT_TRUE(applied) << "installing session 1: " << applied.error().message;
    for (const v2::TransactionOpResult &op : applied->record.ops()) {
      EXPECT_EQ(v2::TransactionOpResult::STATUS_APPLIED, op.status()) << op.error();
    }
    generation_ = applied->generation;
  }

  // After the traffic: the engine's checks across owners, over the wire.
  void Check() {
    // An action naming a next hop nobody installs: the whole session is
    // refused, and nothing of it appears.
    auto missing_hop = Apply(SessionOps(2, "10.45.0.2", 2, false));
    ASSERT_FALSE(missing_hop) << "a session naming a missing next hop applied";
    EXPECT_EQ(bess::sdk::ErrorKind::kRejected, missing_hop.error().kind)
        << missing_hop.error().message;
    auto absent_rule = Apply({{"rules", R"({"id": 2})", std::nullopt}});
    ASSERT_FALSE(absent_rule) << "the refused session's rule exists";
    EXPECT_EQ(bess::sdk::ErrorKind::kRejected, absent_rule.error().kind);
    // The router's next hop cannot leave while the application's action
    // names it; with the session, referrers first, it can.
    const Op route{"routes", std::string(R"({"ipv4": ")") + kEnb + R"(", "prefix_length": 32})",
                   std::nullopt};
    const Op hop{"next_hops", R"({"id": 1})", std::nullopt};
    auto kept = Apply({route, hop});
    ASSERT_FALSE(kept) << "a next hop an action names was removed";
    EXPECT_EQ(bess::sdk::ErrorKind::kRejected, kept.error().kind);
    auto removed = Apply({{"rules", R"({"id": 1})", std::nullopt},
                          {"actions", R"({"id": 1})", std::nullopt},
                          {"qos", R"({"id": 1})", std::nullopt},
                          route,
                          hop});
    ASSERT_TRUE(removed) << "removing the session with its route and next hop: "
                         << removed.error().message;
    // The three rejected transactions moved no generation.
    EXPECT_EQ(generation_ + 1, removed->generation);
  }

 private:
  bess::sdk::Client &client_;
  Types &types_;
  std::map<std::string, bess::sdk::Resource> resources_;
  uint64_t generation_ = 0;
};

TEST_F(StandalonePlugins, Appliances) {
  SessionWire wire(client(), *types_);
  Run({
          // R1: 172.16.1.1 arriving on igate 0 (VRF 1) leaves by interface 1 (gate 0).
          {"RouterAppliance", "router0", Ipv4UdpFrame({172, 16, 1, 1}), 0},
          // R2: an inside host's UDP to 8.8.8.8 is admitted, mapped and leaves
          // outside (an unprivileged source port: a privileged one maps only
          // below 1024, outside this appliance's pool).
          {"NatAppliance", "nat0", Ipv4UdpFrame({8, 8, 8, 8}, 40000), 0},
          // R3: DNS to 10.0.0.9 for tenant 0 is allowed by its ACL layer.
          {"VswitchAppliance", "vswitch0", Ipv4UdpFrame({10, 0, 0, 9}, 40000), 0},
          // R4: a downlink to the UE 10.45.0.1 that the installed session
          // claims leaves as GTP-U by its next hop's gate 0.
          {"SessionAppliance", "session0", Ipv4UdpFrame({10, 45, 0, 1}), 0},
      },
      [&] { wire.Install(); });
  ASSERT_FALSE(HasFatalFailure());
  std::unique_ptr<gp::Message> empty = types_->Make("bess.pb.EmptyArg", "{}");
  for (const char *module : {"router0", "nat0", "vswitch0", "session0"}) {
    auto ran = client().InvokeAction(module, "self_test", *empty);
    EXPECT_TRUE(ran) << module << " self_test: " << ran.error().message;
  }
  wire.Check();
}

}  // namespace
