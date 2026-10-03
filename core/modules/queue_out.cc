// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "queue_out.h"

#include "port.h"
#include "utils/format.h"
#include "tx_checksum_profile.h"

CommandResponse QueueOut::Init(const bess::pb::QueueOutArg &arg) {
  const char *port_name;
  int ret;

  if (!arg.port().length()) {
    return CommandFailure(EINVAL, "Field 'port' must be specified");
  }

  port_name = arg.port().c_str();
  qid_ = arg.qid();

  port_ = init_context().ports().Find(port_name);
  if (!port_) {
    return CommandFailure(ENODEV, "Port %s not found", port_name);
  }

  tx_checksum_profile_ = {};
  if (arg.has_tx_checksum_profile()) {
    const auto profile =
        bess::modules::ParseTxChecksumProfile(arg.tx_checksum_profile());
    if (!profile) {
      return CommandFailure(EINVAL, "Invalid tx_checksum_profile");
    }
    const auto bound = bess::packet::BindTxFinalizationProfile(
        *profile, port_->GetTxOffloadCapabilities(qid_));
    if (!bound) {
      return CommandFailure(EINVAL, "Invalid tx_checksum_profile");
    }
    tx_checksum_profile_ = *bound;
  }

  node_constraints_ = port_->GetNodePlacementConstraint();

  ret = port_->AcquireQueues(reinterpret_cast<const module *>(this),
                             PACKET_DIR_OUT, &qid_, 1);
  if (ret < 0) {
    return CommandFailure(-ret);
  }

  return CommandSuccess();
}

void QueueOut::DeInit() {
  if (port_) {
    port_->ReleaseQueues(reinterpret_cast<const module *>(this), PACKET_DIR_OUT,
                         &qid_, 1);
  }
}

std::string QueueOut::GetDesc() const {
  return bess::utils::Format("%s:%hhu/%s", port_->name().c_str(), qid_,
                             port_->port_builder()->class_name().c_str());
}

void QueueOut::ProcessBatch(Context *, bess::PacketBatch *batch) {
  Port *p = port_;
  const queue_t qid = qid_;
  uint64_t sent_bytes = 0;
  uint64_t prepare_errors = 0;
  int sent_pkts = 0;

  if (p->conf().admin_up) {
    if (tx_checksum_profile_.enabled()) {
      prepare_errors = bess::packet::FinalizeTxPacketBatch(
                           *batch, tx_checksum_profile_)
                           .rejected;
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

  if (sent_pkts < batch->cnt()) {
    bess::PacketFreeBulk(batch->handles() + sent_pkts,
                         batch->cnt() - sent_pkts);
  }
}

ADD_MODULE(QueueOut, "queue_out",
           "sends packets to a port via a specific queue")
