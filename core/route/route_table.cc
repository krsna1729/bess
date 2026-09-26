// Copyright (c) 2026, Nefeli Networks, Inc.
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
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
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

#include "route/route_table.h"

#include <rte_errno.h>

#include <cstdio>
#include <cstring>

#include "dpdk.h"

namespace bess::route {

const char *RouteErrorName(RouteError error) {
  switch (error) {
    case RouteError::kInvalidPrefixLength:
      return "prefix length must be 0-32";
    case RouteError::kHostBitsSet:
      return "address has bits set beyond the prefix length";
    case RouteError::kValueOutOfRange:
      return "route value does not fit in 24 bits";
    case RouteError::kTableFull:
      return "route table is full (rules or tbl8 groups)";
    case RouteError::kNotFound:
      return "no such route";
    case RouteError::kBackendFailure:
      return "rte_lpm failure";
    case RouteError::kInvalidId:
      return "id is zero or beyond the configured capacity";
    case RouteError::kUnknownNextHop:
      return "no such next hop";
    case RouteError::kNextHopInUse:
      return "next hop is still referenced by routes";
    case RouteError::kNextHopRetiring:
      return "next hop retiring";
  }
  return "unknown route error";
}

std::expected<Ipv4Prefix, RouteError> Ipv4Prefix::Make(uint32_t addr,
                                                        uint8_t length) {
  if (length > 32) {
    return std::unexpected(RouteError::kInvalidPrefixLength);
  }
  const uint32_t mask = length == 0 ? 0 : ~uint32_t{0} << (32 - length);
  if ((addr & ~mask) != 0) {
    return std::unexpected(RouteError::kHostBitsSet);
  }
  return Ipv4Prefix(addr, length);
}

LpmRouteTable::Instance::~Instance() {
  if (lpm != nullptr) {
    rte_lpm_free(lpm);  // also drains its QSBR defer queue
  }
}

namespace {

// rte_lpm names are global to the EAL and limited to RTE_LPM_NAMESIZE-1
// characters; a replacement instance exists alongside the one it replaces.
std::string InstanceName(const std::string &base) {
  static std::atomic<uint64_t> next{0};
  char suffix[24];
  snprintf(suffix, sizeof(suffix), "_r%lu",
           static_cast<unsigned long>(next.fetch_add(1)));
  const size_t room = RTE_LPM_NAMESIZE - 1 - strlen(suffix);
  return base.substr(0, room) + suffix;
}

}  // namespace

std::expected<std::unique_ptr<LpmRouteTable::Instance>, RouteError>
LpmRouteTable::NewInstance(uint32_t default_value) {
  const std::string name = InstanceName(name_);
  struct rte_lpm_config conf = {};
  conf.max_rules = config_.max_routes;
  conf.number_tbl8s = config_.tbl8_groups;
  struct rte_lpm *lpm = rte_lpm_create(name.c_str(), config_.socket, &conf);
  if (lpm == nullptr) {
    return std::unexpected(rte_errno == ENOMEM ? RouteError::kTableFull
                                               : RouteError::kBackendFailure);
  }

  struct rte_lpm_rcu_config rcu = {};
  rcu.v = domain_.dpdk_qsbr();
  rcu.mode = RTE_LPM_QSBR_MODE_DQ;
  if (rte_lpm_rcu_qsbr_add(lpm, &rcu) != 0) {
    rte_lpm_free(lpm);
    return std::unexpected(RouteError::kBackendFailure);
  }

  auto instance = std::make_unique<Instance>();
  instance->lpm = lpm;
  instance->default_value.store(default_value, std::memory_order_relaxed);
  return instance;
}

std::expected<std::unique_ptr<LpmRouteTable>, RouteError>
LpmRouteTable::Create(std::string name, const Config &config,
                      rcu::RcuDomain &domain) {
  if (!IsDpdkInitialized()) {
    InitDpdk();  // rte_lpm lives in EAL memory
  }
  std::unique_ptr<LpmRouteTable> table(
      new LpmRouteTable(std::move(name), config, domain));
  auto instance = table->NewInstance(kNoDefault);
  if (!instance) {
    return std::unexpected(instance.error());
  }
  table->live_.Initialize(std::move(*instance));
  return table;
}

LpmRouteTable::~LpmRouteTable() {
  // The owner guarantees no reader still uses the table (a module is
  // destroyed with workers paused), as with every RcuPtr teardown.
  live_.ResetQuiesced();
}

std::expected<void, RouteError> LpmRouteTable::Upsert(Ipv4Prefix prefix,
                                                      uint32_t value) {
  if (value > kMaxValue) {
    return std::unexpected(RouteError::kValueOutOfRange);
  }
  std::lock_guard<std::mutex> lock(writer_mutex_);
  // Whatever the caller published before this call (a next hop, say) must be
  // visible to any reader that sees the new route: the release fence orders
  // those earlier writes before rte_lpm's entry stores, whatever memory order
  // rte_lpm uses for them.
  std::atomic_thread_fence(std::memory_order_release);

  Instance *live = const_cast<Instance *>(live_.Read());
  if (prefix.length() == 0) {
    live->default_value.store(value, std::memory_order_release);
    default_ = value;
    return {};
  }
  const int ret = rte_lpm_add(live->lpm, prefix.addr(), prefix.length(), value);
  if (ret != 0) {
    return std::unexpected(ret == -ENOSPC ? RouteError::kTableFull
                                          : RouteError::kBackendFailure);
  }
  rules_[prefix] = value;
  return {};
}

std::expected<void, RouteError> LpmRouteTable::Erase(Ipv4Prefix prefix) {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  Instance *live = const_cast<Instance *>(live_.Read());
  if (prefix.length() == 0) {
    if (!default_) {
      return std::unexpected(RouteError::kNotFound);
    }
    live->default_value.store(kNoDefault, std::memory_order_release);
    default_.reset();
    return {};
  }
  if (rules_.find(prefix) == rules_.end()) {
    return std::unexpected(RouteError::kNotFound);
  }
  const int ret = rte_lpm_delete(live->lpm, prefix.addr(), prefix.length());
  if (ret != 0) {
    return std::unexpected(RouteError::kBackendFailure);
  }
  rules_.erase(prefix);
  return {};
}

std::expected<void, RouteError> LpmRouteTable::Clear() {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  auto fresh = NewInstance(default_.value_or(kNoDefault));
  if (!fresh) {
    return std::unexpected(fresh.error());
  }
  live_.Publish(std::move(*fresh));
  domain_.ReclaimReady();
  rules_.clear();
  return {};
}

std::optional<uint32_t> LpmRouteTable::Find(Ipv4Prefix prefix) const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  if (prefix.length() == 0) {
    return default_;
  }
  const auto it = rules_.find(prefix);
  return it == rules_.end() ? std::nullopt : std::optional<uint32_t>(it->second);
}

size_t LpmRouteTable::size() const {
  std::lock_guard<std::mutex> lock(writer_mutex_);
  return rules_.size() + (default_ ? 1 : 0);
}

}  // namespace bess::route
