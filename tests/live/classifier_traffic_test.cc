// SPDX-License-Identifier: BSD-3-Clause

// Modules on running workers (was the run_for / run_with_live_commands cases
// of bessctl/module_tests/{iplookup,acl,hash_lb,bpf,url_filter,drr}.py, and
// conntrack.py's PER_WORKER cases and worker_split.py): traffic flows while
// transactions change the module's resources, the daemon survives, and the
// gate counters (D-116) show where the packets went. What a module does to
// given packets is tested in process (core/modules/*_test.cc).

#include <sched.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "live_daemon.h"
#include "packet_builder.h"

namespace {

namespace v2 = bess::pb::v2;
namespace gp = google::protobuf;
using bess::live::AllowedCores;
using bess::live::LiveTest;
using bess::live::OnWorker;
using bess::live::Types;
using bess::sdk::PipelineBuilder;
using bess::testing::Bytes;
using bess::testing::PacketBuilder;
using std::chrono::milliseconds;

// Random source and destination addresses, as run_for and
// run_with_live_commands used.
constexpr char kRandomAddresses[] =
    R"({"fields": [{"offset": 26, "size": 4, "min": 1, "max": 4294967295},
                   {"offset": 30, "size": 4, "min": 1, "max": 4294967295}]})";

std::string Base64(const Bytes &data) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  for (size_t i = 0; i < data.size(); i += 3) {
    const uint32_t n = (uint32_t{data[i]} << 16) |
                       (i + 1 < data.size() ? uint32_t{data[i + 1]} << 8 : 0) |
                       (i + 2 < data.size() ? uint32_t{data[i + 2]} : 0);
    out += kAlphabet[(n >> 18) & 63];
    out += kAlphabet[(n >> 12) & 63];
    out += i + 1 < data.size() ? kAlphabet[(n >> 6) & 63] : '=';
    out += i + 2 < data.size() ? kAlphabet[n & 63] : '=';
  }
  return out;
}

v2::TaskPlacement UnderClass(const std::string &module, const std::string &parent) {
  v2::TaskPlacement t;
  t.set_module(module);
  t.set_parent(parent);
  return t;
}

v2::TrafficClass RateLimit(const std::string &name, int wid, const std::string &resource,
                           int64_t limit) {
  v2::TrafficClass tc;
  tc.set_name(name);
  tc.set_policy("rate_limit");
  tc.set_resource(resource);
  tc.set_wid(wid);
  (*tc.mutable_limit())[resource] = limit;
  return tc;
}

class ClassifierTrafficTest : public LiveTest {
 protected:
  void SetUp() override {
    LiveTest::SetUp();
    types_ = std::make_unique<Types>(client());
  }

  std::unique_ptr<gp::Message> Arg(const std::string &type, const std::string &json) {
    return types_->Make("bess.pb." + type, json);
  }

  void Apply(const PipelineBuilder &builder) {
    auto pipeline = builder.Build();
    ASSERT_TRUE(pipeline) << pipeline.error().message;
    auto applied = client().ApplyPipeline(*pipeline);
    ASSERT_TRUE(applied) << applied.error().message;
  }

  // One transaction that must apply: the singleton `r` set to `json`.
  void Set(const bess::sdk::Resource &r, const std::string &json) {
    auto value = types_->Make(r.value_type, json);
    auto tx = client().NewTransaction();
    auto staged = tx.Set(r, *value);
    EXPECT_TRUE(staged) << r.name << " = " << json << ": " << staged.error().message;
    Commit(tx, r.name + " = " + json);
  }
  // One transaction that must apply: `key` -> `value` upserted into the map `r`.
  void Upsert(const bess::sdk::Resource &r, const std::string &key, const std::string &value) {
    auto k = types_->Make(r.key_type, key);
    auto v = types_->Make(r.value_type, value);
    auto tx = client().NewTransaction();
    auto staged = tx.Upsert(r, *k, *v);
    EXPECT_TRUE(staged) << r.name << " " << key << ": " << staged.error().message;
    Commit(tx, "upsert " + r.name + " " + key + " " + value);
  }
  // One transaction that must apply: `key` erased from the map `r`.
  void Erase(const bess::sdk::Resource &r, const std::string &key) {
    auto k = types_->Make(r.key_type, key);
    auto tx = client().NewTransaction();
    auto staged = tx.Erase(r, *k);
    EXPECT_TRUE(staged) << r.name << " " << key << ": " << staged.error().message;
    Commit(tx, "erase " + r.name + " " + key);
  }
  void Commit(bess::sdk::Transaction &tx, const std::string &what) {
    auto done = tx.Commit();
    EXPECT_TRUE(done) << what << ": " << done.error().message;
  }

  // Packets that left `module` through output gate `gate` (nullopt: the
  // gate is not connected).
  std::optional<double> GatePackets(const std::string &module, int gate) {
    auto samples = client().Metrics();
    EXPECT_TRUE(samples) << samples.error().message;
    if (!samples) {
      return std::nullopt;
    }
    for (const v2::MetricSample &s : *samples) {
      if (s.name() != "bess_gate_packets_total") {
        continue;
      }
      const auto &l = s.labels();
      if (l.contains("module") && l.at("module") == module && l.contains("gate") &&
          l.at("gate") == std::to_string(gate) && l.contains("direction") &&
          l.at("direction") == "out") {
        return s.value();
      }
    }
    return std::nullopt;
  }
  double Packets(const std::string &module, int gate) {
    const auto n = GatePackets(module, gate);
    EXPECT_TRUE(n) << module << ":" << gate << " has no counter";
    return n.value_or(-1);
  }

  // Source -> RandomUpdate -> `name` (igate 0) on worker 0, each of
  // `ogates` to a Sink: what run_for and run_with_live_commands built.
  PipelineBuilder Traffic(const std::string &name, const std::string &mclass,
                          const gp::Message *arg, const std::vector<int> &ogates) {
    random_ = Arg("RandomUpdateArg", kRandomAddresses);
    PipelineBuilder b;
    b.Worker(0, AllowedCores().at(0), "")
        .Module("src", "Source", nullptr)
        .Module("rnd", "RandomUpdate", random_.get())
        .Module(name, mclass, arg)
        .Chain({"src", "rnd", name})
        .Task(OnWorker("src", 0));
    for (int g : ogates) {
      const std::string sink = "sink" + std::to_string(g);
      b.Module(sink, "Sink", nullptr).Connect(name, g, sink, 0);
    }
    return b;
  }

  // Workers resumed for `duration`, `command(i)` (i = 0, 1, ...: a
  // transaction) called in a loop meanwhile; then paused. The number run.
  int RunWhileForwarding(std::chrono::milliseconds duration,
                         const std::function<void(int)> &command = {}) {
    auto resumed = client().ResumeWorkers();
    EXPECT_TRUE(resumed) << resumed.error().message;
    const auto deadline = std::chrono::steady_clock::now() + duration;
    int i = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      if (command) {
        command(i++);
      } else {
        std::this_thread::sleep_for(milliseconds(10));
      }
    }
    auto paused = client().PauseWorkers();
    EXPECT_TRUE(paused) << paused.error().message;
    EXPECT_TRUE(daemon().Alive());
    return i;
  }

  std::unique_ptr<Types> types_;
  std::unique_ptr<gp::Message> random_;
};

// -- transactions while a worker forwards (run_with_live_commands) -----------

// Routes change while a worker forwards traffic (K7, mode C): the FIB is the
// Router's now (D-155, destination mode), and the route's next hop is where
// the packet leaves.
TEST_F(ClassifierTrafficTest, RouterRoutesChangeWhileForwarding) {
  auto arg = Arg("RouterArg",
                 R"({"mode": "ROUTE_MODE_DESTINATION", "max_routes": 512, "max_next_hops": 2})");
  Apply(Traffic("rt", "Router", arg.get(), {0, 1})
            .Entry("rt/next_hops", *Arg("RouterNextHopIdKey", R"({"id": 1})"),
                   *Arg("RouterNextHopValue",
                        R"({"egress_gate": 0, "dst_mac": "AgAAAAAC", "src_mac": "AgAAAAAB"})"))
            .Entry("rt/next_hops", *Arg("RouterNextHopIdKey", R"({"id": 2})"),
                   *Arg("RouterNextHopValue",
                        R"({"egress_gate": 1, "dst_mac": "AgAAAAAC", "src_mac": "AgAAAAAB"})"))
            .Entry("rt/routes", *Arg("RouterRouteKey", R"({"ipv4": "0.0.0.0", "prefix_length": 0})"),
                   *Arg("RouterRouteValue", R"({"next_hop_id": 1})")));
  auto routes = client().Resource("rt/routes");
  ASSERT_TRUE(routes) << routes.error().message;
  const int commands = RunWhileForwarding(milliseconds(2000), [&](int i) {
    const std::string ipv4 =
        std::to_string(1 + (i / 2) % 200) + "." + std::to_string((i / 400) % 256) + ".0.0";
    const std::string key = R"({"ipv4": ")" + ipv4 + R"(", "prefix_length": 16})";
    if (i % 2 == 0) {
      Upsert(*routes, key, R"({"next_hop_id": 2})");
    } else {
      Erase(*routes, key);
    }
  });
  EXPECT_GT(commands, 10) << "too few transactions ran";
  EXPECT_GT(Packets("rt", 0) + Packets("rt", 1), 0);
}

// Rules are added and cleared while a worker forwards traffic (mode G): each
// transaction sets the whole rule list.
TEST_F(ClassifierTrafficTest, AclRulesChangeWhileForwarding) {
  const std::string pass_all = R"({"src_ip": "0.0.0.0/0", "drop": false})";
  auto arg = Arg("ACLArg", R"({"rules": [)" + pass_all + "]}");
  Apply(Traffic("acl", "ACL", arg.get(), {0}));
  auto rules = client().Resource("acl/rules");
  ASSERT_TRUE(rules) << rules.error().message;
  std::string list = pass_all;
  const int commands = RunWhileForwarding(milliseconds(2000), [&](int i) {
    if (i % 10 == 9) {
      list = pass_all;
    } else {
      list += R"(, {"dst_port": )" + std::to_string(1000 + i) + R"(, "drop": true})";
    }
    Set(*rules, R"({"rules": [)" + list + "]}");
  });
  EXPECT_GT(commands, 10) << "too few transactions ran";
  EXPECT_GT(Packets("acl", 0), 0);
}

// Gates and mode change while a worker forwards traffic (mode G): each
// transaction sets both, one of them changed.
TEST_F(ClassifierTrafficTest, HashLbGatesAndModeChangeWhileForwarding) {
  auto arg = Arg("HashLBArg", R"({"gates": [0, 1]})");
  Apply(Traffic("lb", "HashLB", arg.get(), {0, 1, 2, 3}));
  auto settings = client().Resource("lb/settings");
  ASSERT_TRUE(settings) << settings.error().message;
  std::string gates = "[0, 1]", mode = "l4";
  const int commands = RunWhileForwarding(milliseconds(2000), [&](int i) {
    if (i % 2 == 0) {
      gates = (i / 2) % 2 == 0 ? "[0, 1]" : "[1, 2, 3]";
    } else {
      mode = (i / 2) % 2 == 0 ? "l3" : "l4";
    }
    Set(*settings, R"({"mode": ")" + mode + R"(", "gates": )" + gates + "}");
  });
  EXPECT_GT(commands, 10) << "too few transactions ran";
  double total = 0;
  for (int g = 0; g < 4; g++) {
    total += Packets("lb", g);
  }
  EXPECT_GT(total, 0);
}

// -- a module on a worker (run_for) ------------------------------------------

// Every packet leaves BPF on some gate (unmatched ones on gate 0): what
// RandomUpdate sent is what BPF's gates forwarded.
TEST_F(ClassifierTrafficTest, BpfSingleFilterOnAWorker) {
  Apply(Traffic("bpf", "BPF", nullptr, {0, 1})
            .Setting("bpf/filters",
                     *Arg("BPFArg", R"({"filters": [{"priority": 0, "filter": "tcp src port 92",
                                                     "gate": 1}]})")));
  RunWhileForwarding(milliseconds(1000));
  const double sent = Packets("rnd", 0);
  EXPECT_GT(sent, 0);
  EXPECT_EQ(sent, Packets("bpf", 0) + Packets("bpf", 1));
}

TEST_F(ClassifierTrafficTest, BpfFiveFiltersOnAWorker) {
  const char *filters[] = {"tcp src port 92", "len <= 1000", "ether proto 0x800",
                           "ip proto 47 or ip6 proto 47", "ip host 22.22.22.22"};
  std::string list;
  for (int i = 0; i < 5; i++) {
    list += std::string(i ? ", " : "") + R"({"priority": )" + std::to_string(i) +
            R"(, "filter": ")" + filters[i] + R"(", "gate": )" + std::to_string(i) + "}";
  }
  Apply(Traffic("bpf", "BPF", nullptr, {0, 1, 2, 3, 4})
            .Setting("bpf/filters", *Arg("BPFArg", R"({"filters": [)" + list + "]}")));
  RunWhileForwarding(milliseconds(1000));
  const double sent = Packets("rnd", 0);
  EXPECT_GT(sent, 0);
  double out = 0;
  for (int g = 0; g < 5; g++) {
    out += Packets("bpf", g);
  }
  EXPECT_EQ(sent, out);
  // Every Source packet is 60 bytes: at least "len <= 1000" (gate 1)
  // matches, so none falls through to gate 0.
  EXPECT_EQ(0, Packets("bpf", 0));
}

// Random traffic is not HTTP to a blacklisted host: all of it passes on
// gate 0, nothing is answered on gate 1.
TEST_F(ClassifierTrafficTest, UrlFilterOnAWorker) {
  Apply(Traffic("uf", "UrlFilter", nullptr, {0, 1})
            .Setting("uf/blacklist",
                     *Arg("UrlFilterConfig",
                          R"({"blacklist": [{"host": "www.foo.com", "path": "/foo"},
                                            {"host": "www.bar.com", "path": "/bar"}]})")));
  RunWhileForwarding(milliseconds(1000));
  const double sent = Packets("rnd", 0);
  EXPECT_GT(sent, 0);
  EXPECT_EQ(sent, Packets("uf", 0));
  EXPECT_EQ(0, Packets("uf", 1));
}

// DRR queues what arrives and its own task sends it on.
TEST_F(ClassifierTrafficTest, DrrOnAWorker) {
  Apply(Traffic("drr", "DRR", nullptr, {0}).Task(OnWorker("drr", 0)));
  RunWhileForwarding(milliseconds(1000));
  EXPECT_GT(Packets("rnd", 0), 0);
  EXPECT_GT(Packets("drr", 0), 0);
}

// Producers on two workers and the DRR task on a third, rate-limited so flow
// queues fill and grow, with settings transactions throughout. Before D-019
// the producers created, resized and freed flow queues that the task was
// dequeuing from; a stress test cannot prove the race is gone (D-007), but
// it did crash the old code, and it checks that the settings (which do not
// require paused workers) apply without a pause.
TEST_F(ClassifierTrafficTest, DrrAcrossWorkersWithLiveCommands) {
  const auto cores = AllowedCores();
  if (cores.size() < 3) {
    GTEST_SKIP() << "needs three allowed CPUs";
  }
  auto drr_arg = Arg("DRRArg", R"({"num_flows": 1024, "max_flow_queue_size": 8192})");
  auto flows = Arg("RandomUpdateArg",
                   R"({"fields": [{"offset": 26, "size": 4, "min": 1, "max": 2000},
                                  {"offset": 34, "size": 2, "min": 1, "max": 4}]})");
  PipelineBuilder b;
  for (int wid = 0; wid < 3; wid++) {
    b.Worker(wid, cores[wid], "");
  }
  b.TrafficClass(RateLimit("drr_rl", 2, "packet", 2000000))
      .Module("drr", "DRR", drr_arg.get())
      .Module("sink", "Sink", nullptr)
      .Chain({"drr", "sink"})
      .Task(UnderClass("drr", "drr_rl"));
  for (int wid = 0; wid < 2; wid++) {
    const std::string src = "src" + std::to_string(wid), rnd = "rnd" + std::to_string(wid);
    b.Module(src, "Source", nullptr)
        .Module(rnd, "RandomUpdate", flows.get())
        .Chain({src, rnd, "drr"})
        .Task(OnWorker(src, wid));
  }
  Apply(b);
  auto settings = client().Resource("drr/settings");
  ASSERT_TRUE(settings) << settings.error().message;
  const int commands = RunWhileForwarding(milliseconds(3000), [&](int i) {
    Set(*settings, R"({"quantum": )" + std::to_string(1000 + i % 1000) +
                       R"(, "max_flow_queue_size": )" + std::to_string(4096 + i % 4096) + "}");
  });
  EXPECT_GT(commands, 10) << "too few transactions ran";
  EXPECT_GT(Packets("drr", 0), 0);
}

// -- WorkerSplit --------------------------------------------------------------

// By default packets leave on the gate of the worker that runs them.
TEST_F(ClassifierTrafficTest, WorkerSplitSendsEachWorkerToItsGate) {
  const auto cores = AllowedCores();
  if (cores.size() < 2) {
    GTEST_SKIP() << "worker split needs at least two allowed CPUs";
  }
  for (int wid = 0; wid < 2; wid++) {
    SCOPED_TRACE("source on worker " + std::to_string(wid));
    daemon().Reset();
    PipelineBuilder b;
    b.Worker(0, cores[0], "")
        .Worker(1, cores[1], "")
        .Module("src", "Source", nullptr)
        .Module("ws", "WorkerSplit", nullptr)
        .Chain({"src", "ws"})
        .Task(OnWorker("src", wid));
    for (int g = 0; g < 2; g++) {
      const std::string sink = "sink" + std::to_string(g);
      b.Module(sink, "Sink", nullptr).Connect("ws", g, sink, 0);
    }
    Apply(b);
    RunWhileForwarding(milliseconds(500));
    EXPECT_GT(Packets("ws", wid), 0);
    EXPECT_EQ(0, Packets("ws", 1 - wid));
  }
}

// worker_gates maps workers to gates: even workers to gate 0, odd ones to
// gate 1, each gate forwarding exactly what its workers' sources sent.
TEST_F(ClassifierTrafficTest, WorkerSplitMapsWorkersToGates) {
  const auto cores = AllowedCores();
  if (cores.size() < 2) {
    GTEST_SKIP() << "worker split needs at least two allowed CPUs";
  }
  const int workers = static_cast<int>(std::min<size_t>(cores.size(), 4));
  std::string gates;
  for (int i = 0; i < workers; i++) {
    gates += (i ? ", \"" : "\"") + std::to_string(i) + "\": " + std::to_string(i & 1);
  }
  auto ws_arg = Arg("WorkerSplitArg", R"({"worker_gates": {)" + gates + "}}");
  PipelineBuilder b;
  b.Module("ws", "WorkerSplit", ws_arg.get())
      .Module("sink0", "Sink", nullptr)
      .Module("sink1", "Sink", nullptr)
      .Connect("ws", 0, "sink0", 0)
      .Connect("ws", 1, "sink1", 0);
  for (int i = 0; i < workers; i++) {
    const std::string src = "src" + std::to_string(i), tc = "rl_" + std::to_string(i);
    b.Worker(i, cores[i], "")
        .TrafficClass(RateLimit(tc, i, "count", 1000))
        .Module(src, "Source", nullptr)
        .Connect(src, 0, "ws", 0)
        .Task(UnderClass(src, tc));
  }
  Apply(b);
  RunWhileForwarding(milliseconds(1000));
  double even = 0, odd = 0;
  for (int i = 0; i < workers; i++) {
    (i % 2 == 0 ? even : odd) += Packets("src" + std::to_string(i), 0);
  }
  EXPECT_GT(even, 0);
  EXPECT_GT(odd, 0);
  EXPECT_EQ(even, Packets("ws", 0));
  EXPECT_EQ(odd, Packets("ws", 1));
}

// -- ConnTrack PER_WORKER: inputs checked at resume -------------------------

constexpr char kInside[] = "172.16.0.2";
constexpr char kOutside[] = "8.8.8.8";

Bytes TcpFrame(const char *src, const char *dst, uint16_t sport, uint16_t dport, uint8_t flags) {
  return PacketBuilder()
      .Ether("06:16:3e:1b:72:32", "02:1e:67:9f:4d:ae")
      .IPv4(src, dst)
      .Tcp(sport, dport, flags)
      .Payload("x")
      .Build();
}

// A client of a UnixSocketPort at the abstract address `name`.
class SocketClient {
 public:
  explicit SocketClient(const std::string &name) {
    fd_ = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path + 1, name.data(), name.size());
    const auto len = static_cast<socklen_t>(sizeof(addr.sun_family) + 1 + name.size());
    connected_ = fd_ >= 0 && connect(fd_, reinterpret_cast<sockaddr *>(&addr), len) == 0;
  }
  ~SocketClient() {
    if (fd_ >= 0) {
      close(fd_);
    }
  }
  bool connected() const { return connected_; }
  bool Send(const Bytes &frame) {
    return send(fd_, frame.data(), frame.size(), 0) == static_cast<ssize_t>(frame.size());
  }

 private:
  int fd_ = -1;
  bool connected_ = false;
};

// The harness fed the module from single-queue ports on one worker:
// symmetric, so per-worker tables are allowed, and the inside starts
// connections that the outside's replies then match.
TEST_F(ClassifierTrafficTest, ConnTrackPerWorkerOnSymmetricInputs) {
  const std::string tag = "bess_live_ct_" + std::to_string(getpid());
  auto port_in = Arg("UnixSocketPortArg", R"({"path": "@)" + tag + R"(_in"})");
  auto port_out = Arg("UnixSocketPortArg", R"({"path": "@)" + tag + R"(_out"})");
  auto inc_in = Arg("PortIncArg", R"({"port": "p_in"})");
  auto inc_out = Arg("PortIncArg", R"({"port": "p_out"})");
  auto ct_arg = Arg("ConnTrackArg", R"({"mode": "PER_WORKER"})");
  PipelineBuilder b;
  b.Worker(0, AllowedCores().at(0), "")
      .Port("p_in", "UnixSocketPort", port_in.get())
      .Port("p_out", "UnixSocketPort", port_out.get())
      .Module("inside", "PortInc", inc_in.get())
      .Module("outside", "PortInc", inc_out.get())
      .Module("ct", "ConnTrack", ct_arg.get())
      .Module("sink0", "Sink", nullptr)
      .Module("sink1", "Sink", nullptr)
      .Connect("inside", 0, "ct", 0)
      .Connect("outside", 0, "ct", 1)
      .Connect("ct", 0, "sink0", 0)
      .Connect("ct", 1, "sink1", 0)
      .Task(OnWorker("inside", 0))
      .Task(OnWorker("outside", 0));
  Apply(b);
  auto resumed = client().ResumeWorkers();
  ASSERT_TRUE(resumed) << resumed.error().message;

  SocketClient inside(tag + "_in"), outside(tag + "_out");
  ASSERT_TRUE(inside.connected());
  ASSERT_TRUE(outside.connected());
  // Waits until ct's gates 0 and 1 have forwarded `tracked` and `rest`.
  auto reached = [&](double tracked, double rest) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
      if (GatePackets("ct", 0) == tracked && GatePackets("ct", 1) == rest) {
        return true;
      }
      std::this_thread::sleep_for(milliseconds(20));
    }
    ADD_FAILURE() << "ct:0 " << Packets("ct", 0) << " (want " << tracked << "), ct:1 "
                  << Packets("ct", 1) << " (want " << rest << ")";
    return false;
  };
  ASSERT_TRUE(inside.Send(TcpFrame(kInside, kOutside, 40000, 80, 0x02)));
  ASSERT_TRUE(reached(1, 0)) << "an inside SYN starts a connection";
  ASSERT_TRUE(outside.Send(TcpFrame(kOutside, kInside, 80, 40000, 0x12)));
  ASSERT_TRUE(reached(2, 0)) << "its reply is tracked";
  ASSERT_TRUE(outside.Send(TcpFrame(kOutside, kInside, 443, 40001, 0x02)));
  ASSERT_TRUE(reached(2, 1)) << "an outside SYN starts none";
  auto paused = client().PauseWorkers();
  EXPECT_TRUE(paused) << paused.error().message;
}

// An inside SYN from a Source (not a port queue): no symmetric hash to rely
// on. `fallback_shared` false: per-worker tracking is refused and everything
// goes to ogate 1; true: one shared table tracks it, to ogate 0.
class ConnTrackSourceFedTest : public ClassifierTrafficTest,
                               public ::testing::WithParamInterface<bool> {};

TEST_P(ConnTrackSourceFedTest, PerWorkerOnOtherInputs) {
  const bool fallback = GetParam();
  auto syn = Arg("RewriteArg",
                 R"({"templates": [")" + Base64(TcpFrame(kInside, kOutside, 40000, 80, 0x02)) +
                     R"("]})");
  auto ct_arg = Arg("ConnTrackArg", std::string(R"({"mode": "PER_WORKER", "fallback_shared": )") +
                                        (fallback ? "true" : "false") + "}");
  Apply(PipelineBuilder()
            .Worker(0, AllowedCores().at(0), "")
            .Module("src", "Source", nullptr)
            .Module("syn", "Rewrite", syn.get())
            .Module("ct", "ConnTrack", ct_arg.get())
            .Module("sink0", "Sink", nullptr)
            .Module("sink1", "Sink", nullptr)
            .Chain({"src", "syn", "ct"})
            .Connect("ct", 0, "sink0", 0)
            .Connect("ct", 1, "sink1", 0)
            .Task(OnWorker("src", 0)));
  RunWhileForwarding(milliseconds(500));
  const double sent = Packets("syn", 0);
  ASSERT_GT(sent, 0);
  if (fallback) {
    EXPECT_EQ(sent, Packets("ct", 0)) << "tracked in the shared table";
    EXPECT_EQ(0, Packets("ct", 1));
  } else {
    EXPECT_EQ(0, Packets("ct", 0));
    EXPECT_EQ(sent, Packets("ct", 1)) << "fails closed";
  }
}

INSTANTIATE_TEST_SUITE_P(FallbackShared, ConnTrackSourceFedTest, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool> &info) {
                           return info.param ? std::string("FallsBackToShared")
                                             : std::string("FailsClosed");
                         });

}  // namespace
