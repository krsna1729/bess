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

#include "bpf_program.h"

#include <pcap.h>
#include <rte_errno.h>
#include <rte_malloc.h>

#include <vector>

#include "../dpdk.h"

namespace bess {
namespace utils {

namespace {

// What a matching classic program returns; any non-zero value is a match.
constexpr int kSnapLen = 0xffff;

// Registers of converted programs: rte_bpf_convert() keeps classic BPF's X in
// r7 and never uses r9, which the repair below takes as scratch.
constexpr uint8_t kConvertedX = EBPF_REG_7;
constexpr uint8_t kScratch = EBPF_REG_9;

bool IsJump(uint8_t code) {
  return BPF_CLASS(code) == BPF_JMP && BPF_OP(code) != EBPF_CALL &&
         BPF_OP(code) != EBPF_EXIT;
}

bool IsSmallJsetImmediate(const struct ebpf_insn &insn) {
  return insn.code == (BPF_JMP | BPF_JSET | BPF_K) &&
         insn.imm == static_cast<int8_t>(insn.imm);
}

// Works around two bugs in DPDK's BPF library (25.11, and still on main as
// of 2026-09-26), each caught by BpfProgramTest.SameVerdictsAsLibpcap*
// (D-007); both repairs are no-ops for a program that does not need them.
//
// 1. rte_bpf_convert() picks the base register of a classic indirect load
//    ([x + k]) with BPF_SRC(code), bit 0x08, which for loads is part of the
//    size field: halfword loads get X by accident, byte and word loads get
//    the accumulator, so `tcp[13]` or `udp[8:4]` read the wrong offset. The
//    base of every classic indirect load is X.
// 2. The x86-64 JIT encodes `jset #k` as TEST r/m64, imm32 (F7 /0) but writes
//    a one-byte immediate when k fits in int8, so the CPU decodes the jump
//    that follows as immediate bytes: wrong verdicts or a crash for any flag
//    test such as `tcp[tcpflags] & tcp-syn != 0`. Such a jset becomes
//    `mov64 r9, #k; jset rA, r9` (the same sign extension), and every jump
//    offset is remapped around the inserted instructions.
std::vector<struct ebpf_insn> Repair(const struct rte_bpf_prm &prm) {
  const uint32_t n = prm.nb_ins;
  // start[i]: where old instruction i begins in the repaired program; a jump
  // to i lands on its first instruction.
  std::vector<uint32_t> start(n + 1);
  uint32_t size = 0;
  for (uint32_t i = 0; i < n; i++) {
    start[i] = size;
    size += IsSmallJsetImmediate(prm.ins[i]) ? 2 : 1;
  }
  start[n] = size;

  std::vector<struct ebpf_insn> out;
  out.reserve(size);
  for (uint32_t i = 0; i < n; i++) {
    struct ebpf_insn insn = prm.ins[i];
    if (BPF_CLASS(insn.code) == BPF_LD && BPF_MODE(insn.code) == BPF_IND) {
      insn.src_reg = kConvertedX;
    }
    if (IsSmallJsetImmediate(insn)) {
      struct ebpf_insn mov = {};
      mov.code = EBPF_ALU64 | EBPF_MOV | BPF_K;
      mov.dst_reg = kScratch;
      mov.imm = insn.imm;
      out.push_back(mov);
      insn.code = BPF_JMP | BPF_JSET | BPF_X;
      insn.src_reg = kScratch;
      insn.imm = 0;
    }
    if (IsJump(insn.code)) {
      const int64_t target = static_cast<int64_t>(i) + 1 + insn.off;
      // Out-of-range targets stay out of range, for the verifier to refuse.
      if (target >= 0 && target <= n) {
        insn.off = static_cast<int16_t>(static_cast<int64_t>(start[target]) -
                                        (static_cast<int64_t>(out.size()) + 1));
      }
    }
    out.push_back(insn);
  }
  return out;
}

}  // namespace

std::unique_ptr<BpfProgram> BpfProgram::Compile(const std::string &expression,
                                                std::string *error) {
  // rte_bpf_convert() allocates from DPDK's heap.
  if (!bess::IsDpdkInitialized()) {
    bess::InitDpdk();
  }

  pcap_t *pcap = pcap_open_dead(DLT_EN10MB, kSnapLen);
  if (pcap == nullptr) {
    *error = "failed to create a BPF compile context";
    return nullptr;
  }
  struct bpf_program classic;
  const int compiled = pcap_compile(pcap, &classic, expression.c_str(),
                                    /*optimize=*/1, PCAP_NETMASK_UNKNOWN);
  if (compiled == -1) {
    *error = std::string("BPF compilation error: ") + pcap_geterr(pcap);
    pcap_close(pcap);
    return nullptr;
  }
  pcap_close(pcap);

  struct rte_bpf_prm *prm = rte_bpf_convert(&classic);
  pcap_freecode(&classic);
  if (prm == nullptr) {
    *error = std::string("BPF conversion error: ") + rte_strerror(rte_errno);
    return nullptr;
  }
  // rte_bpf_load() copies the instructions, so `repaired` can go after it.
  const std::vector<struct ebpf_insn> repaired = Repair(*prm);
  struct rte_bpf_prm repaired_prm = *prm;
  repaired_prm.ins = repaired.data();
  repaired_prm.nb_ins = static_cast<uint32_t>(repaired.size());
  struct rte_bpf *bpf = rte_bpf_load(&repaired_prm);
  rte_free(prm);
  if (bpf == nullptr) {
    *error = std::string("BPF load error: ") + rte_strerror(rte_errno);
    return nullptr;
  }
  return std::unique_ptr<BpfProgram>(new BpfProgram(bpf));
}

BpfProgram::BpfProgram(struct rte_bpf *bpf) : bpf_(bpf), jit_(nullptr) {
  struct rte_bpf_jit jit = {};
  if (rte_bpf_get_jit(bpf_, &jit) == 0) {
    jit_ = jit.func;
  }
}

BpfProgram::~BpfProgram() { rte_bpf_destroy(bpf_); }

}  // namespace utils
}  // namespace bess
