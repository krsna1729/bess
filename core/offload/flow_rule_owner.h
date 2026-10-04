// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_OFFLOAD_FLOW_RULE_OWNER_H_
#define BESS_OFFLOAD_FLOW_RULE_OWNER_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace bess::offload {

// The hardware flow-rule lifecycle owner (M20, D-070). An application compiles
// its own decisions to NIC rules -- native rte_flow patterns and actions, or
// whatever its backend takes; BESS defines no flow IR -- and the owner keeps
// what BESS is responsible for: which port a rule is on, its state through an
// asynchronous install and removal, bounded outstanding requests
// (backpressure), batch teardown, statistics, reconciliation after a device
// reset, and MARK values that are never reused while packets carrying them
// can still arrive.
//
// Control side only: one control thread drives an owner. Lookups of a MARK
// on the packet path go through MarkCookie, which is a plain array read the
// application publishes as it sees fit (copy it into an RCU object, or call
// it from the control thread that owns the owner).
//
// Backend contract (a type, so a fake backend and an rte_flow backend share
// the owner):
//
//   struct Backend {
//     using Rule = ...;        // the application's native rule (pattern + actions)
//     using HwHandle = ...;    // what the device returns (rte_flow*)
//     // Starts installing; the completion arrives through Poll. False: refused
//     // at once (the request never left), with `error` set.
//     bool Submit(uint16_t port, const Rule&, uint32_t mark, uint64_t tag, int &error);
//     // Starts removing a rule the device holds; completion through Poll.
//     bool Remove(uint16_t port, HwHandle, uint64_t tag, int &error);
//     // Delivers completions: fn(tag, ok, HwHandle, error).
//     template <class Fn> size_t Poll(Fn &&fn);
//     // Statistics of an installed rule (hits, bytes); false if unsupported.
//     bool Query(uint16_t port, HwHandle, FlowRuleStats &out);
//     FlowCapabilities Capabilities(uint16_t port) const;
//   };

enum class RuleState : uint8_t {
  kPreparing,   // reserved, not yet submitted (the request is being built)
  kSubmitted,   // the device has the request; not known to be installed
  kInstalled,   // the device confirmed
  kFailed,      // the device refused (or refused at submit)
  kRemoving,    // removal requested, not confirmed
  kRemoved,     // gone from the device; the handle is about to be retired
  kUnknown,     // the device was reset: whether the rule exists is unknown
};

inline const char *RuleStateName(RuleState s);

// A rule's identity: an index and a generation, so a handle kept after its
// rule was retired never names the next rule in the same slot.
struct FlowRuleHandle {
  uint32_t index = 0;  // 0 is no rule
  uint32_t generation = 0;
  friend bool operator==(const FlowRuleHandle &, const FlowRuleHandle &) = default;
};

struct FlowRuleStats {
  uint64_t hits = 0;
  uint64_t bytes = 0;
};

// What a port's device offers for flow offload, as the backend reports it for
// that port. The fake backend reports its fixture; RteFlowBackend asks the PMD
// (rte_flow_validate). `supported` gates Install. The rest are hints for the
// application's compiler -- RteFlowBackend's are the PMD's answers for one
// sample rule each, and `mark_bits` is a lower bound -- not gates: the device's
// answer for the application's own rule decides (kRefused, or a failed
// completion).
struct FlowCapabilities {
  bool supported = false;       // any flow rules at all
  bool async = false;           // installs complete later (else at once)
  bool count_action = false;    // per-rule counters
  bool mark_action = false;     // MARK on matched packets
  uint32_t mark_bits = 0;       // usable MARK width
  bool transfer = false;        // switch-domain (transfer) rules
  bool tunnel_match = false;    // matching on tunnel headers
  uint32_t max_rules = 0;       // 0: unknown
};

enum class InstallError : uint8_t {
  kOk,
  kUnsupported,   // the port reports no flow offload, or this owner's MARK
                  // values do not fit 32 bits (mark_base + marks - 1)
  kBackpressure,  // `max_outstanding` requests are in flight: retry after Poll
  kNoMark,        // every MARK value is in use or still draining
  kNoHandle,      // `max_rules` rules exist
  kRefused,       // the backend refused the request at submit (see `error`)
};

struct InstallResult {
  InstallError status;
  FlowRuleHandle handle;
  uint32_t mark = 0;
  int error = 0;
};

// One finished request, reported by Poll so the application can act (fall
// back to software for a failed install, free its state for a removed rule).
// A removal the device refused or failed is reported as kInstalled with the
// error: the rule is still there, with its MARK, and can be removed again.
struct Completion {
  FlowRuleHandle handle;
  RuleState state;   // kInstalled, kFailed or kRemoved
  uint64_t cookie;   // the application's value for the rule
  int error;         // nonzero: the install failed, or (kInstalled) a removal failed
};

template <typename Backend>
class FlowRuleOwner {
 public:
  using Rule = typename Backend::Rule;
  using HwHandle = typename Backend::HwHandle;

  struct Config {
    size_t max_rules = 4096;
    size_t max_outstanding = 256;  // submitted + removing, all ports
    uint32_t mark_base = 1;        // MARK values handed out: [mark_base, mark_base + marks)
    uint32_t marks = 4096;         // 0: rules carry no MARK
  };

  FlowRuleOwner(Backend &backend, const Config &config)
      : backend_(backend), config_(config),
        marks_fit_(config.marks == 0 ||
                   uint64_t{config.mark_base} + config.marks - 1 <= UINT32_MAX),
        slots_(config.max_rules + 1), free_marks_(config.marks),
        mark_cookie_(config.marks, kNoCookie) {
    free_slots_.reserve(config.max_rules);
    for (uint32_t i = config.max_rules; i >= 1; i--) {
      free_slots_.push_back(i);
    }
    for (uint32_t m = 0; m < config.marks; m++) {
      free_marks_.PushBack(m);
    }
    // A MARK is free, on a rule or draining, so the queues never outgrow
    // config.marks: retiring a rule cannot fail.
    quarantine_.reserve(config.marks);
  }

  // -- install and remove --------------------------------------------------------------

  // Submits `rule` on `port`. `cookie` is the application's value for the rule
  // (its continuation, its software decision); with marks, the rule carries
  // `result.mark`, which MarkCookie maps back to `cookie` once installed.
  InstallResult Install(uint16_t port, const Rule &rule, uint64_t cookie) {
    if (!marks_fit_ || !backend_.Capabilities(port).supported) {
      return {InstallError::kUnsupported, {}, 0, 0};
    }
    if (outstanding_ >= config_.max_outstanding) {
      return {InstallError::kBackpressure, {}, 0, 0};
    }
    if (free_slots_.empty()) {
      return {InstallError::kNoHandle, {}, 0, 0};
    }
    uint32_t mark = 0;
    if (config_.marks != 0) {
      if (free_marks_.empty()) {
        return {InstallError::kNoMark, {}, 0, 0};
      }
      mark = free_marks_.PopFront();
    }
    const uint32_t index = free_slots_.back();
    free_slots_.pop_back();
    Slot &s = slots_[index];
    s.generation++;
    s.live = true;
    s.remove_when_installed = false;
    s.port = port;
    s.state = RuleState::kPreparing;
    s.cookie = cookie;
    s.mark = mark;
    s.hw = HwHandle{};
    const FlowRuleHandle h{index, s.generation};
    int error = 0;
    bool submitted = false;
    try {
      submitted = backend_.Submit(port, rule, mark + config_.mark_base, Tag(h), error);
    } catch (...) {
      // The request never left (std::bad_alloc in the backend, say): the
      // handle and the MARK are free again before the caller sees the
      // exception.
      Retire(index, /*mark_seen_by_device=*/false);
      throw;
    }
    if (!submitted) {
      s.state = RuleState::kFailed;
      Retire(index, /*mark_seen_by_device=*/false);
      return {InstallError::kRefused, {}, 0, error};
    }
    s.state = RuleState::kSubmitted;
    outstanding_++;
    return {InstallError::kOk, h, config_.marks ? mark + config_.mark_base : 0, 0};
  }

  // Installs rules[i] with cookies[i] in order, writing out[i]; stops at the
  // first one not submitted (backpressure, no handle or MARK, refused), whose
  // result is in out[n]. Returns n, the number submitted. The spans have the
  // same size.
  size_t InstallBatch(uint16_t port, std::span<const Rule> rules, std::span<const uint64_t> cookies,
                      std::span<InstallResult> out) {
    const size_t count = std::min({rules.size(), cookies.size(), out.size()});
    for (size_t i = 0; i < count; i++) {
      out[i] = Install(port, rules[i], cookies[i]);
      if (out[i].status != InstallError::kOk) {
        return i;
      }
    }
    return count;
  }

  // Requests removal. A rule still being installed is removed when its
  // install completes. False for a stale handle or a rule already going.
  bool Remove(FlowRuleHandle h) {
    Slot *s = Resolve(h);
    if (s == nullptr) {
      return false;
    }
    switch (s->state) {
      case RuleState::kSubmitted:
        s->remove_when_installed = true;
        return true;
      case RuleState::kInstalled:
        return StartRemove(h.index, /*report=*/false);
      case RuleState::kFailed:
      case RuleState::kUnknown:
        // Nothing (known) on the device: retire now. An unknown rule's MARK
        // still drains: packets matched before the reset may be in flight.
        Retire(h.index, /*mark_seen_by_device=*/s->state == RuleState::kUnknown);
        return true;
      default:
        return false;
    }
  }

  // Removes every rule on `port` (teardown). Returns how many removals started.
  size_t RemoveAll(uint16_t port) {
    size_t n = 0;
    for (uint32_t i = 1; i < slots_.size(); i++) {
      Slot &s = slots_[i];
      if (s.port == port && s.live && Remove(FlowRuleHandle{i, s.generation})) {
        n++;
      }
    }
    return n;
  }

  // -- completions ------------------------------------------------------------------

  // Takes the backend's completions, advances rule states, and reports each
  // finished request to `fn(const Completion&)`.
  template <typename Fn>
  size_t Poll(Fn &&fn) {
    // Removals refused when they were started from inside the owner.
    size_t reported = 0;
    while (!reports_.empty()) {
      const Completion c = reports_.front();
      reports_.pop_front();
      fn(c);
      reported++;
    }
    return reported + backend_.Poll([&](uint64_t tag, bool ok, HwHandle hw, int error) {
      const FlowRuleHandle h = Untag(tag);
      Slot *s = Resolve(h);
      if (s == nullptr) {
        return;  // a completion for a rule retired meanwhile (a reset)
      }
      if (s->state != RuleState::kSubmitted && s->state != RuleState::kRemoving) {
        return;  // a late completion for a rule a reset made unknown
      }
      outstanding_--;
      if (s->state == RuleState::kSubmitted) {
        if (!ok) {
          // A failed rule keeps its handle until the application removes it
          // (it chooses the fallback first), unless removal was already asked.
          s->state = RuleState::kFailed;
          fn(Completion{h, RuleState::kFailed, s->cookie, error});
          // The callback may have acted on the rule: look again.
          s = Resolve(h);
          if (s != nullptr && s->state == RuleState::kFailed && s->remove_when_installed) {
            Retire(h.index, /*mark_seen_by_device=*/false);
          }
          return;
        }
        s->hw = hw;
        s->state = RuleState::kInstalled;
        if (config_.marks) {
          mark_cookie_[s->mark] = s->cookie;
        }
        fn(Completion{h, RuleState::kInstalled, s->cookie, 0});
        s = Resolve(h);
        if (s != nullptr && s->state == RuleState::kInstalled && s->remove_when_installed) {
          (void)StartRemove(h.index);
        }
        return;
      }
      if (s->state == RuleState::kRemoving) {
        if (!ok) {
          // The device kept the rule: it is still installed, with its MARK.
          s->state = RuleState::kInstalled;
          if (config_.marks) {
            mark_cookie_[s->mark] = s->cookie;
          }
          fn(Completion{h, RuleState::kInstalled, s->cookie, error != 0 ? error : -1});
          return;
        }
        s->state = RuleState::kRemoved;
        fn(Completion{h, RuleState::kRemoved, s->cookie, 0});
        Retire(h.index, /*mark_seen_by_device=*/true);
      }
    });
  }

  // -- MARK identity ------------------------------------------------------------------

  // The cookie of the installed rule that set `mark` on a packet, or nullopt
  // (no such rule, not installed yet, removed). A MARK value is handed out
  // again only after its rule's removal completed AND the application has
  // reported a drain of the port's receive queues since (NoteDrained), so a
  // packet marked by the old rule can never be mapped to a new one.
  std::optional<uint64_t> MarkCookie(uint32_t mark) const {
    if (mark < config_.mark_base || mark - config_.mark_base >= config_.marks) {
      return std::nullopt;
    }
    const uint64_t c = mark_cookie_[mark - config_.mark_base];
    return c == kNoCookie ? std::nullopt : std::optional<uint64_t>(c);
  }

  // The application has drained `port`'s receive queues (every packet that
  // arrived before this call has been processed): MARK values retired on that
  // port before the previous drain become reusable.
  void NoteDrained(uint16_t port) {
    drain_epoch_[port]++;
    for (auto it = quarantine_.begin(); it != quarantine_.end();) {
      if (it->port == port && drain_epoch_[port] > it->epoch) {
        free_marks_.PushBack(it->mark);
        it = quarantine_.erase(it);
      } else {
        ++it;
      }
    }
  }

  // -- device reset and reconciliation ------------------------------------------------

  // The device on `port` was reset (or disrupted): every rule there is in an
  // unknown state, and completions still in flight for it are dropped. The
  // application decides, per rule, through Reconcile.
  void OnDeviceReset(uint16_t port) {
    for (uint32_t i = 1; i < slots_.size(); i++) {
      Slot &s = slots_[i];
      if (s.live && s.port == port) {
        if (s.state == RuleState::kSubmitted || s.state == RuleState::kRemoving) {
          outstanding_--;
        }
        if (config_.marks) {
          mark_cookie_[s.mark] = kNoCookie;
        }
        s.state = RuleState::kUnknown;
      }
    }
  }

  // For each rule on `port` in the unknown state: `fn(handle, cookie)` returns
  // true to keep it (the application will install it again: the rule is
  // retired here and the application calls Install), false to drop it. Either
  // way the old handle is retired. Returns the handles visited.
  template <typename Fn>
  size_t Reconcile(uint16_t port, Fn &&fn) {
    size_t n = 0;
    for (uint32_t i = 1; i < slots_.size(); i++) {
      Slot &s = slots_[i];
      if (s.live && s.port == port && s.state == RuleState::kUnknown) {
        (void)fn(FlowRuleHandle{i, s.generation}, s.cookie);
        Retire(i, /*mark_seen_by_device=*/true);
        n++;
      }
    }
    return n;
  }

  // -- queries ----------------------------------------------------------------------

  std::optional<RuleState> StateOf(FlowRuleHandle h) const {
    const Slot *s = Resolve(h);
    return s ? std::optional<RuleState>(s->state) : std::nullopt;
  }
  bool Stats(FlowRuleHandle h, FlowRuleStats &out) {
    Slot *s = Resolve(h);
    return s != nullptr && s->state == RuleState::kInstalled && backend_.Query(s->port, s->hw, out);
  }
  FlowCapabilities CapabilitiesOf(uint16_t port) const { return backend_.Capabilities(port); }
  size_t outstanding() const noexcept { return outstanding_; }
  size_t live_rules() const noexcept { return config_.max_rules - free_slots_.size(); }
  size_t free_marks() const noexcept { return free_marks_.size(); }
  size_t draining_marks() const noexcept { return quarantine_.size(); }

 private:
  static constexpr uint64_t kNoCookie = ~uint64_t{0};

  struct Slot {
    uint32_t generation = 0;
    bool live = false;
    bool remove_when_installed = false;
    uint16_t port = 0;
    RuleState state = RuleState::kRemoved;
    uint32_t mark = 0;
    uint64_t cookie = 0;
    HwHandle hw{};
  };
  struct Draining {
    uint32_t mark;
    uint16_t port;
    uint64_t epoch;  // the port's drain epoch at retirement
  };

  static uint64_t Tag(FlowRuleHandle h) { return uint64_t{h.generation} << 32 | h.index; }
  static FlowRuleHandle Untag(uint64_t t) {
    return {static_cast<uint32_t>(t), static_cast<uint32_t>(t >> 32)};
  }

  Slot *Resolve(FlowRuleHandle h) {
    return const_cast<Slot *>(std::as_const(*this).Resolve(h));
  }
  const Slot *Resolve(FlowRuleHandle h) const {
    if (h.index == 0 || h.index >= slots_.size()) return nullptr;
    const Slot &s = slots_[h.index];
    return s.live && s.generation == h.generation ? &s : nullptr;
  }

  // Starts removing an installed rule. If the device refuses, the rule stays
  // installed with its MARK and hardware handle: the direct caller gets false;
  // a removal started by the owner itself (deferred) is reported through the
  // next Poll (`report`). Nothing changes before the step that can fail (a
  // queue that grows, the backend), so an exception leaves the rule as it was.
  bool StartRemove(uint32_t index, bool report = true) {
    Slot &s = slots_[index];
    if (outstanding_ >= config_.max_outstanding) {
      pending_removals_.push_back(FlowRuleHandle{index, s.generation});
      s.remove_when_installed = true;  // retried by RetryPendingRemovals
      return true;
    }
    int error = 0;
    const bool removing = backend_.Remove(s.port, s.hw, Tag({index, s.generation}), error);
    s.remove_when_installed = false;
    if (!removing) {
      if (report) {
        reports_.push_back(Completion{FlowRuleHandle{index, s.generation}, RuleState::kInstalled,
                                      s.cookie, error != 0 ? error : -1});
      }
      return false;
    }
    if (config_.marks) {
      mark_cookie_[s.mark] = kNoCookie;  // new packets with this MARK no longer map
    }
    s.state = RuleState::kRemoving;
    outstanding_++;
    return true;
  }

  void Retire(uint32_t index, bool mark_seen_by_device) noexcept {
    Slot &s = slots_[index];
    if (config_.marks) {
      mark_cookie_[s.mark] = kNoCookie;
      if (mark_seen_by_device) {
        quarantine_.push_back({s.mark, s.port, drain_epoch_[s.port]});
      } else {
        free_marks_.PushBack(s.mark);
      }
    }
    s.live = false;
    s.state = RuleState::kRemoved;
    s.remove_when_installed = false;
    free_slots_.push_back(index);
  }

 public:
  // Starts removals that waited for room among the outstanding requests.
  size_t RetryPendingRemovals() {
    size_t n = 0;
    while (!pending_removals_.empty() && outstanding_ < config_.max_outstanding) {
      const FlowRuleHandle h = pending_removals_.front();
      Slot *s = Resolve(h);
      if (s != nullptr && s->state == RuleState::kInstalled) {
        n += StartRemove(h.index) ? 1 : 0;  // on an exception, h stays queued
      }
      pending_removals_.pop_front();
    }
    return n;
  }

 private:
  // A FIFO of free MARK indices in storage sized once (every MARK is in one
  // place at a time, so it never overflows): returning a MARK cannot fail.
  class MarkQueue {
   public:
    explicit MarkQueue(uint32_t capacity) : slots_(capacity) {}
    bool empty() const noexcept { return count_ == 0; }
    size_t size() const noexcept { return count_; }
    uint32_t PopFront() noexcept {
      const uint32_t mark = slots_[head_];
      head_ = head_ + 1 == slots_.size() ? 0 : head_ + 1;
      count_--;
      return mark;
    }
    void PushBack(uint32_t mark) noexcept {
      const size_t tail = head_ + count_;
      slots_[tail < slots_.size() ? tail : tail - slots_.size()] = mark;
      count_++;
    }

   private:
    std::vector<uint32_t> slots_;
    size_t head_ = 0;
    size_t count_ = 0;
  };

  Backend &backend_;
  Config config_;
  bool marks_fit_;  // every MARK value (mark_base + i) fits 32 bits
  std::vector<Slot> slots_;
  std::vector<uint32_t> free_slots_;
  MarkQueue free_marks_;  // FIFO: the longest-free value goes first
  std::vector<uint64_t> mark_cookie_;
  std::vector<Draining> quarantine_;
  std::deque<FlowRuleHandle> pending_removals_;
  std::deque<Completion> reports_;  // refused deferred removals, for Poll
  std::vector<uint64_t> drain_epoch_ = std::vector<uint64_t>(65536, 0);
  size_t outstanding_ = 0;
};

inline const char *RuleStateName(RuleState s) {
  switch (s) {
    case RuleState::kPreparing: return "preparing";
    case RuleState::kSubmitted: return "submitted";
    case RuleState::kInstalled: return "installed";
    case RuleState::kFailed: return "failed";
    case RuleState::kRemoving: return "removing";
    case RuleState::kRemoved: return "removed";
    case RuleState::kUnknown: return "unknown";
  }
  return "?";
}

}  // namespace bess::offload

#endif  // BESS_OFFLOAD_FLOW_RULE_OWNER_H_
