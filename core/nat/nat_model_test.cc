// SPDX-License-Identifier: BSD-3-Clause

// Nat (M18, D-068) against a forward/reverse binding model with expiry (M22).
//
// Contract under test, as the model encodes it (nat.h's comments and D-068):
//   - Endpoint = (IPv4 address, TCP/UDP port or ICMP query identifier,
//     protocol). A binding maps an internal endpoint to an external one; both
//     are in one key space: no endpoint is two bindings' key.
//   - Forward (outbound) packet, endpoint e = its source:
//       not IPv4 -> kNotIpv4; not TCP, UDP or an ICMP query (types 0, 8,
//       13-16) -> kUnsupported;
//       e is a binding's internal endpoint -> kTranslated, the source becomes
//       the external endpoint, the binding's last_refresh = now;
//       e is a binding's external endpoint -> kConflict;
//       otherwise a new binding, verdicts in this order of precedence:
//         TCP/UDP port 0 -> kPortZero;
//         size == capacity -> kFull;
//         the external address is entry PickAddress(e.addr) = crc32c(addr) %
//         entries (the legacy hash, computed here with rte_hash_crc); the
//         candidate ports are, over that entry's non-suspended ranges
//         [begin, end), ICMP: the whole range; an internal port 1-1023:
//         [max(1, begin), min(1024, end)); any other: [max(1024, begin), end),
//         minus the ports in use for that external IP (shared by entries
//         listing the same IP) and protocol class (TCP, UDP, ICMP);
//         no candidate -> kExhausted;
//         else kTranslated with an external port among the candidates whose
//         external endpoint is no binding's internal endpoint (nor the new
//         one's: no identity mapping), or kConflict (nothing changed, the
//         port stays free) -- allowed only when some candidate's external
//         endpoint is such an internal endpoint.
//   - Reverse (inbound) packet, endpoint e = its destination: e is a binding's
//     external endpoint -> kTranslated, the destination becomes the internal
//     endpoint, no refresh; otherwise -> kNoBinding.
//   - A translated packet differs from the original only in the rewritten
//     address and port/identifier and the checksums, which stay valid (a UDP
//     checksum of 0 stays 0); any other verdict leaves the packet unchanged.
//   - Expire(now, budget): a binding is due once now >= last_refresh +
//     timeout. With granularity 2^g, an unlimited budget removes every
//     binding with roundup(due) <= rounddown(now) and none with due > now;
//     with g = 0 that is exactly the due set. A budget b removes at most b,
//     only due ones. A removed binding frees its external port.
//   - TranslateBatch: verdicts and rewrites equal to Translate packet by
//     packet in order; a packet whose parse is not ok is kUnsupported.

#include "nat/nat.h"

#include <gtest/gtest.h>

#include <rte_hash_crc.h>

#include <algorithm>
#include <compare>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <utility>
#include <vector>

#include "utils/checksum.h"
#include "utils/ip.h"
#include "utils/tcp.h"
#include "utils/udp.h"

namespace bess::nat {
namespace {

using conntrack::ParsedFlowPacket;
using conntrack::ParseFrame;
using conntrack::ParseStatus;

void Put16(std::vector<uint8_t> &f, size_t off, uint16_t v) {
  f[off] = static_cast<uint8_t>(v >> 8);
  f[off + 1] = static_cast<uint8_t>(v);
}
void Put32(std::vector<uint8_t> &f, size_t off, uint32_t v) {
  Put16(f, off, static_cast<uint16_t>(v >> 16));
  Put16(f, off + 2, static_cast<uint16_t>(v));
}
uint16_t Get16(const std::vector<uint8_t> &f, size_t off) {
  return static_cast<uint16_t>(f[off] << 8 | f[off + 1]);
}
uint32_t Get32(const std::vector<uint8_t> &f, size_t off) {
  return uint32_t{Get16(f, off)} << 16 | Get16(f, off + 2);
}

// The utils checksum helpers read the headers as structs, so this oracle
// runs them over a copy of the IP packet at an aligned address: behind a
// 14-byte Ethernet header the IPv4 header sits at 2 mod 4 (UBSan: misaligned
// load). The code under test still rewrites the real, Ethernet-framed bytes.
struct alignas(8) IpCopy {
  uint8_t bytes[64];
  utils::Ipv4 *ip() { return reinterpret_cast<utils::Ipv4 *>(bytes); }
  uint8_t *l4() { return bytes + 20; }
};

// Ethernet + IPv4 + L4 with valid checksums. ICMP: `sport` is the
// identifier, `dport` the type.
std::vector<uint8_t> Frame(uint8_t proto, uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport,
                           bool udp_checksum) {
  const size_t l4len = proto == 6 ? 20 + 6 : 8 + 6;
  std::vector<uint8_t> f(14 + 20 + l4len, 0);
  Put16(f, 12, 0x0800);
  f[14] = 0x45;
  Put16(f, 16, static_cast<uint16_t>(20 + l4len));
  Put16(f, 18, static_cast<uint16_t>(src ^ dst));  // an IP id that varies
  f[22] = 64;
  f[23] = proto;
  Put32(f, 26, src);
  Put32(f, 30, dst);
  for (size_t i = 0; i < 6; i++) f[f.size() - 6 + i] = static_cast<uint8_t>(0x30 + i + sport);
  if (proto == 6 || proto == 17) {
    Put16(f, 34, sport);
    Put16(f, 36, dport);
  }
  if (proto == 6) {
    f[34 + 12] = 5 << 4;
    f[34 + 13] = 0x10;
  } else if (proto == 17) {
    Put16(f, 38, static_cast<uint16_t>(l4len));
  } else {
    f[34] = static_cast<uint8_t>(dport);
    Put16(f, 38, sport);
  }
  IpCopy c;
  std::memcpy(c.bytes, f.data() + 14, f.size() - 14);
  c.ip()->checksum = utils::CalculateIpv4NoOptChecksum(*c.ip());
  if (proto == 6) {
    auto *tcp = reinterpret_cast<utils::Tcp *>(c.l4());
    tcp->checksum = utils::CalculateIpv4TcpChecksum(*c.ip(), *tcp);
  } else if (proto == 17) {
    auto *udp = reinterpret_cast<utils::Udp *>(c.l4());
    if (udp_checksum) udp->checksum = utils::CalculateIpv4UdpChecksum(*c.ip(), *udp);
  } else {
    const uint16_t sum = utils::CalculateGenericChecksum(c.l4(), l4len);
    std::memcpy(c.l4() + 2, &sum, 2);
  }
  std::memcpy(f.data() + 14, c.bytes, f.size() - 14);
  return f;
}

bool ChecksumsValid(const std::vector<uint8_t> &f) {
  IpCopy c;
  std::memcpy(c.bytes, f.data() + 14, f.size() - 14);
  const utils::Ipv4 &ip = *c.ip();
  if (!utils::VerifyIpv4NoOptChecksum(ip)) return false;
  if (ip.protocol == 6) return utils::VerifyIpv4TcpChecksum(ip, *reinterpret_cast<const utils::Tcp *>(c.l4()));
  if (ip.protocol == 17) {
    const auto *udp = reinterpret_cast<const utils::Udp *>(c.l4());
    return udp->checksum == 0 || utils::VerifyIpv4UdpChecksum(ip, *udp);
  }
  return utils::VerifyGenericChecksum(c.l4(), f.size() - 34);
}

// -- the model ---------------------------------------------------------------------

struct Ep {
  uint32_t addr;   // host order
  uint16_t port;   // port or ICMP identifier
  uint8_t proto;
  auto operator<=>(const Ep &) const = default;
};

Endpoint AsEndpoint(const Ep &e) { return Endpoint{be32_t(e.addr), be16_t(e.port), e.proto}; }

int ClassOf(uint8_t proto) { return proto == 6 ? 0 : proto == 17 ? 1 : 2; }

struct ModelBinding {
  Ep external;
  uint64_t last_refresh;
};

struct Packet {
  std::vector<uint8_t> frame;
  ParsedFlowPacket parsed;
  bool parsed_ok;
  Direction dir;
  // What the packet carries for its direction (source out, destination in).
  Ep endpoint;
  bool v6 = false;  // a synthesised IPv6 parse (kNotIpv4)
};

struct NatModelConfig {
  uint32_t seed;
  size_t capacity;
  unsigned shift;
  uint64_t timeout;
  std::vector<ExternalAddress> addresses;
  size_t steps;
  size_t max_capacity = 0;  // > capacity: the NAT grows (TP5) at random points
};

class NatModel {
 public:
  explicit NatModel(const NatModelConfig &cfg) : cfg_(cfg), rng_(cfg.seed) {
    Nat::Config c;
    c.addresses = cfg.addresses;
    c.capacity = cfg.capacity;
    c.timeout = cfg.timeout;
    c.granularity_shift = cfg.shift;
    c.start = 0;
    c.seed = cfg.seed;
    c.max_capacity = cfg.max_capacity;
    auto made = Nat::Create(c);
    EXPECT_TRUE(made.has_value());
    nat_ = std::move(*made);
    for (const auto &a : cfg.addresses) {
      const uint32_t ip = a.addr.value();
      if (std::find(ext_ips_.begin(), ext_ips_.end(), ip) == ext_ips_.end()) ext_ips_.push_back(ip);
    }
    // Internal hosts include the external addresses themselves, so an
    // internal endpoint can be another binding's external one (kConflict).
    hosts_ = {0x0a000001, 0x0a000002, 0x0a000003, 0x0a000004, 0x0a000005, 0x0a000006};
    for (uint32_t ip : ext_ips_) hosts_.push_back(ip);
    for (const auto &a : cfg.addresses) {
      for (const auto &r : a.ranges) {
        interesting_ports_.push_back(static_cast<uint16_t>(r.begin + 1));
        interesting_ports_.push_back(static_cast<uint16_t>(r.end - 2));
      }
    }
  }

  void Run() {
    for (size_t step = 0; step < cfg_.steps; step++) {
      SCOPED_TRACE(::testing::Message() << "step " << step << " now " << now_);
      const uint32_t op = rng_() % 100;
      if (op < 70) {
        Packet p = MakePacket(rng_() % 3 != 0 ? Direction::kForward : Direction::kReverse);
        const std::vector<uint8_t> before = p.frame;
        const Verdict v = p.parsed_ok
                              ? nat_->Translate(p.frame, p.parsed, p.dir, now_)
                              : Verdict::kUnsupported;
        Check(p, before, v);
      } else if (op < 80) {
        Batch();
      } else if (op < 90) {
        Expire();
      } else {
        now_ += rng_() % 10 == 0 ? cfg_.timeout / 2 + rng_() % cfg_.timeout : rng_() % 8;
      }
      if (::testing::Test::HasFatalFailure()) return;
      // Growth, as the module drives it, at random points: the owner adopts a
      // larger table when asked and moves a few slots between operations.
      // Every check above and below must hold throughout.
      if (nat_->NeedsGrowth() && rng_() % 4 == 0) {
        nat_->Adopt(Nat::NewTable(nat_->GrowthTarget()));
        grown++;
      }
      if (nat_->migrating()) {
        (void)nat_->MigrateSome(1 + rng_() % 8);
      }
      ASSERT_EQ(bindings_.size(), nat_->size());
      if (step % 50 == 0) {
        CheckAll();
        if (::testing::Test::HasFatalFailure()) return;
      }
    }
    CheckAll();
  }

  std::map<Verdict, size_t> verdicts;
  size_t expired = 0, budget_stops = 0, grown = 0;

 private:
  uint64_t Mask() const { return (uint64_t{1} << cfg_.shift) - 1; }

  Packet MakePacket(Direction dir) {
    Packet p;
    p.dir = dir;
    const uint32_t kind = rng_() % 20;
    uint8_t proto = kind < 8 ? 6 : kind < 14 ? 17 : kind < 19 ? 1 : 47;
    const uint32_t remote = 0xcb007100u | (1 + rng_() % 2);
    const uint16_t rport = rng_() % 2 ? 53 : 443;
    Ep e{0, 0, proto};
    if (dir == Direction::kForward) {
      // A binding's internal endpoint, its external one, or a fresh one.
      const uint32_t pick = rng_() % 10;
      if (pick < 4 && !bindings_.empty()) {
        auto it = bindings_.begin();
        std::advance(it, rng_() % bindings_.size());
        e = it->first;
      } else if (pick < 5 && !externals_seen_.empty()) {
        e = externals_seen_[rng_() % externals_seen_.size()];
      } else {
        e.addr = hosts_[rng_() % hosts_.size()];
        const uint32_t r = rng_() % 10;
        e.port = r == 0   ? 0
                 : r == 1 ? 22
                 : r < 4  ? interesting_ports_[rng_() % interesting_ports_.size()]
                          : static_cast<uint16_t>(40000 + rng_() % 6);
        if (proto == 1) e.port = static_cast<uint16_t>(rng_() % 3 == 0 ? interesting_ports_[0] : rng_() % 4);
      }
    } else {
      const uint32_t pick = rng_() % 10;
      if (pick < 6 && !externals_seen_.empty()) {
        e = externals_seen_[rng_() % externals_seen_.size()];
      } else if (pick < 7 && !bindings_.empty()) {
        auto it = bindings_.begin();
        std::advance(it, rng_() % bindings_.size());
        e = it->first;  // inbound to an internal endpoint: no binding
      } else {
        e.addr = ext_ips_[rng_() % ext_ips_.size()];
        e.port = interesting_ports_[rng_() % interesting_ports_.size()];
      }
    }
    if (e.proto != proto) proto = e.proto;
    if (proto == 47) e.port = 0;
    const bool udp_checksum = rng_() % 4 != 0;
    if (proto == 1) {
      // Queries: forward requests, reverse replies; sometimes an error
      // (unreachable), which is not translated.
      static constexpr uint8_t kOut[] = {8, 13, 15, 16, 3};
      static constexpr uint8_t kIn[] = {0, 14, 16, 0, 11};
      const uint8_t type = dir == Direction::kForward ? kOut[rng_() % 5] : kIn[rng_() % 5];
      p.frame = dir == Direction::kForward ? Frame(1, e.addr, e.port, remote, type, true)
                                           : Frame(1, remote, e.port, e.addr, type, true);
    } else if (dir == Direction::kForward) {
      p.frame = Frame(proto, e.addr, e.port, remote, rport, udp_checksum);
    } else {
      p.frame = Frame(proto, remote, rport, e.addr, e.port, udp_checksum);
    }
    p.endpoint = e;
    p.parsed_ok = ParseFrame(p.frame, p.parsed) == ParseStatus::kOk;
    EXPECT_TRUE(p.parsed_ok);
    if (rng_() % 40 == 0) {
      p.parsed.l3 = conntrack::L3Kind::kIpv6;  // as the parser reports IPv6
      p.v6 = true;
    }
    return p;
  }

  // The model's candidates for a new binding of `e`: the external IP and the
  // usable spans; a candidate is a port in a span not in use for that IP and
  // protocol class.
  struct Candidates {
    uint32_t ip;
    std::vector<std::pair<uint32_t, uint32_t>> spans;  // [lo, hi)
    const std::set<uint16_t> *used;
    bool Free(uint16_t port) const {
      for (const auto &[lo, hi] : spans) {
        if (port >= lo && port < hi) return used->count(port) == 0;
      }
      return false;
    }
    bool Any() const {
      for (const auto &[lo, hi] : spans) {
        const auto in_use = std::distance(used->lower_bound(static_cast<uint16_t>(lo)),
                                          hi > 65535 ? used->end() : used->lower_bound(static_cast<uint16_t>(hi)));
        if (static_cast<uint32_t>(in_use) < hi - lo) return true;
      }
      return false;
    }
  };
  Candidates CandidatesFor(const Ep &e) {
    uint32_t key = be32_t(e.addr).raw_value();
    const size_t entry = rte_hash_crc(&key, sizeof(key), 0) % cfg_.addresses.size();
    const ExternalAddress &a = cfg_.addresses[entry];
    Candidates c{a.addr.value(), {}, &used_[{a.addr.value(), ClassOf(e.proto)}]};
    for (const PortRange &r : a.ranges) {
      if (r.suspended) continue;
      uint32_t lo = r.begin, hi = r.end;
      if (e.proto == 6 || e.proto == 17) {
        if (e.port < 1024) {
          lo = std::max<uint32_t>(1, lo), hi = std::min<uint32_t>(1024, hi);
        } else {
          lo = std::max<uint32_t>(1024, lo);
        }
      }
      if (lo < hi) c.spans.push_back({lo, hi});
    }
    return c;
  }

  // Compares one packet's outcome with the model and updates it.
  void Check(const Packet &p, const std::vector<uint8_t> &before, Verdict v) {
    verdicts[v]++;
    const Ep &e = p.endpoint;
    const uint8_t type = e.proto == 1 ? before[34] : 0;
    auto unchanged = [&] { ASSERT_EQ(before, p.frame) << "a refused packet is not modified"; };
    if (!p.parsed_ok) {
      ASSERT_EQ(Verdict::kUnsupported, v);
      return unchanged();
    }
    if (p.v6) {
      ASSERT_EQ(Verdict::kNotIpv4, v);
      return unchanged();
    }
    const bool query = type == 0 || type == 8 || (type >= 13 && type <= 16);
    if (e.proto == 47 || (e.proto == 1 && !query)) {
      ASSERT_EQ(Verdict::kUnsupported, v);
      return unchanged();
    }
    auto internal = bindings_.find(e);
    auto external = ext_to_int_.find(e);
    if (p.dir == Direction::kReverse) {
      if (external == ext_to_int_.end()) {
        ASSERT_EQ(Verdict::kNoBinding, v) << "inbound to " << e.addr << ":" << e.port;
        return unchanged();
      }
      ASSERT_EQ(Verdict::kTranslated, v);
      return ExpectRewrite(p, before, external->second);
    }
    if (internal != bindings_.end()) {
      ASSERT_EQ(Verdict::kTranslated, v);
      internal->second.last_refresh = now_;
      return ExpectRewrite(p, before, internal->second.external);
    }
    if (external != ext_to_int_.end()) {
      ASSERT_EQ(Verdict::kConflict, v) << "outbound from another binding's external endpoint";
      return unchanged();
    }
    if ((e.proto == 6 || e.proto == 17) && e.port == 0) {
      ASSERT_EQ(Verdict::kPortZero, v);
      return unchanged();
    }
    if (bindings_.size() == nat_->capacity()) {
      ASSERT_EQ(Verdict::kFull, v);
      return unchanged();
    }
    const Candidates cand = CandidatesFor(e);
    const uint32_t ip = cand.ip;
    if (!cand.Any()) {
      ASSERT_EQ(Verdict::kExhausted, v) << "no free port for " << e.addr << ":" << e.port;
      return unchanged();
    }
    // A candidate whose external endpoint is an internal endpoint's key --
    // another binding's, or the new one's own (an identity mapping is the
    // same key twice) -- may be drawn; the mapping is then refused.
    bool conflict_possible = e.addr == ip && cand.Free(e.port);
    for (auto it = bindings_.lower_bound(Ep{ip, 0, 0}); it != bindings_.end() && it->first.addr == ip;
         ++it) {
      conflict_possible |= it->first.proto == e.proto && cand.Free(it->first.port);
    }
    if (v == Verdict::kConflict) {
      ASSERT_TRUE(conflict_possible) << "kConflict without a colliding candidate";
      return unchanged();
    }
    ASSERT_EQ(Verdict::kTranslated, v);
    // The new external endpoint, as the rewrite shows it.
    const Ep ext{Get32(p.frame, 26), e.proto == 1 ? Get16(p.frame, 38) : Get16(p.frame, 34), e.proto};
    ASSERT_EQ(ip, ext.addr) << "paired address pooling";
    ASSERT_TRUE(cand.Free(ext.port)) << "port " << ext.port << " not a free candidate";
    ASSERT_TRUE(ext != e && bindings_.count(ext) == 0) << "the external endpoint is an internal one's key";
    bindings_[e] = {ext, now_};
    ext_to_int_[ext] = e;
    used_[{ip, ClassOf(e.proto)}].insert(ext.port);
    externals_seen_.push_back(ext);
    ExpectRewrite(p, before, ext);
  }

  // The packet with its endpoint replaced by `to`, checksums valid.
  void ExpectRewrite(const Packet &p, const std::vector<uint8_t> &before, const Ep &to) {
    std::vector<uint8_t> want = before;
    const bool fwd = p.dir == Direction::kForward;
    Put32(want, fwd ? 26 : 30, to.addr);
    if (to.proto == 1) {
      Put16(want, 38, to.port);
    } else {
      Put16(want, fwd ? 34 : 36, to.port);
    }
    // Compare everything but the checksum fields, which must verify.
    std::vector<uint8_t> got = p.frame;
    const size_t sums[] = {24, size_t{to.proto == 6 ? 50u : to.proto == 17 ? 40u : 36u}};
    const bool udp_zero = to.proto == 17 && Get16(before, 40) == 0;
    for (size_t s : sums) {
      if (udp_zero && s == 40) {
        ASSERT_EQ(0, Get16(got, 40)) << "an absent UDP checksum stays absent";
      }
      got[s] = got[s + 1] = want[s] = want[s + 1] = 0;
    }
    ASSERT_EQ(want, got) << "only the endpoint is rewritten";
    ASSERT_TRUE(ChecksumsValid(p.frame));
  }

  void Batch() {
    const Direction dir = rng_() % 2 ? Direction::kForward : Direction::kReverse;
    const size_t n = 1 + rng_() % 12;
    std::vector<Packet> packets;
    for (size_t i = 0; i < n; i++) {
      // Repeat an earlier packet's endpoint sometimes: two packets of one
      // new flow in a batch share one mapping.
      if (i > 0 && rng_() % 4 == 0) {
        Packet copy = packets[rng_() % i];
        packets.push_back(copy);
      } else {
        packets.push_back(MakePacket(dir));
      }
      packets.back().dir = dir;
      if (rng_() % 16 == 0) packets.back().parsed_ok = false;
    }
    std::vector<std::vector<uint8_t>> before;
    std::vector<std::span<uint8_t>> frames;
    std::vector<ParsedFlowPacket> parsed;
    std::unique_ptr<bool[]> ok(new bool[n]);
    for (size_t i = 0; i < n; i++) {
      before.push_back(packets[i].frame);
      parsed.push_back(packets[i].parsed);
      ok[i] = packets[i].parsed_ok;
    }
    for (auto &p : packets) frames.emplace_back(p.frame);
    std::vector<Verdict> out(n);
    nat_->TranslateBatch(frames, parsed, std::span<const bool>(ok.get(), n), dir, now_, out);
    for (size_t i = 0; i < n; i++) {
      SCOPED_TRACE(::testing::Message() << "batch position " << i);
      Check(packets[i], before[i], out[i]);
      if (::testing::Test::HasFatalFailure()) return;
    }
  }

  void Expire() {
    const bool limited = rng_() % 2;
    const size_t budget = limited ? 1 + rng_() % 3 : size_t{1} << 30;
    const size_t removed = nat_->Expire(now_, budget);
    std::vector<Ep> gone;
    for (const auto &[in, b] : bindings_) {
      if (nat_->Find(AsEndpoint(in)) == nullptr) gone.push_back(in);
    }
    ASSERT_EQ(gone.size(), removed);
    ASSERT_LE(removed, budget);
    size_t due = 0;
    for (const auto &[in, b] : bindings_) {
      const uint64_t deadline = b.last_refresh + cfg_.timeout;
      const bool left = std::find(gone.begin(), gone.end(), in) != gone.end();
      const bool is_due = deadline <= now_;
      due += is_due;
      if (left) {
        ASSERT_TRUE(is_due) << "a binding refreshed at " << b.last_refresh << " left early";
      } else if (!limited) {
        ASSERT_FALSE(((deadline + Mask()) & ~Mask()) <= (now_ & ~Mask()))
            << "an unlimited expiry left a binding due at " << deadline;
      }
    }
    if (limited && removed < due) budget_stops++;
    for (const Ep &in : gone) {
      const Ep ext = bindings_.at(in).external;
      ASSERT_EQ(nullptr, nat_->Find(AsEndpoint(ext)));
      used_[{ext.addr, ClassOf(ext.proto)}].erase(ext.port);
      ext_to_int_.erase(ext);
      bindings_.erase(in);
    }
    expired += removed;
  }

  void CheckAll() {
    ASSERT_EQ(bindings_.size(), nat_->size());
    for (const auto &[in, b] : bindings_) {
      const Binding *got = nat_->Find(AsEndpoint(in));
      ASSERT_NE(nullptr, got);
      ASSERT_EQ(AsEndpoint(in), got->internal);
      ASSERT_EQ(AsEndpoint(b.external), got->external);
      ASSERT_EQ(b.last_refresh, got->last_refresh) << "refreshed by outbound packets only";
      ASSERT_EQ(got, nat_->Find(AsEndpoint(b.external))) << "one binding, two keys";
    }
  }

  NatModelConfig cfg_;
  std::mt19937 rng_;
  uint64_t now_ = 0;
  std::unique_ptr<Nat> nat_;
  std::vector<uint32_t> ext_ips_;
  std::vector<uint32_t> hosts_;
  std::vector<uint16_t> interesting_ports_;
  std::map<Ep, ModelBinding> bindings_;  // internal -> binding
  std::map<Ep, Ep> ext_to_int_;
  std::map<std::pair<uint32_t, int>, std::set<uint16_t>> used_;  // (external IP, class) -> ports
  std::vector<Ep> externals_seen_;  // every external endpoint ever handed out
};

ExternalAddress Pub(uint32_t a, std::vector<PortRange> r) { return {be32_t(a), std::move(r)}; }

TEST(NatModelTest, RandomTrafficBothDirectionsWithExpiryMatchesABindingModel) {
  constexpr uint32_t kA1 = 0xc6336401, kA2 = 0xc6336402;
  std::vector<NatModelConfig> configs;
  // Small spans: exhaustion per (IP, class), a suspended range, an address
  // listed twice (one shared bitmap), privileged spans empty on A2.
  const std::vector<ExternalAddress> small = {
      Pub(kA1, {{1010, 1030, false}, {2000, 2008, true}, {3000, 3006, false}}),
      Pub(kA2, {{60000, 60012, false}}),
      Pub(kA1, {{1020, 1036, false}}),
  };
  for (uint32_t seed = 1; seed <= 4; seed++) configs.push_back({seed, 64, 0, 300, small, 30000});
  // Growing tables (TP5): 4 -> 8 -> ... -> 64 while traffic, expiry and kFull
  // at every intermediate size go on.
  for (uint32_t seed = 21; seed <= 24; seed++) {
    configs.push_back({seed, 4, 0, 300, small, 30000, 64});
  }
  // A small table: kFull dominates.
  configs.push_back({11, 6, 0, 300, small, 20000});
  // A coarse wheel: removal within one granule of the deadline.
  configs.push_back({12, 64, 4, 300, small, 30000});
  // One wide range: capacity before ports.
  configs.push_back({13, 24, 0, 200, {Pub(kA1, {{1, 65536, false}})}, 20000});
  std::map<Verdict, size_t> total;
  size_t expired = 0, budget_stops = 0, grown = 0;
  for (const auto &cfg : configs) {
    SCOPED_TRACE(::testing::Message() << "seed " << cfg.seed);
    NatModel model(cfg);
    model.Run();
    if (::testing::Test::HasFatalFailure()) return;
    for (const auto &[v, n] : model.verdicts) total[v] += n;
    expired += model.expired;
    budget_stops += model.budget_stops;
    grown += model.grown;
  }
  // Every verdict and path the model distinguishes was reached.
  for (const Verdict v : {Verdict::kTranslated, Verdict::kNotIpv4, Verdict::kUnsupported,
                          Verdict::kNoBinding, Verdict::kPortZero, Verdict::kExhausted,
                          Verdict::kFull, Verdict::kConflict}) {
    EXPECT_GT(total[v], 0u) << "verdict " << static_cast<int>(v);
  }
  EXPECT_GT(expired, 0u);
  EXPECT_GT(budget_stops, 0u);
  EXPECT_GE(grown, 8u) << "the growing configurations grew";
}

}  // namespace
}  // namespace bess::nat
