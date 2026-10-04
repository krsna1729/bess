// SPDX-License-Identifier: BSD-3-Clause

#ifndef APPLIANCES_NAT_NAT_APP_H_
#define APPLIANCES_NAT_NAT_APP_H_

// Reference appliance R2 (roadmap M24): a stateful NAT gateway -- a firewall
// that admits only connections started inside, in front of endpoint-
// independent NAPT. The application owns the pool policy (which public
// addresses and ports, per tenant here), the order of the two stages, and the
// clock. BESS provides the checked parse, connection tracking (worker-owned
// flow table, expiry wheel), the NAT bindings (worker-owned flow table with the
// external endpoint as the reverse alias, a port pool, idle expiry) and the
// rewrite. Nothing NAT-specific lives in BESS's runtime or framework: both
// libraries are plain objects this class owns.

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "conntrack/conntrack.h"
#include "conntrack/packet_parse.h"
#include "nat/nat.h"

namespace appliance {

class NatApp {
 public:
  enum class Verdict : uint8_t { kForward, kRefused, kNotTranslated };
  using Tick = uint64_t;  // ns, the caller's clock

  // The pool policy: tenant 10.1.0.0/16 leaves from 203.0.113.1, ports
  // 20000-29999; mappings idle for `timeout` end.
  static std::expected<std::unique_ptr<NatApp>, std::string> Create(Tick timeout, Tick start) {
    bess::nat::Nat::Config c;
    c.addresses = {{bess::utils::be32_t(kPublic), {{20000, 30000, false}}}};
    c.capacity = 4096;
    c.timeout = timeout;
    c.granularity_shift = 10;
    c.start = start;
    c.seed = 1;
    auto nat = bess::nat::Nat::Create(c);
    if (!nat) {
      return std::unexpected(std::string("nat"));
    }
    bess::conntrack::TimeoutPolicy policy;  // seconds; scaled to the ns clock
    auto ct = Ct::Create(4096, policy.Scaled(1000000000ull), start, 10);
    if (!ct) {
      return std::unexpected(std::string("conntrack"));
    }
    return std::unique_ptr<NatApp>(new NatApp(std::move(*nat), std::move(*ct)));
  }

  // Outbound (inside -> outside): the firewall admits it (and may start a
  // connection), then the NAT maps the source. Inbound: the NAT maps the
  // destination back through the reverse alias, then the firewall admits it
  // only as part of a connection started inside. `frame` is the packet's
  // first segment, `total_len` the packet's length (0: the span's).
  Verdict Process(std::span<uint8_t> frame, bool outbound, Tick now, size_t total_len = 0) noexcept {
    bess::conntrack::ParsedFlowPacket p;
    if (bess::conntrack::ParseFrame(frame, p, total_len) != bess::conntrack::ParseStatus::kOk) {
      return Verdict::kNotTranslated;
    }
    if (outbound) {
      if (!Admitted(frame, p, now, /*may_create=*/true)) {
        return Verdict::kRefused;
      }
      return nat_->Translate(frame, p, bess::nat::Direction::kForward, now) ==
                     bess::nat::Verdict::kTranslated
                 ? Verdict::kForward
                 : Verdict::kNotTranslated;
    }
    if (nat_->Translate(frame, p, bess::nat::Direction::kReverse, now) !=
        bess::nat::Verdict::kTranslated) {
      return Verdict::kNotTranslated;  // no mapping: dropped
    }
    // Re-parse: the rewrite changed the destination the firewall keys on.
    if (bess::conntrack::ParseFrame(frame, p, total_len) != bess::conntrack::ParseStatus::kOk ||
        !Admitted(frame, p, now, /*may_create=*/false)) {
      return Verdict::kRefused;
    }
    return Verdict::kForward;
  }

  // Ends idle mappings and connections (the application calls this each batch).
  void Expire(Tick now) {
    nat_->Expire(now, 256);
    ct_->Expire(now, 256);
  }

  size_t mappings() const noexcept { return nat_->size(); }
  size_t connections() const noexcept { return ct_->size(); }

  static constexpr uint32_t kPublic = 0xcb007101;  // 203.0.113.1

 private:
  using Ct = bess::conntrack::Conntrack<>;

  NatApp(std::unique_ptr<bess::nat::Nat> nat, std::unique_ptr<Ct> ct)
      : nat_(std::move(nat)), ct_(std::move(ct)) {}

  bool Admitted(std::span<const uint8_t> frame, const bess::conntrack::ParsedFlowPacket &p,
                Tick now, bool may_create) noexcept {
    const auto r = ct_->Track(frame, p, now, 0, may_create);
    return r.status == bess::conntrack::TrackStatus::kNew ||
           r.status == bess::conntrack::TrackStatus::kExisting ||
           r.status == bess::conntrack::TrackStatus::kRelated;
  }

  std::unique_ptr<bess::nat::Nat> nat_;
  std::unique_ptr<Ct> ct_;
};

}  // namespace appliance

#endif  // APPLIANCES_NAT_NAT_APP_H_
