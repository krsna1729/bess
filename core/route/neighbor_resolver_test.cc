// SPDX-License-Identifier: BSD-3-Clause

// IPv4 neighbor resolution (M15, D-175): ARP solicitation with retry and
// backoff, replies, aging, MAC changes, failure and recovery, interface and
// address identity, transactional publication, and concurrent publishing
// against packet-path lookups.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dataplane/interface_id.h"
#include "dataplane/transaction_engine.h"
#include "route/l3_packet.h"
#include "route/neighbor_resolver.h"
#include "route/neighbor_table.h"
#include "route/next_hop_id.h"
#include "route/router.h"
#include "runtime/runtime_state.h"
#include "utils/arp.h"
#include "utils/ether.h"

namespace bess::route {
namespace {

using dataplane::InterfaceId;
using dataplane::TransactionEngine;
using utils::Arp;
using utils::be32_t;
using utils::Ethernet;

constexpr InterfaceId kIface1{uint16_t{1}};
constexpr InterfaceId kIface2{uint16_t{2}};
constexpr uint32_t kPeer1 = 0x0a000002;   // 10.0.0.2
constexpr uint32_t kPeer2 = 0x0a000003;   // 10.0.0.3
constexpr uint32_t kMyIp = 0x0a000001;    // 10.0.0.1
constexpr uint32_t kMovedIp = 0x0a000009; // 10.0.0.9

Ethernet::Address Mac(const char *text) {
  Ethernet::Address mac;
  EXPECT_TRUE(mac.FromString(text));
  return mac;
}

NeighborResolver::Interface Iface(InterfaceId id, const char *mac,
                                  uint32_t address) {
  return NeighborResolver::Interface{id, Mac(mac), address};
}

NeighborResolver::Key Key(InterfaceId id, uint32_t address) {
  return NeighborResolver::Key{id, address};
}

NeighborResolver::Config TestConfig() {
  NeighborResolver::Config config;
  config.capacity = 8;
  config.timing = {
      .first_probe = 10,
      .probe_interval = 10,
      .max_probe_interval = 20,
      .max_probes = 3,
      .reachable_lifetime = 100,
      .unreachable_retry = 50,
  };
  return config;
}

std::unique_ptr<NeighborResolver> MakeResolver(
    NeighborResolver::Config config = TestConfig()) {
  auto resolver = NeighborResolver::Create(config);
  EXPECT_TRUE(resolver.has_value()) << resolver.error();
  return resolver.has_value() ? std::move(*resolver) : nullptr;
}

// The reply a neighbor with `sender_mac` sends to the solicitation in
// `request_frame`: its own MAC and address to us, addressed to our MAC.
std::array<uint8_t, kArpFrameSize> ReplyTo(
    const std::array<uint8_t, kArpFrameSize> &request_frame,
    const Ethernet::Address &sender_mac) {
  const auto request = ParseArp(request_frame);
  std::array<uint8_t, kArpFrameSize> reply{};
  if (!request) {
    ADD_FAILURE() << "a solicitation is a valid ARP request";
    return reply;
  }
  EXPECT_EQ(kArpFrameSize, BuildArpReply(request.value(), sender_mac, reply));
  return reply;
}

void SetSenderMac(std::array<uint8_t, kArpFrameSize> *frame,
                  const Ethernet::Address &mac) {
  auto *eth = reinterpret_cast<Ethernet *>(frame->data());
  auto *arp = reinterpret_cast<Arp *>(frame->data() + sizeof(Ethernet));
  eth->src_addr = mac;
  arp->sender_hw_addr = mac;
}

NeighborTable::Neighbor State(const NeighborResolver &resolver,
                              NeighborResolver::Key key) {
  return resolver.Find(key).value_or(NeighborTable::Neighbor{});
}

// -- solicitation, retry, failure, recovery ------------------------------------

TEST(NeighborResolverTest, SolicitsWithBackoffThenFailsAndRecovers) {
  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(Iface(kIface1, "02:00:00:00:00:01",
                                                 kMyIp),
                                           0)
                  .has_value());
  const NeighborResolver::Key key = Key(kIface1, kPeer1);

  auto initial = resolver->Bind(NextHopId(1), key, 0);
  ASSERT_TRUE(initial.has_value());
  EXPECT_EQ(NeighborState::kIncomplete, initial->neighbor);
  EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);

  // first_probe = 10.
  EXPECT_TRUE(resolver->Poll(9, 16).empty());
  NeighborResolver::Progress progress = resolver->Poll(10, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  EXPECT_TRUE(progress.updates.empty());
  EXPECT_EQ(kIface1, progress.solicitations[0].egress);
  const auto request = ParseArp(progress.solicitations[0].frame);
  if (!request) {
    ADD_FAILURE() << "a solicitation is a valid ARP request";
    return;
  }
  EXPECT_EQ(utils::Arp::kRequest, request.value().opcode);
  EXPECT_EQ(be32_t(kMyIp), request.value().sender_ip);
  EXPECT_EQ(be32_t(kPeer1), request.value().target_ip);
  EXPECT_EQ(Mac("02:00:00:00:00:01"), request.value().sender_mac);
  EXPECT_TRUE(
      reinterpret_cast<const Ethernet *>(progress.solicitations[0].frame.data())
          ->dst_addr.IsBroadcast());
  EXPECT_EQ(1u, resolver->armed());

  // probe_interval 10, then doubled to max_probe_interval 20.
  EXPECT_TRUE(resolver->Poll(19, 16).solicitations.empty());
  EXPECT_EQ(1u, resolver->Poll(20, 16).solicitations.size());
  EXPECT_EQ(1u, resolver->Poll(40, 16).solicitations.size());
  EXPECT_TRUE(resolver->Poll(59, 16).empty());

  // The third probe's retry fires: the attempt failed.
  progress = resolver->Poll(60, 16);
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NextHopId(1), progress.updates[0].first);
  EXPECT_EQ(NeighborState::kUnreachable, progress.updates[0].second.neighbor);
  EXPECT_EQ(Ethernet::Address{}, progress.updates[0].second.dst_mac);
  EXPECT_EQ(NeighborState::kUnreachable, State(*resolver, key).state);

  // unreachable_retry = 50: a new attempt starts.
  EXPECT_TRUE(resolver->Poll(109, 16).empty());
  progress = resolver->Poll(110, 16);
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kIncomplete, progress.updates[0].second.neighbor);
  ASSERT_EQ(1u, progress.solicitations.size());
  EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);

  // A reply resolves it; aging = 100.
  const auto reply = ReplyTo(progress.solicitations[0].frame,
                             Mac("02:00:00:00:00:aa"));
  progress = resolver->OnArp(110, kIface1, reply);
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kResolved, progress.updates[0].second.neighbor);
  EXPECT_EQ(Mac("02:00:00:00:00:aa"), progress.updates[0].second.dst_mac);
  EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);

  // Aging solicits again and takes the MAC out of service meanwhile.
  EXPECT_TRUE(resolver->Poll(209, 16).empty());
  progress = resolver->Poll(210, 16);
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kIncomplete, progress.updates[0].second.neighbor);
  EXPECT_EQ(Ethernet::Address{}, progress.updates[0].second.dst_mac);
  EXPECT_EQ(1u, progress.solicitations.size());
}

TEST(NeighborResolverTest, ARepeatedReplyRefreshesAgingWithoutRepublishing) {
  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(Iface(kIface1, "02:00:00:00:00:01",
                                                 kMyIp),
                                           0)
                  .has_value());
  const NeighborResolver::Key key = Key(kIface1, kPeer1);
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key, 0).has_value());
  auto progress = resolver->Poll(10, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  const auto reply = ReplyTo(progress.solicitations[0].frame,
                             Mac("02:00:00:00:00:aa"));

  progress = resolver->OnArp(10, kIface1, reply);
  ASSERT_EQ(1u, progress.updates.size());

  // The same answer changes nothing the dataplane reads, but it does move the
  // aging deadline: the old one (110) no longer fires.
  EXPECT_TRUE(resolver->OnArp(50, kIface1, reply).empty());
  EXPECT_TRUE(resolver->Poll(110, 16).empty());
  progress = resolver->Poll(150, 16);
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kIncomplete, progress.updates[0].second.neighbor);
  EXPECT_EQ(1u, progress.solicitations.size());
}

TEST(NeighborResolverTest, AMacChangeRepublishesEveryBoundNextHopAndNoRoute) {
  LpmRouteTable::Config config;
  auto made = Router::Create("neighbor_mac_change", config, 8,
                             bess::runtime::runtime().rcu());
  ASSERT_TRUE(made.has_value());
  Router &router = **made;

  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(Iface(kIface1, "02:00:00:00:00:01",
                                                 kMyIp),
                                           0)
                  .has_value());
  const NeighborResolver::Key key = Key(kIface1, kPeer1);
  const NextHop hop1 = resolver->Bind(NextHopId(1), key, 0).value();
  const NextHop hop2 = resolver->Bind(NextHopId(2), key, 0).value();
  const std::vector<std::pair<NextHopId, NextHop>> initial{
      std::pair{NextHopId(1), hop1}, std::pair{NextHopId(2), hop2}};
  ASSERT_TRUE(Publish(router, initial));
  const auto p1 = Ipv4Prefix::Make(0x0a000000, 8).value();
  const auto p2 = Ipv4Prefix::Make(0x14000000, 8).value();
  ASSERT_TRUE(router.SetRoute(p1, NextHopId(1)));
  ASSERT_TRUE(router.SetRoute(p2, NextHopId(2)));
  const size_t routes = router.route_count();

  auto progress = resolver->Poll(10, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  progress = resolver->OnArp(10, kIface1,
                             ReplyTo(progress.solicitations[0].frame,
                                     Mac("02:00:00:00:00:aa")));
  ASSERT_EQ(2u, progress.updates.size());
  ASSERT_TRUE(Publish(router, progress.updates));
  EXPECT_EQ(Mac("02:00:00:00:00:aa"), router.Resolve(0x0a010101)->dst_mac);
  EXPECT_EQ(Mac("02:00:00:00:00:aa"), router.Resolve(0x14010101)->dst_mac);

  // A new MAC is one update per bound hop, and the FIB did not move. Aging
  // starts a fresh solicitation; the answer carries the new MAC.
  progress = resolver->Poll(110, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  progress = resolver->OnArp(150, kIface1,
                             ReplyTo(progress.solicitations[0].frame,
                                     Mac("02:00:00:00:00:bb")));
  ASSERT_EQ(2u, progress.updates.size());
  ASSERT_TRUE(Publish(router, progress.updates));
  EXPECT_EQ(Mac("02:00:00:00:00:bb"), router.Resolve(0x0a010101)->dst_mac);
  EXPECT_EQ(Mac("02:00:00:00:00:bb"), router.Resolve(0x14010101)->dst_mac);
  EXPECT_EQ(routes, router.route_count());
}

// -- interface and address identity --------------------------------------------

TEST(NeighborResolverTest, RejectsWrongInterfaceAndMovedAddress) {
  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver
                  ->ConfigureInterface(Iface(kIface1, "02:00:00:00:00:01", kMyIp),
                                       0)
                  .has_value());
  ASSERT_TRUE(resolver
                  ->ConfigureInterface(Iface(kIface2, "02:00:00:00:00:02",
                                             kPeer2),
                                       0)
                  .has_value());
  const NeighborResolver::Key key = Key(kIface1, kPeer1);
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key, 0).has_value());
  auto progress = resolver->Poll(10, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  auto reply = ReplyTo(progress.solicitations[0].frame,
                       Mac("02:00:00:00:00:aa"));

  // The same frame on another interface is not this neighbor.
  EXPECT_TRUE(resolver->OnArp(10, kIface2, reply).empty());
  // A frame not addressed to the interface's MAC is not for it.
  auto wrong_dst = reply;
  reinterpret_cast<Ethernet *>(wrong_dst.data())->dst_addr =
      Mac("02:00:00:00:00:cc");
  EXPECT_TRUE(resolver->OnArp(10, kIface1, wrong_dst).empty());
  // The Ethernet source and the ARP sender must agree.
  auto mismatched_src = reply;
  reinterpret_cast<Ethernet *>(mismatched_src.data())->src_addr =
      Mac("02:00:00:00:00:dd");
  EXPECT_TRUE(resolver->OnArp(10, kIface1, mismatched_src).empty());
  // No entry for an address nobody bound (an unsolicited reply).
  auto stranger = reply;
  reinterpret_cast<Arp *>(stranger.data() + sizeof(Ethernet))->sender_ip_addr =
      be32_t(kPeer2);
  EXPECT_TRUE(resolver->OnArp(10, kIface1, stranger).empty());
  // A target address that is not ours.
  auto wrong_target = reply;
  reinterpret_cast<Arp *>(wrong_target.data() + sizeof(Ethernet))
      ->target_ip_addr = be32_t(0x0a000008);
  EXPECT_TRUE(resolver->OnArp(10, kIface1, wrong_target).empty());

  // The valid reply resolves it.
  progress = resolver->OnArp(10, kIface1, reply);
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);

  // Moving the interface's address invalidates the neighbor and every reply
  // that still names the old address.
  progress = resolver
                 ->ConfigureInterface(
                     Iface(kIface1, "02:00:00:00:00:01", kMovedIp), 20)
                 .value();
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kIncomplete, progress.updates[0].second.neighbor);
  EXPECT_TRUE(resolver->OnArp(20, kIface1, reply).empty());
  EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);

  // A reply for the new address is accepted.
  auto solicitation = resolver->Poll(30, 16);
  ASSERT_EQ(1u, solicitation.solicitations.size());
  EXPECT_EQ(be32_t(kMovedIp),
            ParseArp(solicitation.solicitations[0].frame).value_or(ArpMessage{}).sender_ip);
  auto moved_reply = ReplyTo(solicitation.solicitations[0].frame,
                             Mac("02:00:00:00:00:aa"));
  EXPECT_EQ(1u, resolver->OnArp(30, kIface1, moved_reply).updates.size());
}

TEST(NeighborResolverTest, RejectsUnusableSenderAddresses) {
  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(Iface(kIface1, "02:00:00:00:00:01",
                                                 kMyIp),
                                           0)
                  .has_value());
  const NeighborResolver::Key key = Key(kIface1, kPeer1);
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key, 0).has_value());
  auto progress = resolver->Poll(10, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  const auto reply = ReplyTo(progress.solicitations[0].frame,
                             Mac("02:00:00:00:00:aa"));

  for (const char *bad : {"00:00:00:00:00:00", "ff:ff:ff:ff:ff:ff",
                          "01:00:5e:00:00:01"}) {
    auto frame = reply;
    SetSenderMac(&frame, Mac(bad));
    EXPECT_TRUE(resolver->OnArp(10, kIface1, frame).empty()) << bad;
    EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);
  }
  EXPECT_EQ(1u, resolver->OnArp(10, kIface1, reply).updates.size());
}

TEST(NeighborResolverTest, ConfigureAndRemoveRestartResolutionSafely) {
  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  const auto iface = Iface(kIface1, "02:00:00:00:00:01", kMyIp);
  ASSERT_TRUE(resolver->ConfigureInterface(iface, 0).has_value());
  const NeighborResolver::Key key = Key(kIface1, kPeer1);
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key, 0).has_value());
  auto progress = resolver->Poll(10, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  progress = resolver->OnArp(10, kIface1,
                             ReplyTo(progress.solicitations[0].frame,
                                     Mac("02:00:00:00:00:aa")));
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
  EXPECT_EQ(1u, resolver->interface_epoch(kIface1));

  // The same identity does nothing.
  EXPECT_TRUE(resolver->ConfigureInterface(iface, 20).value().empty());
  EXPECT_EQ(1u, resolver->interface_epoch(kIface1));

  // A new MAC on the interface is a new generation: the neighbor must be
  // confirmed from the new source, and its solicitation carries the new MAC.
  const auto moved = Iface(kIface1, "02:00:00:00:00:09", kMyIp);
  progress = resolver->ConfigureInterface(moved, 20).value();
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kIncomplete, progress.updates[0].second.neighbor);
  // The binding captured the old source MAC; the republished hop must carry
  // the new one, or RewriteL2 would keep transmitting the old interface MAC.
  for (const auto &[id, hop] : progress.updates) {
    EXPECT_EQ(Mac("02:00:00:00:00:09"), hop.src_mac);
  }
  progress = resolver->Poll(30, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  EXPECT_EQ(Mac("02:00:00:00:00:09"),
            ParseArp(progress.solicitations[0].frame).value_or(ArpMessage{}).sender_mac);
  EXPECT_EQ(2u, resolver->interface_epoch(kIface1));
  const auto solicitation_after_move = progress.solicitations[0].frame;

  // Removing the interface makes the neighbor unreachable, forgets the
  // resolver state, and rejects later frames.
  progress = resolver->RemoveInterface(kIface1);
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kUnreachable, progress.updates[0].second.neighbor);
  EXPECT_EQ(0u, resolver->size());
  EXPECT_EQ(0u, resolver->armed());
  EXPECT_EQ(0u, resolver->interface_epoch(kIface1));
  EXPECT_EQ(NeighborState::kUnreachable, State(*resolver, key).state);
  EXPECT_TRUE(resolver->OnArp(30, kIface1,
                              ReplyTo(solicitation_after_move,
                                      Mac("02:00:00:00:00:aa")))
                  .empty());
  EXPECT_FALSE(resolver->Bind(NextHopId(2), Key(kIface1, kPeer2), 30)
                   .has_value());
}

TEST(NeighborResolverTest, RetainedLearningDiesWithTheInterfaceGeneration) {
  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(Iface(kIface1, "02:00:00:00:00:01",
                                                 kMyIp),
                                           0)
                  .has_value());
  const NeighborResolver::Key key = Key(kIface1, kPeer1);
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key, 0).has_value());
  auto progress = resolver->Poll(10, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  progress = resolver->OnArp(10, kIface1,
                             ReplyTo(progress.solicitations[0].frame,
                                     Mac("02:00:00:00:00:aa")));
  ASSERT_EQ(1u, progress.updates.size());

  // Unbinding leaves the learned neighbor in NeighborTable (by design: a
  // later Bind may reuse a still-valid answer). The resolver's own entry and
  // timer are gone.
  resolver->Unbind(NextHopId(1));
  EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
  EXPECT_EQ(0u, resolver->size());
  EXPECT_EQ(0u, resolver->armed());

  // A new interface generation must invalidate that retained learning even
  // though no hop is bound: a rebind must not resurrect the old MAC.
  progress = resolver->ConfigureInterface(
                 Iface(kIface1, "02:00:00:00:00:09", kMyIp), 20)
                 .value();
  EXPECT_TRUE(progress.empty());
  EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);
  auto rebound = resolver->Bind(NextHopId(2), key, 20);
  ASSERT_TRUE(rebound.has_value());
  EXPECT_EQ(NeighborState::kIncomplete, rebound->neighbor);
  EXPECT_EQ(Mac("02:00:00:00:00:09"), rebound->src_mac);
  progress = resolver->Poll(30, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  EXPECT_EQ(Mac("02:00:00:00:00:09"),
            ParseArp(progress.solicitations[0].frame)
                .value_or(ArpMessage{})
                .sender_mac);

  // Removal invalidates retained learning too, including when no hop is
  // bound: re-adding the interface must not revive the old MAC.
  progress = resolver->OnArp(30, kIface1,
                             ReplyTo(progress.solicitations[0].frame,
                                     Mac("02:00:00:00:00:aa")));
  ASSERT_EQ(1u, progress.updates.size());
  resolver->Unbind(NextHopId(2));
  EXPECT_TRUE(resolver->RemoveInterface(kIface1).empty());
  EXPECT_EQ(NeighborState::kUnreachable, State(*resolver, key).state);
  ASSERT_TRUE(resolver->ConfigureInterface(
                  Iface(kIface1, "02:00:00:00:00:01", kMyIp), 40)
                  .has_value());
  auto after_remove = resolver->Bind(NextHopId(3), key, 40);
  ASSERT_TRUE(after_remove.has_value());
  EXPECT_NE(NeighborState::kResolved, after_remove->neighbor);
  EXPECT_EQ(Ethernet::Address{}, after_remove->dst_mac);
}

TEST(NeighborResolverTest, CapacityAccountsForANeighborTheRebindReleases) {
  NeighborResolver::Config config = TestConfig();
  config.capacity = 1;
  auto resolver = MakeResolver(config);
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(Iface(kIface1, "02:00:00:00:00:01",
                                                 kMyIp),
                                           0)
                  .has_value());
  const NeighborResolver::Key key_a = Key(kIface1, kPeer1);
  const NeighborResolver::Key key_b = Key(kIface1, kPeer2);
  const NeighborResolver::Key key_c = Key(kIface1, 0x0a000004);

  ASSERT_TRUE(resolver->Bind(NextHopId(1), key_a, 0).has_value());
  EXPECT_EQ(1u, resolver->size());
  EXPECT_EQ(1u, resolver->armed());

  // The hop moves to a new neighbor: its old one has no hops left, so the
  // entry and timer are released and the full table accepts the new key.
  auto moved = resolver->Bind(NextHopId(1), key_b, 0);
  ASSERT_TRUE(moved.has_value());
  EXPECT_EQ(1u, resolver->size());
  EXPECT_EQ(1u, resolver->armed());
  EXPECT_FALSE(resolver->Find(key_a).has_value());
  auto progress = resolver->Poll(10, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  EXPECT_EQ(be32_t(kPeer2),
            ParseArp(progress.solicitations[0].frame)
                .value_or(ArpMessage{})
                .target_ip);

  // A new key with no hop to release is refused; a second hop on the same
  // key is not a new entry.
  EXPECT_FALSE(resolver->Bind(NextHopId(2), key_c, 10).has_value());
  ASSERT_TRUE(resolver->Bind(NextHopId(2), key_b, 10).has_value());
  // Moving hop 1 would leave hop 2 on B, so C still needs a second entry.
  EXPECT_FALSE(resolver->Bind(NextHopId(1), key_c, 10).has_value());
  EXPECT_EQ(1u, resolver->size());
  EXPECT_EQ(1u, resolver->armed());
  progress = resolver->Poll(20, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  EXPECT_EQ(be32_t(kPeer2),
            ParseArp(progress.solicitations[0].frame)
                .value_or(ArpMessage{})
                .target_ip);
}

TEST(NeighborResolverTest, RebindStartsFreshStateAndRejectsBadKeys) {
  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(Iface(kIface1, "02:00:00:00:00:01",
                                                 kMyIp),
                                           0)
                  .has_value());
  const NeighborResolver::Key key = Key(kIface1, kPeer1);
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key, 0).has_value());
  EXPECT_EQ(1u, resolver->Poll(10, 16).solicitations.size());

  resolver->Unbind(NextHopId(1));
  EXPECT_EQ(0u, resolver->size());
  EXPECT_EQ(0u, resolver->armed());

  // The old timer is gone: nothing fires at the old deadline.
  EXPECT_TRUE(resolver->Poll(10, 16).empty());
  // A later bind starts its own schedule.
  ASSERT_TRUE(resolver->Bind(NextHopId(2), key, 100).has_value());
  EXPECT_TRUE(resolver->Poll(109, 16).empty());
  EXPECT_EQ(1u, resolver->Poll(110, 16).solicitations.size());

  // Bad identities are refused.
  const auto unknown =
      resolver->Bind(NextHopId(3), Key(kIface2, kPeer2), 100);
  EXPECT_FALSE(unknown.has_value());
  EXPECT_FALSE(resolver->Bind(NextHopId(3), Key(kIface1, kMyIp), 100)
                   .has_value());
  EXPECT_FALSE(resolver->Bind(NextHopId(3), Key(kIface1, 0), 100).has_value());
  EXPECT_FALSE(resolver->Bind(NextHopId(3), Key(kIface1, 0xffffffffu), 100)
                   .has_value());
}

TEST(NeighborResolverTest, CapacityCountsNeighborsWhoseTimerIsGone) {
  NeighborResolver::Config config = TestConfig();
  config.capacity = 1;
  config.timing.reachable_lifetime = 0;  // a resolved neighbor is not timed
  auto resolver = MakeResolver(config);
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(Iface(kIface1, "02:00:00:00:00:01",
                                                 kMyIp),
                                           0)
                  .has_value());
  const NeighborResolver::Key key_a = Key(kIface1, kPeer1);
  const NeighborResolver::Key key_b = Key(kIface1, kPeer2);
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key_a, 0).has_value());
  auto progress = resolver->Poll(10, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  ASSERT_EQ(1u, resolver->OnArp(10, kIface1,
                                ReplyTo(progress.solicitations[0].frame,
                                        Mac("02:00:00:00:00:aa")))
                    .updates.size());
  EXPECT_EQ(1u, resolver->size());
  EXPECT_EQ(0u, resolver->armed());  // resolved, aging disabled

  // The entry limit is about neighbors, not armed timers.
  EXPECT_FALSE(resolver->Bind(NextHopId(2), key_b, 10).has_value());
  // The hop's own entry is released by moving it, so the rebind fits. The
  // old neighbor's learning stays in the table (same interface generation),
  // but the resolver tracks only the new entry.
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key_b, 10).has_value());
  EXPECT_EQ(1u, resolver->size());
  EXPECT_TRUE(resolver->Find(key_b).has_value());
}

// -- publication ---------------------------------------------------------------

TEST(NeighborResolverTest, ATransactionPublishesAllAdjacencyOrNone) {
  TransactionEngine engine{bess::runtime::runtime().rcu()};
  LpmRouteTable::Config config;
  auto made = Router::Create("neighbor_txn", config, 8,
                             bess::runtime::runtime().rcu());
  ASSERT_TRUE(made.has_value());
  Router &router = **made;
  ASSERT_TRUE(router.Enroll(engine.registry()).has_value());

  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(Iface(kIface1, "02:00:00:00:00:01",
                                                 kMyIp),
                                           0)
                  .has_value());
  const NeighborResolver::Key key = Key(kIface1, kPeer1);
  const NextHop hop1 = resolver->Bind(NextHopId(1), key, 0).value();
  const NextHop hop2 = resolver->Bind(NextHopId(2), key, 0).value();
  const auto p1 = Ipv4Prefix::Make(0x0a000000, 8).value();
  const auto p2 = Ipv4Prefix::Make(0x14000000, 8).value();
  std::vector<dataplane::Op> ops{
      router.SetNextHopOp(NextHopId(1), hop1),
      router.SetNextHopOp(NextHopId(2), hop2),
      router.SetRouteOp(p1, NextHopId(1)),
      router.SetRouteOp(p2, NextHopId(2))};
  auto result = engine.Apply(ops);
  ASSERT_EQ(TransactionEngine::Outcome::kApplied, result.outcome);

  auto progress = resolver->Poll(10, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  progress = resolver->OnArp(10, kIface1,
                             ReplyTo(progress.solicitations[0].frame,
                                     Mac("02:00:00:00:00:aa")));
  ASSERT_EQ(2u, progress.updates.size());
  result = engine.Apply(NextHopOps(router, progress.updates));
  ASSERT_EQ(TransactionEngine::Outcome::kApplied, result.outcome);
  ASSERT_EQ(Mac("02:00:00:00:00:aa"), router.Resolve(0x0a010101)->dst_mac);
  ASSERT_EQ(Mac("02:00:00:00:00:aa"), router.Resolve(0x14010101)->dst_mac);

  // A MAC change with one invalid operation publishes nothing.
  progress = resolver->Poll(110, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  progress = resolver->OnArp(150, kIface1,
                             ReplyTo(progress.solicitations[0].frame,
                                     Mac("02:00:00:00:00:bb")));
  ASSERT_EQ(2u, progress.updates.size());
  std::vector<dataplane::Op> change_ops =
      NextHopOps(router, progress.updates);
  // A next hop that does not exist rejects the whole transaction, so neither
  // update becomes visible. Publication is referential (per hop, each object
  // whole), not a snapshot: this asserts rejection, not simultaneous
  // visibility.
  NextHop bogus = progress.updates[0].second;
  change_ops.push_back(router.SetNextHopOp(NextHopId(99), bogus));
  result = engine.Apply(change_ops);
  EXPECT_NE(TransactionEngine::Outcome::kApplied, result.outcome);
  EXPECT_EQ(Mac("02:00:00:00:00:aa"), router.Resolve(0x0a010101)->dst_mac);
  EXPECT_EQ(Mac("02:00:00:00:00:aa"), router.Resolve(0x14010101)->dst_mac);
}

// Readers resolve the route while the writer publishes every learned MAC.
// Each lookup must see a whole NextHop: the right egress and source, and one
// of the two destination MACs, never a mixture or an unresolved state.
TEST(NeighborResolverTest, ConcurrentPublishAndLookupNeverSeeAHalfUpdate) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  LpmRouteTable::Config config;
  auto made = Router::Create("neighbor_concurrent", config, 4, domain);
  ASSERT_TRUE(made.has_value());
  Router &router = **made;
  const Ethernet::Address mac_a = Mac("02:00:00:00:00:aa");
  const Ethernet::Address mac_b = Mac("02:00:00:00:00:bb");
  const Ethernet::Address my_mac = Mac("02:00:00:00:00:01");
  NextHop initial;
  initial.egress = kIface1;
  initial.neighbor = NeighborState::kResolved;
  initial.dst_mac = mac_a;
  initial.src_mac = my_mac;
  ASSERT_TRUE(router.SetNextHop(NextHopId(1), initial));
  const auto prefix = Ipv4Prefix::Make(0x0a000000, 8).value();
  ASSERT_TRUE(router.SetRoute(prefix, NextHopId(1)));
  constexpr uint32_t kDst = 0x0a010101;

  NeighborResolver::Config resolver_config = TestConfig();
  resolver_config.timing.reachable_lifetime = 1000000;  // no aging in the test
  auto resolver = MakeResolver(resolver_config);
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(Iface(kIface1, "02:00:00:00:00:01",
                                                 kMyIp),
                                           0)
                  .has_value());
  const NeighborResolver::Key key = Key(kIface1, kPeer1);
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key, 0).has_value());
  auto progress = resolver->Poll(10, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  const auto request = progress.solicitations[0].frame;

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> bad{0}, lookups{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 2; r++) {
    const uint32_t reader_id = 30 + r;
    ASSERT_TRUE(domain.Register(reader_id).has_value());
    readers.emplace_back([&, reader_id] {
      domain.Online(reader_id);
      uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const NextHop *hop = router.Resolve(kDst);
        if (hop == nullptr || hop->neighbor != NeighborState::kResolved ||
            hop->egress != kIface1 || hop->src_mac != my_mac ||
            (hop->dst_mac != mac_a && hop->dst_mac != mac_b)) {
          bad++;
        }
        local++;
        domain.Quiescent(reader_id);
      }
      lookups += local;
      domain.Offline(reader_id);
    });
  }

  uint64_t now = 10;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (std::chrono::steady_clock::now() < deadline) {
    const Ethernet::Address &mac = (now & uint64_t{1}) ? mac_a : mac_b;
    auto updates = resolver->OnArp(now++, kIface1, ReplyTo(request, mac));
    if (!updates.updates.empty()) {
      ASSERT_TRUE(Publish(router, updates.updates));
    }
  }
  stop = true;
  for (auto &t : readers) {
    t.join();
  }
  for (int r = 0; r < 2; r++) {
    domain.Unregister(30 + r);
  }
  EXPECT_EQ(0u, bad.load()) << "of " << lookups.load() << " lookups";
  EXPECT_GT(lookups.load(), 100u);
}

// The in-place responder the VIF uses: the same reply fields the ad-hoc code
// wrote, and no change for a request that is not for this address.
TEST(NeighborResolverTest, AnswerArpRequestRewritesARequestForMyAddressOnly) {
  Ethernet::Address me = Mac("02:00:00:00:00:01");
  Ethernet::Address peer = Mac("02:00:00:00:00:aa");
  std::array<uint8_t, kArpFrameSize> frame{};
  ASSERT_EQ(kArpFrameSize,
            BuildArpRequest(peer, be32_t(kPeer1), be32_t(kMyIp), frame));

  auto untouched = frame;
  EXPECT_FALSE(AnswerArpRequest(std::span<uint8_t>(untouched.data(),
                                                   untouched.size()),
                                me, be32_t(0x0a000008)));
  EXPECT_EQ(0, std::memcmp(frame.data(), untouched.data(), frame.size()));

  ASSERT_TRUE(AnswerArpRequest(
      std::span<uint8_t>(frame.data(), frame.size()), me, be32_t(kMyIp)));
  const auto reply = ParseArp(frame);
  if (!reply) {
    ADD_FAILURE() << "the rewritten frame is a valid ARP reply";
    return;
  }
  EXPECT_EQ(utils::Arp::kReply, reply.value().opcode);
  EXPECT_EQ(me, reply.value().sender_mac);
  EXPECT_EQ(be32_t(kMyIp), reply.value().sender_ip);
  EXPECT_EQ(peer, reply.value().target_mac);
  EXPECT_EQ(be32_t(kPeer1), reply.value().target_ip);
  EXPECT_EQ(peer, reinterpret_cast<const Ethernet *>(frame.data())->dst_addr);
  EXPECT_EQ(me, reinterpret_cast<const Ethernet *>(frame.data())->src_addr);

  // A reply is not answered again.
  EXPECT_FALSE(AnswerArpRequest(
      std::span<uint8_t>(frame.data(), frame.size()), me, be32_t(kMyIp)));
}

}  // namespace
}  // namespace bess::route
