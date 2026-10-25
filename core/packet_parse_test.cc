// SPDX-License-Identifier: BSD-3-Clause

// CheckFastIpv4 and ParseFrame against the common frame's checks as they were
// before the accept test (FastFrame's whole-field test of the usual frame),
// kept here as the reference: the same verdict, the same FastIpv4 and the same
// ParsedFrame for every frame. The general path's agreement with the fast one
// is conntrack_test's FastPathAgreesWithTheGeneralPath.

#include "packet_parse.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "testing/packet_builder.h"
#include "testing/parse_corpus.h"
#include "testing/rng.h"

namespace bess::packet {
namespace {

using parse_internal::Be16;
using parse_internal::CheckL4;
using bess::testing::Bytes;
using bess::testing::PacketBuilder;

// The reference: the common frame's checks one at a time, each byte compared
// on its own, as FastFrame ran them before the accept test.
bool RefIsCommonFrame(std::span<const uint8_t> frame) {
  return frame.size() >= 34 && frame[12] == 0x08 && frame[13] == 0x00 && frame[14] == 0x45;
}

template <bool kFill>
FastIpv4 RefFastFrame(std::span<const uint8_t> frame, size_t total_len, ParsedFrame *out) {
  FastIpv4 f;
  if (!RefIsCommonFrame(frame)) {
    return f;
  }
  f.taken = true;
  total_len = std::max(total_len, frame.size());
  const uint8_t *ip = frame.data() + 14;
  if constexpr (kFill) {
    out->l3_offset = 14;
  }
  const size_t total = Be16(ip + 2);
  if (total < 20 || total > total_len - 14) {
    return f;
  }
  f.header_ok = true;
  f.total = static_cast<uint16_t>(total);
  f.protocol = ip[9];
  if constexpr (kFill) {
    out->l3 = L3Kind::kIpv4;
    out->protocol = ip[9];
    std::memcpy(out->src.data(), ip + 12, 4);
    std::memcpy(out->dst.data(), ip + 16, 4);
  }
  const uint16_t frag = Be16(ip + 6);
  if ((frag & 0x1fffu) != 0) {
    f.status = ParseStatus::kFragment;
    return f;
  }
  f.first_fragment = (frag & 0x2000u) != 0;
  if constexpr (kFill) {
    out->first_fragment = f.first_fragment;
    out->l4_offset = 34;
    out->l4_length = static_cast<uint16_t>(total - 20);
  }
  f.status = CheckL4<kFill>(f.protocol, ip + 20, std::min(frame.size() - 14, total) - 20,
                            total - 20, f.first_fragment, out);
  return f;
}

ParseStatus RefParseFrame(std::span<const uint8_t> frame, ParsedFrame &out, size_t total_len) {
  out = ParsedFrame{};
  if (RefIsCommonFrame(frame)) {
    return RefFastFrame<true>(frame, total_len, &out).status;
  }
  return parse_internal::ParseFrameGeneral(frame, out, std::max(total_len, frame.size()));
}

bool Same(const FastIpv4 &a, const FastIpv4 &b) {
  return a.taken == b.taken && a.header_ok == b.header_ok &&
         a.first_fragment == b.first_fragment && a.protocol == b.protocol &&
         a.total == b.total && a.status == b.status;
}

bool Same(const ParsedFrame &a, const ParsedFrame &b) {
  return a.l3 == b.l3 && a.l4 == b.l4 && a.protocol == b.protocol && a.tcp_flags == b.tcp_flags &&
         a.l3_offset == b.l3_offset && a.l4_offset == b.l4_offset && a.l4_length == b.l4_length &&
         a.src == b.src && a.dst == b.dst && a.src_port == b.src_port &&
         a.dst_port == b.dst_port && a.icmp_type == b.icmp_type && a.icmp_code == b.icmp_code &&
         a.first_fragment == b.first_fragment;
}

// Counts of what the comparisons covered, so a test that stops reaching the
// accept test (or only reaches it) fails rather than passes vacuously.
struct Coverage {
  size_t compared = 0;
  size_t accepted = 0;  // the reference's kOk for unfragmented TCP or UDP
  size_t rejected = 0;  // taken, and the reference's verdict not kOk
};

// One frame, at the packet lengths a caller can pass: 0 (the span's), the
// span's, shorter than the span (ignored: the span's is used) and longer (a
// chained packet).
::testing::AssertionResult Agree(std::span<const uint8_t> frame, Coverage &cov) {
  const size_t n = frame.size();
  for (const size_t total_len : {size_t{0}, n, n > 3 ? n - 3 : 0, n + 9, n + 1000}) {
    const FastIpv4 want = RefFastFrame<false>(frame, total_len, nullptr);
    const FastIpv4 got = CheckFastIpv4(frame, total_len);
    if (!Same(want, got)) {
      return ::testing::AssertionFailure() << "CheckFastIpv4 differs, total_len " << total_len;
    }
    ParsedFrame pa, pb;
    const ParseStatus sa = RefParseFrame(frame, pa, total_len);
    const ParseStatus sb = ParseFrame(frame, pb, total_len);
    if (sa != sb || !Same(pa, pb)) {
      return ::testing::AssertionFailure() << "ParseFrame differs, total_len " << total_len;
    }
    cov.compared++;
    cov.accepted += want.status == ParseStatus::kOk && !want.first_fragment &&
                    (want.protocol == 6 || want.protocol == 17);
    cov.rejected += want.taken && want.status != ParseStatus::kOk;
  }
  return ::testing::AssertionSuccess();
}

std::string Hex(std::span<const uint8_t> b) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string s;
  for (const uint8_t x : b) {
    s += kDigits[x >> 4u];
    s += kDigits[x & 15u];
  }
  return s;
}

PacketBuilder Ether() {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:01", "02:00:00:00:00:02");
  return b;
}

TEST(PacketParseTest, AcceptTestDecidesTheParseCorpusAsTheChecks) {
  Coverage cov;
  for (const auto &[bytes, total_len] : bess::testing::ParseCorpus()) {
    ASSERT_TRUE(Agree(bytes, cov)) << Hex(bytes) << " (corpus total " << total_len << ")";
  }
  EXPECT_GT(cov.accepted, 1000u);
  EXPECT_GT(cov.rejected, 1000u);
}

// Every value of every header byte (through the end of the TCP or UDP header)
// of a valid TCP and a valid UDP frame, at every length from 0 to the frame's:
// the field boundaries (lengths one either side of the bytes, IHL and version,
// MF and the offset, the protocol, the L4 lengths) all come out of these.
TEST(PacketParseTest, AcceptTestDecidesEveryHeaderByteAsTheChecks) {
  const std::vector<Bytes> bases = {
      Ether().IPv4("10.0.0.1", "10.1.0.1").Tcp(1000, 80, 0x12).PadTo(60).Build(),
      Ether().IPv4("10.0.0.1", "10.1.0.1").Udp(40000, 2000).PadTo(60).Build(),
  };
  Coverage cov;
  for (const Bytes &base : bases) {
    const size_t header_end = base[23] == 6 ? 54 : 42;
    for (size_t len = 0; len <= base.size(); len++) {
      const Bytes cut(base.begin(), base.begin() + static_cast<std::ptrdiff_t>(len));
      ASSERT_TRUE(Agree(cut, cov)) << Hex(cut);
      for (size_t i = 0; i < std::min(len, header_end); i++) {
        Bytes v = cut;
        for (unsigned x = 0; x < 256; x++) {
          v[i] = static_cast<uint8_t>(x);
          ASSERT_TRUE(Agree(v, cov)) << Hex(v);
        }
      }
    }
  }
  EXPECT_GT(cov.accepted, 10000u);
  EXPECT_GT(cov.rejected, 100000u);
}

// Random frames shaped like the common one: 08 00 45 at 12, a TCP, UDP or
// other protocol, random fragment bits, total and L4 lengths near the frame's;
// each shaping step is skipped one time in eight, leaving those bytes random.
TEST(PacketParseTest, AcceptTestDecidesRandomFramesAsTheChecks) {
  std::mt19937 rng = bess::testing::SeededRng(0x45);
  Coverage cov;
  for (int i = 0; i < 200000; i++) {
    Bytes b(rng() % 80);
    for (uint8_t &x : b) {
      x = static_cast<uint8_t>(rng());
    }
    auto put16 = [&](size_t off, uint32_t v) {
      if (off + 2 <= b.size()) {
        b[off] = static_cast<uint8_t>(v >> 8u);
        b[off + 1] = static_cast<uint8_t>(v);
      }
    };
    auto near = [&](size_t n) { return static_cast<uint32_t>(n + rng() % 9) - 4; };
    if (rng() % 8 != 0 && b.size() >= 15) {
      b[12] = 0x08;
      b[13] = 0x00;
      b[14] = 0x45;
    }
    if (rng() % 8 != 0 && b.size() >= 24) {
      static constexpr uint8_t kProtocols[] = {6, 17, 17, 6, 1, 47};
      b[23] = kProtocols[rng() % std::size(kProtocols)];
    }
    if (rng() % 8 != 0) {
      put16(20, rng() % 2 == 0 ? (rng() & 0x4000u) : (rng() & 0xe003u));
    }
    if (rng() % 8 != 0) {
      put16(16, near(b.size() > 14 ? b.size() - 14 : 0));
    }
    if (rng() % 8 != 0) {
      put16(38, near(b.size() > 34 ? b.size() - 34 : 0));  // UDP length
    }
    if (rng() % 8 != 0 && b.size() > 46) {
      b[46] = static_cast<uint8_t>((rng() % 8 + 3) << 4u | (b[46] & 15u));  // TCP offset
    }
    ASSERT_TRUE(Agree(b, cov)) << Hex(b);
  }
  EXPECT_GT(cov.accepted, 10000u);
  EXPECT_GT(cov.rejected, 10000u);
}

}  // namespace
}  // namespace bess::packet
