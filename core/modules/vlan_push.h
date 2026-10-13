// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_VLANPUSH_H_
#define BESS_MODULES_VLANPUSH_H_

#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"

#include "utils/endian.h"

class VLANPush final : public Module {
 public:
  static const Commands cmds;
  static const ResourceTables resource_tables;

  VLANPush() : Module(), vlan_tag_(), qinq_tag_() {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::VLANPushArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  std::string GetDesc() const override;

  CommandResponse CommandSetTci(const bess::pb::VLANPushArg &arg);

 private:
  bess::utils::be32_t vlan_tag_;
  bess::utils::be32_t qinq_tag_;

  // "<name>/settings" (D-129): the TCI, paused workers only.
  bess::framework::Singleton<bess::pb::VLANPushArg, uint16_t> settings_;
};

#endif  // BESS_MODULES_VLANPUSH_H_
