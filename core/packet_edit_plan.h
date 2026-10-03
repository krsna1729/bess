// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_PACKET_EDIT_PLAN_H_
#define BESS_PACKET_EDIT_PLAN_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <rte_mbuf.h>

#include "packet.h"
#include "packet_mutation.h"
#include "utils/logging.h"

namespace bess::packet {

// A bounded, packet-only edit plan (roadmap M13, Decision D-063; experimental).
//
// A dynamic policy compiler (a VFP layer stack, an OVS action list, a UPF FAR)
// that knows at compile time what it will do to every packet of a flow can
// build one EditPlan per flow and apply it per packet, instead of parsing and
// mutating field by field. The plan is data, not a language: a short sequence
// of fixed-size steps over the packet's leading bytes, executed with no heap
// allocation and no virtual dispatch.
//
// Allowed: replace a prefix (remove n bytes, prepend m), write fixed bytes,
// copy bytes, adjust a 16-bit ones-complement checksum by a precomputed
// delta, set a 16-bit length field from the packet length, and recompute an
// outer IPv4 header checksum from a precomputed partial sum. Nothing else: no
// metering, routing, drop decisions, branches or loops (the roadmap's
// forbidden list). Applications with fixed rewrites should keep writing typed
// C++; existing typed rewrites do not go through this.
//
// Build time (EditPlanBuilder::Build) does what need not be done per packet:
// adjacent writes are merged, a remove followed by a prepend becomes one
// prefix replacement (in place when it shrinks or keeps the length), checksum
// deltas for fixed field changes are folded once, and the contiguous writable
// range the plan touches is derived. Apply() then checks that range, the
// headroom and the storage's writeability once, and performs the steps.
//
// Byte offsets are relative to the packet's first byte after the prefix
// replacement. 16-bit checksum and length fields are in network order.

enum class EditError : uint8_t {
  kNeedsReshape,  // the first segment is shorter than the plan's range, or its
                  // storage is shared: EnsureWritable/EnsureContiguous first
  kNoHeadroom,    // the prefix replacement needs more headroom than is free
  kTooShort,      // the packet is shorter than the bytes the plan removes
};

enum class EditBuildError : uint8_t {
  kTooManySteps,  // more than EditPlan::kMaxSteps after merging
  kTooMuchData,   // more than EditPlan::kMaxData bytes of literal data
  kOutOfRange,    // an offset or length beyond 16 bits, or a zero length
};

class EditPlan {
 public:
  static constexpr size_t kMaxSteps = 12;
  static constexpr size_t kMaxData = 160;

  enum class Op : uint8_t {
    kWrite,          // data[aux .. aux+len) -> packet[offset ..)
    kCopy,           // packet[aux .. aux+len) -> packet[offset ..)
    kAddChecksum,    // 16-bit field at offset += aux (ones complement)
    kSetLength,      // 16-bit field at offset = packet length - aux
    kIpv4Checksum,   // field at offset = fold(partial(aux|len<<16) + len field)
  };
  struct Step {
    Op op;
    uint8_t len;      // bytes (kWrite, kCopy)
    uint16_t offset;  // destination
    uint32_t aux;     // data index, source offset, delta, base or partial sum
  };
  static_assert(sizeof(Step) == 8);

  // Checks the packet once (length, the plan's range in the first segment,
  // headroom, writeable storage) and performs the prefix replacement; returns
  // the first byte. Apply() runs the steps after it. On error nothing has
  // changed. Public so that other step representations can share it.
  std::expected<uint8_t *, EditError> ApplyPrefix(PacketRef packet) const noexcept {
    ::rte_mbuf *m = packet.handle();
    if (m->pkt_len < remove_) [[unlikely]] {
      return std::unexpected(EditError::kTooShort);
    }
    // Range the plan touches in the first segment, before the prefix change.
    const int64_t needed = static_cast<int64_t>(range_) + remove_ - prepend_;
    if (static_cast<int64_t>(m->data_len) < needed || m->data_len < remove_ ||
        PayloadWriteabilityOf(packet) != PayloadWriteability::kWritable)
        [[unlikely]] {
      return std::unexpected(EditError::kNeedsReshape);
    }
    if (prepend_ > remove_ &&
        prepend_ - remove_ > rte_pktmbuf_headroom(m)) [[unlikely]] {
      return std::unexpected(EditError::kNoHeadroom);
    }
    // One data_off/len adjustment for the whole prefix replacement.
    const int delta = static_cast<int>(prepend_) - static_cast<int>(remove_);
    m->data_off = static_cast<uint16_t>(m->data_off - delta);
    m->data_len = static_cast<uint16_t>(m->data_len + delta);
    m->pkt_len = static_cast<uint32_t>(static_cast<int64_t>(m->pkt_len) + delta);
    return rte_pktmbuf_mtod(m, uint8_t *);
  }

  // Applies the plan. On error nothing has changed.
  std::expected<void, EditError> Apply(PacketRef packet) const noexcept {
    const auto prefix = ApplyPrefix(packet);
    if (!prefix) [[unlikely]] {
      return std::unexpected(prefix.error());
    }
    RunSteps(*prefix, packet.handle()->pkt_len);
    return {};
  }

  // Applies the plan without the per-packet checks, for a caller whose parse
  // contract already proves them: the first segment holds the plan's range,
  // the storage is writable, and the headroom covers the prefix growth (for
  // example packets from a pool this application owns, already parsed to the
  // headers the plan edits). Debug builds check; a violation in release is
  // memory corruption.
  void ApplyTrusted(PacketRef packet) const noexcept {
    ::rte_mbuf *m = packet.handle();
    DCHECK(m->data_len >= static_cast<int64_t>(range_) + remove_ - prepend_ &&
           m->data_len >= remove_);
    DCHECK(PayloadWriteabilityOf(packet) == PayloadWriteability::kWritable);
    DCHECK(prepend_ <= remove_ || prepend_ - remove_ <= rte_pktmbuf_headroom(m));
    const int delta = static_cast<int>(prepend_) - static_cast<int>(remove_);
    m->data_off = static_cast<uint16_t>(m->data_off - delta);
    m->data_len = static_cast<uint16_t>(m->data_len + delta);
    m->pkt_len = static_cast<uint32_t>(static_cast<int64_t>(m->pkt_len) + delta);
    RunSteps(rte_pktmbuf_mtod(m, uint8_t *), m->pkt_len);
  }

 private:
  void RunSteps(uint8_t *p, uint32_t pkt_len) const noexcept {
    for (size_t i = 0; i < steps_; i++) {
      const Step &s = step_[i];
      switch (s.op) {
        case Op::kWrite:
          std::memcpy(p + s.offset, data_.data() + s.aux, s.len);
          break;
        case Op::kCopy:
          std::memmove(p + s.offset, p + s.aux, s.len);
          break;
        case Op::kAddChecksum: {
          uint16_t c;
          std::memcpy(&c, p + s.offset, 2);
          uint32_t sum = static_cast<uint16_t>(~c) + s.aux;
          sum = (sum & 0xFFFF) + (sum >> 16);
          sum = (sum & 0xFFFF) + (sum >> 16);
          c = static_cast<uint16_t>(~sum);
          std::memcpy(p + s.offset, &c, 2);
          break;
        }
        case Op::kSetLength: {
          const uint16_t v = rte_cpu_to_be_16(static_cast<uint16_t>(pkt_len - s.aux));
          std::memcpy(p + s.offset, &v, 2);
          break;
        }
        case Op::kIpv4Checksum: {
          // aux: low 16 bits = offset of the length field, high bits = which
          // precomputed partial sum (of every other header word) to use.
          uint16_t len_field;
          std::memcpy(&len_field, p + (s.aux & 0xFFFF), 2);
          uint32_t sum = partial_[s.aux >> 16] + len_field;
          sum = (sum & 0xFFFF) + (sum >> 16);
          sum = (sum & 0xFFFF) + (sum >> 16);
          const uint16_t c = static_cast<uint16_t>(~sum);
          std::memcpy(p + s.offset, &c, 2);
          break;
        }
      }
    }
  }

 public:
  std::expected<void, EditError> Apply(PacketHandle packet) const noexcept {
    return Apply(PacketRef(packet));
  }

  size_t steps() const noexcept { return steps_; }
  std::span<const Step> step_list() const noexcept { return {step_.data(), steps_}; }
  uint16_t remove_bytes() const noexcept { return remove_; }
  uint16_t prepend_bytes() const noexcept { return prepend_; }
  // Bytes from the (post-replacement) start that the steps touch.
  uint16_t range() const noexcept { return range_; }
  std::span<const uint8_t> data() const noexcept { return data_; }
  uint32_t ipv4_partial(size_t i) const noexcept { return partial_[i]; }

 private:
  friend class EditPlanBuilder;

  std::array<Step, kMaxSteps> step_{};
  std::array<uint8_t, kMaxData> data_{};
  std::array<uint32_t, 2> partial_{};  // IPv4 partial sums (at most two headers)
  uint8_t steps_ = 0;
  uint16_t remove_ = 0;
  uint16_t prepend_ = 0;
  uint16_t range_ = 0;
};

// Builds an EditPlan. Calls describe the edits in packet order; Build()
// merges and checks them.
class EditPlanBuilder {
 public:
  // Removes `bytes` from the front (an outer header to strip).
  EditPlanBuilder &RemovePrefix(uint16_t bytes) {
    remove_ += bytes;
    return *this;
  }
  // Prepends `header` (an encapsulation). Its bytes become offsets 0..n-1, and
  // the original packet (after any RemovePrefix) follows.
  EditPlanBuilder &Prepend(std::span<const uint8_t> header) {
    if (header.size() > 0xFFFF) {
      error_ = EditBuildError::kOutOfRange;
      return *this;
    }
    prepend_ = static_cast<uint16_t>(header.size());
    Write(0, header);
    return *this;
  }
  EditPlanBuilder &Write(uint16_t offset, std::span<const uint8_t> bytes) {
    if (bytes.empty() || offset + bytes.size() > 0xFFFF) {
      error_ = EditBuildError::kOutOfRange;
      return *this;
    }
    // Merge into the previous write when adjacent or overlapping.
    if (!pending_.empty() && pending_.back().op == EditPlan::Op::kWrite) {
      Pending &prev = pending_.back();
      if (offset >= prev.offset && offset <= prev.offset + prev.bytes.size()) {
        const size_t at = offset - prev.offset;
        if (at + bytes.size() > prev.bytes.size()) {
          prev.bytes.resize(at + bytes.size());
        }
        std::memcpy(prev.bytes.data() + at, bytes.data(), bytes.size());
        return *this;
      }
    }
    pending_.push_back(Pending{EditPlan::Op::kWrite, offset, 0,
                               std::vector<uint8_t>(bytes.begin(), bytes.end())});
    return *this;
  }
  EditPlanBuilder &Copy(uint16_t to, uint16_t from, uint16_t length) {
    if (length == 0 || length > 255) {
      error_ = EditBuildError::kOutOfRange;
      return *this;
    }
    pending_.push_back(Pending{EditPlan::Op::kCopy, to, from, {}, length});
    return *this;
  }
  // The checksum at `offset` covers a field that changes from `old_bytes` to
  // `new_bytes` (same length, even, as they appear in the packet): the
  // ones-complement delta is computed here, once. Preconditions, which the
  // plan cannot check:
  //  - every packet the plan is applied to carries `old_bytes` there: the old
  //    value is fixed by the flow match (an address or port in the key), not a
  //    per-packet field (TOS/ECN, TTL, IP ID);
  //  - the checksum is a TCP or IPv4 header checksum. A UDP checksum is not
  //    expressible: 0 means "absent" and must stay 0, and a computed 0 must be
  //    written as 0xFFFF, which needs a branch the plan does not have.
  EditPlanBuilder &AdjustChecksum(uint16_t offset, std::span<const uint8_t> old_bytes,
                                  std::span<const uint8_t> new_bytes) {
    if (old_bytes.size() != new_bytes.size() || old_bytes.size() % 2 != 0) {
      error_ = EditBuildError::kOutOfRange;
      return *this;
    }
    uint32_t delta = 0;
    for (size_t i = 0; i < old_bytes.size(); i += 2) {
      uint16_t o, n;
      std::memcpy(&o, old_bytes.data() + i, 2);
      std::memcpy(&n, new_bytes.data() + i, 2);
      delta += static_cast<uint16_t>(~o) + n;
    }
    // Several adjustments of one checksum combine into one step.
    for (Pending &p : pending_) {
      if (p.op == EditPlan::Op::kAddChecksum && p.offset == offset) {
        p.aux = Fold(p.aux + delta);
        return *this;
      }
    }
    pending_.push_back(Pending{EditPlan::Op::kAddChecksum, offset, Fold(delta), {}});
    return *this;
  }
  // The 16-bit field at `offset` is set to (packet length after the edit) -
  // `base`: an outer IPv4 total length (base = offset of the IPv4 header) or a
  // UDP length (base = offset of the UDP header).
  EditPlanBuilder &SetLength(uint16_t offset, uint16_t base) {
    pending_.push_back(Pending{EditPlan::Op::kSetLength, offset, base, {}});
    return *this;
  }
  // Recomputes the checksum of the 20-byte IPv4 header at `header` after its
  // total-length field is set per packet. Every other header word must be
  // fixed by this plan's writes; their sum is computed here.
  EditPlanBuilder &Ipv4HeaderChecksum(uint16_t header) {
    ipv4_.push_back(header);
    return *this;
  }

  std::expected<EditPlan, EditBuildError> Build() const {
    if (error_) {
      return std::unexpected(*error_);
    }
    EditPlan plan;
    plan.remove_ = remove_;
    plan.prepend_ = prepend_;
    size_t data_used = 0;
    uint32_t range = 0;
    auto add = [&](EditPlan::Step s) -> bool {
      if (plan.steps_ == EditPlan::kMaxSteps) {
        return false;
      }
      plan.step_[plan.steps_++] = s;
      return true;
    };
    // Literal image of the fixed bytes, for the IPv4 partial sums.
    std::array<int, 0x400> image{};
    image.fill(-1);
    for (const Pending &p : pending_) {
      switch (p.op) {
        case EditPlan::Op::kWrite: {
          for (size_t off = 0; off < p.bytes.size(); off += 255) {
            const size_t n = std::min<size_t>(255, p.bytes.size() - off);
            if (data_used + n > EditPlan::kMaxData) {
              return std::unexpected(EditBuildError::kTooMuchData);
            }
            std::memcpy(plan.data_.data() + data_used, p.bytes.data() + off, n);
            if (!add({EditPlan::Op::kWrite, static_cast<uint8_t>(n),
                      static_cast<uint16_t>(p.offset + off),
                      static_cast<uint32_t>(data_used)})) {
              return std::unexpected(EditBuildError::kTooManySteps);
            }
            data_used += n;
          }
          for (size_t i = 0; i < p.bytes.size() && p.offset + i < image.size(); i++) {
            image[p.offset + i] = p.bytes[i];
          }
          range = std::max<uint32_t>(range, p.offset + static_cast<uint32_t>(p.bytes.size()));
          break;
        }
        case EditPlan::Op::kCopy:
          if (!add({EditPlan::Op::kCopy, static_cast<uint8_t>(p.length), p.offset, p.aux})) {
            return std::unexpected(EditBuildError::kTooManySteps);
          }
          // Copied bytes are not known at build time: a header checksum
          // computed from a partial sum must not cover them.
          for (size_t i = 0; i < p.length && p.offset + i < image.size(); i++) {
            image[p.offset + i] = -1;
          }
          range = std::max<uint32_t>(range, std::max<uint32_t>(p.offset, p.aux) + p.length);
          break;
        default:
          if (!add({p.op, 0, p.offset, p.aux})) {
            return std::unexpected(EditBuildError::kTooManySteps);
          }
          range = std::max<uint32_t>(range, p.offset + 2u);
      }
    }
    for (size_t h = 0; h < ipv4_.size(); h++) {
      if (h == plan.partial_.size()) {
        return std::unexpected(EditBuildError::kTooManySteps);
      }
      const uint16_t at = ipv4_[h];
      uint32_t sum = 0;
      for (uint16_t w = 0; w < 20; w += 2) {
        if (w == 2 || w == 10) {
          continue;  // total length (per packet) and the checksum itself
        }
        if (static_cast<size_t>(at + w + 1) >= image.size() || image[at + w] < 0 || image[at + w + 1] < 0) {
          return std::unexpected(EditBuildError::kOutOfRange);  // not fixed by the plan
        }
        const uint8_t bytes[2] = {static_cast<uint8_t>(image[at + w]),
                                  static_cast<uint8_t>(image[at + w + 1])};
        uint16_t v;
        std::memcpy(&v, bytes, 2);
        sum += v;
      }
      plan.partial_[h] = sum;
      if (!add({EditPlan::Op::kIpv4Checksum, 0, static_cast<uint16_t>(at + 10),
                static_cast<uint32_t>(at + 2) | static_cast<uint32_t>(h) << 16})) {
        return std::unexpected(EditBuildError::kTooManySteps);
      }
      range = std::max<uint32_t>(range, at + 20u);
    }
    if (range > 0xFFFF) {
      return std::unexpected(EditBuildError::kOutOfRange);
    }
    plan.range_ = static_cast<uint16_t>(range);
    return plan;
  }

 private:
  struct Pending {
    EditPlan::Op op;
    uint16_t offset;
    uint32_t aux;
    std::vector<uint8_t> bytes;
    uint16_t length = 0;
  };
  static uint32_t Fold(uint32_t v) {
    v = (v & 0xFFFF) + (v >> 16);
    v = (v & 0xFFFF) + (v >> 16);
    return v;
  }

  std::vector<Pending> pending_;
  std::vector<uint16_t> ipv4_;
  uint16_t remove_ = 0;
  uint16_t prepend_ = 0;
  std::optional<EditBuildError> error_;
};

}  // namespace bess::packet

#endif  // BESS_PACKET_EDIT_PLAN_H_
