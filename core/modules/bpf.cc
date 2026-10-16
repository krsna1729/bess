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
#include <string>
#include <utility>

#include "bpf.h"
#include "utils/bpf.h"

/* -------------------------------------------------------------------------
 * Module code begins from here
 * ------------------------------------------------------------------------- */

/* Note: bpf_filter will return SNAPLEN if matched, and 0 if unmatched. */
/* Note: unmatched packets are sent to gate 0 */
#define SNAPLEN 0xffff

// "<name>/filters" holds the whole filter list; the class has no actions.
const Commands BPF::cmds = {};

const ResourceTables BPF::resource_tables = {
    ResourceTable::Singleton<bess::pb::BPFArg>("filters", true)};

CommandResponse BPF::Init(const bess::pb::BPFArg &arg) {
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
        Staged staged{{}, v};
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
  const int compile_ret = pcap_compile(pcap, &il, filter.exp.c_str(), 1,
                                       PCAP_NETMASK_UNKNOWN);
  pcap_close(pcap);
  if (compile_ret == -1) {
    return CommandFailure(EINVAL, "BPF compilation error");
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
  const bess::utils::Filter &filter = filters_[0];

  for (bess::PacketRef pkt : batch->packets()) {
    if (Match(filter, pkt.head_data<u_char *>(), pkt.total_len(),
              pkt.head_len())) {
      EmitPacket(ctx, pkt, filter.gate);
    } else {
      EmitPacket(ctx, pkt);
    }
  }
}

void BPF::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int n_filters = filters_.size();

  if (n_filters == 0) {
    RunNextModule(ctx, batch);
    return;
  } else if (n_filters == 1) {
    ProcessBatch1Filter(ctx, batch);
    return;
  }

  // slow version for general cases
  for (bess::PacketRef pkt : batch->packets()) {
    gate_idx_t gate = 0;  // default gate for unmatched pkts

    // high priority filters are checked first
    for (const bess::utils::Filter &filter : filters_) {
      if (Match(filter, pkt.head_data<uint8_t *>(), pkt.total_len(),
                pkt.head_len())) {
        gate = filter.gate;
        break;
      }
    }
    EmitPacket(ctx, pkt, gate);
  }
}

ADD_MODULE(BPF, "bpf", "classifies packets with pcap-filter(7) syntax")
