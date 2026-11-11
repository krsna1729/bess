// SPDX-License-Identifier: BSD-3-Clause

// ExactMatch's packet path measured through the module itself
// (ExactMatch::ClassifyBatch runs the code ProcessBatch runs, minus the
// emit), over real packets.
//
//   BM_ModuleClassify/N/mix  one 32-packet batch per iteration against a
//                            module holding N rules; mix 0 = all hits,
//                            1 = all misses. Items = packets.
//   BM_ClassifyUnderTransactions/readers/rate
//                            `readers` threads (registered RCU readers,
//                            each on its own CPU) classify packets through
//                            two modules, "ul" and "dl", while this thread
//                            establishes and releases sessions -- one
//                            transaction adds a rule to each module, another
//                            removes them -- at `rate` sessions/s (0: none,
//                            -1: as fast as it can). Reported: Mpps per
//                            reader (a packet = one lookup in one module),
//                            sessions/s achieved.

#include <benchmark/benchmark.h>

#include <pthread.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <any>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <exception>
#include <memory>
#include <cstring>
#include <random>
#include <string>
#include <stdexcept>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "control/api_v2.h"
#include "control/control_plane.h"
#include "control/dataplane_transactions.h"
#include "framework/resource_bindings.h"
#include "pb/control_v2.grpc.pb.h"
#include "runtime/runtime_state.h"
#include "dataplane/transaction_engine.h"
#include "module.h"
#include "module_graph.h"
#include "modules/exact_match.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "rcu/rcu_domain.h"

namespace {

// The CPUs this process may use, captured before DPDK's EAL starts (it pins
// the main thread to one core, and threads created later inherit that).
const cpu_set_t kInitialCpus = [] {
  cpu_set_t set;
  CPU_ZERO(&set);
  sched_getaffinity(0, sizeof(set), &set);
  return set;
}();

constexpr size_t kBatch = 32;
constexpr size_t kBatches = 1024;  // 32K packets: keys spread over the table

// The packed key of a rule over the two fields (4 bytes, 2 bytes).
std::string Key(uint32_t a, uint16_t b) {
  std::string key(6, '\0');
  std::memcpy(key.data(), &a, sizeof(a));
  std::memcpy(key.data() + 4, &b, sizeof(b));
  return key;
}

// Fields: 4 bytes at offset 26 (IPv4 source), 2 bytes at offset 34 (UDP
// source port). Rule r matches (r, r & 0xffff) and goes to gate 1 + r % 63;
// misses take the default gate, set to 0 (unset, it is DROP_GATE, which the
// hit count would take for a hit).
ExactMatch *CreateModule(const char *name, size_t rules) {
  bess::pb::ExactMatchArg arg;
  auto *f = arg.add_fields();
  f->set_offset(26);
  f->set_num_bytes(4);
  f = arg.add_fields();
  f->set_offset(34);
  f->set_num_bytes(2);
  arg.set_default_gate(0);
  google::protobuf::Any packed;
  if (!packed.PackFrom(arg)) {
    std::abort();
  }
  pb_error_t perr;
  Module *m = ModuleGraph::CreateModule(
      ModuleBuilder::all_module_builders().at("ExactMatch"), name, packed,
      &perr);
  if (m == nullptr || perr.code() != 0) {
    std::abort();
  }
  auto *em = static_cast<ExactMatch *>(m);
  // Prefill in one bulk transaction -- the way a client loads a table
  // (D-140: the legacy set_runtime_config restore is gone).
  std::vector<bess::dataplane::Op> ops;
  ops.reserve(rules);
  for (size_t r = 0; r < rules; r++) {
    ops.push_back(bess::dataplane::Op::Upsert(
        std::string(name) + "/rules",
        Key(static_cast<uint32_t>(r), static_cast<uint16_t>(r)),
        std::any(bess::classifier::RuleResult{
            static_cast<gate_idx_t>(1 + r % 63), {}})));
  }
  if (bess::runtime::runtime().transactions().Apply(ops).outcome !=
      bess::dataplane::TransactionEngine::Outcome::kApplied) {
    std::abort();
  }
  return em;
}

// A UDP packet whose key is (a, b).
void SetKey(bess::PacketHandle pkt, uint32_t a, uint16_t b) {
  uint8_t *p = bess::PacketRef(pkt).head_data<uint8_t *>();
  std::memcpy(p + 26, &a, sizeof(a));
  std::memcpy(p + 34, &b, sizeof(b));
}

struct Packets {
  explicit Packets(size_t n) : pool(n + 64) {
    for (size_t i = 0; i < n; i++) {
      bess::PacketHandle pkt = pool.Alloc(60);
      if (pkt == nullptr) {
        std::abort();
      }
      uint8_t *p = bess::PacketRef(pkt).head_data<uint8_t *>();
      std::memset(p, 0, 60);
      p[12] = 0x08;
      p[14] = 0x45;
      p[23] = 17;
      handles.push_back(pkt);
    }
  }
  ~Packets() {
    for (bess::PacketHandle pkt : handles) {
      bess::PacketFree(pkt);
    }
  }
  bess::PlainPacketPool pool;
  std::vector<bess::PacketHandle> handles;
};

void Load(bess::PacketBatch *batch, const Packets &packets, size_t first) {
  batch->clear();
  for (size_t i = 0; i < kBatch; i++) {
    batch->add(bess::PacketRef(packets.handles[first + i]));
  }
}

void BM_ModuleClassify(benchmark::State &state) {
  const size_t rules = static_cast<size_t>(state.range(0));
  const bool miss = state.range(1) == 1;
  ModuleGraph::DestroyAllModules();
  ExactMatch *em = CreateModule("em", rules);
  Packets packets(kBatch * kBatches);
  std::mt19937_64 rng(42);
  for (bess::PacketHandle pkt : packets.handles) {
    const uint32_t r = static_cast<uint32_t>(rng() % rules);
    SetKey(pkt, miss ? r | 0x80000000u : r, static_cast<uint16_t>(r));
  }
  std::vector<bess::PacketBatch> batches(kBatches);
  for (size_t b = 0; b < kBatches; b++) {
    Load(&batches[b], packets, b * kBatch);
  }
  std::array<gate_idx_t, kBatch> gates;
  std::array<uint32_t, kBatch> action_ids;
  uint64_t hits = 0;
  size_t b = 0;
  for (auto _ : state) {
    em->ClassifyBatch(&batches[b], gates.data(), action_ids.data());
    benchmark::DoNotOptimize(gates);
    for (gate_idx_t g : gates) {
      hits += g != 0;
    }
    b = (b + 1) % kBatches;
  }
  if (hits != (miss ? 0 : state.iterations() * kBatch)) {
    state.SkipWithError("wrong hit count");
  }
  state.SetItemsProcessed(state.iterations() * kBatch);
  ModuleGraph::DestroyAllModules();
}
BENCHMARK(BM_ModuleClassify)
    ->ArgsProduct({{1000, 128000, 1000000}, {0, 1}});

void BM_ClassifyUnderTransactions(benchmark::State &state) {
  using bess::dataplane::Op;
  using bess::dataplane::TransactionEngine;
  const int readers = static_cast<int>(state.range(0));
  const int64_t rate = state.range(1);  // sessions/s; 0 none; -1 max
  constexpr uint32_t kSessions = 65536;
  ModuleGraph::DestroyAllModules();
  // Session s: rule (s, 1) in "ul" and (s, 2) in "dl", both to gate
  // 1 + s % 63. Half the sessions are live at any time.
  ExactMatch *ul = CreateModule("ul", 0);
  ExactMatch *dl = CreateModule("dl", 0);
  TransactionEngine &engine = bess::runtime::runtime().transactions();
  auto establish = [](uint32_t s) {
    const uint64_t gate = 1 + s % 63;
    return std::vector<Op>{
        Op::Upsert("ul/rules", Key(s, 1),
                   std::any(bess::classifier::RuleResult{
                       static_cast<gate_idx_t>(gate), {}})),
        Op::Upsert("dl/rules", Key(s, 2),
                   std::any(bess::classifier::RuleResult{
                       static_cast<gate_idx_t>(gate), {}}))};
  };
  auto release = [](uint32_t s) {
    return std::vector<Op>{Op::Erase("ul/rules", Key(s, 1)),
                           Op::Erase("dl/rules", Key(s, 2))};
  };
  uint32_t oldest = 0, next = 0;
  for (; next < kSessions / 2; next++) {
    if (engine.Apply(establish(next)).outcome !=
        TransactionEngine::Outcome::kApplied) {
      state.SkipWithError("populate failed");
      return;
    }
  }

  // Per reader: packets for random sessions (live or not), half for each
  // module.
  std::vector<std::unique_ptr<Packets>> packets;
  std::mt19937_64 rng(42);
  for (int r = 0; r < readers; r++) {
    packets.push_back(std::make_unique<Packets>(kBatch * kBatches));
    for (size_t i = 0; i < kBatch * kBatches; i++) {
      const uint32_t session = static_cast<uint32_t>(rng() % kSessions);
      SetKey(packets[r]->handles[i], session, (i / kBatch) % 2 ? 2 : 1);
    }
  }

  bess::rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  std::atomic<bool> stop{false};
  std::vector<std::atomic<uint64_t>> classified(readers);
  std::vector<std::thread> threads;
  const int main_cpu = sched_getcpu();
  int cpu = 0;
  for (int r = 0; r < readers; r++) {
    // The next allowed CPU that is not the writer's.
    while (cpu < CPU_SETSIZE &&
           (cpu == main_cpu || !CPU_ISSET(cpu, &kInitialCpus))) {
      cpu++;
    }
    const int reader_cpu = cpu < CPU_SETSIZE ? cpu++ : -1;
    const bess::rcu::ReaderId id = static_cast<bess::rcu::ReaderId>(20 + r);
    (void)domain.Register(id);
    classified[r] = 0;
    threads.emplace_back([&, r, id, reader_cpu] {
      if (reader_cpu >= 0) {
        cpu_set_t one;
        CPU_ZERO(&one);
        CPU_SET(reader_cpu, &one);
        pthread_setaffinity_np(pthread_self(), sizeof(one), &one);
      }
      std::vector<bess::PacketBatch> batches(kBatches);
      for (size_t b = 0; b < kBatches; b++) {
        Load(&batches[b], *packets[r], b * kBatch);
      }
      domain.Online(id);
      std::array<gate_idx_t, kBatch> gates;
      std::array<uint32_t, kBatch> action_ids;
      uint64_t local = 0, sink = 0;
      size_t b = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        (b % 2 ? dl : ul)->ClassifyBatch(&batches[b], gates.data(),
                                         action_ids.data());
        for (gate_idx_t g : gates) {
          sink += g;
        }
        local += kBatch;
        b = (b + 1) % kBatches;
        domain.Quiescent(id);
      }
      benchmark::DoNotOptimize(sink);
      classified[r] = local;
      domain.Offline(id);
    });
  }

  uint64_t busy = 0, sessions = 0;
  const auto start = std::chrono::steady_clock::now();
  auto due = start;
  for (auto _ : state) {
    if (rate == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (rate > 0) {
      due += std::chrono::nanoseconds(1000000000 / rate);
      while (std::chrono::steady_clock::now() < due) {
      }
    }
    // kBusy means readers are behind on quiescing: try again.
    while (engine.Apply(establish(next % kSessions)).outcome !=
           TransactionEngine::Outcome::kApplied) {
      busy++;
    }
    while (engine.Apply(release(oldest % kSessions)).outcome !=
           TransactionEngine::Outcome::kApplied) {
      busy++;
    }
    next++;
    oldest++;
    sessions++;
    domain.ReclaimReady();
  }
  const double seconds = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - start)
                             .count();
  stop = true;
  for (auto &t : threads) {
    t.join();
  }
  uint64_t total = 0;
  for (auto &c : classified) {
    total += c;
  }
  state.counters["Mpps_per_reader"] =
      static_cast<double>(total) / readers / seconds / 1e6;
  state.counters["sessions_per_s"] = static_cast<double>(sessions) / seconds;
  state.counters["busy_retries"] = static_cast<double>(busy);
  for (int r = 0; r < readers; r++) {
    domain.Unregister(static_cast<bess::rcu::ReaderId>(20 + r));
  }
  packets.clear();
  ModuleGraph::DestroyAllModules();
  domain.Drain();
}
BENCHMARK(BM_ClassifyUnderTransactions)
    ->ArgsProduct({{1, 2, 4}, {0, 10000, 100000, -1}})
    ->UseRealTime()
    ->MinTime(2.0);

// Layered replacement floor. All eight real modules and their resident sets
// exist at every stage count; only the modified prefix changes. There is no
// packet reader, maintenance thread, SDK, dependency edge, or N4 frontend here.
// The TCP/Unix cases use the real service, but client and server share a
// process: MeasureProcessCPUTime includes both, not daemon-only CPU.
namespace stage_floor {

namespace v2 = bess::pb::v2;
using bess::dataplane::Op;
using bess::dataplane::TransactionEngine;

constexpr uint32_t kResident = 1024;
constexpr size_t kStages = 8;
enum class Boundary { kOwned, kAdapter, kService, kProto, kTcp, kUnix };

std::string StageKey(uint32_t slot) {
  const uint32_t address = 0x0b000001u + slot;
  return {static_cast<char>(address >> 24u), static_cast<char>(address >> 16u),
          static_cast<char>(address >> 8u), static_cast<char>(address)};
}

std::string Hex(const std::string &bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) {
    result.push_back(digits[byte >> 4u]);
    result.push_back(digits[byte & 15u]);
  }
  return result;
}

struct Writer {
  std::vector<Op> ops;
  v2::ApplyTransactionRequest request;
  v2::ApplyTransactionRequest decoded;
  v2::ApplyTransactionResponse response;
  v2::ApplyTransactionResponse decoded_response;
  std::unique_ptr<v2::Control::Stub> stub;
  std::string wire_request, wire_response;
  uint64_t ordinal = 1;  // ordinal zero is the separately observed cold call
};

class Fixture {
 public:
  Fixture(Boundary boundary, size_t stages, bool spread, int writers)
      : boundary_(boundary),
        stages_(stages),
        spread_(spread),
        service_(control_),
        adapter_(bess::runtime::runtime().transactions(),
                 bess::framework::ResourceBindings::ProcessDefault(),
                 service_.epoch()),
        writers_(static_cast<size_t>(writers)) {
    ModuleGraph::DestroyAllModules();
    std::array<Module *, kStages> modules{};
    for (size_t stage = 0; stage < kStages; ++stage) {
      const std::string name = "stage" + std::to_string(stage);
      bess::pb::ExactMatchArg arg;
      arg.add_fields()->set_offset(26);
      arg.mutable_fields(0)->set_num_bytes(4);
      arg.set_default_gate(0);
      google::protobuf::Any packed;
      if (!packed.PackFrom(arg)) {
        throw std::runtime_error("stage argument encoding failed");
      }
      pb_error_t error;
      modules[stage] = ModuleGraph::CreateModule(
          ModuleBuilder::all_module_builders().at("ExactMatch"), name, packed,
          &error);
      if (modules[stage] == nullptr || error.code() != 0) {
        throw std::runtime_error("stage module creation failed");
      }
      auto &engine = bess::runtime::runtime().transactions();
      const std::string resource_name = name + "/rules";
      const auto *resource = engine.FindResource(resource_name);
      if (resource == nullptr) {
        throw std::runtime_error("stage resource was not registered");
      }
      owned_.push_back(resource);
      std::vector<Op> seed;
      seed.reserve(kResident);
      for (uint32_t slot = 0; slot < kResident; ++slot) {
        seed.push_back(Op::Upsert(
            resource_name, StageKey(slot),
            std::any(bess::classifier::RuleResult{1, {}})));
      }
      if (engine.Apply(seed).outcome != TransactionEngine::Outcome::kApplied) {
        throw std::runtime_error("stage population failed");
      }
      const auto capacity = resource->Capacity();
      // Growable ExactMatch resources deliberately have no fixed public
      // Capacity(). An absent bound is not a setup failure or a zero capacity.
      if (capacity) {
        capacities_.push_back(*capacity);
      }
    }
    pb_error_t error;
    Module *sink = ModuleGraph::CreateModule(
        ModuleBuilder::all_module_builders().at("Sink"), "stage_sink",
        google::protobuf::Any{}, &error);
    if (sink == nullptr || error.code() != 0) {
      throw std::runtime_error("stage sink creation failed");
    }
    for (size_t stage = 0; stage < kStages; ++stage) {
      Module *next = stage + 1 < kStages ? modules[stage + 1] : sink;
      for (gate_idx_t gate = 0; gate <= 2; ++gate) {
        if (ModuleGraph::ConnectModules(modules[stage], gate, next, 0) != 0) {
          throw std::runtime_error("stage graph connection failed");
        }
      }
    }
    owned_.resize(stages_);
    if (boundary_ == Boundary::kTcp || boundary_ == Boundary::kUnix) {
      grpc::ServerBuilder builder;
      bess::control::ConfigureControlServer(&builder);
      builder.RegisterService(&service_);
      int port = 0;
      if (boundary_ == Boundary::kTcp) {
        builder.AddListeningPort("127.0.0.1:0",
                                 grpc::InsecureServerCredentials(), &port);
      } else {
        socket_path_ = "/tmp/bess-stage-floor-" + std::to_string(getpid()) + ".sock";
        builder.AddListeningPort("unix:" + socket_path_,
                                 grpc::InsecureServerCredentials());
      }
      server_ = builder.BuildAndStart();
      if (!server_ || (boundary_ == Boundary::kTcp && port == 0)) {
        throw std::runtime_error("stage RPC server creation failed");
      }
      const std::string endpoint = boundary_ == Boundary::kTcp
                                       ? "127.0.0.1:" + std::to_string(port)
                                       : "unix:" + socket_path_;
      for (Writer &writer : writers_) {
        writer.stub = v2::Control::NewStub(
            grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials()));
      }
    }
    for (size_t index = 0; index < writers_.size(); ++index) {
      Writer &writer = writers_[index];
      writer.request.set_daemon_epoch(service_.epoch());
      writer.ops.reserve(stages_);
      for (size_t stage = 0; stage < stages_; ++stage) {
        const std::string resource = "stage" + std::to_string(stage) + "/rules";
        writer.ops.push_back(Op::Upsert(
            resource, StageKey(static_cast<uint32_t>(index)),
            std::any(bess::classifier::RuleResult{2, {}})));
        auto *op = writer.request.add_ops();
        op->set_resource(resource);
        bess::pb::ExactMatchRuleKey key;
        key.add_fields()->set_value_bin(StageKey(static_cast<uint32_t>(index)));
        bess::pb::ExactMatchRuleValue value;
        value.set_gate(2);
        if (!op->mutable_key()->PackFrom(key) ||
            !op->mutable_value()->PackFrom(value)) {
          throw std::runtime_error("stage operation encoding failed");
        }
      }
      Prepare(writer, index, 0);
      const auto start = std::chrono::steady_clock::now();
      const std::string failure = Apply(writer);
      const auto end = std::chrono::steady_clock::now();
      if (!failure.empty()) {
        throw std::runtime_error(failure);
      }
      if (index == 0) {
        cold_ns_ = std::chrono::duration<double, std::nano>(end - start).count();
        request_bytes_ = writer.request.ByteSizeLong();
        response_bytes_ = writer.response.ByteSizeLong();
        example_key_hex_ = Hex(writer.request.ops(0).key().value());
        example_value_hex_ = Hex(writer.request.ops(0).value().value());
      }
    }
  }

  ~Fixture() {
    if (server_) {
      server_->Shutdown();
      server_->Wait();
    }
    if (!socket_path_.empty()) {
      (void)unlink(socket_path_.c_str());
    }
    bess::runtime::runtime().rcu().Drain();
    ModuleGraph::DestroyAllModules();
    bess::runtime::runtime().rcu().Drain();
  }

  void Prepare(Writer &writer, size_t index, uint64_t ordinal) {
    const uint64_t working_set = spread_ ? kResident / writers_.size() : 1;
    const uint32_t slot = static_cast<uint32_t>(
        index + (spread_ ? ordinal % working_set * writers_.size() : 0));
    const gate_idx_t gate = static_cast<gate_idx_t>(2 - (ordinal / working_set) % 2);
    const std::string key = StageKey(slot);
    for (size_t stage = 0; stage < stages_; ++stage) {
      if (boundary_ == Boundary::kOwned) {
        writer.ops[stage].key = key;
        std::any_cast<bess::classifier::RuleResult>(&writer.ops[stage].value)->gate = gate;
      } else {
        // Reuse protobuf/Any capacity. This timed boundary includes input
        // mutation, not discovery or dynamic SDK message construction.
        auto *op = writer.request.mutable_ops(static_cast<int>(stage));
        std::string *key_wire = op->mutable_key()->mutable_value();
        key_wire->replace(key_wire->size() - key.size(), key.size(), key);
        op->mutable_value()->mutable_value()->back() = static_cast<char>(gate);
      }
    }
    if (boundary_ == Boundary::kOwned) {
      return;
    }
    char id[33];
    const int size = std::snprintf(id, sizeof(id), "%016llx%016llx",
                                   static_cast<unsigned long long>(index),
                                   static_cast<unsigned long long>(ordinal));
    if (size != 32) {
      throw std::runtime_error("stage request ID formatting failed");
    }
    writer.request.set_request_id(id);
  }

  std::string Apply(Writer &writer) {
    auto &engine = bess::runtime::runtime().transactions();
    if (boundary_ == Boundary::kOwned) {
      auto lock = control_.AcquireLock();
      const auto result = engine.ApplyOwned(owned_, writer.ops);
      if (result.outcome != TransactionEngine::Outcome::kApplied) {
        return "owned transaction was not applied";
      }
      for (const auto &op : result.ops) {
        if (op.status != TransactionEngine::OpStatus::kApplied) {
          return "owned operation was not applied";
        }
      }
      return {};
    }
    writer.response.Clear();
    if (boundary_ == Boundary::kAdapter) {
      auto lock = control_.AcquireLock();
      auto result = adapter_.Apply(writer.request);
      if (!result) {
        return result.error().message;
      }
      writer.response = std::move(*result);
    } else if (boundary_ == Boundary::kTcp || boundary_ == Boundary::kUnix) {
      grpc::ClientContext context;
      context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
      const auto status =
          writer.stub->ApplyTransaction(&context, writer.request, &writer.response);
      if (!status.ok()) {
        return status.error_message();
      }
    } else {
      const v2::ApplyTransactionRequest *request = &writer.request;
      if (boundary_ == Boundary::kProto) {
        if (!writer.request.SerializeToString(&writer.wire_request) ||
            !writer.decoded.ParseFromString(writer.wire_request)) {
          return "request protobuf round trip failed";
        }
        request = &writer.decoded;
      }
      grpc::ServerContext context;
      const auto status = service_.ApplyTransaction(&context, request, &writer.response);
      if (!status.ok()) {
        return status.error_message();
      }
      if (boundary_ == Boundary::kProto) {
        if (!writer.response.SerializeToString(&writer.wire_response) ||
            !writer.decoded_response.ParseFromString(writer.wire_response)) {
          return "receipt protobuf round trip failed";
        }
        writer.response.Swap(&writer.decoded_response);
      }
    }
    const auto &record = writer.response.record();
    if (writer.response.replayed() ||
        writer.response.daemon_epoch() != service_.epoch() ||
        record.request_id() != writer.request.request_id() ||
        record.outcome() != v2::TransactionRecord::OUTCOME_APPLIED ||
        record.ops_size() != static_cast<int>(stages_)) {
      return "incorrect individual transaction receipt";
    }
    for (const auto &op : record.ops()) {
      if (op.status() != v2::TransactionOpResult::STATUS_APPLIED) {
        return "incorrect individual operation receipt";
      }
    }
    return {};
  }

  void Counters(benchmark::State &state) const {
    state.counters["modified_stages"] = static_cast<double>(stages_);
    state.counters["ops_per_tx"] = static_cast<double>(stages_);
    state.counters["registered_stages"] = kStages;
    state.counters["resident_per_stage"] = kResident;
    state.counters["capacity_reported_stages"] = capacities_.size();
    if (!capacities_.empty()) {
      state.counters["capacity_min"] =
          static_cast<double>(*std::min_element(capacities_.begin(), capacities_.end()));
      state.counters["capacity_max"] =
          static_cast<double>(*std::max_element(capacities_.begin(), capacities_.end()));
    }
    state.counters["cold_first_apply_ns"] = cold_ns_;
    state.counters["request_bytes_example"] =
        boundary_ == Boundary::kOwned ? 0 : static_cast<double>(request_bytes_);
    state.counters["receipt_bytes_example"] =
        boundary_ == Boundary::kOwned ? 0 : static_cast<double>(response_bytes_);
    state.SetLabel("key_any_value_hex=" + example_key_hex_ +
                   ";value_any_value_hex=" + example_value_hex_ +
                   ";input=reused-mutation+apply;reader=none;maintenance=none;"
                   "cold=first-post-seed;cpu=whole-process");
  }

  Writer &writer(size_t index) { return writers_[index]; }

 private:
  Boundary boundary_;
  size_t stages_;
  bool spread_;
  bess::control::ControlPlane control_;
  bess::control::ControlV2Service service_;
  bess::control::DataplaneTransactions adapter_;
  std::vector<const bess::dataplane::Resource *> owned_;
  std::vector<uint64_t> capacities_;
  std::vector<Writer> writers_;
  std::unique_ptr<grpc::Server> server_;
  std::string socket_path_, example_key_hex_, example_value_hex_;
  double cold_ns_ = 0;
  size_t request_bytes_ = 0, response_bytes_ = 0;
};

// Google Benchmark barriers bracket its multithreaded loop: thread zero owns
// setup/teardown, and nobody accesses the shared fixture before/after the loop.
// Every writer owns its request/receipt and disjoint hot key or spread slice.
void Run(benchmark::State &state, Boundary boundary) {
  static std::unique_ptr<Fixture> fixture;
  static std::string setup_error;
  if (state.thread_index() == 0) {
    setup_error.clear();
    try {
      fixture = std::make_unique<Fixture>(
          boundary, static_cast<size_t>(state.range(0)), state.range(1) != 0,
          state.threads());
    } catch (const std::exception &error) {
      setup_error = error.what();
      bess::runtime::runtime().rcu().Drain();
      ModuleGraph::DestroyAllModules();
      bess::runtime::runtime().rcu().Drain();
    }
  }
  uint64_t applied = 0;
  uint64_t errors = 0;
  for (auto _ : state) {
    if (!setup_error.empty()) {
      ++errors;
      state.SkipWithError(setup_error.c_str());
      break;
    }
    Writer &writer = fixture->writer(static_cast<size_t>(state.thread_index()));
    fixture->Prepare(writer, static_cast<size_t>(state.thread_index()), writer.ordinal++);
    const std::string failure = fixture->Apply(writer);
    if (!failure.empty()) {
      ++errors;
      state.SkipWithError(failure.c_str());
      break;
    }
    ++applied;
  }
  state.SetItemsProcessed(static_cast<int64_t>(applied));  // transactions, not operations
  state.counters["applied"] = static_cast<double>(applied);
  state.counters["errors"] = static_cast<double>(errors);
  if (state.thread_index() == 0) {
    if (fixture) {
      fixture->Counters(state);
    }
    fixture.reset();
  }
}

void BM_StageOwned(benchmark::State &state) { Run(state, Boundary::kOwned); }
void BM_StageAdapter(benchmark::State &state) { Run(state, Boundary::kAdapter); }
void BM_StageService(benchmark::State &state) { Run(state, Boundary::kService); }
void BM_StageProto(benchmark::State &state) { Run(state, Boundary::kProto); }
void BM_StageTcp(benchmark::State &state) { Run(state, Boundary::kTcp); }
void BM_StageUnix(benchmark::State &state) { Run(state, Boundary::kUnix); }

// `spread=0`: repeated replacement of each writer's existing hot key.
// `spread=1`: disjoint writer slices cover the same 1024 existing keys.
// Threads(8) is a separate bounded-contention case, never the one-writer floor.
BENCHMARK(BM_StageOwned)->ArgsProduct({{1, 2, 3, 4, 8}, {0, 1}})
    ->ArgNames({"stages", "spread"})->Threads(1)->Threads(8)
    ->UseRealTime()->MeasureProcessCPUTime();
BENCHMARK(BM_StageAdapter)->ArgsProduct({{1, 2, 3, 4, 8}, {0, 1}})
    ->ArgNames({"stages", "spread"})->Threads(1)->Threads(8)
    ->UseRealTime()->MeasureProcessCPUTime();
BENCHMARK(BM_StageService)->ArgsProduct({{1, 2, 3, 4, 8}, {0, 1}})
    ->ArgNames({"stages", "spread"})->Threads(1)->Threads(8)
    ->UseRealTime()->MeasureProcessCPUTime();
BENCHMARK(BM_StageProto)->ArgsProduct({{1, 2, 3, 4, 8}, {0, 1}})
    ->ArgNames({"stages", "spread"})->Threads(1)->Threads(8)
    ->UseRealTime()->MeasureProcessCPUTime();
BENCHMARK(BM_StageTcp)->ArgsProduct({{1, 2, 3, 4, 8}, {0, 1}})
    ->ArgNames({"stages", "spread"})->Threads(1)->Threads(8)
    ->UseRealTime()->MeasureProcessCPUTime();
BENCHMARK(BM_StageUnix)->ArgsProduct({{1, 2, 3, 4, 8}, {0, 1}})
    ->ArgNames({"stages", "spread"})->Threads(1)->Threads(8)
    ->UseRealTime()->MeasureProcessCPUTime();

}  // namespace stage_floor

}  // namespace
