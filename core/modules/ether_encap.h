// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_ETHERENCAP_H_
#define BESS_MODULES_ETHERENCAP_H_

#include "module.h"
#include "pb/module_msg.pb.h"
#include "utils/ether.h"

class EtherEncap final : public Module {
 public:
  EtherEncap() : Module() { max_allowed_workers_ = Worker::kMaxWorkers; }

  CommandResponse Init(const bess::pb::EtherEncapArg &arg);

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  // Writes the Ethernet header into the packet's headroom, or returns false
  // when there is not enough. Public for the unit test; ProcessBatch drops
  // such a packet rather than forward its payload bare.
  static bool Encap(bess::PacketRef pkt,
                    const bess::utils::Ethernet::Address &src,
                    const bess::utils::Ethernet::Address &dst,
                    bess::utils::be16_t ether_type);
};

#endif  // BESS_MODULES_ETHERENCAP_H_
