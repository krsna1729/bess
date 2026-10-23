/* x86_64 BPF JIT code was adopted from FreeBSD 10 - Sangjin */

/*-
 * Copyright (C) 2002-2003 NetGroup, Politecnico di Torino (Italy)
 * Copyright (C) 2005-2009 Jung-uk Kim <jkim@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Politecnico di Torino nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/mman.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <expected>
#include <format>
#include <string>
#include <utility>

#include "bpf.h"
#include "utils/bpf.h"
#include "utils/endian.h"

/* -------------------------------------------------------------------------
 * Module code begins from here
 * ------------------------------------------------------------------------- */

/* Note: bpf_filter will return SNAPLEN if matched, and 0 if unmatched. */
/* Note: unmatched packets are sent to gate 0 */
#define SNAPLEN 0xffff

// "<name>/filters" holds the whole filter list; the class has no actions.
const Commands BPF::cmds = {};

const ResourceTables BPF::resource_tables = {
    ResourceTable::Singleton<bess::pb::BPFArg>("filters", true).Rebuilds()};

CommandResponse BPF::Init(const bess::pb::BPFArg &arg) {
  if (arg.has_default_gate()) {
    if (arg.default_gate() >= MAX_GATES) {
      return CommandFailure(EINVAL, "default_gate %lu is invalid; must be < %u",
                            arg.default_gate(), MAX_GATES);
    }
    default_gate_ = static_cast<gate_idx_t>(arg.default_gate());
  } else {
    default_gate_ = 0;
  }
  // The init argument's filters, compiled and sorted as the resource's are:
  // a filter that does not compile refuses the creation. OwnedFilters frees
  // the list it replaced.
  OwnedFilters compiled;
  compiled.filters.reserve(arg.filters_size());
  for (const auto &f : arg.filters()) {
    bess::utils::Filter filter;
    CommandResponse ret = Compile(f, &filter);
    if (ret.has_error()) {
      return ret;
    }
    compiled.filters.push_back(std::move(filter));
  }
  // Descending priority; equal priorities keep the order given.
  std::stable_sort(
      compiled.filters.begin(), compiled.filters.end(),
      [](const bess::utils::Filter &a, const bess::utils::Filter &b) {
        return b.priority < a.priority;
      });
  filters_.swap(compiled.filters);
  arg_ = arg;

  // Stage compiles the whole list (a filter that does not compile refuses
  // the transaction) and sorts it; apply swaps it in. The list it replaced
  // is freed with the staged value. ProcessBatch reads filters_ in place:
  // paused workers only.
  auto registered = filters_resource_.Register(
      init_context().resources(), init_context().codecs(), name() + "/filters",
      [](const bess::pb::BPFArg &v) -> std::expected<Staged, std::string> {
        Staged staged{{}, v, 0};
        if (v.has_default_gate()) {
          if (v.default_gate() >= MAX_GATES) {
            return std::unexpected(
                std::format("default_gate {} is invalid; must be < {}",
                            v.default_gate(), MAX_GATES));
          }
          staged.default_gate = static_cast<gate_idx_t>(v.default_gate());
        }
        std::vector<bess::utils::Filter> &filters = staged.compiled.filters;
        filters.reserve(v.filters_size());
        for (const auto &f : v.filters()) {
          bess::utils::Filter filter;
          CommandResponse r = Compile(f, &filter);
          if (r.has_error()) {
            return std::unexpected(r.error().errmsg());
          }
          filters.push_back(std::move(filter));
        }
        // Descending priority; equal priorities keep the order given.
        std::stable_sort(
            filters.begin(), filters.end(),
            [](const bess::utils::Filter &a, const bess::utils::Filter &b) {
              return b.priority < a.priority;
            });
        return staged;
      },
      [this](Staged &s) {
        filters_.swap(s.compiled.filters);
        arg_.Swap(&s.arg);
        default_gate_ = s.default_gate;
      },
      [this] { return arg_; }, [this] { return HasRunningWorker(); });
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void BPF::DeInit() {
  filters_resource_.Release();
  FreeFilters(&filters_);
}

void BPF::FreeFilters(std::vector<bess::utils::Filter> *filters) {
  for (auto &filter : *filters) {
#ifdef __x86_64
    munmap(reinterpret_cast<void *>(filter.func), filter.mmap_size);
#else
    pcap_freecode(&filter.il_code);
#endif
  }

  filters->clear();
}

CommandResponse BPF::Compile(const bess::pb::BPFArg::Filter &f,
                             bess::utils::Filter *out) {
  if (f.gate() < 0 || f.gate() >= MAX_GATES) {
    return CommandFailure(EINVAL, "Invalid gate");
  }

  bess::utils::Filter filter;
  filter.priority = f.priority();
  filter.gate = f.gate();
  filter.exp = f.filter();

  struct bpf_program il;
  pcap_t *pcap = pcap_open_dead(DLT_EN10MB, SNAPLEN);
  if (!pcap) {
    return CommandFailure(ENOMEM, "failed to create BPF compile context");
  }
  const int compile_ret =
      pcap_compile(pcap, &il, filter.exp.c_str(), 1, PCAP_NETMASK_UNKNOWN);
  pcap_close(pcap);
  if (compile_ret == -1) {
    return CommandFailure(EINVAL, "BPF compilation error");
  }

  // Detect single comparison fast paths (16-bit or 32-bit equality):
  if (il.bf_len == 4 && il.bf_insns[0].code == (BPF_LD | BPF_H | BPF_ABS) &&
      il.bf_insns[1].code == (BPF_JMP | BPF_JEQ | BPF_K) &&
      il.bf_insns[2].code == (BPF_RET | BPF_K) && il.bf_insns[2].k != 0 &&
      il.bf_insns[3].code == (BPF_RET | BPF_K) && il.bf_insns[3].k == 0) {
    filter.fast_kind = bess::utils::Filter::FastKind::k16Bit;
    filter.fast_offset = il.bf_insns[0].k;
    filter.fast_val = il.bf_insns[1].k;
  } else if (il.bf_len == 4 &&
             il.bf_insns[0].code == (BPF_LD | BPF_W | BPF_ABS) &&
             il.bf_insns[1].code == (BPF_JMP | BPF_JEQ | BPF_K) &&
             il.bf_insns[2].code == (BPF_RET | BPF_K) &&
             il.bf_insns[2].k != 0 &&
             il.bf_insns[3].code == (BPF_RET | BPF_K) &&
             il.bf_insns[3].k == 0) {
    filter.fast_kind = bess::utils::Filter::FastKind::k32Bit;
    filter.fast_offset = il.bf_insns[0].k;
    filter.fast_val = il.bf_insns[1].k;
  }

#ifdef __x86_64
  filter.func =
      bess::utils::bpf_jit_compile(il.bf_insns, il.bf_len, &filter.mmap_size);
  pcap_freecode(&il);
  if (!filter.func) {
    return CommandFailure(ENOMEM, "BPF JIT compilation error");
  }
#else
  filter.il_code = il;
#endif
  *out = std::move(filter);
  return CommandSuccess();
}

// UBSan's -fsanitize=function reads the 8 bytes before a called function for
// its type signature; JIT code starts a fresh mapping, so the read faults on
// the page before it (as in utils/bpf_program.h). Exempt the one indirect call.
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
inline bool BPF::Match(const bess::utils::Filter &filter, u_char *pkt,
                       u_int wirelen, u_int buflen) {
#ifdef __x86_64
  int ret = filter.func(pkt, wirelen, buflen);
#else
  int ret = bpf_filter(filter.il_code.bf_insns, pkt, wirelen, buflen);
#endif

  return ret != 0;
}

void BPF::ProcessBatch1Filter(Context *ctx, bess::PacketBatch *batch) {
  const size_t cnt = batch->cnt();
  if (cnt == 0)
    return;

  gate_idx_t ogates[bess::PacketBatch::kMaxBurst];
  const auto &filter = filters_[0];
  const gate_idx_t match_gate = static_cast<gate_idx_t>(filter.gate);

  if (filter.fast_kind == bess::utils::Filter::FastKind::k16Bit) {
    const size_t off = filter.fast_offset;
    const uint16_t want_be = bess::utils::be16_t(filter.fast_val).raw_value();

    for (size_t i = 0; i < cnt; i++) {
      rte_prefetch0(batch->packet(i).head_data());
    }

#pragma GCC unroll 4
    for (size_t i = 0; i < cnt; i++) {
      bess::PacketRef pkt = batch->packet(i);
      if (pkt.head_len() < off + 2) [[unlikely]] {
        ogates[i] = default_gate_;
        continue;
      }
      uint16_t val = 0;
      std::memcpy(&val, pkt.head_data<const uint8_t *>() + off, sizeof(val));
      ogates[i] = (val == want_be) ? match_gate : default_gate_;
    }
    EmitBatch(ctx, batch, ogates);
    return;
  }

  if (filter.fast_kind == bess::utils::Filter::FastKind::k32Bit) {
    const size_t off = filter.fast_offset;
    const uint32_t want_be = bess::utils::be32_t(filter.fast_val).raw_value();

    for (size_t i = 0; i < cnt; i++) {
      rte_prefetch0(batch->packet(i).head_data());
    }

#pragma GCC unroll 4
    for (size_t i = 0; i < cnt; i++) {
      bess::PacketRef pkt = batch->packet(i);
      if (pkt.head_len() < off + 4) [[unlikely]] {
        ogates[i] = default_gate_;
        continue;
      }
      uint32_t val = 0;
      std::memcpy(&val, pkt.head_data<const uint8_t *>() + off, sizeof(val));
      ogates[i] = (val == want_be) ? match_gate : default_gate_;
    }
    EmitBatch(ctx, batch, ogates);
    return;
  }

  for (size_t i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    ogates[i] = Match(filter, pkt.head_data<u_char *>(), pkt.total_len(),
                      pkt.head_len())
                    ? match_gate
                    : default_gate_;
  }
  EmitBatch(ctx, batch, ogates);
}

void BPF::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const size_t cnt = batch->cnt();
  if (cnt == 0)
    return;

  const size_t n_filters = filters_.size();
  if (n_filters == 0) {
    if (default_gate_ == 0) {
      RunNextModule(ctx, batch);
    } else {
      gate_idx_t ogates[bess::PacketBatch::kMaxBurst];
      std::fill_n(ogates, cnt, default_gate_);
      EmitBatch(ctx, batch, ogates);
    }
    return;
  }
  if (n_filters == 1) {
    ProcessBatch1Filter(ctx, batch);
    return;
  }

  gate_idx_t ogates[bess::PacketBatch::kMaxBurst];
  for (size_t i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    gate_idx_t gate = default_gate_;
    for (const auto &filter : filters_) {
      if (Match(filter, pkt.head_data<uint8_t *>(), pkt.total_len(),
                pkt.head_len())) {
        gate = filter.gate;
        break;
      }
    }
    ogates[i] = gate;
  }
  EmitBatch(ctx, batch, ogates);
}

ADD_MODULE(BPF, "bpf", "classifies packets with pcap-filter(7) syntax")
