// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "rewrite.h"

#include <cstdio>
#include <cstring>
#include <expected>
#include <string>
#include <utility>

#include "packet_mutation.h"
#include "packet_pool.h"
#include "packet_reshape.h"
#include "utils/copy.h"

// "<name>/templates" holds the whole template list; the class has no
// actions.
const Commands Rewrite::cmds = {};

const ResourceTables Rewrite::resource_tables = {
    ResourceTable::Singleton<bess::pb::RewriteArg>("templates", true).Rebuilds()};

CommandResponse Rewrite::Init(const bess::pb::RewriteArg &arg) {
  // The init argument's templates; a refused one refuses the creation.
  auto next = std::make_unique<Templates>();
  CommandResponse ret = Build(arg, next.get());
  if (ret.has_error()) {
    return ret;
  }
  templates_ = std::move(next);
  arg_ = arg;

  // Stage builds the whole template table; apply swaps it in and restarts
  // the round robin. The table it replaced is freed with the staged value.
  // ProcessBatch reads templates_ in place: paused workers only.
  auto registered = templates_resource_.Register(
      init_context().resources(), init_context().codecs(),
      name() + "/templates",
      // clang-tidy 19's analyzer loses the unique_ptr moved into the returned
      // std::expected and reports a leak at the closing brace.
      // NOLINTBEGIN(clang-analyzer-cplusplus.NewDeleteLeaks)
      [](const bess::pb::RewriteArg &v) -> std::expected<Staged, std::string> {
        if (static_cast<size_t>(v.templates_size()) >
            bess::PacketBatch::kMaxBurst) {
          return std::unexpected(
              "max " + std::to_string(bess::PacketBatch::kMaxBurst) +
              " packet templates can be used");
        }
        Staged staged{std::make_unique<Templates>(), v};
        CommandResponse r = Build(v, staged.templates.get());
        if (r.has_error()) {
          return std::unexpected(r.error().errmsg());
        }
        return staged;
      },
      // NOLINTEND(clang-analyzer-cplusplus.NewDeleteLeaks)
      [this](Staged &s) {
        templates_.swap(s.templates);
        for (const auto worker : bess::dataplane::AllWorkerIds()) {
          next_turn_[worker] = 0;
        }
        arg_.Swap(&s.arg);
      },
      [this] { return arg_; }, [this] { return HasRunningWorker(); });
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }
  return CommandSuccess();
}

void Rewrite::DeInit() {
  templates_resource_.Release();
}

CommandResponse Rewrite::Build(const bess::pb::RewriteArg &arg,
                               Templates *out) {
  const size_t num = arg.templates_size();
  // The payload room of the default pool's packets: what a reset packet can
  // hold (DPDK's reset leaves the default headroom in front of it).
  const size_t data_room = bess::PacketPool::DefaultDataRoomSize();
  for (size_t i = 0; i < num; i++) {
    const auto &templ = arg.templates(static_cast<int>(i));

    if (templ.length() > kMaxTemplateSize) {
      return CommandFailure(EINVAL, "template is too big");
    }
    if (templ.length() > data_room) {
      return CommandFailure(EINVAL, "template is larger than the %zu-byte packet data room",
                            data_room);
    }

    bess::utils::Copy(out->data[i], templ.c_str(), templ.length());
    out->size[i] = templ.length();
  }

  out->num = num;
  if (num == 0) {
    return CommandSuccess();
  }

  for (size_t i = num; i < kNumSlots; i++) {
    size_t j = i % num;
    bess::utils::Copy(out->data[i], out->data[j], out->size[j]);
    out->size[i] = out->size[j];
  }
  for (size_t i = 0; i <= kNumSlots; i++) {
    out->jump[i] = i % num;
  }

  return CommandSuccess();
}

namespace {

// Whether `pkt` can be reset and written in place: DPDK's reset is for a
// direct mbuf (on an indirect one it clears the attachment flag and leaves
// the parent's buffer under it), and the payload must be this packet's
// alone (D-153). A direct mbuf held once is PayloadWriteabilityOf's own fast
// case, checked with one branch.
inline bool Ready(bess::PacketRef pkt) {
  const ::rte_mbuf *m = pkt.handle();
  return static_cast<bool>(
      static_cast<unsigned>(RTE_MBUF_DIRECT(m)) &  // NOLINT(bugprone-signed-bitwise)
      static_cast<unsigned>(rte_mbuf_refcnt_read(m) == 1));
}

// Frees a chain's tail: the template replaces every byte, and DPDK's reset
// takes one segment (resetting a chained mbuf would detach its tail without
// freeing it).
inline void DropTail(bess::PacketRef pkt) {
  if (pkt.nb_segs() > 1) {
    bess::PacketRef tail = pkt.next();
    pkt.set_next(bess::PacketRef());
    pkt.set_nb_segs(1);
    pkt.set_total_len(pkt.head_len());
    bess::PacketFree(tail.handle());
  }
}

inline void ResetPacket(bess::PacketRef pkt) {
  DropTail(pkt);
  pkt.reset();
}

}  // namespace

// A packet whose mbuf cannot be reset and written in place first gets a
// buffer of its own. Its bytes are about to be replaced, so none are copied
// where that can be avoided: a clone (an indirect mbuf), or an unpinned
// external buffer someone else also holds, is detached, which gives the mbuf
// back its own buffer and releases its reference to the other one. A payload
// still shared after that (a clone's parent, a shared pinned external buffer)
// is copied (EnsureWritable, D-153), which may replace the mbuf. A packet
// that still cannot be written is dropped (nullptr). A writable external
// buffer is written in place: DPDK's reset keeps it attached.
bess::PacketHandle Rewrite::MakeWritable(Context *ctx, bess::PacketRef pkt) {
  using bess::packet::PayloadWriteability;
  using bess::packet::PayloadWriteabilityOf;
  DropTail(pkt);
  bess::PacketHandle handle = pkt.handle();
  if (RTE_MBUF_CLONED(handle) ||  // NOLINT(bugprone-signed-bitwise)
      (RTE_MBUF_HAS_EXTBUF(handle) &&  // NOLINT(bugprone-signed-bitwise)
       PayloadWriteabilityOf(pkt) != PayloadWriteability::kWritable)) {
    rte_pktmbuf_detach(handle);  // a pinned external buffer stays attached
    handle->pkt_len = handle->data_len;
  }
  if (PayloadWriteabilityOf(bess::PacketRef(handle)) != PayloadWriteability::kWritable &&
      !bess::packet::EnsureWritable(handle)) {
    EmitPacket(ctx, bess::PacketRef(handle), DROP_GATE);
    return nullptr;
  }
  // An empty shared payload is not copied (there is nothing to copy); it is
  // still not writable.
  if (RTE_MBUF_CLONED(handle) ||  // NOLINT(bugprone-signed-bitwise)
      PayloadWriteabilityOf(bess::PacketRef(handle)) != PayloadWriteability::kWritable) {
    EmitPacket(ctx, bess::PacketRef(handle), DROP_GATE);
    return nullptr;
  }
  return handle;
}

// Rewrites `pkt` with the template and keeps it in the batch at `out_cnt`.
// A packet that cannot get a buffer of its own, or whose buffer cannot hold
// the template (one from a pool with a smaller data room than the default
// pool's, which Build checked against), is dropped. The copy is exact:
// nothing past the template is written.
inline void Rewrite::RewriteOne(Context *ctx, bess::PacketBatch *batch, int &out_cnt,
                                bess::PacketRef pkt, const void *templ, uint16_t size) {
  if (unlikely(!Ready(pkt))) {
    pkt = bess::PacketRef(MakeWritable(ctx, pkt));
    if (pkt.handle() == nullptr) {
      return;
    }
  }
  ResetPacket(pkt);
  void *ptr = pkt.append(size);
  if (unlikely(ptr == nullptr)) {
    EmitPacket(ctx, pkt, DROP_GATE);
    return;
  }
  bess::utils::CopyInlined(ptr, templ, size);
  batch->handles()[out_cnt++] = pkt.handle();
}

inline void Rewrite::DoRewriteSingle(Context *ctx, const Templates &t,
                                     bess::PacketBatch *batch) {
  uint16_t size = t.size[0];
  const void *templ = t.data[0];

  int out_cnt = 0;
  for (bess::PacketRef pkt : batch->packets()) {
    RewriteOne(ctx, batch, out_cnt, pkt, templ, size);
  }
  batch->set_cnt(out_cnt);
}

inline void Rewrite::DoRewrite(Context *ctx, const Templates &t, bess::PacketBatch *batch,
                                size_t &next_turn) {
  size_t start = next_turn;
  const size_t cnt = batch->cnt();

  int out_cnt = 0;
  for (size_t i = 0; i < cnt; i++) {
    RewriteOne(ctx, batch, out_cnt, batch->packet(i), t.data[start + i], t.size[start + i]);
  }
  batch->set_cnt(out_cnt);

  next_turn = t.jump[start + cnt];
}

void Rewrite::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const Templates &t = *templates_;
  if (t.num == 1) {
    DoRewriteSingle(ctx, t, batch);
  } else if (t.num > 1) {
    DoRewrite(ctx, t, batch, next_turn_[ctx->wid]);
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(Rewrite, "rewrite", "replaces entire packet data")
