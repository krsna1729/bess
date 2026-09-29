// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_GATE_HOOKS_TCPDUMP_
#define BESS_GATE_HOOKS_TCPDUMP_

#include "../message.h"
#include "../module.h"

#include "../utils/fifo_opener.h"

class TcpdumpOpener final : public bess::utils::FifoOpener {
 public:
  TcpdumpOpener() : FifoOpener() {}
  bool InitFifo(int fd) override;
};

// Tcpdump dumps copies of the packets seen by a gate. Useful for debugging.
class Tcpdump final : public bess::GateHook {
 public:
  Tcpdump()
      : bess::GateHook(Tcpdump::kName, "tcpdump", Tcpdump::kPriority),
        opener_() {}

  virtual ~Tcpdump() {}

  CommandResponse Init(const bess::Gate *, const bess::pb::TcpdumpArg &);

  void ProcessBatch(const bess::PacketBatch *batch);

  static constexpr uint16_t kPriority = 1;
  static const std::string kName;

 private:
  TcpdumpOpener opener_;
};

#endif  // BESS_GATE_HOOKS_TCPDUMP_
