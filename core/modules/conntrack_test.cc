// SPDX-License-Identifier: BSD-3-Clause

// The ConnTrack module in process (was bessctl/module_tests/conntrack.py,
// OWNED and SHARED): igate 0 (inside) may start connections, igate 1
// (outside) may not; tracked packets leave on ogate 0, the rest on ogate 1.
// PER_WORKER needs a resume (its inputs are checked then): live, in
// tests/live/module_traffic_test.cc.

#include <gtest/gtest.h>

#include <vector>

#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;

constexpr char kInside[] = "172.16.0.2";
constexpr char kOutside[] = "8.8.8.8";
constexpr uint8_t kSyn = 0x02, kSynAck = 0x12;

Bytes Tcp(const char *src, const char *dst, uint16_t sport, uint16_t dport, uint8_t flags) {
  return PacketBuilder()
      .Ether("06:16:3e:1b:72:32", "02:1e:67:9f:4d:ae")
      .IPv4(src, dst)
      .Tcp(sport, dport, flags)
      .Payload("x")
      .Build();
}

void ExpectStateful(ModuleHarness &h, Module *ct) {
  const Bytes syn = Tcp(kInside, kOutside, 40000, 80, kSyn);
  auto out = h.Run(ct, 0, {syn}, {0, 1});
  EXPECT_EQ((std::vector<Bytes>{syn}), out[0]) << "an inside SYN starts a connection";
  EXPECT_TRUE(out[1].empty());

  const Bytes reply = Tcp(kOutside, kInside, 80, 40000, kSynAck);
  out = h.Run(ct, 1, {reply}, {0, 1});
  EXPECT_EQ((std::vector<Bytes>{reply}), out[0]) << "its reply is tracked";
  EXPECT_TRUE(out[1].empty());

  const Bytes outside_syn = Tcp(kOutside, kInside, 443, 40001, kSyn);
  out = h.Run(ct, 1, {outside_syn}, {0, 1});
  EXPECT_TRUE(out[0].empty());
  EXPECT_EQ((std::vector<Bytes>{outside_syn}), out[1]) << "an outside SYN starts none";
}

TEST(ConnTrackModuleTest, Owned) {
  ModuleHarness h;
  Module *ct = h.Create("ConnTrack", bess::pb::ConnTrackArg());
  ASSERT_NE(nullptr, ct);
  ExpectStateful(h, ct);
  EXPECT_EQ("1 connections", ct->GetDesc());
}

TEST(ConnTrackModuleTest, Shared) {
  ModuleHarness h;
  bess::pb::ConnTrackArg arg;
  arg.set_mode(bess::pb::ConnTrackArg::SHARED);
  Module *ct = h.Create("ConnTrack", arg);
  ASSERT_NE(nullptr, ct);
  ExpectStateful(h, ct);
  EXPECT_EQ("1 connections", ct->GetDesc());
}

}  // namespace
