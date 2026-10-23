// SPDX-License-Identifier: BSD-3-Clause

// The ConnTrack module in process (was bessctl/module_tests/conntrack.py,
// OWNED and SHARED): igate 0 (inside) may start connections, igate 1
// (outside) may not; tracked packets leave on ogate 0, the rest on ogate 1.
// PER_WORKER needs a resume (its inputs are checked then): live, in
// tests/live/module_traffic_test.cc.

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "module_graph.h"
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

// D-182: with status_attr each packet carries its result; a Split reading the
// attribute sends it to the gate of that ConnTrackStatus value.
TEST(ConnTrackModuleTest, StatusAttrCarriesEachPacketsResult) {
  ModuleHarness h;
  bess::pb::ConnTrackArg arg;
  arg.set_status_attr("ct_status");
  Module *ct = h.Create("ConnTrack", arg);
  ASSERT_NE(nullptr, ct);
  bess::pb::SplitArg split_arg;
  split_arg.set_attribute("ct_status");
  split_arg.set_size(1);
  Module *split = h.Create("Split", split_arg);
  ASSERT_NE(nullptr, split);
  // Both verdict gates feed the reader: the status, not the gate, routes.
  ASSERT_EQ(0, ModuleGraph::ConnectModules(ct, 0, split, 0, true));
  ASSERT_EQ(0, ModuleGraph::ConnectModules(ct, 1, split, 0, true));
  // Every status value is a wired gate, unspecified (0) included, so a packet
  // marked wrongly, or not at all, shows up on another gate.
  const std::vector<gate_idx_t> gates = {0, 1, 2, 3, 4, 5, 6};
  auto run = [&](gate_idx_t igate, const Bytes &pkt) {
    return h.RunPipeline(ct, igate, {pkt}, split, gates);
  };
  auto only = [](gate_idx_t gate, const Bytes &pkt,
                 const std::map<gate_idx_t, std::vector<Bytes>> &out) {
    EXPECT_EQ((std::map<gate_idx_t, std::vector<Bytes>>{{gate, {pkt}}}), out);
  };

  const Bytes syn = Tcp(kInside, kOutside, 40000, 80, kSyn);
  only(bess::pb::CONNTRACK_STATUS_NEW, syn, run(0, syn));
  const Bytes reply = Tcp(kOutside, kInside, 80, 40000, kSynAck);
  only(bess::pb::CONNTRACK_STATUS_EXISTING, reply, run(1, reply));
  const Bytes outside_syn = Tcp(kOutside, kInside, 443, 40001, kSyn);
  only(bess::pb::CONNTRACK_STATUS_INVALID, outside_syn, run(1, outside_syn));
  const Bytes not_ip = PacketBuilder()
                           .Ether("06:16:3e:1b:72:32", "02:1e:67:9f:4d:ae", 0x0806)  // ARP
                           .Payload(std::string(46, 'x'))
                           .Build();
  only(bess::pb::CONNTRACK_STATUS_UNTRACKED, not_ip, run(0, not_ip));
}

}  // namespace
