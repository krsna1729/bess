// SPDX-License-Identifier: BSD-3-Clause

// Conntrack (M17, D-067) against two reference models (M22).
//
// 1. The TCP state model: Linux nf_conntrack's tcp_error() flag filter, then
//    its tcp_conntracks[dir][index][state] table and get_conntrack_index()
//    (nf_conntrack_proto_tcp.c), encoded below from the Linux source: the
//    filter as the set of tcp_valid_flags combinations, the table in Linux's
//    own mnemonics (sNO..sS2, sIV invalid, sIG ignore). Flag classes in
//    Linux's precedence: RST, then SYN (SYN|ACK when ACK is set), then FIN,
//    then ACK, else none.
//    How the tracker uses them (D-067):
//      - the filter first: flags & ~(PSH|ECE|CWR) must be one of SYN,
//        SYN|URG, SYN|ACK, RST, RST|ACK, FIN|ACK, FIN|ACK|URG, ACK, ACK|URG;
//        otherwise kInvalid and nothing changes or is created (an existing
//        connection's direction is still reported).
//      - an existing connection: the packet's direction is original when its
//        sender is the connection's initiator, else reply. sIV: kInvalid,
//        nothing changes (no state, no timeout, no `replied`). sIG: the state
//        stays. A transition to sSS from sTW or sCL is a reopen: the SYN's
//        sender becomes the initiator, `replied` is cleared, the state is sSS,
//        the verdict kNew (original) on the same flow handle; with
//        may_create false it is kInvalid and nothing changes.
//      - no connection: a SYN creates sSS; an ACK creates sES only with
//        tcp_pickup; any other TCP packet is kInvalid.
//    Not modelled (not done by the tracker, D-067): sequence/window tracking,
//    tcp_error()'s length and checksum checks (the parser's and the
//    caller's), the sIG "last index" bookkeeping.
//
// 2. The connection-table model: canonical key (zone, protocol, family, the
//    lesser (address, port) endpoint, the greater one) -> {TCP state,
//    initiator endpoint, replied, deadline, flow handle}.
//      - Track(p, now, zone, may_create):
//          ICMP other than echo request/reply -> kUntracked (errors are
//          not sent here);
//          existing connection: TCP per the table above; UDP/ICMP/other
//          accepted; `replied` set by an accepted reply-direction packet;
//          the deadline becomes now + the timeout of the (new) state:
//          TCP: tcp[state]; UDP: udp_replied once replied, else
//          udp_unreplied; ICMP: icmp; other protocols: other.
//          no connection: TCP per the table; an ICMP echo reply -> kInvalid;
//          may_create false -> kInvalid; size == capacity -> kFull;
//          else kNew, original, the sender is the initiator.
//      - Expire(now, budget): a connection is due when its deadline, rounded
//        up to the wheel granularity 2^g, is at or before now rounded down to
//        it (serial-number arithmetic: ticks may wrap). With an unlimited
//        budget exactly the due connections leave. With a budget b at most b
//        leave, only due ones, and in non-decreasing (rounded) deadline order:
//        none left while an earlier-due one stays.
//      - Remove(handle): true and gone for a live connection, false for a
//        dead handle; SetDeadline(handle, d) re-arms the deadline to d.
//      - A connection's handle is stable for its life (reopen included) and
//        dead once it left.

#include "conntrack/conntrack.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <compare>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <utility>
#include <vector>

namespace bess::conntrack {
namespace {

// -- the TCP state model (Linux) --------------------------------------------------

enum : uint8_t { sNO, sSS, sSR, sES, sFW, sCW, sLA, sTW, sCL, sS2, sIV, sIG };
enum : uint8_t { kOrig = 0, kRepl = 1 };
enum : uint8_t { iSYN, iSYNACK, iFIN, iACK, iRST, iNONE };

// tcp_conntracks from Linux nf_conntrack_proto_tcp.c.
constexpr uint8_t kLinux[2][6][10] = {
    {
        /* ORIGINAL   sNO  sSS  sSR  sES  sFW  sCW  sLA  sTW  sCL  sS2 */
        /* syn    */ {sSS, sSS, sIG, sIG, sIG, sIG, sIG, sSS, sSS, sS2},
        /* synack */ {sIV, sIV, sSR, sIV, sIV, sIV, sIV, sIV, sIV, sSR},
        /* fin    */ {sIV, sIV, sFW, sFW, sLA, sLA, sLA, sTW, sCL, sIV},
        /* ack    */ {sES, sIV, sES, sES, sCW, sCW, sTW, sTW, sCL, sIV},
        /* rst    */ {sIV, sCL, sCL, sCL, sCL, sCL, sCL, sCL, sCL, sCL},
        /* none   */ {sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV},
    },
    {
        /* REPLY      sNO  sSS  sSR  sES  sFW  sCW  sLA  sTW  sCL  sS2 */
        /* syn    */ {sIV, sS2, sIV, sIV, sIV, sIV, sIV, sSS, sIV, sS2},
        /* synack */ {sIV, sSR, sIG, sIG, sIG, sIG, sIG, sIG, sIG, sSR},
        /* fin    */ {sIV, sIV, sFW, sFW, sLA, sLA, sLA, sTW, sCL, sIV},
        /* ack    */ {sIV, sIG, sSR, sES, sCW, sCW, sTW, sTW, sCL, sIG},
        /* rst    */ {sIV, sCL, sCL, sCL, sCL, sCL, sCL, sCL, sCL, sCL},
        /* none   */ {sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV, sIV},
    },
};

// get_conntrack_index() from the same file.
uint8_t LinuxIndex(uint8_t flags) {
  constexpr uint8_t kFin = 0x01, kSyn = 0x02, kRst = 0x04, kAck = 0x10;
  if (flags & kRst) return iRST;
  if (flags & kSyn) return (flags & kAck) ? iSYNACK : iSYN;
  if (flags & kFin) return iFIN;
  if (flags & kAck) return iACK;
  return iNONE;
}

// tcp_valid_flags and its use in tcp_error(), same file: PUSH, ECE and CWR are
// always valid; the rest must be one of these combinations.
bool LinuxValidFlags(uint8_t flags) {
  constexpr uint8_t kFin = 0x01, kSyn = 0x02, kRst = 0x04, kPsh = 0x08, kAck = 0x10, kUrg = 0x20,
                    kEce = 0x40, kCwr = 0x80;
  static const std::set<uint8_t> kValid = {
      kSyn, kSyn | kUrg, kSyn | kAck, kRst, kRst | kAck, kFin | kAck, kFin | kAck | kUrg, kAck,
      kAck | kUrg,
  };
  return kValid.count(static_cast<uint8_t>(flags & ~(kEce | kCwr | kPsh))) != 0;
}

TcpState AsState(uint8_t s) { return static_cast<TcpState>(s); }

// -- packets -----------------------------------------------------------------------

struct ModelEndpoint {
  std::array<uint8_t, 16> addr{};
  uint16_t port = 0;
  auto operator<=>(const ModelEndpoint &) const = default;
};

struct Flow {
  L4Kind l4;
  uint8_t protocol;
  bool v6;
  ModelEndpoint x, y;  // for ICMP both ports are the echo identifier
  uint16_t zone;
};

ParsedFlowPacket Packet(const Flow &f, bool from_x, uint8_t tcp_flags, uint8_t icmp_type) {
  ParsedFlowPacket p;
  p.l3 = f.v6 ? L3Kind::kIpv6 : L3Kind::kIpv4;
  p.l4 = f.l4;
  p.protocol = f.protocol;
  p.l3_offset = 14;
  p.l4_offset = f.v6 ? 54 : 34;
  p.l4_length = 20;
  const ModelEndpoint &s = from_x ? f.x : f.y;
  const ModelEndpoint &d = from_x ? f.y : f.x;
  p.src = s.addr;
  p.dst = d.addr;
  p.src_port = s.port;
  p.dst_port = d.port;
  p.tcp_flags = tcp_flags;
  p.icmp_type = icmp_type;
  return p;
}

ModelEndpoint V4(uint32_t a, uint16_t port) {
  ModelEndpoint e;
  e.addr[0] = static_cast<uint8_t>(a >> 24);
  e.addr[1] = static_cast<uint8_t>(a >> 16);
  e.addr[2] = static_cast<uint8_t>(a >> 8);
  e.addr[3] = static_cast<uint8_t>(a);
  e.port = port;
  return e;
}

ModelEndpoint V6(uint8_t hi, uint8_t lo, uint16_t port) {
  ModelEndpoint e;
  e.addr = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, hi, 0, 0, 0, 0, 0, 0, 0, lo};
  e.port = port;
  return e;
}

using Ct = Conntrack<>;

// -- 2. the connection-table model -------------------------------------------------

struct ModelKey {
  uint16_t zone;
  uint8_t protocol;
  uint8_t family;
  ModelEndpoint a, b;  // a <= b
  auto operator<=>(const ModelKey &) const = default;
};

ModelKey KeyOf(const Flow &f) {
  ModelKey k{f.zone, f.protocol, static_cast<uint8_t>(f.v6 ? 6 : 4), f.x, f.y};
  if (k.b < k.a) std::swap(k.a, k.b);
  return k;
}

CtKey AsCtKey(const ModelKey &k) {
  CtKey c{};
  c.addr_a = k.a.addr;
  c.addr_b = k.b.addr;
  c.port_a = k.a.port;
  c.port_b = k.b.port;
  c.protocol = k.protocol;
  c.family = k.family;
  c.zone = k.zone;
  return c;
}

struct ModelConn {
  L4Kind l4;
  uint8_t tcp = sNO;
  ModelEndpoint initiator;
  bool replied = false;
  uint64_t deadline = 0;
  flow::FlowHandle handle{};
};

struct ModelConfig {
  uint32_t seed;
  size_t capacity;
  unsigned shift;  // wheel granularity
  uint64_t start;  // first tick (near the wrap for some runs)
  bool pickup;
  size_t steps;
};

class TableModel {
 public:
  explicit TableModel(const ModelConfig &cfg) : cfg_(cfg), rng_(cfg.seed), now_(cfg.start) {
    policy_.tcp = {5, 13, 11, 400, 17, 12, 9, 19, 6, 15};
    policy_.udp_unreplied = 10;
    policy_.udp_replied = 30;
    policy_.icmp = 8;
    policy_.other = 50;
    policy_.tcp_pickup = cfg.pickup;
    auto made = Ct::Create(cfg.capacity, policy_, cfg.start, cfg.shift);
    EXPECT_TRUE(made.has_value());
    ct_ = std::move(*made);
    MakeFlows();
  }

  void Run() {
    for (size_t step = 0; step < cfg_.steps; step++) {
      SCOPED_TRACE(::testing::Message() << "step " << step << " now " << now_);
      const uint32_t op = rng_() % 100;
      if (op < 78) {
        Track();
      } else if (op < 86) {
        Expire();
      } else if (op < 90) {
        Remove();
      } else if (op < 92) {
        SetDeadline();
      } else {
        Advance();
      }
      if (::testing::Test::HasFatalFailure()) return;
      ASSERT_EQ(model_.size(), ct_->size());
      if (step % 64 == 0) {
        CheckAll();
        if (::testing::Test::HasFatalFailure()) return;
      }
    }
    CheckAll();
  }

  enum Count { kNewC, kFullC, kInvalidC, kReopenC, kRefusedC, kExpiredC, kBudgetStopC,
               kUntrackedC, kBadFlagsC, kCounts };
  size_t count(Count c) const { return counts_[c]; }

 private:

  uint64_t Mask() const { return (uint64_t{1} << cfg_.shift) - 1; }
  // Serial-number "a is at or before b".
  static bool NotAfter(uint64_t a, uint64_t b) { return static_cast<int64_t>(b - a) >= 0; }
  uint64_t RoundUp(uint64_t t) const { return (t + Mask()) & ~Mask(); }
  bool Due(const ModelConn &c) const { return NotAfter(RoundUp(c.deadline), now_ & ~Mask()); }

  uint64_t TimeoutOf(const ModelConn &c) const {
    switch (c.l4) {
      case L4Kind::kTcp: return policy_.tcp[c.tcp];
      case L4Kind::kUdp: return c.replied ? policy_.udp_replied : policy_.udp_unreplied;
      case L4Kind::kIcmp:
      case L4Kind::kIcmpv6: return policy_.icmp;
      default: return policy_.other;
    }
  }

  void MakeFlows() {
    // Few hosts and ports, so tuples collide across protocols, zones and
    // directions; 44 flows over a 16-entry table.
    const uint16_t ports[] = {80, 443, 40000, 40001};
    for (int i = 0; i < 44; i++) {
      Flow f;
      const uint32_t kind = rng_() % 10;
      f.v6 = rng_() % 4 == 0;
      f.zone = static_cast<uint16_t>(rng_() % 3 == 0 ? 7 : 0);
      if (kind < 5) {
        f.l4 = L4Kind::kTcp, f.protocol = 6;
      } else if (kind < 7) {
        f.l4 = L4Kind::kUdp, f.protocol = 17;
      } else if (kind < 9) {
        f.l4 = f.v6 ? L4Kind::kIcmpv6 : L4Kind::kIcmp, f.protocol = f.v6 ? 58 : 1;
      } else {
        f.l4 = L4Kind::kOther, f.protocol = 47;
      }
      const uint8_t hx = static_cast<uint8_t>(1 + rng_() % 3), hy = static_cast<uint8_t>(1 + rng_() % 3);
      uint16_t px = ports[rng_() % 4], py = ports[rng_() % 4];
      if (f.l4 == L4Kind::kIcmp || f.l4 == L4Kind::kIcmpv6) px = py = static_cast<uint16_t>(rng_() % 3);
      if (f.l4 == L4Kind::kOther) px = py = 0;
      f.x = f.v6 ? V6(hx, hx, px) : V4(0x0a000000u | hx, px);
      f.y = f.v6 ? V6(hy, static_cast<uint8_t>(hy + 9), py) : V4(0x0a000100u | hy, py);
      flows_.push_back(f);
    }
  }

  uint8_t RandomFlags() {
    // Common segments, the invalid combinations tcp_error() names (FIN, SYN|FIN,
    // SYN|RST, none), and any byte (PSH, URG, ECE and CWR included).
    static constexpr uint8_t kCommon[] = {0x02, 0x12, 0x11, 0x10, 0x18, 0x04, 0x14,
                                          0x01, 0x00, 0x03, 0x06, 0xd0, 0x52};
    return rng_() % 4 == 0 ? static_cast<uint8_t>(rng_()) : kCommon[rng_() % 13];
  }

  void Track() {
    const Flow &f = flows_[rng_() % flows_.size()];
    const bool from_x = rng_() % 2;
    const bool icmp = f.l4 == L4Kind::kIcmp || f.l4 == L4Kind::kIcmpv6;
    uint8_t type = 0;
    if (icmp) {
      const uint32_t r = rng_() % 8;
      // echo request, echo reply, or a query that is neither (untracked)
      type = f.v6 ? (r < 4 ? 128 : r < 7 ? 129 : 135) : (r < 4 ? 8 : r < 7 ? 0 : 13);
    }
    const uint8_t flags = f.l4 == L4Kind::kTcp ? RandomFlags() : 0;
    const bool may_create = rng_() % 6 != 0;
    const ParsedFlowPacket p = Packet(f, from_x, flags, type);
    const ModelEndpoint sender = from_x ? f.x : f.y;
    const ModelKey key = KeyOf(f);

    // The model's verdict.
    TrackStatus want = TrackStatus::kInvalid;
    Direction want_dir = Direction::kOriginal;
    bool reopen = false;
    auto it = model_.find(key);
    const bool echo_request = icmp && (type == 8 || type == 128);
    const bool echo_reply = icmp && (type == 0 || type == 129);
    if (icmp && !echo_request && !echo_reply) {
      want = TrackStatus::kUntracked;
    } else if (it != model_.end()) {
      ModelConn &c = it->second;
      const uint8_t dir = sender == c.initiator ? kOrig : kRepl;
      want_dir = dir == kOrig ? Direction::kOriginal : Direction::kReply;
      uint8_t next = c.tcp;
      bool accepted = true;
      if (f.l4 == L4Kind::kTcp && !LinuxValidFlags(flags)) {
        accepted = false;
        counts_[kBadFlagsC]++;
      } else if (f.l4 == L4Kind::kTcp) {
        next = kLinux[dir][LinuxIndex(flags)][c.tcp];
        if (next == sIV) {
          accepted = false;
        } else if (next == sSS && (c.tcp == sTW || c.tcp == sCL)) {
          reopen = true;
          if (!may_create) {
            accepted = false;
            counts_[kRefusedC]++;
          }
        } else if (next == sIG) {
          next = c.tcp;
        }
      }
      if (!accepted) {
        want = TrackStatus::kInvalid;
      } else if (reopen) {
        want = TrackStatus::kNew;
        want_dir = Direction::kOriginal;
        c.tcp = sSS;
        c.initiator = sender;
        c.replied = false;
        c.deadline = now_ + TimeoutOf(c);
        counts_[kReopenC]++;
      } else {
        want = TrackStatus::kExisting;
        c.tcp = next;
        if (dir == kRepl) c.replied = true;
        c.deadline = now_ + TimeoutOf(c);
      }
    } else {
      ModelConn c;
      c.l4 = f.l4;
      c.initiator = sender;
      bool may_start = true;
      if (f.l4 == L4Kind::kTcp && !LinuxValidFlags(flags)) {
        may_start = false;
        counts_[kBadFlagsC]++;
      } else if (f.l4 == L4Kind::kTcp) {
        const uint8_t cls = LinuxIndex(flags);
        if (cls == iSYN) {
          c.tcp = sSS;
        } else if (cls == iACK && cfg_.pickup) {
          c.tcp = sES;
        } else {
          may_start = false;
        }
      } else if (echo_reply) {
        may_start = false;
      }
      if (!may_start || !may_create) {
        want = TrackStatus::kInvalid;
        counts_[kRefusedC] += may_start;
      } else if (model_.size() == cfg_.capacity) {
        want = TrackStatus::kFull;
      } else {
        want = TrackStatus::kNew;
        c.deadline = now_ + TimeoutOf(c);
        it = model_.emplace(key, c).first;
      }
    }

    const Ct::Result r = ct_->Track({}, p, now_, f.zone, may_create);
    ASSERT_EQ(want, r.status) << "flags " << int{flags} << " type " << int{type} << " may_create "
                              << may_create;
    ASSERT_EQ(want_dir, r.direction);
    if (want == TrackStatus::kNew || want == TrackStatus::kExisting) {
      ModelConn &c = it->second;
      ASSERT_NE(nullptr, r.entry);
      if (want == TrackStatus::kNew && !reopen) {
        c.handle = r.handle;
        counts_[kNewC]++;
      } else {
        ASSERT_EQ(c.handle, r.handle) << "a connection keeps its handle";
      }
      ASSERT_EQ(r.entry, ct_->Lookup(r.handle));
      const CtKey *k = ct_->KeyOf(r.handle);
      ASSERT_NE(nullptr, k);
      const CtKey want_key = AsCtKey(key);
      ASSERT_EQ(0, std::memcmp(&want_key, k, sizeof(CtKey))) << "canonical key";
      ExpectEntry(c, key, *r.entry);
    } else {
      ASSERT_EQ(nullptr, r.entry);
      counts_[want == TrackStatus::kFull ? kFullC
              : want == TrackStatus::kUntracked ? kUntrackedC
                                                : kInvalidC]++;
    }
  }

  void ExpectEntry(const ModelConn &c, const ModelKey &key, const Ct::Entry &e) {
    ASSERT_EQ(AsState(c.tcp), e.tcp);
    ASSERT_EQ(c.replied, e.replied);
    ASSERT_EQ(c.initiator == key.a, e.initiator_is_a);
  }

  void Expire() {
    std::set<ModelKey> due;
    for (const auto &[k, c] : model_) {
      if (Due(c)) due.insert(k);
    }
    const bool limited = rng_() % 2;
    const size_t budget = limited ? 1 + rng_() % 4 : size_t{1} << 30;
    const size_t removed = ct_->Expire(now_, budget);
    std::vector<ModelKey> gone;
    for (const auto &[k, c] : model_) {
      const CtKey ck = AsCtKey(k);
      if (ct_->Find(ck) == nullptr) gone.push_back(k);
    }
    ASSERT_EQ(gone.size(), removed);
    ASSERT_LE(removed, budget);
    for (const ModelKey &k : gone) {
      ASSERT_TRUE(due.count(k)) << "a connection left before its deadline";
    }
    if (!limited) {
      ASSERT_EQ(due.size(), removed) << "an unlimited expiry leaves nothing due";
    } else {
      if (removed < due.size()) counts_[kBudgetStopC]++;
      // Order: nothing left while an earlier-due connection stays.
      for (const ModelKey &g : gone) {
        for (const ModelKey &d : due) {
          if (std::find(gone.begin(), gone.end(), d) != gone.end()) continue;
          ASSERT_TRUE(NotAfter(RoundUp(model_.at(g).deadline), RoundUp(model_.at(d).deadline)))
              << "expired out of deadline order";
        }
      }
    }
    for (const ModelKey &k : gone) {
      const flow::FlowHandle h = model_.at(k).handle;
      ASSERT_EQ(nullptr, ct_->Lookup(h)) << "the handle of a connection that left is dead";
      model_.erase(k);
    }
    counts_[kExpiredC] += removed;
  }

  void Remove() {
    if (!model_.empty() && rng_() % 4 != 0) {
      auto it = model_.begin();
      std::advance(it, rng_() % model_.size());
      const flow::FlowHandle h = it->second.handle;
      ASSERT_TRUE(ct_->Remove(h));
      model_.erase(it);
      ASSERT_EQ(nullptr, ct_->Lookup(h));
      ASSERT_FALSE(ct_->Remove(h)) << "a dead handle removes nothing";
      dead_.push_back(h);
    } else if (!dead_.empty()) {
      ASSERT_FALSE(ct_->Remove(dead_[rng_() % dead_.size()]));
      ASSERT_FALSE(ct_->SetDeadline(dead_[rng_() % dead_.size()], now_ + 5));
    }
  }

  void SetDeadline() {
    if (model_.empty()) return;
    auto it = model_.begin();
    std::advance(it, rng_() % model_.size());
    const uint64_t d = now_ + 1 + rng_() % 60;
    ASSERT_TRUE(ct_->SetDeadline(it->second.handle, d));
    it->second.deadline = d;
  }

  void Advance() {
    now_ += rng_() % 16 == 0 ? 200 + rng_() % 300 : rng_() % 8;
  }

  void CheckAll() {
    ASSERT_EQ(model_.size(), ct_->size());
    for (const auto &[k, c] : model_) {
      const CtKey ck = AsCtKey(k);
      const Ct::Entry *e = ct_->Find(ck);
      ASSERT_NE(nullptr, e) << "a live connection is findable by its canonical key";
      ASSERT_EQ(e, ct_->Lookup(c.handle));
      ExpectEntry(c, k, *e);
    }
  }

  ModelConfig cfg_;
  std::mt19937 rng_;
  uint64_t now_;
  TimeoutPolicy policy_;
  std::unique_ptr<Ct> ct_;
  std::vector<Flow> flows_;
  std::map<ModelKey, ModelConn> model_;
  std::vector<flow::FlowHandle> dead_;
  size_t counts_[kCounts] = {};
};

TEST(ConntrackModelTest, RandomTrafficExpiryAndRemovalMatchAConnectionTableModel) {
  const uint64_t kNearWrap = ~uint64_t{0} - 3000;  // ticks wrap during the run
  const ModelConfig configs[] = {
      {1, 16, 0, 0, false, 40000},
      {2, 16, 0, kNearWrap, true, 40000},
      {3, 16, 3, 0, true, 40000},
      {4, 16, 2, kNearWrap & ~uint64_t{3}, false, 40000},
      {5, 5, 0, 1000, false, 20000},
      {6, 40, 0, 0, true, 40000},
  };
  size_t total[TableModel::kCounts] = {};
  for (const ModelConfig &cfg : configs) {
    SCOPED_TRACE(::testing::Message() << "seed " << cfg.seed);
    TableModel model(cfg);
    model.Run();
    if (::testing::Test::HasFatalFailure()) return;
    for (int c = 0; c < TableModel::kCounts; c++) {
      total[c] += model.count(static_cast<TableModel::Count>(c));
    }
  }
  // Every path the model distinguishes was reached: creation, refusal by
  // capacity, invalid packets, reopens, may_create refusals, expiry, a
  // budget that stopped early, untracked ICMP.
  for (int c = 0; c < TableModel::kCounts; c++) {
    EXPECT_GT(total[c], 0u) << "count " << c;
  }
}

// -- 1. the TCP state model, every reachable cell ----------------------------------

// From a fresh tracker, a breadth-first walk over the model's states: each
// state reached by a shortest packet path (with the side that sent each
// packet), then every one of the 256 flag bytes from either side is replayed
// onto that path and the verdict, direction and resulting state compared
// with the Linux filter and table. Covers every (state, direction, flag
// class) cell the tracker can reach, reopens from both sides included.
TEST(ConntrackModelTest, TcpTransitionsMatchTheLinuxTableInEveryReachableState) {
  struct Step {
    bool from_client;
    uint8_t flags;
  };
  // A node: the model state and which side is the initiator.
  struct Node {
    uint8_t state;
    bool client_initiates;
    std::vector<Step> path;
  };
  const Flow flow{L4Kind::kTcp, 6, false, V4(0x0a000001, 40000), V4(0x0a000002, 80), 0};
  for (const bool pickup : {false, true}) {
    SCOPED_TRACE(::testing::Message() << "pickup " << pickup);
    TimeoutPolicy policy;  // long timeouts: nothing expires
    policy.tcp_pickup = pickup;
    std::set<std::pair<uint8_t, bool>> seen;
    std::deque<Node> queue;
    bool covered[2][6][10] = {};        // any flag byte of the class
    bool covered_valid[2][6][10] = {};  // a byte tcp_error() lets through
    // Creation: every flag byte from the client on a fresh tracker.
    for (int flags = 0; flags < 256; flags++) {
      auto ct = Ct::Create(4, policy).value();
      const auto r = ct->Track({}, Packet(flow, true, static_cast<uint8_t>(flags), 0), 1);
      const uint8_t cls = LinuxIndex(static_cast<uint8_t>(flags));
      const bool creates =
          LinuxValidFlags(static_cast<uint8_t>(flags)) && (cls == iSYN || (pickup && cls == iACK));
      ASSERT_EQ(creates ? TrackStatus::kNew : TrackStatus::kInvalid, r.status) << flags;
      ASSERT_EQ(creates ? 1u : 0u, ct->size()) << flags;
      if (creates) {
        const uint8_t s = kLinux[kOrig][cls][sNO];
        ASSERT_EQ(AsState(s), r.entry->tcp);
        if (seen.insert({s, true}).second) {
          queue.push_back({s, true, {{true, static_cast<uint8_t>(flags)}}});
        }
      }
    }
    size_t replays = 0;
    while (!queue.empty()) {
      const Node node = queue.front();
      queue.pop_front();
      for (const bool from_client : {true, false}) {
        for (int flags = 0; flags < 256; flags++) {
          auto ct = Ct::Create(4, policy).value();
          uint64_t now = 1;
          for (const Step &s : node.path) {
            const auto r = ct->Track({}, Packet(flow, s.from_client, s.flags, 0), now++);
            ASSERT_TRUE(r.status == TrackStatus::kNew || r.status == TrackStatus::kExisting);
          }
          const Ct::Entry before = *ct->Find(AsCtKey(KeyOf(flow)));
          ASSERT_EQ(AsState(node.state), before.tcp);
          const uint8_t dir = from_client == node.client_initiates ? kOrig : kRepl;
          const uint8_t cls = LinuxIndex(static_cast<uint8_t>(flags));
          const bool valid = LinuxValidFlags(static_cast<uint8_t>(flags));
          covered[dir][cls][node.state] = true;
          covered_valid[dir][cls][node.state] |= valid;
          uint8_t next = valid ? kLinux[dir][cls][node.state] : uint8_t{sIV};
          const auto r = ct->Track({}, Packet(flow, from_client, static_cast<uint8_t>(flags), 0), now);
          replays++;
          SCOPED_TRACE(::testing::Message() << "state " << int{node.state} << " dir " << int{dir}
                                            << " flags " << flags);
          const Ct::Entry after = *ct->Find(AsCtKey(KeyOf(flow)));
          if (next == sIV) {
            ASSERT_EQ(TrackStatus::kInvalid, r.status);
            ASSERT_EQ(dir == kOrig ? Direction::kOriginal : Direction::kReply, r.direction);
            ASSERT_EQ(before.tcp, after.tcp);
            ASSERT_EQ(before.replied, after.replied);
            continue;
          }
          bool client_initiates = node.client_initiates;
          if (next == sSS && (node.state == sTW || node.state == sCL)) {
            ASSERT_EQ(TrackStatus::kNew, r.status) << "a reopen";
            ASSERT_EQ(Direction::kOriginal, r.direction);
            ASSERT_FALSE(after.replied);
            client_initiates = from_client;
            // Refused when the caller may not create.
            auto again = Ct::Create(4, policy).value();
            for (const Step &s : node.path) {
              (void)again->Track({}, Packet(flow, s.from_client, s.flags, 0), 1);
            }
            ASSERT_EQ(TrackStatus::kInvalid,
                      again->Track({}, Packet(flow, from_client, static_cast<uint8_t>(flags), 0), 2,
                                   0, false)
                          .status);
            ASSERT_EQ(AsState(node.state), again->Find(AsCtKey(KeyOf(flow)))->tcp);
          } else {
            ASSERT_EQ(TrackStatus::kExisting, r.status);
            ASSERT_EQ(dir == kOrig ? Direction::kOriginal : Direction::kReply, r.direction);
            if (next == sIG) next = node.state;
            ASSERT_EQ(before.replied || dir == kRepl, after.replied);
          }
          ASSERT_EQ(AsState(next), after.tcp);
          const Flow &f = flow;
          ASSERT_EQ(client_initiates == (KeyOf(f).a == f.x), after.initiator_is_a);
          if (seen.insert({next, client_initiates}).second) {
            Node child{next, client_initiates, node.path};
            child.path.push_back({from_client, static_cast<uint8_t>(flags)});
            queue.push_back(child);
          }
        }
      }
    }
    // Every cell of every state an existing connection can be in (all but
    // sNO), from both directions and in all six flag classes, was tested;
    // and every cell of the five classes a valid flag byte can have (the
    // no-flag class has none) was tested with a byte the filter passes.
    for (int dir = 0; dir < 2; dir++) {
      for (int cls = 0; cls < 6; cls++) {
        for (int s = sSS; s <= sS2; s++) {
          EXPECT_TRUE(covered[dir][cls][s]) << dir << " " << cls << " " << s;
          EXPECT_EQ(cls != iNONE, covered_valid[dir][cls][s]) << dir << " " << cls << " " << s;
        }
      }
    }
    // Both initiator sides of all nine states were reached (reverse reopens).
    EXPECT_EQ(18u, seen.size());
    EXPECT_GT(replays, 9000u);
  }
}

// tcp_error()'s filter on its own, with deadlines (which the walk above
// cannot see): every flag byte Linux refuses, from either side, is kInvalid
// and creates nothing on a fresh tracker (pickup on), and on a connection --
// established, or SYN_SENT with nothing replied yet -- leaves the state,
// `replied` and the deadline as they were. A valid byte with PSH, ECE and CWR
// set is still accepted.
TEST(ConntrackModelTest, InvalidTcpFlagCombinationsChangeNothingAndCreateNothing) {
  constexpr uint8_t F = 0x01, S = 0x02, R = 0x04, P = 0x08, A = 0x10, U = 0x20, E = 0x40, C = 0x80;
  // The combinations scanners and broken stacks send, all refused by Linux.
  for (const uint8_t named : {uint8_t{0}, F, uint8_t(S | F), uint8_t(S | R), uint8_t(F | R), U, P,
                              uint8_t(F | P | U), uint8_t(S | A | U), uint8_t(R | A | U),
                              uint8_t(S | R | A), uint8_t(E | C), uint8_t(0x3f)}) {
    EXPECT_FALSE(LinuxValidFlags(named)) << int{named};
  }
  std::vector<uint8_t> invalid;
  for (int flags = 0; flags < 256; flags++) {
    if (!LinuxValidFlags(static_cast<uint8_t>(flags))) invalid.push_back(static_cast<uint8_t>(flags));
  }
  // 9 valid combinations, each with any of PSH, ECE and CWR.
  ASSERT_EQ(256u - 9 * 8, invalid.size());

  TimeoutPolicy policy;
  policy.tcp[static_cast<size_t>(TcpState::kSynSent)] = 60;
  policy.tcp[static_cast<size_t>(TcpState::kEstablished)] = 100;
  policy.tcp_pickup = true;  // an ACK may create: the filter must still refuse ACK|FIN|RST
  const Flow flow{L4Kind::kTcp, 6, false, V4(0x0a000001, 40000), V4(0x0a000002, 80), 0};
  const CtKey key = AsCtKey(KeyOf(flow));
  for (const uint8_t flags : invalid) {
    for (const bool from_client : {true, false}) {
      SCOPED_TRACE(::testing::Message() << "flags " << int{flags} << " from client " << from_client);
      // No connection: nothing is created.
      auto ct = Ct::Create(4, policy).value();
      auto r = ct->Track({}, Packet(flow, from_client, flags, 0), 0);
      ASSERT_EQ(TrackStatus::kInvalid, r.status);
      ASSERT_EQ(nullptr, r.entry);
      ASSERT_EQ(0u, ct->size());

      // SYN_SENT, nothing replied; deadline 1 + 60.
      ASSERT_EQ(TrackStatus::kNew, ct->Track({}, Packet(flow, true, S, 0), 1).status);
      r = ct->Track({}, Packet(flow, from_client, flags, 0), 30);
      ASSERT_EQ(TrackStatus::kInvalid, r.status);
      ASSERT_EQ(from_client ? Direction::kOriginal : Direction::kReply, r.direction);
      ASSERT_EQ(nullptr, r.entry);
      ASSERT_EQ(TcpState::kSynSent, ct->Find(key)->tcp);
      ASSERT_FALSE(ct->Find(key)->replied);
      ASSERT_EQ(0u, ct->Expire(60, 1u << 20)) << "expired early";
      ASSERT_EQ(1u, ct->Expire(61, 1u << 20)) << "the invalid segment refreshed the deadline";

      // ESTABLISHED; deadline 4 + 100.
      ASSERT_EQ(TrackStatus::kNew, ct->Track({}, Packet(flow, true, S, 0), 2).status);
      ASSERT_EQ(TrackStatus::kExisting, ct->Track({}, Packet(flow, false, S | A, 0), 3).status);
      ASSERT_EQ(TrackStatus::kExisting, ct->Track({}, Packet(flow, true, A, 0), 4).status);
      ASSERT_EQ(TcpState::kEstablished, ct->Find(key)->tcp);
      r = ct->Track({}, Packet(flow, from_client, flags, 0), 50);
      ASSERT_EQ(TrackStatus::kInvalid, r.status);
      ASSERT_EQ(from_client ? Direction::kOriginal : Direction::kReply, r.direction);
      ASSERT_EQ(TcpState::kEstablished, ct->Find(key)->tcp);
      ASSERT_EQ(0u, ct->Expire(103, 1u << 20)) << "expired early";
      ASSERT_EQ(1u, ct->Expire(104, 1u << 20)) << "the invalid segment refreshed the deadline";
    }
  }
  // Valid combinations carrying PSH, ECE and CWR pass the filter.
  auto ct = Ct::Create(4, policy).value();
  ASSERT_EQ(TrackStatus::kNew, ct->Track({}, Packet(flow, true, S | E | C, 0), 1).status);
  ASSERT_EQ(TrackStatus::kExisting, ct->Track({}, Packet(flow, false, S | A | E, 0), 2).status);
  ASSERT_EQ(TrackStatus::kExisting, ct->Track({}, Packet(flow, true, A | P | U | C, 0), 3).status);
  ASSERT_EQ(TcpState::kEstablished, ct->Find(key)->tcp);
  ASSERT_EQ(TrackStatus::kExisting, ct->Track({}, Packet(flow, true, F | A | P | U, 0), 4).status);
  ASSERT_EQ(TcpState::kFinWait, ct->Find(key)->tcp);
}

}  // namespace
}  // namespace bess::conntrack
