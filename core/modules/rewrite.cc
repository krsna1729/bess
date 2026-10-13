// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "rewrite.h"

#include <cstdio>
#include <cstring>
#include <expected>
#include <string>
#include <utility>

#include "utils/copy.h"

// D-129: "<name>/templates" holds the template list; add and clear stay for
// the legacy ModuleCommand only.
const Commands Rewrite::cmds = {
    {"add", "RewriteArg", MODULE_CMD_FUNC(&Rewrite::CommandAdd),
     Command::THREAD_UNSAFE, "", Command::LEGACY_ONLY},
    {"clear", "EmptyArg", MODULE_CMD_FUNC(&Rewrite::CommandClear),
     Command::THREAD_UNSAFE, "", Command::LEGACY_ONLY},
};

const ResourceTables Rewrite::resource_tables = {
    ResourceTable::Singleton<bess::pb::RewriteArg>("templates", true)};

CommandResponse Rewrite::Init(const bess::pb::RewriteArg &arg) {
  CommandResponse ret = CommandAdd(arg);
  if (ret.has_error()) {
    return ret;
  }

  // Stage builds the whole template table; apply swaps it in and restarts
  // the round robin. The table it replaced is freed with the staged value.
  // ProcessBatch reads templates_ in place: paused workers only.
  auto registered = templates_resource_.Register(
      init_context().resources(), init_context().codecs(),
      name() + "/templates",
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
      [this](Staged &s) {
        templates_.swap(s.templates);
        next_turn_ = 0;
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
  for (size_t i = 0; i < num; i++) {
    const auto &templ = arg.templates(static_cast<int>(i));

    if (templ.length() > kMaxTemplateSize) {
      return CommandFailure(EINVAL, "template is too big");
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

CommandResponse Rewrite::CommandAdd(const bess::pb::RewriteArg &arg) {
  size_t curr = templates_->num;

  if (curr + arg.templates_size() > bess::PacketBatch::kMaxBurst) {
    return CommandFailure(EINVAL, "max %zu packet templates can be used %zu %d",
                          bess::PacketBatch::kMaxBurst, curr,
                          arg.templates_size());
  }

  // The list so far plus `arg`'s, built afresh (all or nothing).
  bess::pb::RewriteArg all = arg_;
  all.mutable_templates()->MergeFrom(arg.templates());
  auto next = std::make_unique<Templates>();
  CommandResponse ret = Build(all, next.get());
  if (ret.has_error()) {
    return ret;
  }
  templates_ = std::move(next);
  arg_ = std::move(all);
  return CommandSuccess();
}

CommandResponse Rewrite::CommandClear(const bess::pb::EmptyArg &) {
  next_turn_ = 0;
  templates_->num = 0;
  arg_.Clear();
  return CommandSuccess();
}

inline void ResetPacket(bess::PacketRef pkt) {
  // Resetting a chained mbuf would detach its tail without freeing it.
  if (pkt.nb_segs() > 1) {
    bess::PacketRef tail = pkt.next();
    pkt.set_next(bess::PacketRef());
    pkt.set_nb_segs(1);
    bess::PacketFree(tail.handle());
  }
  pkt.reset();
}

inline void Rewrite::DoRewriteSingle(const Templates &t,
                                     bess::PacketBatch *batch) {
  uint16_t size = t.size[0];
  const void *templ = t.data[0];

  for (bess::PacketRef pkt : batch->packets()) {
    ResetPacket(pkt);
    void *ptr = pkt.append(size);
    bess::utils::CopyInlined(ptr, templ, size, true);
  }
}

inline void Rewrite::DoRewrite(const Templates &t, bess::PacketBatch *batch) {
  size_t start = next_turn_;
  const size_t cnt = batch->cnt();

  for (size_t i = 0; i < cnt; i++) {
    uint16_t size = t.size[start + i];
    bess::PacketRef pkt = batch->packet(i);
    ResetPacket(pkt);
    void *ptr = pkt.append(size);
    bess::utils::CopyInlined(ptr, t.data[start + i], size, true);
  }

  next_turn_ = t.jump[start + cnt];
}

void Rewrite::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  const Templates &t = *templates_;
  if (t.num == 1) {
    DoRewriteSingle(t, batch);
  } else if (t.num > 1) {
    DoRewrite(t, batch);
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(Rewrite, "rewrite", "replaces entire packet data")
