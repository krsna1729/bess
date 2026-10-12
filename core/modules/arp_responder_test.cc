// SPDX-License-Identifier: BSD-3-Clause

// ArpResponder in process (was bessctl/module_tests/arp.py).

#include <gtest/gtest.h>

#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;

// A request for an address with an entry is answered on the input gate's
// output: the reply comes from the entry's MAC, to the asker.
TEST(ArpResponderTest, AnswersARequestForAnEntry) {
  ModuleHarness h;
  Module *arp = h.Create("ArpResponder", bess::pb::ArpResponderArg());
  ASSERT_NE(nullptr, arp);
  bess::pb::ArpResponderArg entry;
  entry.set_ip("1.2.3.4");
  entry.set_mac_addr("A0:22:33:44:55:66");
  h.MustCommand(arp, "add", entry);

  const Bytes request = PacketBuilder()
                            .Ether("ff:ff:ff:ff:ff:ff", "02:1e:67:9f:4d:ae")
                            .Arp(1, "02:1e:67:9f:4d:ae", "10.0.0.1", "00:00:00:00:00:00",
                                 "1.2.3.4")
                            .Build();
  const Bytes reply = PacketBuilder()
                          .Ether("02:1e:67:9f:4d:ae", "a0:22:33:44:55:66")
                          .Arp(2, "a0:22:33:44:55:66", "1.2.3.4", "02:1e:67:9f:4d:ae",
                               "10.0.0.1")
                          .Build();

  auto out = h.Run(arp, 0, {request}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(reply, out[0][0]);
}

// No entry, no answer.
TEST(ArpResponderTest, DropsARequestForAnUnknownAddress) {
  ModuleHarness h;
  Module *arp = h.Create("ArpResponder", bess::pb::ArpResponderArg());
  ASSERT_NE(nullptr, arp);
  const Bytes request = PacketBuilder()
                            .Ether("ff:ff:ff:ff:ff:ff", "02:1e:67:9f:4d:ae")
                            .Arp(1, "02:1e:67:9f:4d:ae", "10.0.0.1", "00:00:00:00:00:00",
                                 "9.9.9.9")
                            .Build();
  EXPECT_TRUE(h.Run(arp, 0, {request}, {0})[0].empty());
}

// More requests than a batch holds are all answered, in order.
TEST(ArpResponderTest, AnswersEveryRequestOfALongRun) {
  ModuleHarness h;
  bess::pb::ArpResponderArg entry;
  entry.set_ip("1.2.3.4");
  entry.set_mac_addr("A0:22:33:44:55:66");
  Module *arp = h.Create("ArpResponder", entry);
  ASSERT_NE(nullptr, arp);
  std::vector<Bytes> requests;
  for (int i = 0; i < 100; i++) {
    requests.push_back(PacketBuilder()
                           .Ether("ff:ff:ff:ff:ff:ff", "02:1e:67:9f:4d:ae")
                           .Arp(1, "02:1e:67:9f:4d:ae", "10.0.0." + std::to_string(i % 250 + 1),
                                "00:00:00:00:00:00", "1.2.3.4")
                           .Build());
  }
  const auto out = h.Run(arp, 0, requests, {0});
  ASSERT_EQ(100u, out.at(0).size());
  for (int i = 0; i < 100; i++) {
    // The reply's target protocol address (offset 14 + 24) is the asker's.
    EXPECT_EQ(static_cast<uint8_t>(i % 250 + 1), out.at(0)[i][14 + 27]) << i;
  }
}

}  // namespace
