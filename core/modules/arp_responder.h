// Copyright (c) 2017, Cloudigo.
// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_ARP_RESPONDER_H_
#define BESS_MODULES_ARP_RESPONDER_H_

#include <map>
#include <memory>

#include "dataplane/resource.h"
#include "framework/resource_bindings.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "utils/endian.h"
#include "utils/ether.h"
#include "utils/ip.h"

using bess::utils::Ethernet;
using bess::utils::be32_t;

// ARP cache entry struct which keeps mapping between IP and MAC
struct arp_entry {
  Ethernet::Address mac_addr;
  be32_t ip_addr;
  uint64_t time;  // timestamp used to expire cache entries (in milliseconds)
};

// ARP Responder module
// Answer ARP requests from an internal configurable cache
// Currently drops non ARP packets
//
// Control path (D-129): the cache is the "<module>/entries" resource (key
// ArpResponderEntryKey, value ArpResponderEntryValue). Workers read it without
// synchronization, so the resource requires paused workers. `add` stays for
// the legacy ModuleCommand only.
class ArpResponder final : public Module {
 public:
  static const gate_idx_t kNumIGates = 1;
  static const gate_idx_t kNumOGates = 1;

  static const Commands cmds;
  static const ResourceTables resource_tables;

  CommandResponse Init(const bess::pb::ArpResponderArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  CommandResponse CommandAdd(const bess::pb::ArpResponderArg &arg);

  // The module's `entries` resource.
  class EntriesResource;

 private:
  friend class EntriesResource;

  // Mapping between IP (key) and its ARP entry (MAC Address)
  std::map<be32_t, struct arp_entry> entries_;

  std::unique_ptr<bess::dataplane::Resource> resource_;
  bess::framework::ResourceBinding binding_;  // after resource_ (D-044)
};

#endif  // BESS_MODULES_ARP_RESPONDER_H_
