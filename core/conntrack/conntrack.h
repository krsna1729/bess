// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONNTRACK_CONNTRACK_H_
#define BESS_CONNTRACK_CONNTRACK_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>

#include "conntrack/packet_parse.h"
#include "dataplane/batch_tuning.h"
#include "dataplane/expiry_wheel.h"
#include "flow/worker_flow_table.h"

namespace bess::conntrack {

// Connection tracking (M17, D-067): protocol-aware, bidirectional connection
// state over the generic flow table (M9) and expiry wheel (M10). Mechanism
// only: no firewall rule, NAT policy or action syntax. A firewall or NAT
// application asks Track() what a packet is to the connection it belongs to
// (new, existing, related, invalid, untracked), decides itself, and may keep
// its own per-connection data in the entry (UserData).
//
// One worker owns a Conntrack (the flow table is worker-owned). TCP follows
// Linux nf_conntrack: tcp_error()'s flag-combination check, then the state
// table; sequence numbers and windows are not tracked (an out-of-window
// segment is not detected).

enum class Direction : uint8_t { kOriginal, kReply };

// Linux nf_conntrack's TCP states (and their order).
enum class TcpState : uint8_t {
  kNone,
  kSynSent,
  kSynRecv,
  kEstablished,
  kFinWait,
  kCloseWait,
  kLastAck,
  kTimeWait,
  kClose,
  kSynSent2,  // simultaneous open
};
inline constexpr size_t kTcpStates = 10;

// Both endpoints in a canonical order (the lesser (address, port) is A), so
// both directions of a connection have one key. 40 bytes, no padding.
struct CtKey {
  std::array<uint8_t, 16> addr_a;
  std::array<uint8_t, 16> addr_b;
  uint16_t port_a;
  uint16_t port_b;
  uint8_t protocol;
  uint8_t family;  // 4 or 6
  uint16_t zone;   // the caller's separation (a VRF, a tenant): same tuple, other zone, other connection
};
static_assert(sizeof(CtKey) == 40);
static_assert(std::has_unique_object_representations_v<CtKey>);

struct CanonicalKey {
  CtKey key;
  bool src_is_a;  // the packet's source is endpoint A
};

static_assert(offsetof(CtKey, port_a) == 32 && offsetof(CtKey, zone) == 38);

// The key of the connection a packet belongs to. Built with word stores: the
// hash reads the key in 8-byte words, and a word assembled from narrow stores
// defeats store forwarding (D-064, D-067 measured it).
inline CanonicalKey MakeKey(const ParsedFlowPacket &p, uint16_t zone) noexcept {
  static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "CtKey tail word layout");
  uint64_t s[2], d[2];
  std::memcpy(s, p.src.data(), 16);
  std::memcpy(d, p.dst.data(), 16);
  // Address order is byte order: compare big-endian words.
  const uint64_t s0 = __builtin_bswap64(s[0]), d0 = __builtin_bswap64(d[0]);
  const uint64_t s1 = __builtin_bswap64(s[1]), d1 = __builtin_bswap64(d[1]);
  CanonicalKey c;
  c.src_is_a = s0 < d0 || (s0 == d0 && (s1 < d1 || (s1 == d1 && p.src_port <= p.dst_port)));
  const uint64_t *a = c.src_is_a ? s : d;
  const uint64_t *b = c.src_is_a ? d : s;
  std::memcpy(c.key.addr_a.data(), a, 16);
  std::memcpy(c.key.addr_b.data(), b, 16);
  const uint16_t port_a = c.src_is_a ? p.src_port : p.dst_port;
  const uint16_t port_b = c.src_is_a ? p.dst_port : p.src_port;
  const uint64_t tail = uint64_t{port_a} | uint64_t{port_b} << 16 | uint64_t{p.protocol} << 32 |
                        uint64_t{p.l3 == L3Kind::kIpv4 ? 4u : 6u} << 40 | uint64_t{zone} << 48;
  std::memcpy(&c.key.port_a, &tail, sizeof(tail));
  return c;
}

// Timeouts in ticks (the caller's clock; defaults are Linux's in seconds, so
// multiply for another unit with Scaled()).
struct TimeoutPolicy {
  std::array<uint64_t, kTcpStates> tcp = {
      10,      // none (not used)
      120,     // SYN sent
      60,      // SYN received
      432000,  // established (5 days)
      120,     // FIN wait
      60,      // CLOSE wait
      30,      // LAST ACK
      120,     // TIME WAIT
      10,      // CLOSE
      120,     // SYN sent 2
  };
  uint64_t udp_unreplied = 30;
  uint64_t udp_replied = 120;
  uint64_t icmp = 30;
  uint64_t other = 600;
  // Track a TCP connection first seen mid-stream (an ACK without a SYN), as
  // Linux's nf_conntrack_tcp_loose does; off: such a packet is kInvalid.
  bool tcp_pickup = false;

  TimeoutPolicy Scaled(uint64_t ticks_per_second) const noexcept {
    TimeoutPolicy p = *this;
    for (auto &t : p.tcp) t *= ticks_per_second;
    p.udp_unreplied *= ticks_per_second;
    p.udp_replied *= ticks_per_second;
    p.icmp *= ticks_per_second;
    p.other *= ticks_per_second;
    return p;
  }
};

enum class TrackStatus : uint8_t {
  kNew,        // this packet created the connection
  kExisting,   // a packet of a tracked connection, accepted by its state machine
  kRelated,    // an ICMP error about a tracked connection (handle names it)
  kInvalid,    // no connection and not one that may start one, an invalid TCP
               // flag combination, or rejected by the TCP state machine;
               // nothing changed
  kUntracked,  // not trackable: a non-initial fragment, ICMP neither echo nor error
  kFull,       // would start a connection, but the table is full
};

struct NoUserData {};

namespace ct_internal {
// Linux nf_conntrack_proto_tcp.c tcp_conntracks[dir][flag class][state].
// kIg: ignore (no change, packet accepted); kIv: invalid.
inline constexpr uint8_t kIv = 0xfe, kIg = 0xff;
enum FlagClass : uint8_t { kSyn, kSynAck, kFin, kAck, kRst, kNoFlags, kFlagClasses };
using S = TcpState;
inline constexpr uint8_t s(S x) { return static_cast<uint8_t>(x); }
// Columns: None, SynSent, SynRecv, Established, FinWait, CloseWait, LastAck, TimeWait, Close, SynSent2.
inline constexpr uint8_t kTcpTable[2][kFlagClasses][kTcpStates] = {
    {
        /*syn*/ {s(S::kSynSent), s(S::kSynSent), kIg, kIg, kIg, kIg, kIg, s(S::kSynSent), s(S::kSynSent), s(S::kSynSent2)},
        /*synack*/ {kIv, kIv, s(S::kSynRecv), kIv, kIv, kIv, kIv, kIv, kIv, s(S::kSynRecv)},
        /*fin*/ {kIv, kIv, s(S::kFinWait), s(S::kFinWait), s(S::kLastAck), s(S::kLastAck), s(S::kLastAck), s(S::kTimeWait), s(S::kClose), kIv},
        /*ack*/ {s(S::kEstablished), kIv, s(S::kEstablished), s(S::kEstablished), s(S::kCloseWait), s(S::kCloseWait), s(S::kTimeWait), s(S::kTimeWait), s(S::kClose), kIv},
        /*rst*/ {kIv, s(S::kClose), s(S::kClose), s(S::kClose), s(S::kClose), s(S::kClose), s(S::kClose), s(S::kClose), s(S::kClose), s(S::kClose)},
        /*none*/ {kIv, kIv, kIv, kIv, kIv, kIv, kIv, kIv, kIv, kIv},
    },
    {
        /*syn*/ {kIv, s(S::kSynSent2), kIv, kIv, kIv, kIv, kIv, s(S::kSynSent), kIv, s(S::kSynSent2)},
        /*synack*/ {kIv, s(S::kSynRecv), kIg, kIg, kIg, kIg, kIg, kIg, kIg, s(S::kSynRecv)},
        /*fin*/ {kIv, kIv, s(S::kFinWait), s(S::kFinWait), s(S::kLastAck), s(S::kLastAck), s(S::kLastAck), s(S::kTimeWait), s(S::kClose), kIv},
        /*ack*/ {kIv, kIg, s(S::kSynRecv), s(S::kEstablished), s(S::kCloseWait), s(S::kCloseWait), s(S::kTimeWait), s(S::kTimeWait), s(S::kClose), kIg},
        /*rst*/ {kIv, s(S::kClose), s(S::kClose), s(S::kClose), s(S::kClose), s(S::kClose), s(S::kClose), s(S::kClose), s(S::kClose), s(S::kClose)},
        /*none*/ {kIv, kIv, kIv, kIv, kIv, kIv, kIv, kIv, kIv, kIv},
    },
};

inline FlagClass ClassOf(uint8_t flags) noexcept {
  if (flags & kTcpRst) return kRst;
  if (flags & kTcpSyn) return (flags & kTcpAck) ? kSynAck : kSyn;
  if (flags & kTcpFin) return kFin;
  if (flags & kTcpAck) return kAck;
  return kNoFlags;
}

// Linux nf_conntrack_proto_tcp.c tcp_error(): before the state table, a
// segment whose flags, with PSH, ECE and CWR ignored, are not one of
// tcp_valid_flags' combinations is invalid ("invalid tcp flag combination").
// Valid: SYN, SYN|URG, SYN|ACK, RST, RST|ACK, FIN|ACK, FIN|ACK|URG, ACK,
// ACK|URG. Everything else -- no flags, FIN alone, SYN|FIN, SYN|RST, FIN|RST,
// SYN|ACK|URG, URG alone, ... -- is refused. The checked bits (FIN, SYN, RST,
// ACK, URG = 0x37) index a 64-bit set: one shift, no memory load.
inline constexpr uint8_t kTcpUrg = 0x20;
inline constexpr uint8_t kCheckedFlags = kTcpFin | kTcpSyn | kTcpRst | kTcpAck | kTcpUrg;
inline constexpr uint64_t FlagSet(uint8_t flags) { return uint64_t{1} << flags; }
inline constexpr uint64_t kValidFlagSets =
    FlagSet(kTcpSyn) | FlagSet(kTcpSyn | kTcpUrg) | FlagSet(kTcpSyn | kTcpAck) | FlagSet(kTcpRst) |
    FlagSet(kTcpRst | kTcpAck) | FlagSet(kTcpFin | kTcpAck) | FlagSet(kTcpFin | kTcpAck | kTcpUrg) |
    FlagSet(kTcpAck) | FlagSet(kTcpAck | kTcpUrg);
inline bool ValidFlags(uint8_t flags) noexcept {
  return (kValidFlagSets >> (flags & kCheckedFlags)) & 1;
}

inline bool IsIcmpError(L4Kind k, uint8_t type) noexcept {
  return k == L4Kind::kIcmp ? (type == 3 || type == 4 || type == 5 || type == 11 || type == 12)
                            : (type >= 1 && type <= 4);
}
inline bool IsEchoRequest(L4Kind k, uint8_t type) noexcept {
  return k == L4Kind::kIcmp ? type == 8 : type == 128;
}
inline bool IsEchoReply(L4Kind k, uint8_t type) noexcept {
  return k == L4Kind::kIcmp ? type == 0 : type == 129;
}

// The tuple quoted in an ICMP error (header plus the first 8 bytes of its L4,
// so the parse is lenient about the quoted packet's stated length), written
// as a ParsedFlowPacket. False if the quote is too short.
inline bool ParseQuoted(std::span<const uint8_t> q, bool v6, ParsedFlowPacket &out) noexcept {
  out = ParsedFlowPacket{};
  size_t off;
  if (!v6) {
    if (q.size() < 20 || (q[0] >> 4) != 4) return false;
    off = static_cast<size_t>(q[0] & 0x0f) * 4;
    if (off < 20 || off > q.size()) return false;
    out.l3 = L3Kind::kIpv4;
    out.protocol = q[9];
    std::memcpy(out.src.data(), q.data() + 12, 4);
    std::memcpy(out.dst.data(), q.data() + 16, 4);
    if ((parse_internal::Be16(q.data() + 6) & 0x1fff) != 0) {
      return false;  // a non-initial fragment: no ports to read
    }
  } else {
    if (q.size() < 40 || (q[0] >> 4) != 6) return false;
    off = 40;
    out.l3 = L3Kind::kIpv6;
    out.protocol = q[6];  // extension headers in a quote are not followed
    std::memcpy(out.src.data(), q.data() + 8, 16);
    std::memcpy(out.dst.data(), q.data() + 24, 16);
  }
  if (out.protocol == 6 || out.protocol == 17) {
    if (q.size() < off + 4) return false;
    out.src_port = parse_internal::Be16(q.data() + off);
    out.dst_port = parse_internal::Be16(q.data() + off + 2);
  } else if (out.protocol == 1 || out.protocol == 58) {
    if (q.size() < off + 8) return false;
    // Only an echo has an identifier there; other types carry an MTU, a
    // pointer or nothing.
    const uint8_t t = q[off];
    if (out.protocol == 1 ? (t != 0 && t != 8) : (t != 128 && t != 129)) return false;
    out.src_port = out.dst_port = parse_internal::Be16(q.data() + off + 4);
  }
  return true;
}
}  // namespace ct_internal

template <typename UserData = NoUserData>
  requires std::is_trivially_copyable_v<UserData>
class Conntrack {
 public:
  using Tick = uint64_t;
  using Wheel = dataplane::ExpiryWheel<flow::FlowHandle, Tick>;

  struct Entry {
    TcpState tcp = TcpState::kNone;
    bool initiator_is_a = true;  // endpoint A sent the first packet
    bool replied = false;        // a packet has gone in the reply direction
    dataplane::ExpiryHandle timer{};
    UserData user{};
  };
  using Table = flow::WorkerFlowTable<CtKey, Entry>;

  struct Result {
    TrackStatus status = TrackStatus::kInvalid;
    Direction direction = Direction::kOriginal;
    flow::FlowHandle handle{};
    Entry *entry = nullptr;  // null for kInvalid, kUntracked, kFull
  };

  enum class CreateError : uint8_t { kInvalidConfig, kOutOfMemory };

  static std::expected<std::unique_ptr<Conntrack>, CreateError> Create(
      size_t capacity, const TimeoutPolicy &policy = TimeoutPolicy{}, Tick start = 0,
      unsigned granularity_shift = 0) {
    auto table = Table::Create(capacity);
    if (!table) {
      return std::unexpected(table.error() == flow::FlowTableError::kOutOfMemory
                                 ? CreateError::kOutOfMemory
                                 : CreateError::kInvalidConfig);
    }
    auto wheel = Wheel::Create(capacity, start, granularity_shift);
    if (!wheel) {
      return std::unexpected(wheel.error() == dataplane::ExpiryError::kOutOfMemory
                                 ? CreateError::kOutOfMemory
                                 : CreateError::kInvalidConfig);
    }
    std::unique_ptr<Conntrack> ct(
        new (std::nothrow) Conntrack(std::move(*table), std::move(*wheel), policy));
    if (ct == nullptr) {
      return std::unexpected(CreateError::kOutOfMemory);
    }
    // Staged only once the table outgrows L2 (BESS_LOOKUP_BODY overrides).
    // D-006's L1d threshold is too low here: measured, staging costs
    // conntrack 14-22% with 1K connections (L2-resident) and saves 16-39%
    // at 64K and 58-64% at 1M.
    const dataplane::LookupBody forced = dataplane::LookupBodyOverride();
    ct->body_ = forced != dataplane::LookupBody::kAuto ? forced
                : ct->table_->memory_bytes() > dataplane::CacheGeometry::Smallest().l2_bytes
                    ? dataplane::LookupBody::kStaged
                    : dataplane::LookupBody::kPlain;
    return ct;
  }

  // What `p` is to its connection, updating the connection's state and
  // timeout. `frame` holds the bytes `p` was parsed from (an ICMP error's
  // quote is read from it). `may_create` false: a packet that would start a
  // connection is kInvalid instead (the caller's policy refused it).
  Result Track(std::span<const uint8_t> frame, const ParsedFlowPacket &p, Tick now,
               uint16_t zone = 0, bool may_create = true) noexcept {
    if (!Keyed(p)) {
      return Unkeyed(frame, p, zone);
    }
    const CanonicalKey c = MakeKey(p, zone);
    return Resolve(p, c, table_->FindRef(c.key), now, may_create);
  }

  static constexpr size_t kMaxBatch = Table::kMaxBatch;

  // Track for a batch, in order: the connection lookups go together (the
  // table prefetches the whole batch), then each packet is resolved as Track
  // would. A miss is looked up again, as an earlier packet of the batch may
  // have created its connection. Same results as Track packet by packet.
  // Staged or plain as chosen at Create from the table's footprint (staged
  // once it outgrows L2; BESS_LOOKUP_BODY overrides).
  void TrackBatch(std::span<const std::span<const uint8_t>> frames,
                  std::span<const ParsedFlowPacket> parsed, Tick now, std::span<Result> out,
                  uint16_t zone = 0, bool may_create = true) noexcept {
    promise(frames.size() <= kMaxBatch && parsed.size() == frames.size() &&
            out.size() >= frames.size());
    if (body_ == dataplane::LookupBody::kPlain) {
      for (size_t i = 0; i < frames.size(); i++) {
        out[i] = Track(frames[i], parsed[i], now, zone, may_create);
      }
      return;
    }
    CanonicalKey keys[kMaxBatch];
    CtKey lookup[kMaxBatch];
    uint8_t where[kMaxBatch];
    size_t n = 0;
    for (size_t i = 0; i < frames.size(); i++) {
      if (Keyed(parsed[i])) [[likely]] {
        keys[n] = MakeKey(parsed[i], zone);
        lookup[n] = keys[n].key;
        where[n++] = static_cast<uint8_t>(i);
      }
    }
    flow::FlowRef<Entry> refs[kMaxBatch];
    (void)table_->FindRefBatch(std::span<const CtKey>(lookup, n),
                               std::span<flow::FlowRef<Entry>>(refs, n));
    size_t k = 0;
    for (size_t i = 0; i < frames.size(); i++) {
      if (k < n && where[k] == i) {
        const flow::FlowRef<Entry> ref = refs[k].state != nullptr ? refs[k] : table_->FindRef(lookup[k]);
        out[i] = Resolve(parsed[i], keys[k], ref, now, may_create);
        k++;
      } else {
        out[i] = Unkeyed(frames[i], parsed[i], zone);
      }
    }
  }

 private:
  // A packet that has a connection key: parsed, and not an ICMP error or an
  // ICMP type conntrack does not follow.
  static bool Keyed(const ParsedFlowPacket &p) noexcept {
    if (p.l3 == L3Kind::kNone || p.l4 == L4Kind::kNone) {
      return false;
    }
    if (p.l4 == L4Kind::kIcmp || p.l4 == L4Kind::kIcmpv6) {
      return ct_internal::IsEchoRequest(p.l4, p.icmp_type) ||
             ct_internal::IsEchoReply(p.l4, p.icmp_type);
    }
    return true;
  }

  // What Track answers for a packet without a key.
  Result Unkeyed(std::span<const uint8_t> frame, const ParsedFlowPacket &p,
                 uint16_t zone) noexcept {
    if (p.l3 != L3Kind::kNone && p.l4 != L4Kind::kNone &&
        (p.l4 == L4Kind::kIcmp || p.l4 == L4Kind::kIcmpv6) &&
        ct_internal::IsIcmpError(p.l4, p.icmp_type)) {
      return Related(frame, p, zone);
    }
    return {TrackStatus::kUntracked};
  }

  // The rest of Track once the key has been looked up.
  Result Resolve(const ParsedFlowPacket &p, const CanonicalKey &c, const flow::FlowRef<Entry> &ref,
                 Tick now, bool may_create) noexcept {
    if (ref.state != nullptr) {
      Entry &e = *ref.state;
      const Direction dir = c.src_is_a == e.initiator_is_a ? Direction::kOriginal : Direction::kReply;
      if (p.l4 == L4Kind::kTcp) {
        if (!ct_internal::ValidFlags(p.tcp_flags)) {
          return {TrackStatus::kInvalid, dir};  // tcp_error(): nothing changes
        }
        const uint8_t next =
            ct_internal::kTcpTable[static_cast<int>(dir)][ct_internal::ClassOf(p.tcp_flags)]
                                  [static_cast<int>(e.tcp)];
        if (next == ct_internal::kIv) {
          return {TrackStatus::kInvalid, dir};
        }
        // A SYN that takes TIME_WAIT or CLOSE back to SYN_SENT opens a new
        // connection on the old tuple: as Linux does (it kills the old entry
        // and re-evaluates the packet), the sender becomes the initiator and
        // nothing of the old connection is kept.
        if (next == static_cast<uint8_t>(TcpState::kSynSent) &&
            (e.tcp == TcpState::kTimeWait || e.tcp == TcpState::kClose)) {
          if (!may_create) {
            return {TrackStatus::kInvalid, dir};
          }
          const dataplane::ExpiryHandle timer = e.timer;
          e = Entry{};
          e.tcp = TcpState::kSynSent;
          e.initiator_is_a = c.src_is_a;
          e.timer = timer;
          (void)wheel_->Refresh(e.timer, Wheel::After(now, TimeoutOf(p.l4, e)));
          return {TrackStatus::kNew, Direction::kOriginal, ref.handle, &e};
        }
        if (next != ct_internal::kIg) {
          e.tcp = static_cast<TcpState>(next);
        }
      }
      if (dir == Direction::kReply) {
        e.replied = true;
      }
      (void)wheel_->Refresh(e.timer, Wheel::After(now, TimeoutOf(p.l4, e)));
      return {TrackStatus::kExisting, dir, ref.handle, &e};
    }
    // No connection: may this packet start one?
    Entry fresh;
    fresh.initiator_is_a = c.src_is_a;
    if (p.l4 == L4Kind::kTcp) {
      const auto cls = ct_internal::ClassOf(p.tcp_flags);
      if (!ct_internal::ValidFlags(p.tcp_flags)) {
        return {TrackStatus::kInvalid};  // tcp_error(): creates nothing
      }
      if (cls == ct_internal::kSyn) {
        fresh.tcp = TcpState::kSynSent;
      } else if (cls == ct_internal::kAck && policy_.tcp_pickup) {
        fresh.tcp = TcpState::kEstablished;
      } else {
        return {TrackStatus::kInvalid};
      }
    } else if ((p.l4 == L4Kind::kIcmp || p.l4 == L4Kind::kIcmpv6) &&
               !ct_internal::IsEchoRequest(p.l4, p.icmp_type)) {
      return {TrackStatus::kInvalid};  // an echo reply with no request
    }
    if (!may_create) {
      return {TrackStatus::kInvalid};
    }
    if (wheel_->full()) {
      return {TrackStatus::kFull};
    }
    auto made = table_->Emplace(c.key, fresh);
    if (!made.created()) {
      return {TrackStatus::kFull};
    }
    made.state->timer = wheel_->Schedule(Wheel::After(now, TimeoutOf(p.l4, *made.state)), made.handle);
    if (made.state->timer == dataplane::kNoExpiry) {  // a quarantined wheel node
      (void)table_->Erase(made.handle);
      return {TrackStatus::kFull};
    }
    return {TrackStatus::kNew, Direction::kOriginal, made.handle, made.state};
  }

 public:
  // Removes connections whose timeout has passed by `now`, doing at most
  // `budget` units of wheel work. Returns how many left.
  size_t Expire(Tick now, size_t budget) noexcept {
    size_t removed = 0;
    (void)wheel_->Poll(now, budget, [this, &removed](const flow::FlowHandle &h) noexcept {
      removed += table_->Erase(h) ? 1 : 0;
    });
    return removed;
  }

  // -- state query and update ----------------------------------------------------

  Entry *Find(const CtKey &key) noexcept { return table_->Find(key); }
  Entry *Lookup(flow::FlowHandle h) noexcept { return table_->Lookup(h); }
  const CtKey *KeyOf(flow::FlowHandle h) const noexcept { return table_->KeyOf(h); }
  // Ends a connection now (a firewall's verdict, a management command).
  bool Remove(flow::FlowHandle h) noexcept {
    Entry *e = table_->Lookup(h);
    if (e == nullptr) {
      return false;
    }
    (void)wheel_->Cancel(e->timer);
    return table_->Erase(h);
  }
  // Re-arms a connection's timeout to `deadline` (a policy that wants a
  // longer or shorter life than the defaults).
  bool SetDeadline(flow::FlowHandle h, Tick deadline) noexcept {
    Entry *e = table_->Lookup(h);
    return e != nullptr && wheel_->Refresh(e->timer, deadline);
  }

  size_t size() const noexcept { return table_->size(); }
  // How TrackBatch runs: staged or plain (chosen at Create).
  dataplane::LookupBody batch_body() const noexcept { return body_; }
  void SetBatchBodyForTesting(dataplane::LookupBody body) noexcept { body_ = body; }
  size_t capacity() const noexcept { return table_->capacity(); }
  const TimeoutPolicy &policy() const noexcept { return policy_; }

 private:
  Conntrack(std::unique_ptr<Table> table, std::unique_ptr<Wheel> wheel, const TimeoutPolicy &policy)
      : table_(std::move(table)), wheel_(std::move(wheel)), policy_(policy) {}

  Tick TimeoutOf(L4Kind l4, const Entry &e) const noexcept {
    switch (l4) {
      case L4Kind::kTcp:
        return policy_.tcp[static_cast<size_t>(e.tcp)];
      case L4Kind::kUdp:
        return e.replied ? policy_.udp_replied : policy_.udp_unreplied;
      case L4Kind::kIcmp:
      case L4Kind::kIcmpv6:
        return policy_.icmp;
      default:
        return policy_.other;
    }
  }

  // An ICMP error: the quoted packet went from us toward the error's sender,
  // so its tuple, as written, is the connection's.
  Result Related(std::span<const uint8_t> frame, const ParsedFlowPacket &p, uint16_t zone) noexcept {
    const size_t quote = size_t{p.l4_offset} + 8;
    if (frame.size() < quote) {
      return {TrackStatus::kInvalid};
    }
    const size_t ip_end = std::min(frame.size(), size_t{p.l4_offset} + p.l4_length);
    if (ip_end < quote) {
      return {TrackStatus::kInvalid};
    }
    ParsedFlowPacket inner;
    if (!ct_internal::ParseQuoted(frame.subspan(quote, ip_end - quote),
                                  p.l3 == L3Kind::kIpv6, inner) ||
        inner.l3 != p.l3) {
      return {TrackStatus::kInvalid};
    }
    const CanonicalKey c = MakeKey(inner, zone);
    const auto ref = table_->FindRef(c.key);
    if (ref.state == nullptr) {
      return {TrackStatus::kInvalid};
    }
    // The error travels opposite to the quoted packet.
    const Direction quoted = c.src_is_a == ref.state->initiator_is_a ? Direction::kOriginal
                                                                     : Direction::kReply;
    const Direction dir = quoted == Direction::kOriginal ? Direction::kReply : Direction::kOriginal;
    return {TrackStatus::kRelated, dir, ref.handle, ref.state};
  }

  std::unique_ptr<Table> table_;
  std::unique_ptr<Wheel> wheel_;
  dataplane::LookupBody body_ = dataplane::LookupBody::kStaged;
  TimeoutPolicy policy_;
};

}  // namespace bess::conntrack

#endif  // BESS_CONNTRACK_CONNTRACK_H_
