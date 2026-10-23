// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_URL_FILTER_H_
#define BESS_MODULES_URL_FILTER_H_

#include <cstring>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "arch/crc32c.h"
#include "framework/singleton_resource.h"
#include "module.h"
#include "packet.h"
#include "pb/module_msg.pb.h"
#include "utils/tcp_flow_reconstruct.h"
#include "utils/trie.h"

using bess::utils::TcpFlowReconstruct;
using bess::utils::Trie;
using bess::utils::be16_t;
using bess::utils::be32_t;

// A helper class that defines a TCP flow
class alignas(16) Flow {
 public:
  be32_t src_ip;
  be32_t dst_ip;
  be16_t src_port;
  be16_t dst_port;
  uint32_t padding;

  Flow() : padding(0) {}

  bool operator==(const Flow &other) const {
    return memcmp(this, &other, sizeof(*this)) == 0;
  }
};

static_assert(sizeof(Flow) == 16, "Flow must be 16 bytes.");

// Hash function for std::unordered_map
struct FlowHash {
  std::size_t operator()(const Flow &f) const {
    uint32_t init_val = 0;

    uint64_t words[2];
    std::memcpy(words, &f, sizeof(words));
    init_val = bess::arch::Crc32c(words[0], init_val);
    init_val = bess::arch::Crc32c(words[1], init_val);

    return init_val;
  }
};

class FlowRecord {
 public:
  FlowRecord() : done_analyzing_(false), buffer_(128), expiry_time_(0) {}

  bool IsAnalyzed() { return done_analyzing_; }
  void SetAnalyzed() { done_analyzing_ = true; }
  TcpFlowReconstruct &GetBuffer() { return buffer_; }
  uint64_t ExpiryTime() { return expiry_time_; }
  void SetExpiryTime(uint64_t time) { expiry_time_ = time; }

 private:
  bool done_analyzing_;
  TcpFlowReconstruct buffer_;
  uint64_t expiry_time_;
};

// A module of HTTP URL filtering. Ends an HTTP connection if the Host field
// matches the blacklist.
// igate/ogate 0: traffic from internal network to external network
// igate/ogate 1: traffic from external network to internal network
class UrlFilter final : public Module {
 public:
  typedef std::pair<std::string, std::string> Url;

  static const Commands cmds;
  static const ResourceTables resource_tables;
  static const gate_idx_t kNumIGates = 2;
  static const gate_idx_t kNumOGates = 2;
  // One worker (the default limit); ProcessBatch inserts, refreshes and
  // erases flow_cache_ entries (TCP reassembly per flow).
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kSingleWorker,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  CommandResponse Init(const bess::pb::UrlFilterArg &arg);
  void DeInit() override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  std::string GetDesc() const override;

 private:
  using Blacklist = std::unordered_map<std::string, Trie<std::tuple<>>>;

  // A "<name>/blacklist" value, built: the blacklist and the
  // UrlFilterConfig it came from (read back as given).
  struct Staged {
    Blacklist blacklist;
    bess::pb::UrlFilterConfig config;
  };

  static void Insert(
      const google::protobuf::RepeatedPtrField<bess::pb::UrlFilterArg_Url> &urls,
      Blacklist *blacklist);

  Blacklist blacklist_;
  std::unordered_map<Flow, FlowRecord, FlowHash> flow_cache_;

  // The blacklist as given, in order (control side): "<name>/blacklist"
  // reads it back.
  bess::pb::UrlFilterConfig config_;
  // "<name>/blacklist" (D-129): the whole blacklist; setting it replaces it
  // (as set_runtime_config does). Paused workers: ProcessBatch reads
  // blacklist_ in place.
  bess::framework::Singleton<bess::pb::UrlFilterConfig, Staged>
      blacklist_resource_;
};

#endif  // BESS_MODULES_URL_FILTER_H_
