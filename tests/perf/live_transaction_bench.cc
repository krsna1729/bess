// SPDX-License-Identifier: BSD-3-Clause

// Packet rate of a live bessd while dataplane transactions change its rules
// (the live gate MODERNIZATION.md section 31.0 asked for; D-021 amendment 5,
// D-025, D-139). Per transaction rate it reports:
//
//   - Mpps through the module (its output gates' counters);
//   - the fraction of packets that hit a rule (half the traffic targets live
//     sessions, so ~0.5 confirms the rules steer packets);
//   - transactions/s achieved, client-side commit latency p50/p99 (busy
//     retries included), and the daemon's busy answers in the window.
//
// A controller on the C++ SDK: the pipeline is desired state, the rules are
// SDK transactions on `em/rules` (request ids, busy retries, typed outcomes),
// the gate counters and busy answers are metrics.
//
// Pipeline: Source -> RandomUpdate (IPv4 source address over 2 x sessions
// values) -> ExactMatch (source address) -> Sink per gate. A session is one
// exact rule; each transaction adds the next session's rule and removes the
// oldest: two operations by default, and --upserts/--erases set the count, so
// one sweep prices an operation rather than a transaction. With more upserts
// than erases the live set grows for the length of the run.
//
// Start bessd first (isolated as you benchmark anything else), e.g.
//   omarchy-benchmark --cpu 2,4 --isolate -- sudo env ... bessd -k -f
// then run this from another CPU:
//   live_transaction_bench --worker-core 2 --rates 0,1000,10000,max

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include "bess/sdk/client.h"
#include "placement_check.h"
#include "schema_types.h"

namespace {

namespace v2 = bess::pb::v2;
using Clock = std::chrono::steady_clock;
using bess::perf::Base64;
using bess::perf::Be32;
using bess::perf::Types;

constexpr uint32_t kBase = 0x0a000000;  // 10.0.0.0
constexpr int kGates = 4;               // rules steer to gates 1..kGates; misses go to gate 0

// This thread's CPU time, and this process's (every client thread, gRPC's
// included), both in nanoseconds: the client side of a commit has to be
// visible, or a client-side ceiling looks like a daemon one.
uint64_t ThreadCpuNs() {
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

uint64_t ProcessCpuNs() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  return uint64_t(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000000000ull +
         uint64_t(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1000ull;
}

struct Options {
  std::string grpc_url = "localhost:10514";
  std::optional<int> worker_core = 0;  // nullopt: let the daemon place it (D-123 order)
  bool allow_dirty_placement = false;  // print no placement warning
  int clients = 1;                     // committers, each with its own connection
  int daemon_pid = 0;                  // for the daemon-side CPU split (0: skip)
  bool counter_window = false;  // bracket existing persistent Metrics RPCs
  // Several bench processes against one daemon need disjoint rule space: this
  // one owns blocks [block_offset, block_offset + clients), out of
  // `blocks_total` the built pipeline's traffic generator covers. `attach`
  // joins a pipeline another process built (no pause, no apply, no reset):
  // with every process building, each would tear down the others'.
  int block_offset = 0;
  int blocks_total = 0;  // 0: this process's own clients
  bool attach = false;
  int sessions = 100000;
  int pkt_size = 60;
  double duration = 5.0;
  std::vector<int> rates = {0, 1000, 10000, -1};  // -1: as fast as possible
  int rounds = 1;
  int upserts = 1;  // rules added per transaction
  int erases = 1;   // rules dropped per transaction
  bool keep = false;  // leave the pipeline applied for the next client
  std::vector<int> stages;  // nonempty: idle seeded stage floor, not packet churn
  int stage_iterations = 5000;
  std::string stage_locality = "hot";
  std::string stage_mode = "message";

  // How many blocks the traffic generator must cover: every process's.
  int blocks() const { return blocks_total > 0 ? blocks_total : clients; }
};

// The rule key of `session` in `block`'s slice of the address space: each
// committer owns 2 x sessions addresses of its own, so two of them never touch
// one rule and the traffic generator covers every block. Half of a block's
// addresses carry a rule at any moment, which is the ~0.5 hit fraction the
// report uses as a check that rules steer packets.
std::unique_ptr<google::protobuf::Message> Key(Types &types, const Options &o, int block,
                                               int session) {
  const int owner = o.block_offset + block;
  const uint32_t addr = kBase + uint32_t(owner * 2 * o.sessions + session % (2 * o.sessions));
  return types.Make("ExactMatchRuleKey",
                    R"({"fields": [{"value_bin": ")" + Base64(Be32(addr)) + R"("}]})");
}

std::unique_ptr<google::protobuf::Message> Value(Types &types, int session) {
  return types.Make("ExactMatchRuleValue",
                    R"({"gate": )" + std::to_string(1 + session % kGates) + "}");
}

// One transaction: the sessions `upserts` names get a rule, the ones `erases`
// names lose theirs, and the seconds the answering call took. A busy answer is
// retried with the transaction built again under a new request id: the daemon
// applied nothing. One upsert and one erase per transaction keeps the live set
// at --sessions; --upserts/--erases say otherwise, and the file header says what
// that does to the address window.
double Commit(bess::sdk::Client &client, Types &types, const Options &o,
              const bess::sdk::Resource &rules, int block, const std::vector<int> &upserts,
              const std::vector<int> &erases) {
  while (true) {
    bess::sdk::Transaction tx = client.NewTransaction();
    for (const int s : upserts) {
      const auto ok = tx.Upsert(rules, *Key(types, o, block, s), *Value(types, s));
      if (!ok) {
        throw std::runtime_error("upsert: " + ok.error().message);
      }
    }
    for (const int s : erases) {
      const auto ok = tx.Erase(rules, *Key(types, o, block, s));
      if (!ok) {
        throw std::runtime_error("erase: " + ok.error().message);
      }
    }
    const auto t0 = Clock::now();
    const auto applied = tx.Commit();
    const double seconds = std::chrono::duration<double>(Clock::now() - t0).count();
    if (!applied) {
      if (applied.error().kind == bess::sdk::ErrorKind::kBusy) {
        continue;
      }
      throw std::runtime_error("commit: " + applied.error().message);
    }
    return seconds;
  }
}

// `count` consecutive session numbers: the rules one transaction touches.
std::vector<int> Range(int first, int count) {
  std::vector<int> v;
  v.reserve(count);
  for (int i = 0; i < count; i++) {
    v.push_back(first + i);
  }
  return v;
}

// One client: its own connection, its own slice of the rule space, its own
// pacing. `--clients N` runs N of these at once -- the only concurrency the
// SDK offers today, since Commit is one synchronous unary RPC (the event
// stream is the only streaming call). What N of them achieve together, and
// what saturates first, is the measurement.
class Committer {
 public:
  Committer(bess::sdk::Client &client, Types &types, const Options &o, int block)
      : client_(client), types_(types), o_(o), block_(block), next_(o.sessions), oldest_(0) {}

  void set_rules(const bess::sdk::Resource &rules) { rules_ = rules; }

  // Commits at `rate` transactions/s (0: none, negative: as fast as the daemon
  // answers) until `deadline`. The thread's own CPU time is charged per
  // commit, so a client-side ceiling is visible as such.
  void Run(int rate, const std::chrono::steady_clock::time_point deadline) {
    auto due = Clock::now();
    while (Clock::now() < deadline) {
      if (rate == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (rate > 0) {
        due += std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(1.0 / rate));
        std::this_thread::sleep_until(due);
      }
      const uint64_t cpu0 = ThreadCpuNs();
      latency_.push_back(Commit(client_, types_, o_, rules_, block_,
                                Range(next_, o_.upserts), Range(oldest_, o_.erases)));
      cpu_ns_ += ThreadCpuNs() - cpu0;
      next_ += o_.upserts;
      oldest_ += o_.erases;
      done_++;
    }
  }

  std::vector<double> TakeLatency() { return std::exchange(latency_, {}); }
  int TakeDone() { return std::exchange(done_, 0); }
  uint64_t TakeCpuNs() { return std::exchange(cpu_ns_, 0); }

 private:
  bess::sdk::Client &client_;
  Types &types_;
  const Options &o_;
  const int block_;
  bess::sdk::Resource rules_;
  int next_;    // the next session to add a rule for
  int oldest_;  // the oldest live session, the next to drop
  int done_ = 0;
  uint64_t cpu_ns_ = 0;
  std::vector<double> latency_;
};

class Bench {
 public:
  Bench(bess::sdk::Client &client, const Options &o) : client_(client), types_(client), o_(o) {}

  void Build() {
    if (o_.attach) {
      // A pipeline this process did not build: borrow its rule resource only.
      auto rules = client_.Resource("em/rules");
      if (!rules) {
        throw std::runtime_error("em/rules (--attach): " + rules.error().message);
      }
      rules_ = *rules;
    } else {
      Check(client_.PauseWorkers(), "pause");
      Check(client_.ApplyPipeline(v2::Pipeline()), "reset");
      const auto src = types_.Make(types_.InitType("Source"),
                                   R"({"pkt_size": )" + std::to_string(o_.pkt_size) + "}");
      const auto rnd = types_.Make(
          types_.InitType("RandomUpdate"),
          R"({"fields": [{"offset": 26, "size": 4, "min": )" + std::to_string(kBase) +
              R"(, "max": )" +
              std::to_string(kBase + 2 * o_.sessions * o_.blocks() - 1) + "}]}");
      const auto em = types_.Make(types_.InitType("ExactMatch"),
                                  R"({"fields": [{"offset": 26, "num_bytes": 4}]})");
      const auto settings = types_.Make("ExactMatchSettings", R"({"default_gate": 0})");
      bess::sdk::PipelineBuilder p;
      p.Worker(0, o_.worker_core, "")
          .Module("src", "Source", src.get())
          .Module("rnd", "RandomUpdate", rnd.get())
          .Module("em", "ExactMatch", em.get())
          .Chain({"src", "rnd", "em"})
          .Setting("em/settings", *settings);
      for (int gate = 0; gate <= kGates; gate++) {
        const std::string sink = "sink" + std::to_string(gate);
        p.Module(sink, "Sink", nullptr).Connect("em", gate, sink, 0);
      }
      auto pipeline = p.Build();
      if (!pipeline) {
        throw std::runtime_error("pipeline: " + pipeline.error().message);
      }
      Check(client_.ApplyPipeline(*pipeline), "apply");
      auto rules = client_.Resource("em/rules");
      if (!rules) {
        throw std::runtime_error("em/rules: " + rules.error().message);
      }
      rules_ = *rules;
    }
    // Sessions 0 .. sessions-1 of every block this process owns live from the
    // start, loaded in transactions of 1000 rules each: a committer then only
    // moves its window. A block whose preload failed would make its first
    // erases fail, so this is also the sanity check of the whole setup.
    for (int block = 0; block < o_.clients; block++) {
      for (int first = 0; first < o_.sessions; first += 1000) {
        std::vector<int> upserts;
        for (int s = first; s < std::min(first + 1000, o_.sessions); s++) {
          upserts.push_back(s);
        }
        Commit(client_, types_, o_, rules_, block, upserts, {});
      }
    }
    if (!o_.attach) {
      Check(client_.ResumeWorkers(), "resume");
      std::this_thread::sleep_for(std::chrono::seconds(1));  // warm up
    }
  }

  void Reset() {
    if (o_.attach) {
      return;  // the pipeline belongs to the process that built it
    }
    Check(client_.PauseWorkers(), "pause");
    Check(client_.ApplyPipeline(v2::Pipeline()), "reset");
  }

  const bess::sdk::Resource &rules() const { return rules_; }

  std::vector<v2::MetricSample> Metrics() {
    auto samples = client_.Metrics();
    if (!samples) {
      throw std::runtime_error("metrics: " + samples.error().message);
    }
    return std::move(*samples);
  }

  // Packets out of each of em's gates, by gate label.
  std::map<std::string, double> GatePackets() {
    std::map<std::string, double> out;
    for (const v2::MetricSample &s : Metrics()) {
      const auto &l = s.labels();
      if (s.name() == "bess_gate_packets_total" && l.contains("module") &&
          l.at("module") == "em" && l.contains("direction") && l.at("direction") == "out" &&
          l.contains("gate")) {
        out[l.at("gate")] = s.value();
      }
    }
    return out;
  }

  // The daemon's busy answers to transactions so far.
  double Busy() {
    for (const v2::MetricSample &s : Metrics()) {
      const auto &l = s.labels();
      if (s.name() == "bess_transactions_total" && l.contains("outcome") &&
          l.at("outcome") == "busy") {
        return s.value();
      }
    }
    return 0;
  }

  bess::sdk::Client &client_;
  Types types_;
  const Options &o_;
  bess::sdk::Resource rules_;

 private:
  template <typename T>
  static void Check(const bess::sdk::Result<T> &r, const std::string &what) {
    if (!r) {
      throw std::runtime_error(what + ": " + r.error().message);
    }
  }
};

// The daemon's CPU time for the window. The total comes from /proc/<pid>/stat
// (utime+stime: cumulative, so a thread that has exited still counts). The
// control-path figure subtracts the long-lived packet workers -- they busy-poll
// a core whether or not a transaction arrives -- using the per-thread schedstat
// deltas, which are only used for that attribution.
//
// No switch counter here: /proc/<pid>/status's voluntary+involuntary switches
// moved by 0 over a 5 s, 70K-transaction window (probed 2026-10-17), because the
// gRPC sync server's pollers spin rather than sleep, and per-thread switch
// counters (the earlier version) are meaningless across thread churn.
struct DaemonCpu {
  uint64_t process_ns = 0;
  struct Thread {
    std::string name;
    uint64_t ns = 0;
  };
  std::map<int, Thread> threads;  // alive at the sample: for attribution only

  static DaemonCpu Read(int pid) {
    DaemonCpu out;
    const std::string base = "/proc/" + std::to_string(pid);
    // The comm field of stat may hold spaces and parentheses: parse after its
    // closing one, where utime and stime are fields 14 and 15 (1-based).
    std::ifstream stat(base + "/stat");
    std::string text;
    std::getline(stat, text);
    const long ticks = sysconf(_SC_CLK_TCK);
    const uint64_t ns_per_tick = 1000000000ull / (ticks > 0 ? uint64_t(ticks) : 100);
    const size_t close = text.rfind(')');
    if (close != std::string::npos) {
      std::istringstream rest(text.substr(close + 1));
      std::string field;
      uint64_t utime = 0, stime = 0;
      for (int index = 3; rest >> field; index++) {
        if (index == 14) {
          utime = std::strtoull(field.c_str(), nullptr, 10);
        } else if (index == 15) {
          stime = std::strtoull(field.c_str(), nullptr, 10);
          break;
        }
      }
      out.process_ns = (utime + stime) * ns_per_tick;
    }
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(base + "/task", ec)) {
      std::ifstream schedstat(entry.path() / "schedstat");
      uint64_t runtime = 0, waits = 0, slices = 0;
      if (!(schedstat >> runtime >> waits >> slices)) {
        continue;
      }
      std::ifstream comm(entry.path() / "comm");
      std::string name;
      std::getline(comm, name);
      const std::string leaf = entry.path().filename().string();
      int tid = 0;
      if (std::from_chars(leaf.data(), leaf.data() + leaf.size(), tid).ec != std::errc()) {
        continue;  // not a task directory
      }
      out.threads[tid] = Thread{name, runtime};
    }
    if (out.threads.empty() && out.process_ns == 0) {
      throw std::runtime_error("no threads of daemon pid " + std::to_string(pid)
                               + " (wrong --daemon-pid, or not a bessd?)");
    }
    return out;
  }

  // The threads that used time between `before` and `after`, most first.
  static std::vector<Thread> Deltas(const DaemonCpu &before, const DaemonCpu &after) {
    std::vector<Thread> out;
    for (const auto &[tid, thread] : after.threads) {
      const auto it = before.threads.find(tid);
      const uint64_t was = it == before.threads.end() ? 0 : it->second.ns;
      if (thread.ns > was) {
        out.push_back(Thread{thread.name, thread.ns - was});
      }
    }
    std::sort(out.begin(), out.end(),
              [](const Thread &a, const Thread &b) { return a.ns > b.ns; });
    return out;
  }
};

void ValidateStageOptions(const Options &o) {
  if (o.clients != 1 || o.stage_iterations < 1 || o.stage_iterations > 65536 ||
      (o.stage_locality != "hot" && o.stage_locality != "spread")) {
    throw std::runtime_error("stage floor needs one client, 1..65536 iterations and hot/spread locality");
  }
  if (o.stage_mode != "message" && o.stage_mode != "encoded") {
    throw std::runtime_error("stage mode must be message or encoded");
  }
  for (int n : o.stages) {
    if (n != 1 && n != 2 && n != 3 && n != 4 && n != 8) {
      throw std::runtime_error("stage counts must be 1,2,3,4,8");
    }
  }
}

std::string StageJson(const google::protobuf::Struct &fields) {
  std::string json;
  const auto status = google::protobuf::util::MessageToJsonString(fields, &json);
  if (!status.ok()) {
    throw std::runtime_error("stage JSON: " + std::string(status.message()));
  }
  return json;
}

// The same idle eight-resource fixture as the in-process and Go stage ladders,
// through the public C++ SDK and a real daemon. Input messages or wire bytes
// are prepared; NewTransaction, packing (message mode), Commit and receipts are timed.
// All eight resident sets remain present at every modified-prefix count.
bool StageFloor(bess::sdk::Client &client, const Options &o) {
  google::protobuf::Struct failure;
  auto &evidence = *failure.mutable_fields();
  std::ostringstream measurements;
  const char *phase = "setup";
  int stages = 0, ordinal = 0, accepted = 0;
  bool first = true;
  try {
    ValidateStageOptions(o);
    Types types(client);
    auto caps = client.Capabilities();
    if (!caps) {
      throw std::runtime_error("stage capabilities: " + caps.error().message);
    }
    const uint64_t epoch = caps->daemon_epoch();
    constexpr int resident = 1024;
    std::vector<bess::sdk::Resource> resources;
    bess::sdk::PipelineQuery query;
    query.include_resources = true;
    for (int i = 0; i < 8; ++i) {
      auto resource = client.Resource("stage" + std::to_string(i) + "/rules");
      if (!resource || resource->kind != v2::TransactionResource::KIND_MAP ||
          resource->daemon_epoch != epoch ||
          resource->key_type != "bess.pb.ExactMatchRuleKey" ||
          resource->value_type != "bess.pb.ExactMatchRuleValue") {
        throw std::runtime_error("stage floor needs the caller's idle eight-stage graph");
      }
      query.resources.push_back(resource->name);
      resources.push_back(std::move(*resource));
    }
    std::vector<std::unique_ptr<google::protobuf::Message>> keys;
    std::map<std::string, int> expected_keys;
    std::vector<const std::string *> key_wire;
    keys.reserve(resident);
    key_wire.reserve(resident);
    for (int slot = 0; slot < resident; ++slot) {
      keys.push_back(types.Make("ExactMatchRuleKey",
          R"({"fields":[{"value_bin":")" + Base64(Be32(0x0b000001u + slot)) + R"("}]})"));
      const auto entry = expected_keys.emplace(keys.back()->SerializeAsString(), slot).first;
      key_wire.push_back(&entry->first);
    }
    auto gate1 = types.Make("ExactMatchRuleValue", R"({"gate":1})");
    auto gate2 = types.Make("ExactMatchRuleValue", R"({"gate":2})");
    const std::string value1 = gate1->SerializeAsString(), value2 = gate2->SerializeAsString();
    auto snapshot = client.Pipeline(query);
    if (!snapshot) {
      throw std::runtime_error("stage snapshot: " + snapshot.error().message);
    }
    if (snapshot->version.daemon_epoch != epoch || snapshot->pipeline.resources_size() != 8) {
      throw std::runtime_error("stage snapshot epoch or resource count mismatch");
    }
    if (!snapshot->workers.empty() || !snapshot->pipeline.workers().empty() ||
        !snapshot->pipeline.tasks().empty()) {
      throw std::runtime_error("stage fixture requires a worker-free, task-free idle graph");
    }
    std::vector<bool> seen_resources(8);
    for (const auto &contents : snapshot->pipeline.resources()) {
      const auto found = std::find(query.resources.begin(), query.resources.end(), contents.name());
      if (found == query.resources.end() || seen_resources[found - query.resources.begin()]) {
        throw std::runtime_error("stage snapshot has unexpected or duplicate resource " + contents.name());
      }
      seen_resources[found - query.resources.begin()] = true;
      if (contents.entries_size() != 0 && contents.entries_size() != resident) {
        throw std::runtime_error(contents.name() + " must be empty or have exactly 1024 fixture entries");
      }
      std::vector<bool> seen_keys(resident);
      for (const auto &entry : contents.entries()) {
        const auto key = expected_keys.find(entry.key().value());
        if (entry.key().type_url() != "type.googleapis.com/bess.pb.ExactMatchRuleKey" ||
            entry.value().type_url() != "type.googleapis.com/bess.pb.ExactMatchRuleValue" ||
            key == expected_keys.end() || seen_keys[key->second] ||
            (entry.value().value() != value1 && entry.value().value() != value2)) {
          throw std::runtime_error(contents.name() + " contains unexpected keys or incompatible fixture contents");
        }
        seen_keys[key->second] = true;
      }
    }
    const auto record_evidence = [&](const v2::TransactionRecord &record) {
      evidence["record_request_id"].set_string_value(record.request_id());
      evidence["outcome"].set_string_value(v2::TransactionRecord::Outcome_Name(record.outcome()));
      evidence["record_ops"].set_number_value(record.ops_size());
      evidence["generation"].set_string_value(std::to_string(record.generation()));
      int applied_ops = 0;
      for (const auto &op : record.ops()) {
        applied_ops += op.status() == v2::TransactionOpResult::STATUS_APPLIED;
      }
      evidence["applied_ops"].set_number_value(applied_ops);
    };
    const auto sdk_error = [&](const bess::sdk::Error &error) {
      evidence["error_kind"].set_number_value(static_cast<int>(error.kind));
      evidence["grpc_code"].set_number_value(static_cast<int>(error.code));
      evidence["attempts"].set_number_value(error.attempts);
      evidence["after_unknown_attempt"].set_bool_value(error.after_unknown_attempt);
      if (error.record) {
        record_evidence(*error.record);
      }
    };
    const auto commit = [&](bess::sdk::Transaction &tx, int ops) {
      auto result = tx.Commit();
      if (!result) {
        evidence["request_id"].set_string_value(tx.request_id());
        evidence["expected_ops"].set_number_value(ops);
        sdk_error(result.error());
        throw std::runtime_error("stage commit: " + result.error().message);
      }
      bool valid = !result->replayed && result->daemon_epoch == epoch &&
          result->request_id == tx.request_id() &&
          result->record.request_id() == tx.request_id() &&
          result->record.outcome() == v2::TransactionRecord::OUTCOME_APPLIED &&
          result->record.ops_size() == ops;
      for (const auto &op : result->record.ops()) {
        valid = valid && op.status() == v2::TransactionOpResult::STATUS_APPLIED;
      }
      if (!valid) {
        evidence["request_id"].set_string_value(tx.request_id());
        evidence["applied_request_id"].set_string_value(result->request_id);
        evidence["expected_ops"].set_number_value(ops);
        evidence["daemon_epoch"].set_string_value(std::to_string(result->daemon_epoch));
        evidence["expected_epoch"].set_string_value(std::to_string(epoch));
        evidence["replayed"].set_bool_value(result->replayed);
        record_evidence(result->record);
        throw std::runtime_error("incorrect individual stage receipt");
      }
    };
    const auto upsert = [&](bess::sdk::Transaction &tx, const bess::sdk::Resource &r,
                            int slot, int gate) {
      const auto result = o.stage_mode == "encoded"
          ? tx.UpsertEncoded(r, *key_wire[slot], gate == 1 ? value1 : value2)
          : tx.Upsert(r, *keys[slot], gate == 1 ? *gate1 : *gate2);
      if (!result) {
        evidence["request_id"].set_string_value(tx.request_id());
        evidence["resource"].set_string_value(r.name);
        sdk_error(result.error());
        throw std::runtime_error("stage upsert: " + result.error().message);
      }
    };
    for (int n : o.stages) {
      stages = n;
      accepted = 0;
      phase = "seed";
      ordinal = 0;
      for (const auto &r : resources) {
        for (int begin = 0; begin < resident; begin += 512) {
          auto tx = client.NewTransaction();
          for (int i = begin; i < begin + 512; ++i) {
            upsert(tx, r, i, 1);
          }
          commit(tx, 512);
        }
      }
      const auto apply = [&](uint64_t index) {
        const uint64_t working_set = o.stage_locality == "hot" ? 1 : resident;
        const int slot = static_cast<int>(index % working_set);
        const int gate = index / working_set % 2 == 0 ? 2 : 1;
        auto tx = client.NewTransaction();
        for (int stage = 0; stage < n; ++stage) {
          upsert(tx, resources[stage], slot, gate);
        }
        commit(tx, n);
      };
      phase = "cold";
      apply(0);
      const auto daemon_before = o.daemon_pid > 0
          ? std::optional<DaemonCpu>(DaemonCpu::Read(o.daemon_pid)) : std::nullopt;
      phase = "warm";
      const uint64_t cpu_before = ProcessCpuNs();
      const auto start = Clock::now();
      for (int i = 0; i < o.stage_iterations; ++i) {
        ordinal = i + 1;
        apply(static_cast<uint64_t>(ordinal));
        ++accepted;
      }
      const auto end = Clock::now();
      const double wall_ns = std::chrono::duration<double, std::nano>(end - start).count();
      const uint64_t cpu_ns = ProcessCpuNs() - cpu_before;
      phase = "observation";
      const auto daemon_after = o.daemon_pid > 0
          ? std::optional<DaemonCpu>(DaemonCpu::Read(o.daemon_pid)) : std::nullopt;
      measurements << (first ? "" : ",") << "{\"stages\":" << n
                   << ",\"ops_per_transaction\":" << n << ",\"accepted\":" << accepted
                   << ",\"started_monotonic_ns\":"
                   << std::chrono::duration_cast<std::chrono::nanoseconds>(start.time_since_epoch()).count()
                   << ",\"ended_monotonic_ns\":"
                   << std::chrono::duration_cast<std::chrono::nanoseconds>(end.time_since_epoch()).count()
                   << ",\"mean_wall_ns\":" << wall_ns / accepted
                   << ",\"mean_client_process_cpu_ns\":" << double(cpu_ns) / accepted;
      if (daemon_before && daemon_after) {
        measurements << ",\"mean_daemon_process_cpu_ns\":"
                     << double(daemon_after->process_ns - daemon_before->process_ns) / accepted;
      }
      measurements << "}";
      first = false;
    }
  } catch (const std::exception &e) {
    evidence["error"].set_string_value(e.what());
    evidence["phase"].set_string_value(phase);
    evidence["stages"].set_number_value(stages);
    evidence["ordinal"].set_number_value(ordinal);
    evidence["accepted"].set_number_value(accepted);
    std::printf("{\"kind\":\"cpp-sdk-%s-stage\",\"ok\":false,\"measurements\":[%s],"
                "\"failure\":%s}\n", o.stage_mode == "encoded" ? "encoded" : "message",
                measurements.str().c_str(), StageJson(failure).c_str());
    return false;
  }
  std::printf("{\"kind\":\"cpp-sdk-%s-stage\",\"resident_per_stage\":1024,"
              "\"registered_stages\":8,\"concurrency\":1,\"ok\":true,\"measurements\":[%s]}\n",
              o.stage_mode.c_str(), measurements.str().c_str());
  return true;
}


// One rate point: every client commits its own slice at its share of `rate`
// until the deadline, then the aggregate rate and the pooled latency are
// reported. With `--clients 1` this is the single synchronous client the
// recorded numbers were taken with.
void Measure(std::vector<Committer *> &committers, Bench &bench, const Options &o, int rate) {
  const double busy_before = bench.Busy();
  std::pair<Clock::time_point, Clock::time_point> before_bounds{}, after_bounds{};
  const auto gate_packets = [&](auto &bounds) {
    if (o.counter_window) bounds.first = Clock::now();
    auto packets = bench.GatePackets();
    if (o.counter_window) bounds.second = Clock::now();
    return packets;
  };
  const auto before = gate_packets(before_bounds);
  std::optional<DaemonCpu> cpu_before;
  const uint64_t client_cpu_before = ProcessCpuNs();
  if (o.daemon_pid > 0) {
    cpu_before = DaemonCpu::Read(o.daemon_pid);
  }
  const int per_client = rate > 0 ? std::max(1, rate / static_cast<int>(committers.size())) : rate;
  const auto start = Clock::now();
  const auto deadline =
      start + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(o.duration));
  std::vector<std::thread> threads;
  threads.reserve(committers.size());
  std::vector<std::exception_ptr> errors(committers.size());
  for (size_t i = 0; i < committers.size(); i++) {
    threads.emplace_back([&, i] {
      try {
        committers[i]->Run(per_client, deadline);
      } catch (...) {
        errors[i] = std::current_exception();
      }
    });
  }
  for (std::thread &t : threads) {
    t.join();
  }
  // A committer that threw stops the run, but only after every thread has been
  // joined: rethrowing from inside a thread, or while a joinable thread still
  // exists, terminates the process instead of printing the error.
  for (const std::exception_ptr &error : errors) {
    if (error) {
      std::rethrow_exception(error);
    }
  }
  const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
  std::optional<DaemonCpu> cpu_after;
  if (o.daemon_pid > 0) {
    cpu_after = DaemonCpu::Read(o.daemon_pid);
  }
  std::vector<double> latency;
  long done = 0;
  uint64_t thread_cpu_ns = 0;
  for (Committer *c : committers) {
    const std::vector<double> took = c->TakeLatency();
    latency.insert(latency.end(), took.begin(), took.end());
    done += c->TakeDone();
    thread_cpu_ns += c->TakeCpuNs();
  }
  const double client_cpu_s =
      double(ProcessCpuNs() - client_cpu_before) / 1e9;  // every client thread
  const auto after = gate_packets(after_bounds);
  double total = 0, misses = 0;
  for (const auto &[gate, pkts] : after) {
    const auto it = before.find(gate);
    const double d = pkts - (it == before.end() ? 0 : it->second);
    total += d;
    if (gate == "0") {
      misses = d;
    }
  }
  if (o.counter_window) {
    const auto ns = [](Clock::time_point t) {
      return static_cast<long long>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count());
    };
    const double lower = std::chrono::duration<double>(after_bounds.first - before_bounds.second).count();
    const double upper = std::chrono::duration<double>(after_bounds.second - before_bounds.first).count();
    std::printf("# counter_window {\"rate\":%d,\"packets\":%.0f,"
                "\"before_begin_ns\":%lld,\"before_end_ns\":%lld,"
                "\"after_begin_ns\":%lld,\"after_end_ns\":%lld,"
                "\"elapsed_min_s\":%.9f,\"elapsed_max_s\":%.9f,"
                "\"legacy_elapsed_s\":%.9f",
                rate, total, ns(before_bounds.first), ns(before_bounds.second),
                ns(after_bounds.first), ns(after_bounds.second), lower, upper, elapsed);
    if (cpu_before && cpu_after) {
      uint64_t worker_ns = 0;
      for (const auto &thread : DaemonCpu::Deltas(*cpu_before, *cpu_after)) {
        if (thread.name.rfind("bess-worker", 0) == 0) worker_ns += thread.ns;
      }
      const uint64_t process_ns = cpu_after->process_ns - cpu_before->process_ns;
      std::printf(",\"daemon_process_cpu_ns\":%llu,\"worker_cpu_ns\":%llu,"
                  "\"non_worker_cpu_ns\":%llu",
                  static_cast<unsigned long long>(process_ns),
                  static_cast<unsigned long long>(worker_ns),
                  static_cast<unsigned long long>(process_ns > worker_ns ? process_ns - worker_ns : 0));
    }
    std::printf("}\n");
  }
  std::sort(latency.begin(), latency.end());
  if (latency.empty()) {
    latency.push_back(0);
  }
  const double p50 = latency[latency.size() / 2];
  const double p99 = latency[std::min(latency.size() - 1, size_t(latency.size() * 0.99))];
  std::printf("%8s %8.2f %6.2f %10.0f %9.0f %9.0f %6.0f\n",
              rate < 0 ? "max" : std::to_string(rate).c_str(), total / elapsed / 1e6,
              total > 0 ? 1 - misses / total : 0.0, done / elapsed, p50 * 1e6, p99 * 1e6,
              bench.Busy() - busy_before);
  if (done > 0) {
    std::printf("%8s   client CPU %.1f us/tx (%zu threads, %.2f cores used)",
                rate < 0 ? "max" : std::to_string(rate).c_str(),
                double(thread_cpu_ns) / 1000.0 / double(done), committers.size(),
                client_cpu_s / elapsed);
    if (cpu_before && cpu_after) {
      const std::vector<DaemonCpu::Thread> deltas = DaemonCpu::Deltas(*cpu_before, *cpu_after);
      uint64_t worker_ns = 0;
      std::string top;
      int shown = 0;
      for (const DaemonCpu::Thread &thread : deltas) {
        if (thread.name.rfind("bess-worker", 0) == 0) {
          worker_ns += thread.ns;
          continue;
        }
        if (shown < 3) {  // the busiest control-side threads alive at the end
          char entry_text[96];
          (void)std::snprintf(entry_text, sizeof(entry_text), "%s %.0fus", thread.name.c_str(),
                              double(thread.ns) / 1000.0 / double(done));
          top += (shown ? ", " : "") + std::string(entry_text);
          shown++;
        }
      }
      const uint64_t process_ns = cpu_after->process_ns - cpu_before->process_ns;
      const uint64_t control_ns = process_ns > worker_ns ? process_ns - worker_ns : 0;
      std::printf("; daemon %.1f us/tx process-wide, %.1f control (minus the packet workers), "
                  "worker %.2f cores [%s]",
                  double(process_ns) / 1000.0 / double(done), double(control_ns) / 1000.0 / double(done),
                  double(worker_ns) / 1e9 / elapsed, top.c_str());
    }
    std::printf("\n");
  }
}

// Reports where the daemon put the workers -- its own verdict from
// GetPipeline (D-123; the resolver described the CPUs it chose), never a
// topology the client derives. Covers automatic cores: the daemon says which
// CPU an automatic worker got.
void ReportPlacement(bess::sdk::Client &client, const Options &o) {
  if (auto caps = client.Capabilities()) {
    bess::perf::PrintPlacement(caps->placement());
  }
  if (auto snapshot = client.Pipeline()) {
    bess::perf::ReportWorkerPlacement(snapshot->workers, o.allow_dirty_placement);
  }
}

int Main(int argc, char **argv) {
  Options o;
  for (int i = 1; i < argc; i++) {
    const std::string a = argv[i];
    const auto value = [&]() -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(a + " needs a value");
      }
      return argv[++i];
    };
    if (a == "--grpc-url") {
      o.grpc_url = value();
    } else if (a == "--worker-core") {
      const std::string c = value();
      o.worker_core = c == "auto" ? std::optional<int>{} : std::optional<int>{std::stoi(c)};
    } else if (a == "--clients") {
      o.clients = std::stoi(value());
    } else if (a == "--daemon-pid") {
      o.daemon_pid = std::stoi(value());
    } else if (a == "--counter-window") {
      o.counter_window = true;
    } else if (a == "--block-offset") {
      o.block_offset = std::stoi(value());
    } else if (a == "--blocks-total") {
      o.blocks_total = std::stoi(value());
    } else if (a == "--attach") {
      o.attach = true;
    } else if (a == "--allow-dirty-placement") {
      o.allow_dirty_placement = true;
    } else if (a == "--sessions") {
      o.sessions = std::stoi(value());
    } else if (a == "--pkt-size") {
      o.pkt_size = std::stoi(value());
    } else if (a == "--duration") {
      o.duration = std::stod(value());
    } else if (a == "--rates") {
      o.rates.clear();
      const std::string list = value();
      for (size_t pos = 0; pos <= list.size();) {
        const size_t comma = std::min(list.find(',', pos), list.size());
        const std::string r = list.substr(pos, comma - pos);
        o.rates.push_back(r == "max" ? -1 : std::stoi(r));
        pos = comma + 1;
      }
    } else if (a == "--rounds") {
      o.rounds = std::stoi(value());
    } else if (a == "--upserts") {
      o.upserts = std::stoi(value());
    } else if (a == "--erases") {
      o.erases = std::stoi(value());
    } else if (a == "--stage-counts") {
      const std::string list = value();
      for (size_t pos = 0; pos <= list.size();) {
        const size_t comma = std::min(list.find(',', pos), list.size());
        o.stages.push_back(std::stoi(list.substr(pos, comma - pos)));
        pos = comma + 1;
      }
    } else if (a == "--stage-iterations") {
      o.stage_iterations = std::stoi(value());
    } else if (a == "--stage-mode") {
      o.stage_mode = value();
    } else if (a == "--stage-locality") {
      o.stage_locality = value();
    } else if (a == "--keep") {
      o.keep = true;
    } else {
      std::fprintf(stderr,
                   "usage: live_transaction_bench [--grpc-url HOST:PORT] [--worker-core C|auto]\n"
                   "  [--clients N] [--daemon-pid PID] [--allow-dirty-placement] [--counter-window]\n"
                   "  [--sessions N] [--pkt-size B] [--duration S] [--rates R,...|max] [--rounds N]\n"
                   "  [--upserts N] [--erases N] [--keep]\n"
                   "  [--stage-counts 1,2,3,4,8 --stage-iterations 1..65536 --stage-locality hot|spread]\n"
                   "  [--stage-mode message|encoded]\n"
                   "  [--attach] [--block-offset K] [--blocks-total T]  (several processes, one\n"
                   "  daemon: the first builds with --blocks-total --keep, the rest --attach with\n"
                   "  their own --block-offset)\n");
      for (int j = 1; j < argc; ++j) {
        if (std::string(argv[j]) == "--stage-counts") {
          throw std::runtime_error("unknown stage option: " + a);
        }
      }
      return 2;
    }
  }
  if (o.sessions <= 0) {
    throw std::runtime_error("--sessions must be positive");
  }
  if (o.upserts < 0 || o.erases < 0 || o.upserts + o.erases == 0) {
    throw std::runtime_error("--upserts/--erases must be non-negative and not both zero");
  }
  if (o.clients <= 0) {
    throw std::runtime_error("--clients must be positive");
  }
  if (o.block_offset < 0) {
    throw std::runtime_error("--block-offset must not be negative");
  }
  // An attached process does not size the address range: the process that built
  // the pipeline did, with its own (possibly larger) --blocks-total, which this
  // one does not have to know.
  if (!o.attach && o.block_offset + o.clients > o.blocks()) {
    throw std::runtime_error("--block-offset " + std::to_string(o.block_offset) + " with " +
                             std::to_string(o.clients) + " clients does not fit in " +
                             std::to_string(o.blocks()) + " blocks");
  }
  if (!o.stages.empty()) {
    ValidateStageOptions(o);
  }
  // Busy answers are retried quickly and often: the bench measures them.
  const auto dial = [&] {
    return bess::sdk::Client::Dial(
        o.grpc_url,
        bess::sdk::RetryPolicy{.attempts = 16, .busy_backoff = std::chrono::microseconds(10)});
  };
  auto client = dial();
  if (!o.stages.empty()) {
    return StageFloor(*client, o) ? 0 : 1;
  }
  Bench bench(*client, o);
  bench.Build();
  std::printf("# %d upserts + %d erases per transaction\n", o.upserts, o.erases);
  ReportPlacement(*client, o);
  // One connection and one committer per client: commits run on their own
  // threads, each pacing its share of the rate.
  std::vector<std::unique_ptr<bess::sdk::Client>> clients;
  std::vector<std::unique_ptr<Types>> types;
  std::vector<std::unique_ptr<Committer>> committers;
  std::vector<Committer *> handles;
  for (int i = 0; i < o.clients; i++) {
    clients.push_back(i == 0 ? std::move(client) : dial());
    types.push_back(std::make_unique<Types>(*clients.back()));
    committers.push_back(
        std::make_unique<Committer>(*clients.back(), *types.back(), o, i));
    committers.back()->set_rules(bench.rules());
    handles.push_back(committers.back().get());
  }
  std::printf("clients: %d (aggregate rate and pooled latency)\n", o.clients);
  std::printf("%8s %8s %6s %10s %9s %9s %6s\n", "rate", "Mpps", "hit", "tx/s", "p50 us",
              "p99 us", "busy");
  for (int round = 0; round < o.rounds; round++) {
    for (const int rate : o.rates) {
      Measure(handles, bench, o, rate);
    }
  }
  if (!o.keep) {
    // --keep leaves the pipeline applied, so another client can be measured
    // against the same modules and the same traffic.
    bench.Reset();
  }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    return Main(argc, argv);
  } catch (const std::exception &e) {
    try {
      for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--stage-counts") {
          google::protobuf::Struct failure;
          (*failure.mutable_fields())["error"].set_string_value(e.what());
          (*failure.mutable_fields())["phase"].set_string_value("cli");
          std::printf("{\"kind\":\"cpp-sdk-message-stage\",\"ok\":false,"
                      "\"measurements\":[],\"failure\":%s}\n", StageJson(failure).c_str());
          break;
        }
      }
    } catch (const std::exception &serialization_error) {
      if (std::fprintf(stderr, "failure JSON: %s\n", serialization_error.what()) < 0) {
        return 1;
      }
    }
    std::fprintf(stderr, "live_transaction_bench: %s\n", e.what());
    return 1;
  }
}
