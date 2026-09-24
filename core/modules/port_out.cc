// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
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
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
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

#include "port_out.h"

#include "../control/runtime_state.h"
#include "../utils/format.h"
#include "tx_checksum_profile.h"

const Commands PortOut::cmds = {
    {"get_initial_arg", "EmptyArg", MODULE_CMD_FUNC(&PortOut::GetInitialArg),
     Command::THREAD_SAFE},
};

CommandResponse PortOut::Init(const bess::pb::PortOutArg &arg) {
  const char *port_name;
  int ret;

  has_tx_checksum_profile_ = false;
  tx_checksum_profile_ = {};
  tx_checksum_profile_arg_.Clear();

  if (!arg.port().length()) {
    return CommandFailure(EINVAL, "'port' must be given as a string");
  }

  port_name = arg.port().c_str();

  port_ = bess::control::runtime().ports().Find(port_name);
  if (!port_) {
    return CommandFailure(ENODEV, "Port %s not found", port_name);
  }

  if (arg.has_tx_checksum_profile()) {
    const auto profile =
        bess::modules::ParseTxChecksumProfile(arg.tx_checksum_profile());
    if (!profile) {
      return CommandFailure(EINVAL, "Invalid tx_checksum_profile");
    }
    const auto bound = bess::packet::BindTxFinalizationProfile(
        *profile, port_->GetTxOffloadCapabilities());
    if (!bound) {
      return CommandFailure(EINVAL, "Invalid tx_checksum_profile");
    }
    tx_checksum_profile_ = *bound;
    tx_checksum_profile_arg_ = arg.tx_checksum_profile();
    has_tx_checksum_profile_ = true;
  }

  if (port_->num_queues[PACKET_DIR_OUT] == 0) {
    return CommandFailure(ENODEV, "Port %s has no outgoing queue", port_name);
  }

  ret = port_->AcquireQueues(reinterpret_cast<const module *>(this),
                             PACKET_DIR_OUT, nullptr, 0);

  node_constraints_ = port_->GetNodePlacementConstraint();

  for (size_t i = 0; i < MAX_QUEUES_PER_DIR; i++) {
    mcs_lock_init(&queue_locks_[i]);
  }

  if (ret < 0) {
    return CommandFailure(-ret);
  }

  return CommandSuccess();
}

CommandResponse PortOut::GetInitialArg(const bess::pb::EmptyArg &) {
  bess::pb::PortOutArg arg;
  arg.set_port(port_->name());
  if (has_tx_checksum_profile_) {
    *arg.mutable_tx_checksum_profile() = tx_checksum_profile_arg_;
  }
  return CommandSuccess(arg);
}

void PortOut::DeInit() {
  if (port_) {
    port_->ReleaseQueues(reinterpret_cast<const module *>(this), PACKET_DIR_OUT,
                         nullptr, 0);
  }
}

std::string PortOut::GetDesc() const {
  return bess::utils::Format("%s/%s", port_->name().c_str(),
                             port_->port_builder()->class_name().c_str());
}

static inline int SendBatch(
    bess::PacketBatch *batch, Port *p, queue_t qid,
    const bess::packet::BoundTxFinalizationProfile &profile) {
  uint64_t sent_bytes = 0;
  uint64_t prepare_errors = 0;
  int sent_pkts = 0;

  if (p->conf().admin_up) {
    if (profile.enabled()) {
      prepare_errors =
          bess::packet::FinalizeTxPacketBatch(*batch, profile).rejected;
    }
    if (batch->cnt() != 0) {
      sent_pkts = p->SendPackets(qid, batch->handles(), batch->cnt());
    }
  }

  const packet_dir_t dir = PACKET_DIR_OUT;
  QueueStats &stats = p->queue_stats[dir][qid];
  stats.tx_prepare_errors += prepare_errors;
  if (!(p->GetFlags() & DRIVER_FLAG_SELF_OUT_STATS)) {
    for (int i = 0; i < sent_pkts; i++) {
      sent_bytes += batch->packet(i).total_len();
    }

    stats.packets += sent_pkts;
    stats.dropped += prepare_errors + (batch->cnt() - sent_pkts);
    stats.bytes += sent_bytes;
  }

  return sent_pkts;
}

void PortOut::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  Port *p = port_;

  CHECK(worker_queues_[ctx->wid] >= 0);
  queue_t qid = worker_queues_[ctx->wid];
  int sent_pkts = 0;

  if (queue_users_[qid] == 1) {
    sent_pkts = SendBatch(batch, p, qid, tx_checksum_profile_);
  } else {
    mcslock_node_t me;
    mcs_lock(&queue_locks_[qid], &me);
    sent_pkts = SendBatch(batch, p, qid, tx_checksum_profile_);
    mcs_unlock(&queue_locks_[qid], &me);
  }

  if (sent_pkts < batch->cnt()) {
    bess::PacketFreeBulk(batch->handles() + sent_pkts, batch->cnt() - sent_pkts);
  }
}

int PortOut::OnEvent(bess::Event e) {
  if (e != bess::Event::PreResume) {
    return -ENOTSUP;
  }

  const std::vector<bool> &actives = active_workers();
  int num_queues = port_->num_queues[PACKET_DIR_OUT];
  int next_queue = 0;

  for (int i = 0; i < MAX_QUEUES_PER_DIR; i++) {
    queue_users_[i] = 0;
  }

  for (size_t i = 0; i < Worker::kMaxWorkers; i++) {
    worker_queues_[i] = -1;
    if (actives[i]) {
      worker_queues_[i] = next_queue;
      queue_users_[next_queue]++;
      next_queue = (next_queue + 1) % num_queues;
    }
  }

  return 0;
}

ADD_MODULE(PortOut, "port_out", "sends pakets to a port")
