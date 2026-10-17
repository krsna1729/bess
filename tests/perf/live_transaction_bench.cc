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

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

// One transaction -- add the next session's rule, drop the oldest; two
// operations, so the live set stays at --sessions -- and the seconds the
// answering call took. A busy answer is retried with the transaction built
// again under a new request id: the daemon applied nothing.
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

// One rate point: every client commits its own slice at its share of `rate`
// until the deadline, then the aggregate rate and the pooled latency are
// reported. With `--clients 1` this is the single synchronous client the
// recorded numbers were taken with.
void Measure(std::vector<Committer *> &committers, Bench &bench, const Options &o, int rate) {
  const double busy_before = bench.Busy();
  const auto before = bench.GatePackets();
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
  for (size_t i = 1; i < committers.size(); i++) {
    threads.emplace_back([&, i] { committers[i]->Run(per_client, deadline); });
  }
  committers[0]->Run(per_client, deadline);
  for (std::thread &t : threads) {
    t.join();
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
  const auto after = bench.GatePackets();
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
    } else if (a == "--keep") {
      o.keep = true;
    } else {
      std::fprintf(stderr,
                   "usage: live_transaction_bench [--grpc-url HOST:PORT] [--worker-core C|auto]\n"
                   "  [--clients N] [--daemon-pid PID] [--allow-dirty-placement]\n"
                   "  [--sessions N] [--pkt-size B] [--duration S] [--rates R,...|max] [--rounds N]\n"
                   "  [--upserts N] [--erases N] [--keep]\n"
                   "  [--attach] [--block-offset K] [--blocks-total T]  (several processes, one\n"
                   "  daemon: the first builds with --blocks-total, the rest --attach with their\n"
                   "  own --block-offset)\n");
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
  // Busy answers are retried quickly and often: the bench measures them.
  const auto dial = [&] {
    return bess::sdk::Client::Dial(
        o.grpc_url,
        bess::sdk::RetryPolicy{.attempts = 16, .busy_backoff = std::chrono::microseconds(10)});
  };
  auto client = dial();
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
    std::fprintf(stderr, "live_transaction_bench: %s\n", e.what());
    return 1;
  }
}
