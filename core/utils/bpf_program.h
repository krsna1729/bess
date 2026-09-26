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

#ifndef BESS_UTILS_BPF_PROGRAM_H_
#define BESS_UTILS_BPF_PROGRAM_H_

#include <rte_bpf.h>
#include <rte_mbuf.h>

#include <cstdint>
#include <memory>
#include <string>

namespace bess {
namespace utils {

// A pcap-filter(7) expression, compiled once and run on packets by DPDK's BPF
// library: libpcap compiles it to classic BPF, rte_bpf_convert() turns that
// into eBPF, rte_bpf_load() verifies it, and the program runs through DPDK's
// JIT where the architecture has one for it (x86-64), otherwise through
// DPDK's interpreter. Decision D-018 (docs/decisions.md).
//
// A program reads the packet as an mbuf chain, so filters see the whole
// packet, not only its first segment. A program is immutable and may be run
// by any number of threads at once.
class BpfProgram {
 public:
  // Returns nullptr and sets *error on a compile, conversion or verifier
  // failure.
  static std::unique_ptr<BpfProgram> Compile(const std::string &expression,
                                             std::string *error);

  ~BpfProgram();

  BpfProgram(const BpfProgram &) = delete;
  BpfProgram &operator=(const BpfProgram &) = delete;

  bool Matches(struct rte_mbuf *pkt) const {
    const uint64_t ret = jit_ != nullptr ? jit_(pkt) : rte_bpf_exec(bpf_, pkt);
    return ret != 0;
  }

  // True when the program runs as native code rather than interpreted.
  bool jitted() const { return jit_ != nullptr; }

 private:
  explicit BpfProgram(struct rte_bpf *bpf);

  struct rte_bpf *bpf_;
  uint64_t (*jit_)(void *);
};

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_BPF_PROGRAM_H_
