// SPDX-License-Identifier: BSD-3-Clause

// Fuzzes the in-place packet mutations of core/packet_mutation.h
// (PrependInPlace, AppendInPlace, RemovePrefixInPlace, TrimSuffixInPlace,
// detail::MutationLengthFits / MutationRemovalLengthFits) and the edit plans
// of core/packet_edit_plan.h (EditPlanBuilder::Build, EditPlan::Apply /
// ApplyTrusted) on hand-built mbuf chains. Neither allocates or frees mbufs:
// they only move data_off/data_len/pkt_len and write payload bytes, so no
// mempool and no EAL are needed. The copy-on-write and reshape paths of
// packet_reshape.h allocate and are out of scope.
//
// Input (FuzzInput, little endian):
//   u8  flags: bits 0-2 segments - 1, bit 3 replace pkt_len with a fuzzed u32
//       (a chain whose pkt_len disagrees with its segments; reaches the
//       uint32 pkt_len limit), bits 4-7 data seed
//   [u32 pkt_len if bit 3]
//   per segment: u16 data length (% 4096), u16 headroom, u16 tailroom (the
//       chain shrinks rooms to a 16-bit buf_len), u8 storage: bits 0-1 kind
//       (0 direct, 1 external buffer, 2 indirect, 3 external with no shinfo),
//       bit 2 shared (refcnt 2)
//   then ops until the input ends (at most kMaxOps): u8 op + arguments.
//   Sizes (NextSize) are relative to the current head/last segment rooms and
//   lengths and pkt_len, or special values (0xFFFF, 0x10000, 2^32,
//   UINT32_MAX - pkt_len, SIZE_MAX, raw u64).
//
// Oracle: a model of every segment's whole buffer (headroom, data,
// tailroom), data_off, data_len, writeability, and the head's pkt_len.
//  - Each mutation's result (success or the exact MutationError, in the
//    documented check order) is predicted from the model; on success the
//    returned MutableBytes must be exactly the new head bytes (prepend) or
//    the old tailroom start of the last segment (append), and a fuzzed
//    pattern written through it must land where the model puts it.
//  - After every op every segment's buffer and fields, the links, nb_segs,
//    refcnts and pkt_len equal the model (nothing outside the edited
//    segment's room changes; a failed op changes nothing), and with a
//    consistent chain pkt_len == sum of data_len.
//  - Length predicates match their arithmetic definition for any pkt_len.
//  - Edit plans: an independent model of the builder calls (call order, with
//    the documented combining of checksum adjustments and IPv4 header
//    checksums last) predicts Build's success/failure class, range(),
//    remove/prepend bytes, Apply's error (kTooShort, kNeedsReshape,
//    kNoHeadroom) and the exact packet bytes after Apply; ones-complement
//    sums use a canonical modular form, not the plan's folding. Overlapping
//    IPv4 header checksums are not generated (meaningless in a packet).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include <rte_byteorder.h>
#include <rte_mbuf.h>

#include "fuzz/fuzz_support.h"
#include "packet_edit_plan.h"
#include "packet_mutation.h"

namespace {

using bess::PacketRef;
using bess::fuzz::FuzzInput;
using bess::fuzz::MbufChain;
using bess::fuzz::SegmentSpec;
using bess::packet::EditBuildError;
using bess::packet::EditError;
using bess::packet::EditPlan;
using bess::packet::EditPlanBuilder;
using bess::packet::MutableBytes;
using bess::packet::MutationError;

constexpr size_t kMaxOps = 48;
constexpr size_t kMaxSegments = 8;
constexpr uint64_t kU32Max = std::numeric_limits<uint32_t>::max();

enum Storage : uint8_t { kDirect, kExternal, kIndirect, kExternalNoInfo };

struct SegModel {
  std::vector<uint8_t> buf;  // the whole data room
  uint16_t data_off = 0;
  uint16_t data_len = 0;
  Storage storage = kDirect;
  bool shared = false;

  bool writable() const { return storage != kExternalNoInfo && !shared; }
  size_t headroom() const { return data_off; }
  size_t tailroom() const { return buf.size() - data_off - data_len; }
  uint8_t *data() { return buf.data() + data_off; }
};

struct Model {
  std::vector<SegModel> segs;
  uint32_t pkt_len = 0;
  bool consistent = true;  // pkt_len started equal to the sum of data_len

  SegModel &head() { return segs.front(); }
  SegModel &last() { return segs.back(); }
};

// Applies the model's storage and sharing to segment i.
void SetStorage(const MbufChain &chain,
                std::span<rte_mbuf_ext_shared_info> infos, size_t i,
                const SegModel &seg) {
  rte_mbuf *m = chain.Mbuf(i);
  m->ol_flags &= ~(RTE_MBUF_F_EXTERNAL | RTE_MBUF_F_INDIRECT);
  m->shinfo = nullptr;
  rte_mbuf_refcnt_set(m, 1);
  const uint16_t refs = seg.shared ? 2 : 1;
  switch (seg.storage) {
    case kDirect:
      rte_mbuf_refcnt_set(m, refs);
      break;
    case kExternal:
      m->ol_flags |= RTE_MBUF_F_EXTERNAL;
      m->shinfo = &infos[i];
      rte_mbuf_ext_refcnt_set(&infos[i], refs);
      break;
    case kIndirect:
      // rte_mbuf_from_indirect() maps buf_addr back to this block's own
      // header, so the segment's own refcnt decides.
      m->ol_flags |= RTE_MBUF_F_INDIRECT;
      rte_mbuf_refcnt_set(m, refs);
      break;
    case kExternalNoInfo:
      m->ol_flags |= RTE_MBUF_F_EXTERNAL;
      break;
  }
}

// Compares the chain with the model. Ops only edit the head and the last
// segment, so per op only their buffers are compared (`all` compares every
// buffer); every segment's fields are always compared.
void CheckChain(const MbufChain &chain, const Model &model, bool all) {
  rte_mbuf *head = chain.head_mbuf();
  FUZZ_CHECK(head->pkt_len == model.pkt_len);
  FUZZ_CHECK(head->nb_segs == model.segs.size());
  const size_t n = model.segs.size();
  size_t sum = 0;
  for (size_t i = 0; i < n; i++) {
    const rte_mbuf *m = chain.Mbuf(i);
    const SegModel &seg = model.segs[i];
    FUZZ_CHECK(m->next == (i + 1 < n ? chain.Mbuf(i + 1) : nullptr));
    FUZZ_CHECK(m->buf_len == seg.buf.size());
    FUZZ_CHECK(m->data_off == seg.data_off);
    FUZZ_CHECK(m->data_len == seg.data_len);
    if ((all || i == 0 || i + 1 == n) && !seg.buf.empty()) {
      FUZZ_CHECK(std::memcmp(m->buf_addr, seg.buf.data(), seg.buf.size()) == 0);
    }
    sum += m->data_len;
  }
  if (model.consistent) {
    FUZZ_CHECK(head->pkt_len == sum);
  }
}

size_t Clamped(size_t base, int delta) {
  if (delta < 0 && static_cast<size_t>(-delta) > base) {
    return 0;
  }
  return base + delta;
}

size_t NextSize(FuzzInput &in, Model &model) {
  const uint8_t s = in.U8();
  const int delta = static_cast<int>(s & 31) - 16;
  switch (s >> 5) {
    case 0:
      return s & 31;
    case 1:
      return Clamped(model.head().headroom(), delta);
    case 2:
      return Clamped(model.last().tailroom(), delta);
    case 3:
      return Clamped(model.head().data_len, delta);
    case 4:
      return Clamped(model.last().data_len, delta);
    case 5:
      return Clamped(model.pkt_len, delta);
    case 6:
      switch (s & 7) {
        case 0:
          return 0xFFFF;
        case 1:
          return 0x10000;
        case 2:
          return size_t{1} << 32;
        case 3:
          return kU32Max - model.pkt_len;
        case 4:
          return kU32Max - model.pkt_len + 1;
        case 5:
          return std::numeric_limits<size_t>::max();
        case 6:
          return 0x10000 + model.head().headroom();
        default:
          return in.U64();
      }
    default:
      return in.U16();
  }
}

uint8_t Pattern(uint8_t seed, size_t i) {
  return static_cast<uint8_t>(seed + i * 29 + (i >> 8));
}

bool AddFits(size_t bytes, uint32_t pkt_len) {
  return bytes <= 0xFFFF && bytes + pkt_len <= kU32Max;
}
bool RemoveFits(size_t bytes, uint32_t pkt_len) {
  return bytes <= 0xFFFF && bytes <= pkt_len;
}

template <typename T>
void CheckError(const std::expected<T, MutationError> &got,
                std::optional<MutationError> want) {
  FUZZ_CHECK(got.has_value() == !want.has_value());
  if (want) {
    FUZZ_CHECK(got.error() == *want);
  }
}

// Writes a pattern through bytes returned for model bytes `at`.
void WriteThrough(FuzzInput &in, MutableBytes bytes, uint8_t *at) {
  const uint8_t seed = in.U8();
  for (size_t i = 0; i < bytes.size(); i++) {
    bytes[i] = std::byte{Pattern(seed, i)};
    at[i] = Pattern(seed, i);
  }
}

void Prepend(FuzzInput &in, const MbufChain &chain, Model &model) {
  const size_t n = NextSize(in, model);
  SegModel &head = model.head();
  std::optional<MutationError> want;
  if (n == 0) {
  } else if (!AddFits(n, model.pkt_len)) {
    want = MutationError::kLengthOutOfRange;
  } else if (n > head.headroom()) {
    want = MutationError::kInsufficientHeadroom;
  } else if (!head.writable()) {
    want = MutationError::kSharedStorage;
  }
  const auto got = bess::packet::PrependInPlace(chain.ref(), n);
  CheckError(got, want);
  if (!got || n == 0) {
    FUZZ_CHECK(!got || got->empty());
    return;
  }
  head.data_off = static_cast<uint16_t>(head.data_off - n);
  head.data_len = static_cast<uint16_t>(head.data_len + n);
  model.pkt_len += static_cast<uint32_t>(n);
  rte_mbuf *m = chain.head_mbuf();
  FUZZ_CHECK(got->size() == n);
  FUZZ_CHECK(reinterpret_cast<uint8_t *>(got->data()) ==
             static_cast<uint8_t *>(m->buf_addr) + head.data_off);
  WriteThrough(in, *got, head.data());
}

void Append(FuzzInput &in, const MbufChain &chain, Model &model) {
  const size_t n = NextSize(in, model);
  SegModel &last = model.last();
  std::optional<MutationError> want;
  if (n == 0) {
  } else if (!AddFits(n, model.pkt_len)) {
    want = MutationError::kLengthOutOfRange;
  } else if (n > last.tailroom()) {
    want = MutationError::kInsufficientTailroom;
  } else if (!last.writable()) {
    want = MutationError::kSharedStorage;
  }
  const auto got = bess::packet::AppendInPlace(chain.ref(), n);
  CheckError(got, want);
  if (!got || n == 0) {
    FUZZ_CHECK(!got || got->empty());
    return;
  }
  const size_t tail = size_t{last.data_off} + last.data_len;
  last.data_len = static_cast<uint16_t>(last.data_len + n);
  model.pkt_len += static_cast<uint32_t>(n);
  rte_mbuf *m = chain.Mbuf(model.segs.size() - 1);
  FUZZ_CHECK(got->size() == n);
  FUZZ_CHECK(reinterpret_cast<uint8_t *>(got->data()) ==
             static_cast<uint8_t *>(m->buf_addr) + tail);
  WriteThrough(in, *got, last.buf.data() + tail);
}

void RemovePrefix(FuzzInput &in, const MbufChain &chain, Model &model) {
  const size_t n = NextSize(in, model);
  SegModel &head = model.head();
  std::optional<MutationError> want;
  if (!RemoveFits(n, model.pkt_len)) {
    want = MutationError::kLengthOutOfRange;
  } else if (n != 0 && n > head.data_len) {
    want = MutationError::kCrossesSegment;
  }
  CheckError(bess::packet::RemovePrefixInPlace(chain.ref(), n), want);
  if (!want) {
    head.data_off = static_cast<uint16_t>(head.data_off + n);
    head.data_len = static_cast<uint16_t>(head.data_len - n);
    model.pkt_len -= static_cast<uint32_t>(n);
  }
}

void TrimSuffix(FuzzInput &in, const MbufChain &chain, Model &model) {
  const size_t n = NextSize(in, model);
  SegModel &last = model.last();
  std::optional<MutationError> want;
  if (!RemoveFits(n, model.pkt_len)) {
    want = MutationError::kLengthOutOfRange;
  } else if (n != 0 && n > last.data_len) {
    want = MutationError::kCrossesSegment;
  }
  CheckError(bess::packet::TrimSuffixInPlace(chain.ref(), n), want);
  if (!want) {
    last.data_len = static_cast<uint16_t>(last.data_len - n);
    model.pkt_len -= static_cast<uint32_t>(n);
  }
}

void LengthPredicates(FuzzInput &in, const MbufChain &chain, Model &model) {
  const size_t n = NextSize(in, model);
  rte_mbuf *head = chain.head_mbuf();
  FUZZ_CHECK(bess::packet::detail::MutationLengthFits(head, n) ==
             AddFits(n, model.pkt_len));
  FUZZ_CHECK(bess::packet::detail::MutationRemovalLengthFits(head, n) ==
             RemoveFits(n, model.pkt_len));
  rte_mbuf scratch{};
  scratch.pkt_len = in.U32();
  FUZZ_CHECK(bess::packet::detail::MutationLengthFits(&scratch, n) ==
             AddFits(n, scratch.pkt_len));
  FUZZ_CHECK(bess::packet::detail::MutationRemovalLengthFits(&scratch, n) ==
             RemoveFits(n, scratch.pkt_len));
}

void NullPacket() {
  const PacketRef null;
  CheckError(bess::packet::PrependInPlace(null, 1), MutationError::kNullPacket);
  CheckError(bess::packet::AppendInPlace(null, 1), MutationError::kNullPacket);
  CheckError(bess::packet::RemovePrefixInPlace(null, 1),
             MutationError::kNullPacket);
  CheckError(bess::packet::TrimSuffixInPlace(null, 1),
             MutationError::kNullPacket);
  FUZZ_CHECK(bess::packet::PayloadWriteabilityOf(null) ==
             bess::packet::PayloadWriteability::kShared);
}

// ---- Edit plans -----------------------------------------------------------

// Ones-complement value of a non-negative sum: 0 only for 0, otherwise the
// representative in [1, 0xFFFF].
uint16_t Canonical(uint64_t v) {
  return v == 0 ? 0 : static_cast<uint16_t>((v - 1) % 0xFFFF + 1);
}

uint16_t Load16(const uint8_t *p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return v;
}
void Store16(uint8_t *p, uint16_t v) { std::memcpy(p, &v, 2); }

struct PlanStep {
  enum Kind : uint8_t { kWrite, kCopy, kChecksum, kSetLength } kind;
  size_t offset = 0;
  size_t from = 0;  // kCopy source
  size_t len = 0;   // kWrite / kCopy bytes
  std::vector<uint8_t> bytes;
  uint64_t delta = 0;  // kChecksum: raw sum of (~old + new) words
  uint16_t base = 0;   // kSetLength
};

struct PlanModel {
  std::vector<PlanStep> steps;  // call order, checksum adjustments combined
  std::vector<size_t> ipv4;
  size_t remove = 0;
  size_t prepend = 0;
  bool bad_argument = false;

  void Write(size_t offset, std::span<const uint8_t> bytes) {
    if (bytes.empty() || offset + bytes.size() > 0xFFFF) {
      bad_argument = true;
      return;
    }
    steps.push_back({PlanStep::kWrite, offset, 0, bytes.size(),
                     {bytes.begin(), bytes.end()}});
  }

  size_t Range() const {
    size_t range = 0;
    for (const PlanStep &s : steps) {
      switch (s.kind) {
        case PlanStep::kWrite:
          range = std::max(range, s.offset + s.len);
          break;
        case PlanStep::kCopy:
          range = std::max(range, std::max(s.offset, s.from) + s.len);
          break;
        default:
          range = std::max(range, s.offset + 2);
      }
    }
    for (size_t at : ipv4) {
      range = std::max(range, at + 20);
    }
    return range;
  }

  // Every IPv4 header word other than total length and the checksum holds
  // a byte known at build time: last set by a literal write (prepended
  // headers are writes), not by a copy, checksum adjustment or length.
  bool HeadersFixed() const {
    std::array<bool, 0x400> known{};
    for (const PlanStep &s : steps) {
      const bool literal = s.kind == PlanStep::kWrite;
      const size_t n = s.kind == PlanStep::kWrite || s.kind == PlanStep::kCopy
                           ? s.len
                           : 2;
      for (size_t i = s.offset; i < s.offset + n && i < known.size(); i++) {
        known[i] = literal;
      }
    }
    for (size_t at : ipv4) {
      if (at + 20 > known.size()) {
        return false;
      }
      for (size_t w = 0; w < 20; w += 2) {
        if (w != 2 && w != 10 && !(known[at + w] && known[at + w + 1])) {
          return false;
        }
      }
    }
    return true;
  }

  size_t LiteralBytes() const {
    size_t n = 0;
    for (const PlanStep &s : steps) {
      n += s.kind == PlanStep::kWrite ? s.len : 0;
    }
    return n;
  }
  size_t MaxSteps() const {
    size_t n = ipv4.size();
    for (const PlanStep &s : steps) {
      n += s.kind == PlanStep::kWrite ? (s.len + 254) / 255 : 1;
    }
    return n;
  }

  // Runs the steps on the bytes after the prefix replacement.
  void Run(uint8_t *p, uint32_t pkt_len) const {
    for (const PlanStep &s : steps) {
      switch (s.kind) {
        case PlanStep::kWrite:
          std::memcpy(p + s.offset, s.bytes.data(), s.len);
          break;
        case PlanStep::kCopy:
          std::memmove(p + s.offset, p + s.from, s.len);
          break;
        case PlanStep::kChecksum: {
          const uint16_t c = Load16(p + s.offset);
          Store16(p + s.offset,
                  static_cast<uint16_t>(~Canonical(uint16_t(~c) + s.delta)));
          break;
        }
        case PlanStep::kSetLength:
          Store16(p + s.offset,
                  rte_cpu_to_be_16(static_cast<uint16_t>(pkt_len - s.base)));
          break;
      }
    }
    for (size_t at : ipv4) {
      uint64_t sum = 0;
      for (size_t w = 0; w < 20; w += 2) {
        sum += w == 10 ? 0 : Load16(p + at + w);
      }
      Store16(p + at + 10, static_cast<uint16_t>(~Canonical(sum)));
    }
  }
};

size_t PlanOffset(FuzzInput &in) {
  const uint8_t v = in.U8();
  return v == 0xFF ? in.U16() : v;
}

std::vector<uint8_t> PlanBytes(FuzzInput &in, size_t n) {
  std::vector<uint8_t> out(n);
  const auto got = in.Bytes(n);
  std::copy(got.begin(), got.end(), out.begin());
  return out;
}

// Decodes builder calls, made on both the builder and the model.
void BuildPlan(FuzzInput &in, EditPlanBuilder &builder, PlanModel &model) {
  const size_t calls = in.U8() % 16;
  for (size_t c = 0; c < calls; c++) {
    switch (in.U8() % 7) {
      case 0: {
        const uint16_t n = static_cast<uint16_t>(PlanOffset(in));
        builder.RemovePrefix(n);
        model.remove += n;
        break;
      }
      case 1: {
        const std::vector<uint8_t> header = PlanBytes(in, in.U8() % 65);
        builder.Prepend(header);
        model.prepend = header.size();
        model.Write(0, header);
        break;
      }
      case 2: {
        const size_t offset = PlanOffset(in);
        const std::vector<uint8_t> bytes = PlanBytes(in, in.U8() % 49);
        builder.Write(static_cast<uint16_t>(offset), bytes);
        model.Write(offset, bytes);
        break;
      }
      case 3: {
        const size_t to = PlanOffset(in);
        const size_t from = PlanOffset(in);
        const uint8_t v = in.U8();
        const size_t len = v == 0xFF ? in.U16() : v;
        builder.Copy(static_cast<uint16_t>(to), static_cast<uint16_t>(from),
                     static_cast<uint16_t>(len));
        if (len == 0 || len > 255) {
          model.bad_argument = true;
        } else {
          model.steps.push_back({PlanStep::kCopy, to, from, len, {}});
        }
        break;
      }
      case 4: {
        const size_t offset = PlanOffset(in);
        const uint8_t shape = in.U8();
        const size_t old_len = shape % 17;
        const size_t new_len = (shape & 0x80) ? (shape >> 4) % 8 : old_len;
        const std::vector<uint8_t> old_bytes = PlanBytes(in, old_len);
        const std::vector<uint8_t> new_bytes = PlanBytes(in, new_len);
        builder.AdjustChecksum(static_cast<uint16_t>(offset), old_bytes,
                               new_bytes);
        if (old_len != new_len || old_len % 2 != 0) {
          model.bad_argument = true;
          break;
        }
        uint64_t delta = 0;
        for (size_t i = 0; i < old_len; i += 2) {
          delta += uint16_t(~Load16(&old_bytes[i])) + Load16(&new_bytes[i]);
        }
        auto it = std::find_if(model.steps.begin(), model.steps.end(),
                               [&](const PlanStep &s) {
                                 return s.kind == PlanStep::kChecksum &&
                                        s.offset == offset;
                               });
        if (it != model.steps.end()) {
          it->delta += delta;
        } else {
          model.steps.push_back(
              {PlanStep::kChecksum, offset, 0, 0, {}, delta});
        }
        break;
      }
      case 5: {
        const size_t offset = PlanOffset(in);
        const uint16_t base = static_cast<uint16_t>(PlanOffset(in));
        builder.SetLength(static_cast<uint16_t>(offset), base);
        model.steps.push_back(
            {PlanStep::kSetLength, offset, 0, 0, {}, 0, base});
        break;
      }
      default: {
        const size_t at = PlanOffset(in);
        const bool overlaps =
            std::any_of(model.ipv4.begin(), model.ipv4.end(),
                        [&](size_t h) { return h < at + 20 && at < h + 20; });
        if (!overlaps) {
          builder.Ipv4HeaderChecksum(static_cast<uint16_t>(at));
          model.ipv4.push_back(at);
        }
        break;
      }
    }
  }
}

void EditPlanOp(FuzzInput &in, const MbufChain &chain, Model &model) {
  EditPlanBuilder builder;
  PlanModel plan_model;
  const uint8_t apply_mode = in.U8();
  BuildPlan(in, builder, plan_model);
  const auto plan = builder.Build();

  const size_t range = plan_model.Range();
  if (plan_model.bad_argument || plan_model.remove > 0xFFFF) {
    FUZZ_CHECK(!plan && plan.error() == EditBuildError::kOutOfRange);
    return;
  }
  const bool out_of_range = range > 0xFFFF || !plan_model.HeadersFixed();
  const bool too_much_data = plan_model.LiteralBytes() > EditPlan::kMaxData;
  const bool too_many_steps = plan_model.MaxSteps() > EditPlan::kMaxSteps ||
                              plan_model.ipv4.size() > 2;
  if (!plan) {
    switch (plan.error()) {
      case EditBuildError::kOutOfRange:
        FUZZ_CHECK(out_of_range);
        break;
      case EditBuildError::kTooMuchData:
        FUZZ_CHECK(too_much_data);
        break;
      case EditBuildError::kTooManySteps:
        FUZZ_CHECK(too_many_steps);
        break;
    }
    return;
  }
  FUZZ_CHECK(!out_of_range && plan_model.ipv4.size() <= 2);
  FUZZ_CHECK(plan->range() == range);
  FUZZ_CHECK(plan->remove_bytes() == plan_model.remove);
  FUZZ_CHECK(plan->prepend_bytes() == plan_model.prepend);
  FUZZ_CHECK(plan->steps() <= EditPlan::kMaxSteps);

  // Apply's checks, in the documented order.
  SegModel &head = model.head();
  const size_t remove = plan_model.remove;
  const size_t prepend = plan_model.prepend;
  std::optional<EditError> want;
  if (model.pkt_len < remove) {
    want = EditError::kTooShort;
  } else if (head.data_len + prepend < range + remove ||
             head.data_len < remove || !head.writable()) {
    want = EditError::kNeedsReshape;
  } else if (prepend > remove && prepend - remove > head.headroom()) {
    want = EditError::kNoHeadroom;
  }

  if (!want && (apply_mode & 3) == 2) {
    plan->ApplyTrusted(chain.ref());
  } else {
    const auto got = (apply_mode & 3) == 1 ? plan->Apply(chain.head_mbuf())
                                           : plan->Apply(chain.ref());
    FUZZ_CHECK(got.has_value() == !want.has_value());
    if (want) {
      FUZZ_CHECK(got.error() == *want);
    }
  }
  if (want) {
    return;
  }
  head.data_off = static_cast<uint16_t>(head.data_off + remove - prepend);
  head.data_len = static_cast<uint16_t>(head.data_len + prepend - remove);
  model.pkt_len = static_cast<uint32_t>(model.pkt_len + prepend - remove);
  plan_model.Run(head.data(), model.pkt_len);
}

void ToggleShared(FuzzInput &in, const MbufChain &chain,
                  std::span<rte_mbuf_ext_shared_info> infos, Model &model) {
  const size_t i = in.U8() % model.segs.size();
  model.segs[i].shared = !model.segs[i].shared;
  SetStorage(chain, infos, i, model.segs[i]);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  FuzzInput in(data, size);
  const uint8_t flags = in.U8();
  const size_t segments = (flags & 7) + 1;
  static_assert(kMaxSegments == 8);
  const bool override_len = (flags & 8) != 0;
  const uint32_t pkt_len = override_len ? in.U32() : 0;
  const uint8_t seed = flags >> 4;

  std::vector<SegmentSpec> specs(segments);
  std::vector<uint8_t> storage(segments);
  for (size_t i = 0; i < segments; i++) {
    const size_t len = in.U16() % 4096;
    specs[i].headroom = in.U16();
    specs[i].tailroom = in.U16();
    storage[i] = in.U8();
    specs[i].data.resize(len);
    for (size_t j = 0; j < len; j++) {
      specs[i].data[j] = Pattern(static_cast<uint8_t>(seed * 16 + i), j);
    }
  }
  MbufChain chain(std::move(specs));
  std::array<rte_mbuf_ext_shared_info, kMaxSegments> infos{};

  // The model starts from the chain as built (MbufChain shrinks oversized
  // rooms); the code under test has not run yet.
  Model model;
  for (size_t i = 0; i < segments; i++) {
    const rte_mbuf *m = chain.Mbuf(i);
    SegModel seg;
    const auto *buf = static_cast<const uint8_t *>(m->buf_addr);
    seg.buf.assign(buf, buf + m->buf_len);
    seg.data_off = m->data_off;
    seg.data_len = m->data_len;
    seg.storage = static_cast<Storage>(storage[i] & 3);
    seg.shared = (storage[i] & 4) != 0;
    SetStorage(chain, infos, i, seg);
    model.segs.push_back(std::move(seg));
  }
  model.pkt_len = chain.head_mbuf()->pkt_len;
  if (override_len) {
    model.pkt_len = pkt_len;
    model.consistent = false;
    chain.head_mbuf()->pkt_len = pkt_len;
  }
  CheckChain(chain, model, true);

  for (size_t ops = 0; ops < kMaxOps && !in.empty(); ops++) {
    switch (in.U8() % 8) {
      case 0:
        Prepend(in, chain, model);
        break;
      case 1:
        Append(in, chain, model);
        break;
      case 2:
        RemovePrefix(in, chain, model);
        break;
      case 3:
        TrimSuffix(in, chain, model);
        break;
      case 4:
        LengthPredicates(in, chain, model);
        break;
      case 5:
        NullPacket();
        break;
      case 6:
        EditPlanOp(in, chain, model);
        break;
      default:
        ToggleShared(in, chain, infos, model);
        break;
    }
    CheckChain(chain, model, false);
  }
  CheckChain(chain, model, true);
  for (size_t i = 0; i < segments; i++) {
    FUZZ_CHECK(bess::packet::PayloadWriteabilityOf(PacketRef(chain.Mbuf(i))) ==
               (model.segs[i].writable()
                    ? bess::packet::PayloadWriteability::kWritable
                    : bess::packet::PayloadWriteability::kShared));
  }
  return 0;
}
