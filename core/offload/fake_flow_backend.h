// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_OFFLOAD_FAKE_FLOW_BACKEND_H_
#define BESS_OFFLOAD_FAKE_FLOW_BACKEND_H_

#include <cstdint>
#include <deque>
#include <map>
#include <set>

#include "offload/flow_rule_owner.h"

namespace bess::offload {

// A software flow backend for tests without hardware (M20, D-070): per-port
// capability fixtures, requests that complete only when the test says so (or
// at the next Poll in auto mode), injected refusals and failures, a device
// reset that loses everything in flight, and per-rule counters. Rules are
// opaque integers (the application's "native rule" in tests).
class FakeFlowBackend {
 public:
  using Rule = uint64_t;
  using HwHandle = uint64_t;  // 0: none

  void SetCapabilities(uint16_t port, const FlowCapabilities &c) { caps_[port] = c; }
  FlowCapabilities Capabilities(uint16_t port) const {
    auto it = caps_.find(port);
    return it == caps_.end() ? FlowCapabilities{} : it->second;
  }

  // Auto mode: every pending request completes, successfully, at the next Poll.
  void set_auto_complete(bool on) { auto_ = on; }
  // The next Submit (or Remove) is refused at once with `error`.
  void RefuseNextSubmit(int error) { refuse_submit_ = error; }
  void RefuseNextRemove(int error) { refuse_remove_ = error; }
  // The next completion delivered for an install (or a removal) reports failure.
  void FailNextInstall(int error) { fail_install_ = error; }
  void FailNextRemove(int error) { fail_remove_ = error; }

  bool Submit(uint16_t port, const Rule &rule, uint32_t mark, uint64_t tag, int &error) {
    if (refuse_submit_ != 0) {
      error = refuse_submit_;
      refuse_submit_ = 0;
      return false;
    }
    pending_.push_back({tag, port, rule, mark, true, 0});
    return true;
  }
  bool Remove(uint16_t port, HwHandle hw, uint64_t tag, int &error) {
    if (refuse_remove_ != 0) {
      error = refuse_remove_;
      refuse_remove_ = 0;
      return false;
    }
    pending_.push_back({tag, port, 0, 0, false, hw});
    return true;
  }

  // Completes up to `n` pending requests (oldest first).
  void Complete(size_t n) { ready_ += n; }

  template <typename Fn>
  size_t Poll(Fn &&fn) {
    size_t done = 0;
    while (!pending_.empty() && (auto_ || ready_ > 0)) {
      const Request r = pending_.front();
      pending_.pop_front();
      if (!auto_) ready_--;
      if (r.install) {
        if (fail_install_ != 0) {
          const int e = fail_install_;
          fail_install_ = 0;
          fn(r.tag, false, HwHandle{0}, e);
        } else {
          const HwHandle hw = next_hw_++;
          installed_[hw] = {r.port, r.rule, r.mark};
          fn(r.tag, true, hw, 0);
        }
      } else if (fail_remove_ != 0) {
        const int e = fail_remove_;
        fail_remove_ = 0;
        fn(r.tag, false, r.hw, e);  // the device keeps the rule
      } else {
        installed_.erase(r.hw);
        fn(r.tag, true, r.hw, 0);
      }
      done++;
    }
    return done;
  }

  bool Query(uint16_t port, HwHandle hw, FlowRuleStats &out) {
    auto it = installed_.find(hw);
    if (it == installed_.end() || it->second.port != port || !Capabilities(port).count_action) {
      return false;
    }
    out = it->second.stats;
    return true;
  }

  // The device on `port` resets: its rules and every request in flight for it vanish.
  void Reset(uint16_t port) {
    for (auto it = installed_.begin(); it != installed_.end();) {
      it = it->second.port == port ? installed_.erase(it) : std::next(it);
    }
    std::deque<Request> keep;
    for (const auto &r : pending_) {
      if (r.port != port) keep.push_back(r);
    }
    pending_.swap(keep);
  }

  // Test helpers: traffic hitting a rule, and what the device holds.
  void Hit(HwHandle hw, uint64_t bytes) {
    auto &s = installed_.at(hw).stats;
    s.hits++;
    s.bytes += bytes;
  }
  size_t installed(uint16_t port) const {
    size_t n = 0;
    for (const auto &[hw, r] : installed_) n += r.port == port;
    return n;
  }
  std::set<uint32_t> marks(uint16_t port) const {
    std::set<uint32_t> m;
    for (const auto &[hw, r] : installed_) {
      if (r.port == port) m.insert(r.mark);
    }
    return m;
  }
  size_t pending() const { return pending_.size(); }

 private:
  struct Request {
    uint64_t tag;
    uint16_t port;
    Rule rule;
    uint32_t mark;
    bool install;
    HwHandle hw;
  };
  struct Installed {
    uint16_t port;
    Rule rule;
    uint32_t mark;
    FlowRuleStats stats{};
  };
  std::map<uint16_t, FlowCapabilities> caps_;
  std::deque<Request> pending_;
  std::map<HwHandle, Installed> installed_;
  HwHandle next_hw_ = 1;
  bool auto_ = false;
  size_t ready_ = 0;
  int refuse_submit_ = 0, refuse_remove_ = 0, fail_install_ = 0, fail_remove_ = 0;
};

}  // namespace bess::offload

#endif  // BESS_OFFLOAD_FAKE_FLOW_BACKEND_H_
