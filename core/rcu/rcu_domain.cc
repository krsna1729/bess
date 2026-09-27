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

#include "rcu/rcu_domain.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <glog/logging.h>

namespace bess {
namespace rcu {

namespace {

// Cache-line aligned, as DPDK's QSBR asks; ordinary memory, so a domain can be
// constructed without DPDK's EAL having been initialized (the unit tests rely
// on that).
constexpr size_t kAlignment = 64;

}  // namespace

RcuDomain::RcuDomain(uint32_t max_readers, size_t retire_high_water)
    : max_readers_(max_readers), retire_high_water_(retire_high_water) {
  CHECK_GT(max_readers_, 0u);

  const size_t memsize = rte_rcu_qsbr_get_memsize(max_readers_);
  void *mem = nullptr;
  const int rc = posix_memalign(&mem, kAlignment, memsize);
  CHECK_EQ(0, rc) << "posix_memalign(" << memsize << ") failed";
  memset(mem, 0, memsize);

  qsbr_mem_ = mem;
  qsbr_ = static_cast<struct rte_rcu_qsbr *>(mem);
  CHECK_EQ(0, rte_rcu_qsbr_init(qsbr_, max_readers_))
      << "rte_rcu_qsbr_init() failed";

  registered_.assign(max_readers_, 0);
  online_.assign(max_readers_, 0);
}

RcuDomain::~RcuDomain() {
  // A domain must never disappear while registered readers can still run.
  DCHECK_EQ(0u, registered_readers())
      << "RcuDomain destroyed with " << registered_readers()
      << " reader(s) still registered";

  Drain();
  free(qsbr_mem_);
}

control::ControlResult<void> RcuDomain::Register(ReaderId id) {
  if (id >= max_readers_) {
    return std::unexpected(
        control::Err(EINVAL, "reader id %u is out of range (max %u)", id,
                     max_readers_));
  }

  std::lock_guard<std::mutex> lock(state_mutex_);
  if (registered_[id]) {
    return std::unexpected(
        control::Err(EEXIST, "reader id %u is already registered", id));
  }

  if (rte_rcu_qsbr_thread_register(qsbr_, id) != 0) {
    return std::unexpected(
        control::Err(EIO, "rte_rcu_qsbr_thread_register(%u) failed", id));
  }

  registered_[id] = 1;
  registered_readers_++;
  return {};
}

void RcuDomain::Unregister(ReaderId id) {
  if (id >= max_readers_) {
    return;
  }

  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!registered_[id]) {
    return;
  }

  // Offline first: an unregistered-but-online slot would otherwise still be
  // waited on.
  if (online_[id]) {
    rte_rcu_qsbr_thread_offline(qsbr_, id);
    online_[id] = 0;
  }

  rte_rcu_qsbr_thread_unregister(qsbr_, id);
  registered_[id] = 0;
  registered_readers_--;
}

void RcuDomain::Online(ReaderId id) {
  if (id >= max_readers_) {
    return;
  }

  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!registered_[id] || online_[id]) {
    return;
  }

  rte_rcu_qsbr_thread_online(qsbr_, id);
  online_[id] = 1;
}

void RcuDomain::Offline(ReaderId id) {
  if (id >= max_readers_) {
    return;
  }

  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!registered_[id]) {
    return;
  }

  // Tell DPDK even if our flag says offline: rte_rcu_qsbr_quiescent() on an
  // offline thread marks it online in DPDK's counter, and only
  // rte_rcu_qsbr_thread_offline() (idempotent) takes it back out. Skipping
  // the call left a stray report holding every grace period (external audit,
  // 2026-09-27: a never-resumed worker blocked reclamation, and past the
  // retire high-water mark Synchronize() hung the control plane).
  rte_rcu_qsbr_thread_offline(qsbr_, id);
  online_[id] = 0;
}

bool RcuDomain::IsOnline(ReaderId id) const {
  if (id >= max_readers_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  return online_[id] != 0;
}

bool RcuDomain::IsRegistered(ReaderId id) const {
  if (id >= max_readers_) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_mutex_);
  return registered_[id] != 0;
}

size_t RcuDomain::registered_readers() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  return registered_readers_;
}

size_t RcuDomain::online_readers() const {
  std::lock_guard<std::mutex> lock(state_mutex_);
  size_t online = 0;
  for (uint8_t state : online_) {
    online += state;
  }
  return online;
}

void RcuDomain::Quiescent(ReaderId id) {
  if (id >= max_readers_) {
    return;
  }
  rte_rcu_qsbr_quiescent(qsbr_, id);
}

GracePeriod RcuDomain::StartGracePeriod() {
  const GracePeriod token = rte_rcu_qsbr_start(qsbr_);

  std::lock_guard<std::mutex> lock(retire_mutex_);
  stats_.grace_periods_started++;
  return token;
}

bool RcuDomain::IsGracePeriodComplete(GracePeriod token) const {
  if (token == 0) {
    return true;
  }

  // Non-blocking check: has every online reader passed a quiescent state since
  // the token was issued? With no online readers this is immediately true,
  // which is what lets a paused (or not yet started) runtime reclaim.
  return rte_rcu_qsbr_check(qsbr_, token, 0) != 0;
}

bool RcuDomain::IsComplete(GracePeriod token) const {
  if (!IsGracePeriodComplete(token)) {
    return false;
  }

  std::lock_guard<std::mutex> lock(retire_mutex_);
  stats_.grace_periods_completed++;
  return true;
}

void RcuDomain::Synchronize() {
  rte_rcu_qsbr_synchronize(qsbr_, RTE_QSBR_THRID_INVALID);

  std::lock_guard<std::mutex> lock(retire_mutex_);
  stats_.grace_periods_completed++;
}

void RcuDomain::ReserveRetirements(size_t n) {
  std::lock_guard<std::mutex> lock(retire_mutex_);
  retired_.reserve(retired_.size() + n);
}

void RcuDomain::RetireErased(GracePeriod token, void *object,
                             void (*destroy)(void *),
                             std::atomic<size_t> *outstanding) {
  size_t pending = 0;
  if (outstanding != nullptr) {
    outstanding->fetch_add(1, std::memory_order_relaxed);
  }
  {
    std::lock_guard<std::mutex> lock(retire_mutex_);
    retired_.push_back(RetiredObject{token, object, destroy, outstanding});
    pending = retired_.size() - head_;
    pending_count_.store(pending, std::memory_order_relaxed);
    stats_.objects_retired++;
    stats_.pending_retired_objects = pending;
    if (stats_.oldest_pending_token == 0 ||
        token < stats_.oldest_pending_token) {
      stats_.oldest_pending_token = token;
    }
  }

  if (pending <= retire_high_water_) {
    return;
  }

  // Above the bound: reclaim what is ready, and if that is not enough wait for
  // the outstanding grace periods. This is the control side paying for a
  // pathological update rate -- the packet path never does.
  if (ReclaimReady() == 0) {
    Synchronize();
    ReclaimReady();
  }
}

size_t RcuDomain::ReclaimReady() {
  size_t reclaimed = 0;
  {
    std::lock_guard<std::mutex> lock(retire_mutex_);
    while (head_ < retired_.size() &&
           IsGracePeriodComplete(retired_[head_].token)) {
      const RetiredObject retired = retired_[head_++];
      retired.destroy(retired.object);
      if (retired.outstanding != nullptr) {
        retired.outstanding->fetch_sub(1, std::memory_order_release);
      }
      reclaimed++;
    }
    if (head_ == retired_.size()) {
      retired_.clear();  // keeps the capacity
      head_ = 0;
    } else if (head_ >= 1024 && head_ * 2 >= retired_.size()) {
      retired_.erase(retired_.begin(),
                     retired_.begin() + static_cast<ptrdiff_t>(head_));
      head_ = 0;
    }
    const size_t pending = retired_.size() - head_;
    stats_.objects_reclaimed += reclaimed;
    stats_.pending_retired_objects = pending;
    pending_count_.store(pending, std::memory_order_relaxed);
    stats_.oldest_pending_token = pending == 0 ? 0 : retired_[head_].token;
  }
  return reclaimed;
}

void RcuDomain::Drain() {
  Synchronize();
  ReclaimReady();
}

RcuStats RcuDomain::Stats() const {
  std::lock_guard<std::mutex> lock(retire_mutex_);
  return stats_;
}

}  // namespace rcu
}  // namespace bess
