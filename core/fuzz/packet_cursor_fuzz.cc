// SPDX-License-Identifier: BSD-3-Clause

// Fuzzes bess::packet::PacketCursor (core/packet_cursor.h) over hand-built
// mbuf chains: segmentation (including zero-length segments anywhere, also at
// the head), a pkt_len that may disagree with the physical chain, and an op
// script of Skip / PeekContiguous / ReadBytes / Read<T> (1, 2, 4, 8, 3, 16 and
// 24 byte types) / mark (copy) / restore. SkipUnchecked and
// ReadBytesUnchecked are private; they run under Skip, ReadBytes and Read<T>.
//
// Input (FuzzInput, little endian):
//   u8  flags: bits 0-3 segments - 1, bits 4-5 pkt_len mode (1: shorter than
//       the chain by `delta`, 2: longer by `delta`, else equal), bit 6 also
//       checks a null-packet cursor
//   u8  delta, u8 data seed
//   per segment: u16 data length (% 1024), u8 headroom
//   then ops until the input ends (at most kMaxOps): u8 op, op arguments;
//   sizes are encoded by NextSize() relative to the reference cursor so that
//   segment and packet ends are hit often.
// Segment bytes are a hash of the logical offset and the seed, so a read at a
// wrong offset shows up as wrong bytes.
//
// Oracle: a reference cursor over the flattened bytes: an offset into them,
// the logical length (pkt_len) and the physical segment layout.
//  - Skip/ReadBytes/Read<T> succeed iff the bytes are within pkt_len and the
//    physical chain; on success the bytes equal the flat bytes, on failure the
//    cursor is unchanged and ReadBytes wrote exactly the physically present
//    prefix (the documented partial copy) and nothing else.
//  - PeekContiguous(n) succeeds iff n > 0, n <= remaining() and the n bytes
//    lie inside the segment holding the byte at offset() ("contiguous reads
//    borrow the current segment"); on success it returns exactly n bytes at
//    that segment's address for the offset, otherwise an empty span.
//  - offset()/remaining() match the reference after every op; a restored
//    mark (a cursor copy) behaves like the cursor it was copied from.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "fuzz/fuzz_support.h"
#include "packet_cursor.h"

namespace {

using bess::fuzz::FuzzInput;
using bess::fuzz::MbufChain;
using bess::fuzz::SegmentSpec;
using bess::packet::PacketCursor;

constexpr size_t kMaxSegments = 16;
constexpr size_t kMaxOps = 128;
constexpr size_t kMarks = 4;

struct Odd {
  uint8_t b[3];
};
struct Mixed {
  uint16_t a;
  uint8_t b[6];
  uint64_t c;
};
struct Wide {
  uint8_t b[24];
};
static_assert(sizeof(Odd) == 3 && sizeof(Mixed) == 16 && sizeof(Wide) == 24);

uint8_t DataByte(size_t offset, uint8_t seed) {
  uint32_t x = static_cast<uint32_t>(offset) * 0x9E3779B1u + seed;
  x ^= x >> 15;
  x *= 0x85EBCA6Bu;
  return static_cast<uint8_t>(x >> 24);
}

// Physical layout plus the logical length, and the reference cursor offset.
struct Reference {
  std::vector<uint8_t> flat;            // physical bytes, all segments
  std::vector<size_t> seg_start;        // per segment
  std::vector<size_t> seg_len;          // per segment
  size_t pkt_len = 0;                   // logical length (head pkt_len)
  size_t off = 0;

  size_t physical() const { return flat.size(); }
  size_t remaining() const { return pkt_len - off; }

  // Index of the non-empty segment holding byte `at`, or nullopt at the end.
  std::optional<size_t> SegmentAt(size_t at) const {
    for (size_t i = 0; i < seg_len.size(); i++) {
      if (at >= seg_start[i] && at - seg_start[i] < seg_len[i]) {
        return i;
      }
    }
    return std::nullopt;
  }
  size_t SegmentEnd(size_t at) const {
    const auto i = SegmentAt(at);
    return i ? seg_start[*i] + seg_len[*i] : physical();
  }

  // Whether n bytes from the offset are logically and physically present.
  bool Present(size_t n) const {
    return n <= remaining() && n <= physical() - off;
  }
};

size_t Clamped(size_t base, int delta) {
  if (delta < 0 && static_cast<size_t>(-delta) > base) {
    return 0;
  }
  return base + delta;
}

size_t NextSize(FuzzInput &in, const Reference &ref) {
  const uint8_t s = in.U8();
  const int delta = static_cast<int>(s & 31) - 16;
  switch (s >> 6) {
    case 0:
      return s & 63;
    case 1:
      return Clamped(ref.remaining(), delta);
    case 2:
      return Clamped(ref.SegmentEnd(ref.off) - ref.off, delta);
    default:
      switch (s & 7) {
        case 0:
          return std::numeric_limits<size_t>::max();
        case 1:
          return std::numeric_limits<size_t>::max() - ref.off;
        case 2:
          return size_t{1} << 32;
        case 3:
          return 0xFFFF;
        case 4:
          return 0x10000;
        case 5:
          return in.U32();
        case 6:
          return Clamped(ref.physical() - ref.off, delta / 2);
        default:
          return in.U16();
      }
  }
}

void CheckState(const PacketCursor &cursor, const Reference &ref) {
  FUZZ_CHECK(cursor.offset() == ref.off);
  FUZZ_CHECK(cursor.remaining() == ref.remaining());
}

void CheckPeek(const PacketCursor &cursor, const Reference &ref,
               const MbufChain &chain, size_t n) {
  const std::span<const std::byte> got = cursor.PeekContiguous(n);
  const auto seg = ref.SegmentAt(ref.off);
  const bool expect = n != 0 && n <= ref.remaining() && seg.has_value() &&
                      n <= ref.seg_start[*seg] + ref.seg_len[*seg] - ref.off;
  if (!expect) {
    FUZZ_CHECK(got.empty());
    return;
  }
  FUZZ_CHECK(got.size() == n);
  const auto *want =
      rte_pktmbuf_mtod(chain.Mbuf(*seg), const std::byte *) +
      (ref.off - ref.seg_start[*seg]);
  FUZZ_CHECK(got.data() == want);
  FUZZ_CHECK(std::memcmp(got.data(), ref.flat.data() + ref.off, n) == 0);
}

void CheckReadBytes(PacketCursor &cursor, Reference &ref, size_t n) {
  // Larger requests can only fail before copying (n > remaining); they are
  // covered by Skip/Peek without a buffer of that size.
  if (n > ref.physical() + 512) {
    return;
  }
  constexpr std::byte kSentinel{0x5C};
  std::vector<std::byte> out(n, kSentinel);
  const bool ok = cursor.ReadBytes(out);
  FUZZ_CHECK(ok == ref.Present(n));
  size_t copied = 0;
  if (ok) {
    copied = n;
  } else if (n <= ref.remaining()) {
    copied = ref.physical() - ref.off;  // the chain ended early
  }
  FUZZ_CHECK(copied == 0 ||
             std::memcmp(out.data(), ref.flat.data() + ref.off, copied) == 0);
  for (size_t i = copied; i < n; i++) {
    FUZZ_CHECK(out[i] == kSentinel);
  }
  if (ok) {
    ref.off += n;
  }
}

template <typename T>
void CheckRead(PacketCursor &cursor, Reference &ref) {
  const std::optional<T> got = cursor.template Read<T>();
  FUZZ_CHECK(got.has_value() == ref.Present(sizeof(T)));
  if (got) {
    FUZZ_CHECK(std::memcmp(&*got, ref.flat.data() + ref.off, sizeof(T)) == 0);
    ref.off += sizeof(T);
  }
}

void CheckNullCursor() {
  PacketCursor cursor{bess::PacketRef()};
  FUZZ_CHECK(cursor.offset() == 0 && cursor.remaining() == 0);
  FUZZ_CHECK(cursor.PeekContiguous(1).empty());
  FUZZ_CHECK(!cursor.Skip(1));
  FUZZ_CHECK(!cursor.Read<uint8_t>().has_value());
  FUZZ_CHECK(cursor.Skip(0));
  FUZZ_CHECK(cursor.offset() == 0 && cursor.remaining() == 0);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  FuzzInput in(data, size);
  const uint8_t flags = in.U8();
  const uint8_t delta = in.U8();
  const uint8_t seed = in.U8();
  const size_t segments = (flags & 0x0F) + 1;
  static_assert(kMaxSegments == 16);

  Reference ref;
  std::vector<SegmentSpec> specs(segments);
  for (SegmentSpec &spec : specs) {
    const size_t len = in.U16() % 1024;
    spec.headroom = in.U8();
    ref.seg_start.push_back(ref.flat.size());
    ref.seg_len.push_back(len);
    for (size_t i = 0; i < len; i++) {
      const uint8_t b = DataByte(ref.flat.size(), seed);
      ref.flat.push_back(b);
      spec.data.push_back(b);
    }
  }
  MbufChain chain(std::move(specs));
  FUZZ_CHECK(chain.Flatten() == ref.flat);

  ref.pkt_len = ref.physical();
  switch ((flags >> 4) & 3) {
    case 1:
      ref.pkt_len -= std::min<size_t>(delta, ref.pkt_len);
      break;
    case 2:
      ref.pkt_len += delta;
      break;
    default:
      break;
  }
  chain.head_mbuf()->pkt_len = static_cast<uint32_t>(ref.pkt_len);
  if (flags & 0x40) {
    CheckNullCursor();
  }

  PacketCursor cursor{chain.ref()};
  CheckState(cursor, ref);
  std::vector<PacketCursor> marks(kMarks, cursor);
  std::array<size_t, kMarks> mark_off{};

  for (size_t ops = 0; ops < kMaxOps && !in.empty(); ops++) {
    const uint8_t op = in.U8();
    switch (op % 13) {
      case 0: {
        const size_t n = NextSize(in, ref);
        const bool ok = cursor.Skip(n);
        FUZZ_CHECK(ok == ref.Present(n));
        if (ok) {
          ref.off += n;
        }
        break;
      }
      case 1:
        CheckPeek(cursor, ref, chain, NextSize(in, ref));
        break;
      case 2:
        CheckReadBytes(cursor, ref, NextSize(in, ref));
        break;
      case 3:
        CheckRead<uint8_t>(cursor, ref);
        break;
      case 4:
        CheckRead<uint16_t>(cursor, ref);
        break;
      case 5:
        CheckRead<uint32_t>(cursor, ref);
        break;
      case 6:
        CheckRead<uint64_t>(cursor, ref);
        break;
      case 7:
        CheckRead<Odd>(cursor, ref);
        break;
      case 8:
        CheckRead<Mixed>(cursor, ref);
        break;
      case 9:
        CheckRead<Wide>(cursor, ref);
        break;
      case 10: {
        const size_t slot = (op >> 4) % kMarks;
        marks[slot] = cursor;
        mark_off[slot] = ref.off;
        break;
      }
      case 11: {
        const size_t slot = (op >> 4) % kMarks;
        cursor = marks[slot];
        ref.off = mark_off[slot];
        break;
      }
      default:
        // A failed op of each kind must leave the cursor unchanged.
        FUZZ_CHECK(!cursor.Skip(ref.remaining() + 1));
        CheckReadBytes(cursor, ref, ref.remaining() + 1);
        break;
    }
    CheckState(cursor, ref);
    // Peeking is const: check it at every step, one byte and to segment end.
    CheckPeek(cursor, ref, chain, 1);
    CheckPeek(cursor, ref, chain, ref.SegmentEnd(ref.off) - ref.off);
  }
  return 0;
}
