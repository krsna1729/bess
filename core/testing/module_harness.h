// SPDX-License-Identifier: BSD-3-Clause

// In-process module tests: a module created as bessd creates it, driven by a
// real Task run, without a daemon, a worker or Python.
//
//   ModuleHarness h;
//   Module *em = h.Create("ExactMatch", arg);
//   auto out = h.Run(em, 0, {pkt1, pkt2});   // output gate -> packets
//   EXPECT_EQ(out[1], (std::vector<Bytes>{pkt1}));
//
// Run() connects each output gate asked for to a capture module (no default
// hooks), queues the packets on an injector task that feeds the module's
// input gate, and runs that task once per burst of PacketBatch::kMaxBurst
// packets (in order): the module's ProcessBatch, its
// output-gate processing and the deadend are the dataplane's own. Packets
// the module drops are not captured; gates not asked for are left
// unconnected (their packets go to the deadend).

#ifndef BESS_TESTING_MODULE_HARNESS_H_
#define BESS_TESTING_MODULE_HARNESS_H_

#include <google/protobuf/message.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "dataplane/resource.h"
#include "framework/resource_codec.h"
#include "message.h"
#include "module.h"
#include "packet.h"
#include "testing/packet_builder.h"

namespace bess::testing {

// Packet pools (malloc-backed: no hugepages, no root), the port drivers, and
// this thread as a non-worker. Once per process; ModuleHarness calls it.
void InitModuleTests();

// `bytes` as a packet from the harness's pool: one segment, or, with `split`
// in (0, bytes.size()), a two-segment chain whose first segment ends there.
// nullptr (and a test failure) when it cannot be built. After a ModuleHarness
// exists (the pool is its).
bess::PacketHandle MakePacket(const Bytes &bytes, size_t split = 0);

// `bytes` as a chain of segments of `segment` bytes each (the last one
// shorter), from the harness's pool: a packet longer than one mbuf holds,
// such as one past the 65,535 bytes an IPv4 or UDP length counts. nullptr
// (and a test failure) when it cannot be built.
bess::PacketHandle MakeChain(const Bytes &bytes, size_t segment);

// `bytes` as a one-segment packet with no headroom: its data starts at its
// buffer's start, so a prepend has nowhere to go. nullptr (and a test
// failure) when it cannot be built.
bess::PacketHandle MakePacketWithoutHeadroom(const Bytes &bytes);

// The `n` bytes of `packet`'s headroom right before its data (where a prepend
// writes), and a fill of them: a test that shares the payload checks that
// the other holder's headroom stays.
Bytes HeadroomBytes(bess::PacketHandle packet, size_t n);
void FillHeadroom(bess::PacketHandle packet, size_t n, uint8_t value);

// `bytes` as a packet whose first segment holds only the first `len` bytes
// (0 < len <= bytes.size(), below it with `chain`). The rest stays in that
// segment's buffer past its data and, with `chain`, also follows in a second
// segment. Code that reads past the first segment's data then reads the
// frame's own bytes, not garbage: a test that a short packet is dropped fails
// on such code every time.
bess::PacketHandle MakeShortPacket(const Bytes &bytes, size_t len, bool chain = false);

// A packet's bytes, across its segments.
Bytes PacketBytes(bess::PacketHandle packet);

class ModuleHarness {
 public:
  ModuleHarness();
  ~ModuleHarness();  // destroys every module (the test's and the harness's)

  ModuleHarness(const ModuleHarness &) = delete;
  ModuleHarness &operator=(const ModuleHarness &) = delete;

  // A module of class `mclass` initialized with `arg`; the name is generated
  // when empty. Fails the test (and returns nullptr) when Init refuses.
  Module *Create(const std::string &mclass, const google::protobuf::Message &arg,
                 const std::string &name = "");
  // The refusal of Init(`arg`) ("" when it accepted; the module is then
  // destroyed again).
  std::string CreateError(const std::string &mclass, const google::protobuf::Message &arg);
  // The same refusal with its errno (code 0 when Init accepted).
  pb_error_t CreateFailure(const std::string &mclass, const google::protobuf::Message &arg);

  // Selects the Context worker id for subsequent runs (default 0). Runs stay
  // on the harness thread; this does not start a scheduler worker.
  void SetWorker(dataplane::WorkerId worker) { worker_ = worker; }

  // Runs `in` into `m`'s input gate `igate` and returns what reached each
  // gate of `ogates` (by default 0-15 as far as `m` has them).
  std::map<gate_idx_t, std::vector<Bytes>> Run(Module *m, gate_idx_t igate,
                                               const std::vector<Bytes> &in,
                                               std::vector<gate_idx_t> ogates = {});
  // Runs `m`'s first task (a task module: DRR, Queue, ...) once and returns
  // what reached each gate of `ogates` (by default 0-15 as far as `m` has
  // them): what the module sends of the packets fed to it before.
  std::map<gate_idx_t, std::vector<Bytes>> RunTask(Module *m,
                                                   std::vector<gate_idx_t> ogates = {});
  // Runs `first`'s first task once through the modules the test connected and
  // returns what reached each of `last`'s `ogates`, with metadata offsets
  // computed as in RunPipeline: what a source module writes, as a reader sees it.
  std::map<gate_idx_t, std::vector<Bytes>> RunTaskPipeline(Module *first, Module *last,
                                                           std::vector<gate_idx_t> ogates = {});

  // Runs `in` into `first`'s input gate `igate` through the modules the test
  // connected (ModuleGraph::ConnectModules) and returns what reached each of
  // `last`'s `ogates` (by default 0-15 as far as `last` has them). Metadata
  // offsets are computed for the graph first, as a resume does, so attributes
  // flow from writer to reader.
  std::map<gate_idx_t, std::vector<Bytes>> RunPipeline(Module *first, gate_idx_t igate,
                                                       const std::vector<Bytes> &in, Module *last,
                                                       std::vector<gate_idx_t> ogates = {});
  // RunPipeline with packets the test built itself (a chain, a clone): the
  // harness takes ownership of each of `in`. A test that keeps another
  // reference (a clone's original) frees that one itself.
  std::map<gate_idx_t, std::vector<Bytes>> RunPackets(Module *first, gate_idx_t igate,
                                                      const std::vector<bess::PacketHandle> &in,
                                                      Module *last,
                                                      std::vector<gate_idx_t> ogates = {});

  // An action, as control_v2's InvokeAction runs it (D-140: the class's
  // commands are its actions; an unknown one is ENOTSUP).
  CommandResponse Action(Module *m, const std::string &action,
                         const google::protobuf::Message &arg);

  // Resource operations as a client sends them (D-129): decoded through the
  // resource's codec and applied by the process's transaction engine, each
  // call one transaction. "" when it applied, else why not (a codec's or a
  // resource's refusal, "busy: ..." while a worker runs a module whose
  // resource requires paused workers).
  std::string SetResource(const std::string &resource, const google::protobuf::Message &value);
  std::string UpsertResource(const std::string &resource, const google::protobuf::Message &key,
                             const google::protobuf::Message &value);
  std::string EraseResource(const std::string &resource, const google::protobuf::Message &key);
  // The resource's contents as its codec reads them back (desired state's
  // GetPipeline): (key, value) packed; a singleton's key is a
  // google.protobuf.Empty. Fails the test for an unknown resource.
  std::vector<std::pair<google::protobuf::Any, google::protobuf::Any>> ReadResource(
      const std::string &resource);
  // "" when the resource holds exactly `entries` as desired state compares
  // them (keys by the codec's decoding, values by deterministic bytes), so a
  // manifest listing them applies as a no-op (D-129's round trip); else the
  // first difference. A singleton's one entry has a null key.
  std::string ReadBackDiffers(
      const std::string &resource,
      const std::vector<std::pair<const google::protobuf::Message *,
                                  const google::protobuf::Message *>> &entries);
  // The resource's codec, as a client's writes are decoded through it
  // (nullptr for an unknown resource): for a test that builds several
  // operations itself.
  const bess::framework::ResourceCodec *Codec(const std::string &resource);
  // Several operations in one transaction, as a client sends them: "" when it
  // applied, else the refusal (all-or-nothing, so a refusal applies none).
  std::string Apply(const std::vector<bess::dataplane::Op> &ops);

 private:
  // Connects exactly `m`'s gates `ogates` to the capture (all of 0-15 that
  // `m` has when empty) and sizes `task`'s per-gate batches.
  void WireOutputs(Module *m, std::vector<gate_idx_t> ogates, Task &task);
  // Whether RunQueued can run `first`..`last` (both exist, and so do the
  // harness's own injector and capture).
  bool CanRun(const Module *first, const Module *last) const;
  // Runs `count` packets through `first`..`last` one burst at a time;
  // `packet(i)` builds the i-th when its burst is queued.
  std::map<gate_idx_t, std::vector<Bytes>> RunQueued(
      Module *first, gate_idx_t igate, Module *last, std::vector<gate_idx_t> ogates, size_t count,
      const std::function<bess::PacketHandle(size_t)> &packet);
  // Runs `task` once.
  void RunOnce(Task &task);
  Module *capture_ = nullptr;
  Module *injector_ = nullptr;
  int next_name_ = 0;
  dataplane::WorkerId worker_{0};
};

}  // namespace bess::testing

#endif  // BESS_TESTING_MODULE_HARNESS_H_
