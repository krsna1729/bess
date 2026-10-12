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
// input gate, and runs that task once: the module's ProcessBatch, its
// output-gate processing and the deadend are the dataplane's own. Packets
// the module drops are not captured; gates not asked for are left
// unconnected (their packets go to the deadend).

#ifndef BESS_TESTING_MODULE_HARNESS_H_
#define BESS_TESTING_MODULE_HARNESS_H_

#include <google/protobuf/message.h>

#include <map>
#include <string>
#include <vector>

#include "message.h"
#include "module.h"
#include "testing/packet_builder.h"

namespace bess::testing {

// Packet pools (malloc-backed: no hugepages, no root), the port drivers, and
// this thread as a non-worker. Once per process; ModuleHarness calls it.
void InitModuleTests();

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

  // Runs `in` into `m`'s input gate `igate` and returns what reached each
  // gate of `ogates` (by default 0-15 as far as `m` has them).
  std::map<gate_idx_t, std::vector<Bytes>> Run(Module *m, gate_idx_t igate,
                                               const std::vector<Bytes> &in,
                                               std::vector<gate_idx_t> ogates = {});

  // A command, as RunModuleCommand runs it.
  CommandResponse Command(Module *m, const std::string &command,
                          const google::protobuf::Message &arg);
  // Fails the test unless the command succeeds; returns its answer.
  google::protobuf::Any MustCommand(Module *m, const std::string &command,
                                    const google::protobuf::Message &arg);

 private:
  Module *capture_ = nullptr;
  Module *injector_ = nullptr;
  int next_name_ = 0;
};

}  // namespace bess::testing

#endif  // BESS_TESTING_MODULE_HARNESS_H_
