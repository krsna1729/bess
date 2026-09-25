// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// G1.2 E4: ingress -- how many dataplane modifications per second can reach
// bessd, over which transport, in which encoding?
//
// Server side is always the same: decode ops and (optionally) apply each
// one to a lock-free rte_hash (mode C, one writer). Variants:
//
//   grpc-unary-msgs    unary RPC, one IngressOp message per op
//   grpc-unary-packed  unary RPC, ops as fixed 16-byte records in `bytes`
//   grpc-stream-packed client-streaming RPC of packed batches, one ack
//   shm-ring           a shared-memory SPSC ring of 16-byte records between
//                      two processes (fork); the server thread polls it --
//                      the shape of VPP's shared-memory API queues, with no
//                      serialization at all
//
// Transports for gRPC: TCP loopback and a Unix domain socket. Batch sizes
// sweep 1..4096 ops. "decode" rows skip the table so transport and
// encoding cost is visible on its own; "apply" rows include the rte_hash
// insert/delete (about 50-270 ns each, from E1).

#define ALLOW_EXPERIMENTAL_API 1

#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <grpc++/grpc++.h>
#include <rte_cycles.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_pause.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dpdk.h"
#include "pb/ingress_bench.grpc.pb.h"

namespace {

namespace tpb = bess::pb::test;
constexpr uint64_t kDeleteBit = uint64_t{1} << 63;
constexpr uint64_t kTableKeys = 1 << 20;

uint64_t Mix(uint64_t x) {
  x += 0x9e3779b97f4a7c15ull;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
  return (x ^ (x >> 31)) & ~kDeleteBit;
}

// Op i: the key cycles through kTableKeys; alternating passes insert then
// delete them, so the table size stays bounded.
uint64_t OpKey(uint64_t i) {
  const uint64_t k = Mix(i % kTableKeys);
  return ((i / kTableKeys) & 1) ? (k | kDeleteBit) : k;
}

double Now() { return static_cast<double>(rte_rdtsc()) / rte_get_tsc_hz(); }

struct Table {
  rte_hash *h = nullptr;
  Table() {
    static int seq = 0;
    const std::string name = "ingress_" + std::to_string(seq++);
    rte_hash_parameters p{};
    p.name = name.c_str();
    p.entries = kTableKeys + kTableKeys / 4;
    p.key_len = sizeof(uint64_t);
    p.hash_func = rte_hash_crc;
    p.socket_id = 0;
    p.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF;
    h = rte_hash_create(&p);
  }
  ~Table() { rte_hash_free(h); }
  void Apply(uint64_t key, uint64_t value) {
    if (key & kDeleteBit) {
      key &= ~kDeleteBit;
      rte_hash_del_key(h, &key);
    } else {
      rte_hash_add_key_data(h, &key, reinterpret_cast<void *>(value));
    }
  }
};

// -- gRPC ------------------------------------------------------------------------

class Service final : public tpb::IngressBench::Service {
 public:
  explicit Service(Table &table) : table_(table) {}

  grpc::Status Apply(grpc::ServerContext *, const tpb::IngressBatch *b,
                     tpb::IngressAck *ack) override {
    ack->set_applied(Consume(*b));
    return grpc::Status::OK;
  }

  grpc::Status ApplyStream(grpc::ServerContext *,
                           grpc::ServerReader<tpb::IngressBatch> *reader,
                           tpb::IngressAck *ack) override {
    tpb::IngressBatch b;
    uint64_t n = 0;
    while (reader->Read(&b)) n += Consume(b);
    ack->set_applied(n);
    return grpc::Status::OK;
  }

  uint64_t sink = 0;

 private:
  uint64_t Consume(const tpb::IngressBatch &b) {
    uint64_t n = 0;
    if (!b.packed().empty()) {
      const char *p = b.packed().data();
      const size_t count = b.packed().size() / 16;
      for (size_t i = 0; i < count; i++) {
        uint64_t key, value;
        memcpy(&key, p + 16 * i, 8);
        memcpy(&value, p + 16 * i + 8, 8);
        if (b.apply()) table_.Apply(key, value); else sink += key ^ value;
      }
      n = count;
    } else {
      for (const auto &op : b.ops()) {
        const uint64_t key = op.kind() ? (op.key() | kDeleteBit) : op.key();
        if (b.apply()) table_.Apply(key, op.value()); else sink += key ^ op.value();
      }
      n = static_cast<uint64_t>(b.ops_size());
    }
    return n;
  }

  Table &table_;
};

tpb::IngressBatch MakeBatch(uint64_t start, uint32_t n, bool packed, bool apply) {
  tpb::IngressBatch b;
  b.set_apply(apply);
  if (packed) {
    std::string bytes(16 * n, '\0');
    for (uint32_t i = 0; i < n; i++) {
      const uint64_t key = OpKey(start + i), value = start + i;
      memcpy(&bytes[16 * i], &key, 8);
      memcpy(&bytes[16 * i + 8], &value, 8);
    }
    b.set_packed(std::move(bytes));
  } else {
    for (uint32_t i = 0; i < n; i++) {
      const uint64_t key = OpKey(start + i);
      auto *op = b.add_ops();
      op->set_kind((key & kDeleteBit) ? 1 : 0);
      op->set_key(key & ~kDeleteBit);
      op->set_value(start + i);
    }
  }
  return b;
}

// Ops per second for one variant. Batches are prebuilt; each send still
// serializes, so client-side encoding is included.
double RunGrpc(tpb::IngressBench::Stub &stub, const std::string &variant,
               uint32_t batch, bool apply, double seconds) {
  const bool packed = variant != "grpc-unary-msgs";
  std::vector<tpb::IngressBatch> batches;
  for (uint64_t i = 0; i < 64; i++) {
    batches.push_back(MakeBatch(i * batch, batch, packed, apply));
  }
  uint64_t ops = 0;
  const double t0 = Now();
  if (variant == "grpc-stream-packed") {
    grpc::ClientContext ctx;
    tpb::IngressAck ack;
    auto writer = stub.ApplyStream(&ctx, &ack);
    size_t k = 0;
    while (Now() - t0 < seconds) {
      writer->Write(batches[k++ % batches.size()]);
      ops += batch;
    }
    writer->WritesDone();
    writer->Finish();
  } else {
    size_t k = 0;
    while (Now() - t0 < seconds) {
      grpc::ClientContext ctx;
      tpb::IngressAck ack;
      stub.Apply(&ctx, batches[k++ % batches.size()], &ack);
      ops += batch;
    }
  }
  return ops / (Now() - t0);
}

// -- shared memory ring ------------------------------------------------------------

struct alignas(64) ShmRing {
  static constexpr uint64_t kSlots = 1 << 16;
  alignas(64) std::atomic<uint64_t> head{0};  // producer publishes
  alignas(64) std::atomic<uint64_t> tail{0};  // consumer publishes
  alignas(64) std::atomic<uint64_t> done{0};
  alignas(64) uint64_t rec[kSlots][2];
};

// The producer and consumer spin, so they must be on different CPUs: pin
// them to the first two CPUs of the launch mask (without this, a run under
// a CPU set can put both on one CPU and measure scheduler time slicing).
std::vector<int> LaunchCpus() {
  cpu_set_t set;
  CPU_ZERO(&set);
  sched_getaffinity(0, sizeof(set), &set);
  std::vector<int> cpus;
  for (int c = 0; c < CPU_SETSIZE; c++) {
    if (CPU_ISSET(c, &set)) cpus.push_back(c);
  }
  return cpus;
}

void PinSelf(int cpu) {
  cpu_set_t one;
  CPU_ZERO(&one);
  CPU_SET(cpu, &one);
  sched_setaffinity(0, sizeof(one), &one);
}

double RunShm(Table &table, uint64_t total, uint32_t publish_every, bool apply,
              const std::vector<int> &cpus) {
  void *mem = mmap(nullptr, sizeof(ShmRing), PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  auto *ring = new (mem) ShmRing();
  const pid_t child = fork();
  if (child == 0) {  // producer process: no DPDK calls here
    if (cpus.size() >= 2) PinSelf(cpus[1]);
    uint64_t head = 0;
    for (uint64_t i = 0; i < total; i++) {
      while (head - ring->tail.load(std::memory_order_acquire) >= ShmRing::kSlots) {
        __builtin_ia32_pause();
      }
      auto &r = ring->rec[head % ShmRing::kSlots];
      r[0] = OpKey(i);
      r[1] = i;
      head++;
      if (head % publish_every == 0) ring->head.store(head, std::memory_order_release);
    }
    ring->head.store(head, std::memory_order_release);
    ring->done.store(1, std::memory_order_release);
    _exit(0);
  }
  if (!cpus.empty()) PinSelf(cpus[0]);
  uint64_t tail = 0, sink = 0;
  const double t0 = Now();
  while (tail < total) {
    const uint64_t head = ring->head.load(std::memory_order_acquire);
    while (tail < head) {
      const auto &r = ring->rec[tail % ShmRing::kSlots];
      if (apply) table.Apply(r[0], r[1]); else sink += r[0] ^ r[1];
      tail++;
    }
    ring->tail.store(tail, std::memory_order_release);
    if (head == tail) rte_pause();
  }
  const double dt = Now() - t0;
  waitpid(child, nullptr, 0);
  munmap(mem, sizeof(ShmRing));
  if (sink == 42) printf(" ");
  return total / dt;
}

}  // namespace

int main(int argc, char **argv) {
  const std::vector<int> cpus = LaunchCpus();  // before the EAL pins us
  double seconds = 1.0;
  bool shm_only = false;
  std::vector<uint32_t> batches = {1, 16, 256, 4096};
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--shm-only") shm_only = true;
    if (std::string(argv[i]).rfind("--benchmark_min_time", 0) == 0) {
      seconds = 0.05;
      batches = {16};
    }
  }
  bess::InitDpdk(0);

  Table table;
  Service service(table);
  const std::string uds = "unix:/tmp/bess_ingress_bench." + std::to_string(getpid());
  const std::string tcp = "127.0.0.1:0";
  int tcp_port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort(tcp, grpc::InsecureServerCredentials(), &tcp_port);
  builder.AddListeningPort(uds, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  builder.SetSyncServerOption(grpc::ServerBuilder::MAX_POLLERS, 1);
  auto server = builder.BuildAndStart();

  printf("== E4: ingress (M ops/s; decode = no table, apply = into a 1M-key LF rte_hash)\n");
  printf("%-20s %-5s %6s %10s %10s\n", "variant", "via", "batch", "decode", "apply");
  for (const std::string via : {"uds", "tcp"}) {
    if (shm_only) break;
    const std::string target = via == "uds" ? uds : "127.0.0.1:" + std::to_string(tcp_port);
    auto stub = tpb::IngressBench::NewStub(
        grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
    for (const std::string variant :
         {"grpc-unary-msgs", "grpc-unary-packed", "grpc-stream-packed"}) {
      for (uint32_t b : batches) {
        const double d = RunGrpc(*stub, variant, b, false, seconds);
        const double a = RunGrpc(*stub, variant, b, true, seconds);
        printf("%-20s %-5s %6u %10.3f %10.3f\n", variant.c_str(), via.c_str(),
               b, d / 1e6, a / 1e6);
        fflush(stdout);
      }
    }
  }
  const uint64_t total = seconds < 0.1 ? 100000 : 20000000;
  for (uint32_t every : {1u, 64u}) {
    const double d = RunShm(table, total, every, false, cpus);
    const double a = RunShm(table, total, every, true, cpus);
    printf("%-20s %-5s %6u %10.3f %10.3f\n", "shm-ring", "shm", every, d / 1e6, a / 1e6);
  }
  server->Shutdown();
  unlink(uds.substr(5).c_str());
  return 0;
}
