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
// oldest (two operations), so the live set stays at `--sessions`.
//
// Start bessd first (isolated as you benchmark anything else), e.g.
//   omarchy-benchmark --cpu 2,4 --isolate -- sudo env ... bessd -k -f
// then run this from another CPU:
//   live_transaction_bench --worker-core 2 --rates 0,1000,10000,max

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "bess/sdk/client.h"
#include "schema_types.h"

namespace {

namespace v2 = bess::pb::v2;
using Clock = std::chrono::steady_clock;
using bess::perf::Base64;
using bess::perf::Be32;
using bess::perf::Types;

constexpr uint32_t kBase = 0x0a000000;  // 10.0.0.0
constexpr int kGates = 4;               // rules steer to gates 1..kGates; misses go to gate 0

struct Options {
  std::string grpc_url = "localhost:10514";
  int worker_core = 0;
  int sessions = 100000;
  int pkt_size = 60;
  double duration = 5.0;
  std::vector<int> rates = {0, 1000, 10000, -1};  // -1: as fast as possible
  int rounds = 1;
};

class Bench {
 public:
  Bench(bess::sdk::Client &client, const Options &o) : client_(client), types_(client), o_(o) {}

  void Build() {
    Check(client_.PauseWorkers(), "pause");
    Check(client_.ApplyPipeline(v2::Pipeline()), "reset");
    const auto src = types_.Make(types_.InitType("Source"),
                                 R"({"pkt_size": )" + std::to_string(o_.pkt_size) + "}");
    const auto rnd = types_.Make(
        types_.InitType("RandomUpdate"),
        R"({"fields": [{"offset": 26, "size": 4, "min": )" + std::to_string(kBase) +
            R"(, "max": )" + std::to_string(kBase + 2 * o_.sessions - 1) + "}]}");
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
    // Sessions 0 .. sessions-1 live, loaded in transactions of 1000 rules.
    std::vector<double> ignored;
    for (int first = 0; first < o_.sessions; first += 1000) {
      std::vector<int> upserts;
      for (int s = first; s < std::min(first + 1000, o_.sessions); s++) {
        upserts.push_back(s);
      }
      Commit(upserts, {}, ignored);
    }
    next_ = o_.sessions;
    Check(client_.ResumeWorkers(), "resume");
    std::this_thread::sleep_for(std::chrono::seconds(1));  // warm up
  }

  void Measure(int rate) {
    std::vector<double> latency;
    const double busy_before = Busy();
    int oldest = next_ - o_.sessions;
    const auto before = GatePackets();
    const auto start = Clock::now();
    auto due = start;
    int done = 0;
    while (Clock::now() - start < std::chrono::duration<double>(o_.duration)) {
      if (rate == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      if (rate > 0) {
        due += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / rate));
        std::this_thread::sleep_until(due);
      }
      Commit({next_}, {oldest}, latency);
      next_++;
      oldest++;
      done++;
    }
    const double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    const auto after = GatePackets();
    double total = 0, misses = 0;
    for (const auto &[gate, pkts] : after) {
      const auto it = before.find(gate);
      const double d = pkts - (it == before.end() ? 0 : it->second);
      total += d;
      if (gate == "0") {
        misses = d;
      }
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
                Busy() - busy_before);
  }

  void Reset() {
    Check(client_.PauseWorkers(), "pause");
    Check(client_.ApplyPipeline(v2::Pipeline()), "reset");
  }

 private:
  template <typename T>
  static void Check(const bess::sdk::Result<T> &r, const std::string &what) {
    if (!r) {
      throw std::runtime_error(what + ": " + r.error().message);
    }
  }

  // The rule key of `session` (its address cycles over 2 x sessions).
  std::unique_ptr<google::protobuf::Message> Key(int session) {
    const uint32_t addr = kBase + uint32_t(session % (2 * o_.sessions));
    return types_.Make("ExactMatchRuleKey",
                       R"({"fields": [{"value_bin": ")" + Base64(Be32(addr)) + R"("}]})");
  }
  std::unique_ptr<google::protobuf::Message> Value(int session) {
    return types_.Make("ExactMatchRuleValue",
                       R"({"gate": )" + std::to_string(1 + session % kGates) + "}");
  }

  // One transaction; busy through every retry changed nothing, so it is
  // built again under a new request id. Latency per commit call, in seconds.
  void Commit(const std::vector<int> &upserts, const std::vector<int> &erases,
              std::vector<double> &latency) {
    while (true) {
      bess::sdk::Transaction tx = client_.NewTransaction();
      for (const int s : upserts) {
        Check(tx.Upsert(rules_, *Key(s), *Value(s)), "upsert");
      }
      for (const int s : erases) {
        Check(tx.Erase(rules_, *Key(s)), "erase");
      }
      const auto t0 = Clock::now();
      auto applied = tx.Commit();
      latency.push_back(std::chrono::duration<double>(Clock::now() - t0).count());
      if (!applied && applied.error().kind == bess::sdk::ErrorKind::kBusy) {
        continue;
      }
      Check(applied, "commit");
      return;
    }
  }

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
  int next_ = 0;
};

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
      o.worker_core = std::stoi(value());
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
    } else {
      std::fprintf(stderr,
                   "usage: live_transaction_bench [--grpc-url HOST:PORT] [--worker-core C]\n"
                   "  [--sessions N] [--pkt-size B] [--duration S] [--rates R,...|max]"
                   " [--rounds N]\n");
      return 2;
    }
  }
  if (o.sessions <= 0) {
    throw std::runtime_error("--sessions must be positive");
  }
  // Busy answers are retried quickly and often: the bench measures them.
  auto client = bess::sdk::Client::Dial(
      o.grpc_url, bess::sdk::RetryPolicy{.attempts = 16,
                                         .busy_backoff = std::chrono::microseconds(10)});
  Bench bench(*client, o);
  bench.Build();
  std::printf("%8s %8s %6s %10s %9s %9s %6s\n", "rate", "Mpps", "hit", "tx/s", "p50 us",
              "p99 us", "busy");
  for (int round = 0; round < o.rounds; round++) {
    for (const int rate : o.rates) {
      bench.Measure(rate);
    }
  }
  bench.Reset();
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  try {
    return Main(argc, argv);
  } catch (const std::exception &e) {
    std::fprintf(stderr, "live_transaction_bench: %s\n", e.what());
    return 1;
  }
}
