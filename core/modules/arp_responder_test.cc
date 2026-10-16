// SPDX-License-Identifier: BSD-3-Clause

// ArpResponder in process (was bessctl/module_tests/arp.py).

#include <gtest/gtest.h>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::pb::ArpResponderEntryKey;
using bess::pb::ArpResponderEntryValue;
using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;

// A request for an address with an entry is answered on the input gate's
// output: the reply comes from the entry's MAC, to the asker.
TEST(ArpResponderTest, AnswersARequestForAnEntry) {
  ModuleHarness h;
  bess::pb::ArpResponderArg entry;
  entry.set_ip("1.2.3.4");
  entry.set_mac_addr("A0:22:33:44:55:66");
  Module *arp = h.Create("ArpResponder", entry);
  ASSERT_NE(nullptr, arp);

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

// Whether `resource`'s codec says it requires paused workers.
bool RequiresPausedWorkers(const std::string &resource) {
  const bess::dataplane::Resource *r =
      bess::runtime::runtime().transactions().FindResource(resource);
  const bess::framework::ResourceCodec *codec =
      r == nullptr ? nullptr
                   : bess::framework::ResourceBindings::ProcessDefault().Find(*r);
  return codec != nullptr && codec->requires_paused_workers();
}

ArpResponderEntryKey EntryKey(const std::string &ip) {
  ArpResponderEntryKey key;
  key.set_ip(ip);
  return key;
}

ArpResponderEntryValue EntryValue(const std::string &mac) {
  ArpResponderEntryValue value;
  value.set_mac_addr(mac);
  return value;
}

Bytes RequestFor(const std::string &ip) {
  return PacketBuilder()
      .Ether("ff:ff:ff:ff:ff:ff", "02:1e:67:9f:4d:ae")
      .Arp(1, "02:1e:67:9f:4d:ae", "10.0.0.1", "00:00:00:00:00:00", ip)
      .Build();
}

// The entries resource (D-129): an upserted entry is answered for, reads back
// as written, and once erased is not answered for.
TEST(ArpResponderResourceTest, EntriesAnswerAndReadBack) {
  ModuleHarness h;
  Module *arp = h.Create("ArpResponder", bess::pb::ArpResponderArg());
  ASSERT_NE(nullptr, arp);
  const std::string entries = arp->name() + "/entries";
  EXPECT_TRUE(RequiresPausedWorkers(entries));
  EXPECT_TRUE(h.Run(arp, 0, {RequestFor("1.2.3.4")}, {0})[0].empty());

  const ArpResponderEntryKey key = EntryKey("1.2.3.4");
  const ArpResponderEntryValue value = EntryValue("a0:22:33:44:55:66");
  ASSERT_EQ("", h.UpsertResource(entries, key, value));
  const Bytes reply = PacketBuilder()
                          .Ether("02:1e:67:9f:4d:ae", "a0:22:33:44:55:66")
                          .Arp(2, "a0:22:33:44:55:66", "1.2.3.4", "02:1e:67:9f:4d:ae",
                               "10.0.0.1")
                          .Build();
  auto out = h.Run(arp, 0, {RequestFor("1.2.3.4")}, {0});
  ASSERT_EQ(1u, out[0].size());
  EXPECT_EQ(reply, out[0][0]);
  EXPECT_EQ("", h.ReadBackDiffers(entries, {{&key, &value}}));

  // An upsert of the same address replaces its MAC.
  const ArpResponderEntryValue moved = EntryValue("a0:22:33:44:55:77");
  ASSERT_EQ("", h.UpsertResource(entries, key, moved));
  EXPECT_EQ("", h.ReadBackDiffers(entries, {{&key, &moved}}));

  ASSERT_EQ("", h.EraseResource(entries, key));
  EXPECT_TRUE(h.Run(arp, 0, {RequestFor("1.2.3.4")}, {0})[0].empty());
  EXPECT_EQ("", h.ReadBackDiffers(entries, {}));
  EXPECT_NE("", h.EraseResource(entries, key));  // not found
}

// An upper-case MAC reads back in lower case.
TEST(ArpResponderResourceTest, EntriesReadBackCanonically) {
  ModuleHarness h;
  Module *arp = h.Create("ArpResponder", bess::pb::ArpResponderArg());
  ASSERT_NE(nullptr, arp);
  const std::string entries = arp->name() + "/entries";
  ASSERT_EQ("", h.UpsertResource(entries, EntryKey("10.1.2.3"),
                                 EntryValue("A0:22:33:44:55:66")));
  const ArpResponderEntryKey key = EntryKey("10.1.2.3");
  const ArpResponderEntryValue lower = EntryValue("a0:22:33:44:55:66");
  EXPECT_EQ("", h.ReadBackDiffers(entries, {{&key, &lower}}));
}

TEST(ArpResponderResourceTest, EntriesRefuseBadKeysAndValues) {
  ModuleHarness h;
  Module *arp = h.Create("ArpResponder", bess::pb::ArpResponderArg());
  ASSERT_NE(nullptr, arp);
  const std::string entries = arp->name() + "/entries";
  const ArpResponderEntryValue value = EntryValue("a0:22:33:44:55:66");
  EXPECT_NE("", h.UpsertResource(entries, EntryKey(""), value));
  EXPECT_NE("", h.UpsertResource(entries, EntryKey("1.2.3"), value));
  EXPECT_NE("", h.UpsertResource(entries, EntryKey("1.2.3.4"), EntryValue("nope")));
  EXPECT_NE("", h.UpsertResource(entries, EntryKey("1.2.3.4"), EntryValue("")));
  EXPECT_EQ("", h.ReadBackDiffers(entries, {}));
}

// The init argument's entry is an entry like any other (D-129).
TEST(ArpResponderResourceTest, InitEntryIsAnEntry) {
  ModuleHarness h;
  bess::pb::ArpResponderArg init;
  init.set_ip("1.2.3.4");
  init.set_mac_addr("a0:22:33:44:55:66");
  Module *arp = h.Create("ArpResponder", init);
  ASSERT_NE(nullptr, arp);
  const ArpResponderEntryKey key = EntryKey("1.2.3.4");
  const ArpResponderEntryValue value = EntryValue("a0:22:33:44:55:66");
  EXPECT_EQ("", h.ReadBackDiffers(arp->name() + "/entries", {{&key, &value}}));
}

}  // namespace
