// SPDX-License-Identifier: BSD-3-Clause

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

  // UBSan's -fsanitize=function reads the 8 bytes before a called function
  // for its type signature; JIT code starts a fresh mapping, so the read can
  // fault on the page before it (M22, D-072). Exempt the one indirect call.
#if defined(__clang__)
  __attribute__((no_sanitize("function")))
#endif
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
