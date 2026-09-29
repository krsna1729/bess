// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "rewrite.h"

#include <cstdio>

#include "../utils/copy.h"

const Commands Rewrite::cmds = {
    {"add", "RewriteArg", MODULE_CMD_FUNC(&Rewrite::CommandAdd),
     Command::THREAD_UNSAFE},
    {"clear", "EmptyArg", MODULE_CMD_FUNC(&Rewrite::CommandClear),
     Command::THREAD_UNSAFE},
};

CommandResponse Rewrite::Init(const bess::pb::RewriteArg &arg) {
  return CommandAdd(arg);
}

CommandResponse Rewrite::CommandAdd(const bess::pb::RewriteArg &arg) {
  size_t curr = num_templates_;

  if (curr + arg.templates_size() > bess::PacketBatch::kMaxBurst) {
    return CommandFailure(EINVAL, "max %zu packet templates can be used %zu %d",
                          bess::PacketBatch::kMaxBurst, curr,
                          arg.templates_size());
  }

  for (int i = 0; i < arg.templates_size(); i++) {
    const auto &templ = arg.templates(i);

    if (templ.length() > kMaxTemplateSize) {
      return CommandFailure(EINVAL, "template is too big");
    }

    memset(templates_[curr + i], 0, kMaxTemplateSize);
    bess::utils::Copy(templates_[curr + i], templ.c_str(), templ.length());
    template_size_[curr + i] = templ.length();
  }

  num_templates_ = curr + arg.templates_size();
  if (num_templates_ == 0) {
    return CommandSuccess();
  }

  for (size_t i = num_templates_; i < kNumSlots; i++) {
    size_t j = i % num_templates_;
    bess::utils::Copy(templates_[i], templates_[j], template_size_[j]);
    template_size_[i] = template_size_[j];
    jump_[i] = j;
  }

  return CommandSuccess();
}

CommandResponse Rewrite::CommandClear(const bess::pb::EmptyArg &) {
  next_turn_ = 0;
  num_templates_ = 0;
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

inline void Rewrite::DoRewriteSingle(bess::PacketBatch *batch) {
  const int cnt = batch->cnt();
  uint16_t size = template_size_[0];
  const void *templ = templates_[0];

  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    ResetPacket(pkt);
    void *ptr = pkt.append(size);
    bess::utils::CopyInlined(ptr, templ, size, true);
  }
}

inline void Rewrite::DoRewrite(bess::PacketBatch *batch) {
  size_t start = next_turn_;
  const size_t cnt = batch->cnt();

  for (size_t i = 0; i < cnt; i++) {
    uint16_t size = template_size_[start + i];
    bess::PacketRef pkt = batch->packet(i);
    ResetPacket(pkt);
    void *ptr = pkt.append(size);
    bess::utils::CopyInlined(ptr, templates_[start + i], size, true);
  }

  next_turn_ = jump_[start + cnt];
}

void Rewrite::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  if (num_templates_ == 1) {
    DoRewriteSingle(batch);
  } else if (num_templates_ > 1) {
    DoRewrite(batch);
  }

  RunNextModule(ctx, batch);
}

ADD_MODULE(Rewrite, "rewrite", "replaces entire packet data")
