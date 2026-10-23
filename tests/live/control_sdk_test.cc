// SPDX-License-Identifier: BSD-3-Clause

// The C++ control SDK against a live bessd: a transaction
// steers packets, a stale one changes nothing, an erase returns them to the
// default gate. Black-box recovery, through the daemon's real transport with
// ApplyTransaction's fate scripted: an answer lost after the daemon applied is
// recovered through GetTransaction (applied once); a request lost before it
// arrived is sent again; a restart between the send and the question is
// kDaemonRestarted, the old handles are refused, and a pipeline built under
// the old epoch is refused by the new daemon. Every resource of every class
// round trips as desired state (D-129). Request-id replay, conflicts, typed
// keys, discovery and the pipeline round trip are conformance cases
// (sdk/conformance/cases.json, tests/live/conformance_test.cc).

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <deque>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/util/message_differencer.h>

#include "live_daemon.h"
#include "steering.h"

namespace {

using bess::live::LiveTest;
using bess::live::Types;
using bess::sdk::Deadline;
using bess::sdk::ErrorKind;
using bess::sdk::Trailers;
namespace v2 = bess::pb::v2;

// 65.43.21.0 -> 12.34.56.78.
constexpr uint32_t kSip = 0x412B1500;
constexpr uint32_t kDip = 0x0C22384E;

enum class Fate { kDeliver, kLoseAnswer, kLoseRequest };

// The daemon's real transport, with ApplyTransaction's fate scripted per
// call: delivered; sent, then the answer lost (after `then`); or never sent.
class LossyTransport final : public bess::sdk::Transport {
 public:
  LossyTransport(const std::string &address, std::deque<Fate> fates,
                 std::function<void()> then = {})
      : real_(bess::sdk::GrpcTransport(address)), fates_(std::move(fates)), then_(std::move(then)) {}

  int sent() const { return sent_; }

  grpc::Status ApplyTransaction(Deadline deadline, const v2::ApplyTransactionRequest &req,
                                v2::ApplyTransactionResponse *resp, Trailers *trailers) override {
    Fate fate = Fate::kDeliver;
    if (!fates_.empty()) {
      fate = fates_.front();
      fates_.pop_front();
    }
    if (fate == Fate::kLoseRequest) {
      return grpc::Status(grpc::StatusCode::UNAVAILABLE, "lost");
    }
    sent_++;
    const grpc::Status status = real_->ApplyTransaction(deadline, req, resp, trailers);
    if (fate == Fate::kLoseAnswer) {
      if (then_) {
        then_();
      }
      resp->Clear();
      if (trailers != nullptr) {
        trailers->clear();
      }
      return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "lost");
    }
    return status;
  }

#define FORWARD(Rpc)                                                                      \
  grpc::Status Rpc(Deadline d, const v2::Rpc##Request &q, v2::Rpc##Response *r,           \
                   Trailers *t) override {                                                \
    return real_->Rpc(d, q, r, t);                                                        \
  }
  FORWARD(GetTransaction)
  FORWARD(ListTransactionResources)
  FORWARD(GetCapabilities)
  FORWARD(GetSchema)
  FORWARD(ListMetrics)
  FORWARD(GetPipeline)
  FORWARD(ValidatePipeline)
  FORWARD(DiffPipeline)
  FORWARD(PlanPipeline)
  FORWARD(ApplyPipeline)
  FORWARD(InvokeAction)
  FORWARD(ImportPlugin)
  FORWARD(UnloadPlugin)
  FORWARD(GetPortStatus)
  FORWARD(GetPacketPools)
  FORWARD(SetCapture)
  FORWARD(PauseWorkers)
  FORWARD(ResumeWorkers)
  FORWARD(Shutdown)
#undef FORWARD

  std::unique_ptr<bess::sdk::EventStream> WatchEvents(const v2::WatchEventsRequest &req,
                                                      grpc::Status *status) override {
    return real_->WatchEvents(req, status);
  }

 private:
  std::shared_ptr<bess::sdk::Transport> real_;
  std::deque<Fate> fates_;
  std::function<void()> then_;
  int sent_ = 0;
};

class ControlSdkTest : public LiveTest {
 protected:
  void SetUp() override {
    LiveTest::SetUp();
    types_.emplace(client());
    ASSERT_NO_FATAL_FAILURE(bess::live::ApplySteeringPipeline(*types_, client(), kSip, kDip));
  }

  std::unique_ptr<google::protobuf::Message> Key() {
    return types_->Make("bess.pb.ExactMatchRuleKey", bess::live::RuleKeyJson(kSip, kDip));
  }
  std::unique_ptr<google::protobuf::Message> Gate(int gate) {
    return types_->Make("bess.pb.ExactMatchRuleValue", std::format(R"({{"gate": {}}})", gate));
  }

  double AppliedTransactions(bess::sdk::Client &c) {
    auto samples = c.Metrics();
    EXPECT_TRUE(samples) << samples.error().message;
    if (samples) {
      for (const v2::MetricSample &s : *samples) {
        const auto outcome = s.labels().find("outcome");
        if (s.name() == "bess_transactions_total" && outcome != s.labels().end() &&
            outcome->second == "applied") {
          return s.value();
        }
      }
    }
    ADD_FAILURE() << "no bess_transactions_total{outcome=\"applied\"}";
    return -1;
  }

  std::optional<Types> types_;
};

TEST_F(ControlSdkTest, TransactionsSteerPackets) {
  auto rules = client().Resource(bess::live::kSteeringResource);
  ASSERT_TRUE(rules) << rules.error().message;
  EXPECT_EQ("bess.pb.ExactMatchRuleKey", rules->key_type);
  EXPECT_EQ("bess.pb.ExactMatchRuleValue", rules->value_type);
  EXPECT_TRUE(bess::live::SteeredTo(client(), 3)) << "no rule: the default gate";

  auto tx = client().NewTransaction();
  ASSERT_TRUE(tx.Upsert(*rules, *Key(), *Gate(1)));
  auto applied = tx.Commit();
  ASSERT_TRUE(applied) << applied.error().message;
  EXPECT_TRUE(bess::live::SteeredTo(client(), 1));

  // A stale expected generation changes nothing.
  auto stale = client().NewTransaction({.expected_generation = applied->generation - 1});
  ASSERT_TRUE(stale.Upsert(*rules, *Key(), *Gate(2)));
  auto refused = stale.Commit();
  ASSERT_FALSE(refused);
  EXPECT_EQ(ErrorKind::kConflict, refused.error().kind) << refused.error().message;
  EXPECT_TRUE(bess::live::SteeredTo(client(), 1));

  auto erase = client().NewTransaction();
  ASSERT_TRUE(erase.Erase(*rules, *Key()));
  ASSERT_TRUE(erase.Commit());
  EXPECT_TRUE(bess::live::SteeredTo(client(), 3));
}

TEST_F(ControlSdkTest, ALostAnswerIsRecoveredAndALostRequestResent) {
  // Applied, answer lost: the client asks, and reports the recorded outcome
  // -- one application, not two.
  {
    auto lossy = std::make_shared<LossyTransport>(daemon().address(),
                                                  std::deque<Fate>{Fate::kLoseAnswer});
    bess::sdk::Client c(lossy);
    auto rules = c.Resource(bess::live::kSteeringResource);
    ASSERT_TRUE(rules) << rules.error().message;
    const double before = AppliedTransactions(c);
    auto tx = c.NewTransaction();
    ASSERT_TRUE(tx.Upsert(*rules, *Key(), *Gate(1)));
    auto applied = tx.Commit();
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_TRUE(applied->replayed);
    EXPECT_EQ(1, lossy->sent());
    EXPECT_EQ(before + 1, AppliedTransactions(c));
    EXPECT_TRUE(bess::live::SteeredTo(client(), 1));
  }
  // Never arrived: "not known", so the same request is sent again.
  {
    auto lossy = std::make_shared<LossyTransport>(daemon().address(),
                                                  std::deque<Fate>{Fate::kLoseRequest});
    bess::sdk::Client c(lossy);
    auto rules = c.Resource(bess::live::kSteeringResource);
    ASSERT_TRUE(rules) << rules.error().message;
    auto tx = c.NewTransaction();
    ASSERT_TRUE(tx.Upsert(*rules, *Key(), *Gate(2)));
    auto applied = tx.Commit();
    ASSERT_TRUE(applied) << applied.error().message;
    EXPECT_FALSE(applied->replayed);
    EXPECT_EQ(1, lossy->sent());
    EXPECT_TRUE(bess::live::SteeredTo(client(), 2));
  }
}

TEST_F(ControlSdkTest, ARestartBetweenSendAndQuestionIsDaemonRestarted) {
  auto before = daemon().NewClient()->Pipeline();
  ASSERT_TRUE(before) << before.error().message;
  auto lossy = std::make_shared<LossyTransport>(
      daemon().address(), std::deque<Fate>{Fate::kLoseAnswer},
      [this] { daemon().RestartInPlace(); });
  bess::sdk::Client c(lossy, {.attempt_timeout = std::chrono::seconds(10), .attempts = 6});
  auto rules = c.Resource(bess::live::kSteeringResource);
  ASSERT_TRUE(rules) << rules.error().message;
  const auto epoch = c.DaemonEpoch();
  ASSERT_TRUE(epoch);

  auto tx = c.NewTransaction();
  ASSERT_TRUE(tx.Upsert(*rules, *Key(), *Gate(1)));
  auto lost = tx.Commit();
  ASSERT_FALSE(lost);
  EXPECT_EQ(ErrorKind::kDaemonRestarted, lost.error().kind) << lost.error().message;
  EXPECT_NE(epoch, c.DaemonEpoch());

  // The old handle belongs to the lost daemon: refused, never sent.
  auto again = c.NewTransaction();
  auto stale_handle = again.Upsert(*rules, *Key(), *Gate(1));
  ASSERT_FALSE(stale_handle);
  EXPECT_EQ(ErrorKind::kStaleResource, stale_handle.error().kind);
  EXPECT_EQ(1, lossy->sent());
  // The restarted daemon has no such module.
  auto gone = c.Resource(bess::live::kSteeringResource);
  ASSERT_FALSE(gone);
  EXPECT_EQ(ErrorKind::kInvalidRequest, gone.error().kind);

  // A pipeline built under the old epoch is refused by the new daemon
  // (D-106), even where its generation happens to match: nothing applied.
  auto fresh = daemon().NewClient();
  auto now = fresh->Pipeline();
  ASSERT_TRUE(now) << now.error().message;
  const bess::sdk::PipelineVersion stale{now->version.generation, before->version.daemon_epoch};
  auto refused = fresh->ApplyPipeline(before->pipeline, stale);
  ASSERT_FALSE(refused);
  EXPECT_EQ(ErrorKind::kDaemonRestarted, refused.error().kind) << refused.error().message;
  auto after = fresh->Pipeline();
  ASSERT_TRUE(after) << after.error().message;
  EXPECT_EQ(now->version, after->version);
  EXPECT_EQ(0, after->pipeline.modules_size());
}

// A port's queues and steering in GetCapabilities. (Its version, RPCs,
// classes and resources are conformance's "discovery" case.)
TEST_F(ControlSdkTest, CapabilitiesNamePorts) {
  auto snap = client().Pipeline();
  ASSERT_TRUE(snap) << snap.error().message;
  auto arg = types_->Make("bess.pb.UnixSocketPortArg",
                          std::format(R"({{"path": "@bess-live-caps-{}"}})", getpid()));
  auto p = bess::sdk::PipelineBuilder(snap->pipeline).Port("usp", "UnixSocketPort", arg.get()).Build();
  ASSERT_TRUE(p) << p.error().message;
  auto applied = client().ApplyPipeline(*p);
  ASSERT_TRUE(applied) << applied.error().message;

  auto caps = client().Capabilities();
  ASSERT_TRUE(caps) << caps.error().message;
  EXPECT_EQ(client().DaemonEpoch(), caps->daemon_epoch());
  const v2::PortInfo *port = nullptr;
  for (const v2::PortInfo &info : caps->ports()) {
    if (info.name() == "usp") {
      port = &info;
    }
  }
  ASSERT_NE(nullptr, port);
  EXPECT_EQ("UnixSocketPort", port->driver());
  EXPECT_EQ(1u, port->rx_queues());
  EXPECT_TRUE(port->symmetric_rss()) << "one queue: trivially symmetric";
}

// -- every resource round trips (D-129) ---------------------------------------

// A non-default sample of a resource, as protobuf JSON: a map's entries
// (key, value), or a singleton's value (key "").
struct ResourceSample {
  std::string table;
  std::vector<std::pair<std::string, std::string>> entries;
};

// A module of a class that declares resources, with a sample of each.
struct ModuleSample {
  std::string name;
  std::string mclass;
  std::string arg;    // its init argument as JSON; "" for none
  bool task = false;  // its task is placed on worker 0
  std::vector<ResourceSample> resources;
};

// One module of every class with resources (bessd's own classes; the plugin
// examples are not loaded), each resource set to a sample in the canonical
// form its read-back gives. PortInc and QueueInc read the ports "pa" and "pb".
std::vector<ModuleSample> EveryResourceSample() {
  // 60 bytes: Ethernet, IPv4 10.0.0.1 -> 10.0.0.2, UDP 1000 -> 2000.
  const std::string udp =
      "AgAAAAACAgAAAAABCABFAAAuAAAAAEARAAAKAAABCgAAAgPoB9AAGgAAAAAAAAAAAAAAAAAAAAAAAAAA";
  const std::string flowgen_arg =
      R"({"template": ")" + udp + R"(", "pps": 1000, "flow_rate": 10, "flow_duration": 10,
          "arrival": "uniform", "duration": "uniform"})";
  // FlowGen reads back every field: a sample names them all.
  const std::string flowgen_settings =
      R"({"template": ")" + udp + R"(", "pps": 2000, "flow_rate": 20, "flow_duration": 5,
          "arrival": "exponential", "duration": "uniform", "burst": 16})";
  const auto setting = [](std::string json) {
    return std::vector<std::pair<std::string, std::string>>{{"", std::move(json)}};
  };
  return {
      {"em", "ExactMatch",
       R"({"fields": [{"offset": 26, "num_bytes": 4}, {"offset": 34, "num_bytes": 2}]})", false,
       {{"rules",
         {{R"({"fields": [{"value_bin": "AQIDBA=="}, {"value_bin": "AFA="}]})", R"({"gate": 2})"}}},
        {"settings", setting(R"({"default_gate": 3})")}}},
      {"wm", "WildcardMatch",
       R"({"fields": [{"offset": 26, "num_bytes": 4}, {"offset": 30, "num_bytes": 4}]})", false,
       {{"rules",
         {{R"({"values": [{"value_bin": "GjN3RA=="}, {"value_bin": "AAAAAA=="}],
               "masks": [{"value_bin": "/////w=="}, {"value_bin": "AAAAAA=="}]})",
           R"({"priority": 10, "gate": 1})"}}},
        {"settings", setting(R"({"default_gate": 2})")}}},
      {"fw", "Firewall",
       R"({"rules": [{"src_ip": "10.0.0.0/8", "proto": 17, "dst_port_low": 53, "dst_port_high": 53}]})", false,
       {{"rules",
         {{R"({"id": 1})",
           R"({"rule": {"src_ip": "10.0.0.0/8", "proto": 17, "dst_port_low": 53, "dst_port_high": 53, "action_id": 7}})"},
          {R"({"id": 4})", R"({"priority": "-5", "rule": {"src_ip": "10.0.0.0/8", "drop": true}})"}}}}},
      {"l2", "L2Forward", "", false,
       {{"macs",
         {{R"({"addr": "02:00:00:00:00:02"})", R"({"gate": 3})"},
          {R"({"addr": "aa:bb:cc:dd:ee:ff"})", R"({"gate": 8192})"},
          {R"({"addr": "00:00:00:00:00:01"})", "{}"}}},
        {"settings", setting(R"({"default_gate": 4})")}}},
      {"mt", "Meter", R"({"capacity": 64})", false,
       {{"meters",
         {{R"({"id": 1})", R"({"tr_tcm": {"committed_rate": 1, "committed_burst": 100,
                                          "peak_rate": 1, "peak_burst": 100}})"}}}}},
      // A next hop's canonical form omits defaults: no neighbor when resolved.
      // Destination mode, since only it holds routes (D-183); an interface,
      // since a neighbor binding names one (its id is egress_gate + 1).
      {"rt", "Router",
       R"({"mode": "ROUTE_MODE_DESTINATION", "max_ipv4_routes": 64, "max_ipv4_tbl8s": 16, "max_next_hops": 64, "max_groups": 8,
           "interfaces": [{"egress_gate": 7, "mac_addr": "02:00:00:00:00:02", "addresses": ["192.0.2.1/24"]}]})", false,
       {{"next_hops",
         {{R"({"id": 1})",
           R"({"egress_gate": 7, "dst_mac": "AgAAAAAB", "src_mac": "AgAAAAAC"})"}}},
        {"groups", {{R"({"id": 1})", R"({"member_ids": [1]})"}}},
        {"neighbors", {{R"({"id": 2})", R"({"interface_id": 8, "ipv4": "192.0.2.2"})"}}},
        {"routes", {{R"({"ipv4": "10.0.0.0", "prefix_length": 8})", R"({"next_hop_id": 1})"}}}}},
      {"src", "Source", "", true, {{"settings", setting(R"({"pkt_size": 128, "burst": 7})")}}},
      {"pi", "PortInc", R"({"port": "pa"})", true, {{"settings", setting(R"({"burst": 9})")}}},
      {"qi", "QueueInc", R"({"port": "pb", "qid": 0})", true,
       {{"settings", setting(R"({"burst": 11})")}}},
      {"drr", "DRR", "", true,
       {{"settings", setting(R"({"quantum": 3000, "max_flow_queue_size": 64})")}}},
      {"lb", "HashLB", R"({"gates": [0, 1]})", false,
       {{"settings", setting(R"({"mode": "l2", "gates": [0, 7, 9]})")}}},
      {"fg", "FlowGen", flowgen_arg, true, {{"settings", setting(flowgen_settings)}}},
      {"q", "Queue", "", true,
       {{"settings",
         setting(R"({"size": 256, "burst": 16, "prefetch": true, "backpressure": true})")}}},
      {"rr", "RoundRobin", R"({"gates": [0]})", false,
       {{"settings", setting(R"({"gates": [1, 4, 2], "mode": "packet"})")}}},
      {"rs", "RandomSplit", R"({"drop_rate": 0, "gates": [0]})", false,
       {{"settings", setting(R"({"drop_rate": 0.125, "gates": [2, 7, 4]})")}}},
      {"rep", "Replicate", R"({"gates": [0]})", false,
       {{"settings", setting(R"({"gates": [0, 5, 31]})")}}},
      {"vp", "VLANPush", "", false, {{"settings", setting(R"({"tci": 65535})")}}},
      {"mp", "MPLSPop", "", false,
       {{"settings", setting(R"({"remove_eth_header": true, "next_eth_type": 34525})")}}},
      {"dump", "Dump", "", false, {{"settings", setting(R"({"interval": 0.25})")}}},
      {"ws", "WorkerSplit", "", false,
       {{"settings", setting(R"({"worker_gates": {"0": 2, "3": 1}})")}}},
      {"acl", "ACL", "", false,
       {{"rules",
         setting(R"({"rules": [{"src_ip": "11.0.0.0/8"}, {"src_ip": "10.0.0.0/8", "drop": true}]})")}}},
      {"bpf", "BPF", "", false,
       {{"filters", setting(R"({"filters": [{"priority": 1, "filter": "ip host 22.22.22.22", "gate": 2},
                                            {"priority": 2, "filter": "tcp src port 92", "gate": 1}]})")}}},
      {"ru", "RandomUpdate", "", false,
       {{"fields", setting(R"({"fields": [{"offset": 0, "size": 1, "min": 66, "max": 66},
                                          {"offset": 20, "size": 2, "min": 4660, "max": 4660}]})")}}},
      {"upd", "Update", "", false,
       {{"fields", setting(R"({"fields": [{"offset": 0, "size": 2, "value": 43981},
                                          {"offset": 10, "size": 1, "value": 127}]})")}}},
      {"rw", "Rewrite", "", false,
       {{"templates", setting(R"({"templates": ["YWFhYQ==", "YmJiYg=="]})")}}},
      {"uf", "UrlFilter", "", false,
       {{"blacklist", setting(R"({"blacklist": [{"host": "www.zeta.com", "path": "/"},
                                                {"host": "www.alpha.com", "path": "/"}]})")}}},
      {"br", "Bridge", "", false,
       {{"statics", {{R"({"mac_addr": "02:aa:bb:cc:dd:ee"})", R"({"gate": 2})"}}}}},
      {"vif", "Vif", "", false,
       {{"interfaces",
         {{R"({"id": 3})", R"({"vlan": 5, "mac_addr": "02:00:00:00:00:10", "ip_addr": "10.0.0.10",
                             "route_domain": 7, "gate": 1})"}}}}},
      {"arp", "ArpResponder", "", false,
       {{"entries", {{R"({"ip": "1.2.3.4"})", R"({"mac_addr": "a0:22:33:44:55:66"})"}}}}},
      {"ps", "PacketStore", R"({"max_packets": 64, "max_packets_per_flow": 4})", true,
       {{"flows", {{R"({"id": 1})", R"({"timeout_ns": 1000000, "max_packets_per_flow": 4})"}}}}},
  };
}

const v2::ResourceContents *FindResource(const v2::Pipeline &p, std::string_view name) {
  for (const v2::ResourceContents &r : p.resources()) {
    if (r.name() == name) {
      return &r;
    }
  }
  return nullptr;
}

// One ApplyPipeline creates a module of every class with resources, each
// resource set to a sample and listed. Its read-back applies back as a
// no-op, and so does the desired pipeline itself (each sample is the
// canonical form); a map listed with no entries is cleared, and a resource
// left out is left as it is.
TEST_F(ControlSdkTest, EveryResourceRoundTrips) {
  daemon().Reset();  // not the steering pipeline: its module names are reused
  const auto samples = EveryResourceSample();
  std::map<std::string, const v2::ModuleClassSchema *> classes;
  for (const v2::ModuleClassSchema &c : types_->schema().module_classes()) {
    classes[c.name()] = &c;
  }

  const std::string tag = std::format("bess-live-every-resource-{}", getpid());
  auto port_a = types_->Make("bess.pb.UnixSocketPortArg", std::format(R"({{"path": "@{}-a"}})", tag));
  auto port_b = types_->Make("bess.pb.UnixSocketPortArg", std::format(R"({{"path": "@{}-b"}})", tag));
  bess::sdk::PipelineBuilder b;
  b.Worker(0, 0, "")
      .Port("pa", "UnixSocketPort", port_a.get())
      .Port("pb", "UnixSocketPort", port_b.get());
  size_t entries = 0;
  for (const ModuleSample &m : samples) {
    const auto c = classes.find(m.mclass);
    ASSERT_NE(classes.end(), c) << "no module class " << m.mclass;
    std::unique_ptr<google::protobuf::Message> arg;
    if (!m.arg.empty()) {
      arg = types_->Make(c->second->init_arg_type(), m.arg);
      ASSERT_NE(nullptr, arg);
    }
    b.Module(m.name, m.mclass, arg.get());
    if (m.task) {
      v2::TaskPlacement task;
      task.set_module(m.name);
      task.set_wid(0);
      b.Task(task);
    }
    EXPECT_EQ(static_cast<size_t>(c->second->resources_size()), m.resources.size())
        << m.mclass << ": a sample for every table";
    for (const ResourceSample &r : m.resources) {
      const v2::ResourceSchema *table = nullptr;
      for (const v2::ResourceSchema &t : c->second->resources()) {
        if (t.table() == r.table) {
          table = &t;
        }
      }
      ASSERT_NE(nullptr, table) << m.mclass << " has no table " << r.table;
      const std::string name = m.name + "/" + r.table;
      for (const auto &[key_json, value_json] : r.entries) {
        auto value = types_->Make(table->value_type(), value_json);
        ASSERT_NE(nullptr, value);
        if (table->kind() == v2::TransactionResource::KIND_SINGLETON) {
          b.Setting(name, *value);
        } else {
          auto key = types_->Make(table->key_type(), key_json);
          ASSERT_NE(nullptr, key);
          b.Entry(name, *key, *value);
        }
        entries++;
      }
    }
  }
  ASSERT_FALSE(HasFailure()) << "a sample does not parse";
  // Every class GetSchema lists with resources has a sample.
  std::set<std::string> sampled;
  for (const ModuleSample &m : samples) {
    sampled.insert(m.mclass);
  }
  for (const v2::ModuleClassSchema &c : types_->schema().module_classes()) {
    if (c.resources_size() > 0) {
      EXPECT_TRUE(sampled.contains(c.name())) << c.name() << " declares resources: add a sample";
    }
  }

  auto desired = b.Build();
  ASSERT_TRUE(desired) << desired.error().message;
  auto created = client().ApplyPipeline(*desired);
  ASSERT_TRUE(created) << created.error().message;
  // No sample is a resource's initial value: each one is an operation.
  EXPECT_EQ(static_cast<uint64_t>(entries), created->resource_ops());

  // What changes on an apply of `p` (for a failure's message).
  const auto drift = [&](const v2::Pipeline &p) {
    auto diff = client().DiffPipeline(p);
    return diff ? diff->diff.ShortDebugString() : "diff: " + diff.error().message;
  };

  // (a) The read-back applies back as a no-op at the version it was read at.
  auto read = client().Pipeline({.include_resources = true});
  ASSERT_TRUE(read) << read.error().message;
  ASSERT_TRUE(read->version.resource_generation);
  for (const v2::ResourceContents &listed : desired->resources()) {
    const v2::ResourceContents *got = FindResource(read->pipeline, listed.name());
    ASSERT_NE(nullptr, got) << listed.name() << " is not read back";
    EXPECT_EQ(listed.entries_size(), got->entries_size()) << got->ShortDebugString();
  }
  const std::string read_drift = drift(read->pipeline);
  auto same = client().ApplyPipeline(read->pipeline, read->version);
  ASSERT_TRUE(same) << same.error().message;
  EXPECT_EQ(0u, same->resource_ops()) << read_drift;
  EXPECT_EQ(read->version.generation, same->generation()) << read_drift;
  EXPECT_EQ(*read->version.resource_generation, same->resource_generation()) << read_drift;

  // (b) What the client sent is the canonical form: applying it again
  // changes no resource.
  const std::string desired_drift = drift(*desired);
  auto again = client().ApplyPipeline(*desired);
  ASSERT_TRUE(again) << again.error().message;
  EXPECT_EQ(0u, again->resource_ops()) << desired_drift;
  EXPECT_EQ(same->resource_generation(), again->resource_generation()) << desired_drift;

  // (c) rt/routes listed with no entries is cleared; l2/macs left out of
  // the pipeline keeps what it held.
  v2::Pipeline partial = *desired;
  auto *listed = partial.mutable_resources();
  for (int i = listed->size() - 1; i >= 0; i--) {
    if (listed->Get(i).name() == "l2/macs") {
      listed->DeleteSubrange(i, 1);
    } else if (listed->Get(i).name() == "rt/routes") {
      listed->Mutable(i)->clear_entries();
    }
  }
  auto cleared = client().ApplyPipeline(partial);
  ASSERT_TRUE(cleared) << cleared.error().message;
  EXPECT_EQ(1u, cleared->resource_ops()) << "the one route erased, nothing else";
  auto after = client().Pipeline({.include_resources = true, .resources = {"rt/routes", "l2/macs"}});
  ASSERT_TRUE(after) << after.error().message;
  const v2::ResourceContents *routes = FindResource(after->pipeline, "rt/routes");
  EXPECT_TRUE(routes == nullptr || routes->entries_size() == 0) << routes->ShortDebugString();
  const v2::ResourceContents *macs = FindResource(after->pipeline, "l2/macs");
  const v2::ResourceContents *macs_before = FindResource(read->pipeline, "l2/macs");
  ASSERT_NE(nullptr, macs);
  ASSERT_NE(nullptr, macs_before);
  EXPECT_EQ(3, macs->entries_size());
  EXPECT_TRUE(google::protobuf::util::MessageDifferencer::Equals(*macs_before, *macs))
      << "before: " << macs_before->ShortDebugString() << "\nafter: " << macs->ShortDebugString();
}

}  // namespace
