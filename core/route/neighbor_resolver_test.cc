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
#include <new>
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
#include "testing/allocation_faults.h"
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
  return NeighborResolver::Interface{id, Mac(mac), address, {}};
}

Ipv6Address Ipv6(const char *text) {
  const auto parsed = bess::utils::ParseIpv6Address(text);
  EXPECT_TRUE(parsed.has_value()) << text;
  return parsed.value_or(Ipv6Address{});
}

NdMessage Nd(std::optional<NdMessage> message) {
  EXPECT_TRUE(message.has_value());
  return message.value_or(
      NdMessage{NdType::kNeighborSolicitation});
}

NeighborResolver::Interface Iface6(InterfaceId id, const char *mac,
                                   const char *ipv6) {
  return NeighborResolver::Interface{id, Mac(mac), 0, Ipv6(ipv6)};
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
    std::span<const uint8_t> request_frame,
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

  // Aging solicits again but keeps the neighbor in service: nothing the
  // dataplane reads changes while it is reconfirmed.
  EXPECT_TRUE(resolver->Poll(209, 16).empty());
  progress = resolver->Poll(210, 16);
  EXPECT_TRUE(progress.updates.empty());
  EXPECT_EQ(1u, progress.solicitations.size());
  EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
  EXPECT_EQ(Mac("02:00:00:00:00:aa"), State(*resolver, key).mac);

  // The reconfirmation backs off like a first attempt (10, then 20) and keeps
  // the MAC throughout.
  EXPECT_TRUE(resolver->Poll(219, 16).empty());
  progress = resolver->Poll(220, 16);
  EXPECT_TRUE(progress.updates.empty());
  EXPECT_EQ(1u, progress.solicitations.size());
  progress = resolver->Poll(240, 16);
  EXPECT_TRUE(progress.updates.empty());
  EXPECT_EQ(1u, progress.solicitations.size());
  EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);

  // Only when max_probes went unanswered does it fail closed.
  EXPECT_TRUE(resolver->Poll(259, 16).empty());
  progress = resolver->Poll(260, 16);
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kUnreachable, progress.updates[0].second.neighbor);
  EXPECT_EQ(Ethernet::Address{}, progress.updates[0].second.dst_mac);
  EXPECT_TRUE(progress.solicitations.empty());
  EXPECT_EQ(NeighborState::kUnreachable, State(*resolver, key).state);
}

// A reply during reconfirmation ends it: the neighbor stays resolved with no
// republication, and the next aging is a full reachable lifetime later.
TEST(NeighborResolverTest, AReplyDuringReconfirmationKeepsTheNeighbor) {
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
  ASSERT_EQ(1u, resolver->OnArp(10, kIface1, reply).updates.size());

  // Aging at 110 solicits; the neighbor answers before the attempt runs out.
  progress = resolver->Poll(110, 16);
  EXPECT_TRUE(progress.updates.empty());
  ASSERT_EQ(1u, progress.solicitations.size());
  EXPECT_TRUE(resolver->Poll(120, 16).updates.empty());
  EXPECT_TRUE(resolver->OnArp(125, kIface1, reply).empty());
  EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);

  // The attempt's remaining retries are gone, and nothing fails at 140/160.
  EXPECT_TRUE(resolver->Poll(224, 16).empty());
  EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
  // The next aging, 100 after the reply, starts a fresh reconfirmation.
  progress = resolver->Poll(225, 16);
  EXPECT_TRUE(progress.updates.empty());
  EXPECT_EQ(1u, progress.solicitations.size());
}

TEST(NeighborResolverTest, PollHonorsWorkBudget) {
  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(
                  Iface(kIface1, "02:00:00:00:00:01", kMyIp), 0)
                  .has_value());
  ASSERT_TRUE(resolver->Bind(NextHopId(1), Key(kIface1, kPeer1), 0).has_value());
  ASSERT_TRUE(
      resolver->Bind(NextHopId(2), Key(kIface1, kPeer1 + 1), 0).has_value());

  const auto first = resolver->Poll(10, 1);
  ASSERT_EQ(1u, first.solicitations.size());
  const auto second = resolver->Poll(10, 1);
  ASSERT_EQ(1u, second.solicitations.size());
  EXPECT_NE(first.solicitations[0].frame, second.solicitations[0].frame);
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
  EXPECT_TRUE(progress.updates.empty()) << "aging keeps the neighbor in service";
  EXPECT_EQ(1u, progress.solicitations.size());
  EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
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
  EXPECT_EQ(Mac("02:00:00:00:00:09"),
              rebound.value_or(NextHop{}).src_mac);
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

// -- allocation failure --------------------------------------------------------

using bess::fault_injection::AllocationFaults;
using bess::fault_injection::ForEachFailurePoint;

// A resolver with one learned neighbor: interface 1 at the old MAC, hop 1
// bound to 10.0.0.2, resolved to 02:00:00:00:00:aa, aging armed for tick 110.
// Every failure point needs this state, so each iteration builds it afresh.
std::unique_ptr<NeighborResolver> ResolvedWithOldMac(
    NeighborResolver::Key *key) {
  auto resolver = MakeResolver();
  if (!resolver) {
    return nullptr;
  }
  if (!resolver
           ->ConfigureInterface(Iface(kIface1, "02:00:00:00:00:01", kMyIp), 0)
           .has_value()) {
    ADD_FAILURE() << "configure";
    return nullptr;
  }
  *key = Key(kIface1, kPeer1);
  if (!resolver->Bind(NextHopId(1), *key, 0).has_value()) {
    ADD_FAILURE() << "bind";
    return nullptr;
  }
  NeighborResolver::Progress progress = resolver->Poll(10, 16);
  if (progress.solicitations.size() != 1) {
    ADD_FAILURE() << "solicitation";
    return nullptr;
  }
  progress = resolver->OnArp(10, kIface1,
                             ReplyTo(progress.solicitations[0].frame,
                                     Mac("02:00:00:00:00:aa")));
  if (progress.updates.size() != 1) {
    ADD_FAILURE() << "reply";
    return nullptr;
  }
  return resolver;
}

// The same, for two neighbors so both are due in one Poll: both resolved at
// tick 10, both aging at 110. Used to prove a refused allocation does not
// strand the other due entries or discard their transitions.
std::unique_ptr<NeighborResolver> ResolvedPair(NeighborResolver::Key *a,
                                               NeighborResolver::Key *b) {
  auto resolver = MakeResolver();
  if (!resolver) {
    return nullptr;
  }
  if (!resolver
           ->ConfigureInterface(Iface(kIface1, "02:00:00:00:00:01", kMyIp), 0)
           .has_value()) {
    ADD_FAILURE() << "configure";
    return nullptr;
  }
  *a = Key(kIface1, kPeer1);
  *b = Key(kIface1, kPeer2);
  if (!resolver->Bind(NextHopId(1), *a, 0).has_value() ||
      !resolver->Bind(NextHopId(2), *b, 0).has_value()) {
    ADD_FAILURE() << "bind";
    return nullptr;
  }
  NeighborResolver::Progress progress = resolver->Poll(10, 16);
  if (progress.solicitations.size() != 2) {
    ADD_FAILURE() << "solicitations";
    return nullptr;
  }
  for (const auto &solicitation : progress.solicitations) {
    const uint32_t target =
        ParseArp(solicitation.frame).value_or(ArpMessage{}).target_ip.value();
    const auto reply = ReplyTo(
        solicitation.frame,
        Mac(target == kPeer1 ? "02:00:00:00:00:aa" : "02:00:00:00:00:bb"));
    if (resolver->OnArp(10, kIface1, reply).updates.size() != 1) {
      ADD_FAILURE() << "reply";
      return nullptr;
    }
  }
  return resolver;
}

// A refused allocation while Poll plans a transition must not lose the timer
// or change the table: every due entry is re-armed at its own deadline, and
// the retry produces the same transitions. Checked at both aging steps: the
// start of a reconfirmation (solicitations, no table change) and its failure
// (no solicitation, both neighbors unreachable).
TEST(NeighborResolverFaultTest, PollAllocationFailureLosesNoTimerOrUpdate) {
  // The reconfirmation's probes before `until`, outside the fault window.
  const auto probe_until = [](NeighborResolver &resolver, uint64_t until) {
    for (const uint64_t t : {uint64_t{110}, uint64_t{120}, uint64_t{140}}) {
      if (t >= until) {
        return;
      }
      const auto progress = resolver.Poll(t, 16);
      EXPECT_TRUE(progress.updates.empty());
      EXPECT_EQ(2u, progress.solicitations.size());
    }
  };
  struct Step {
    uint64_t now;
    size_t updates;
    size_t solicitations;
    NeighborState after;
  };
  for (const Step step : {Step{110, 0, 2, NeighborState::kResolved},
                          Step{160, 2, 0, NeighborState::kUnreachable}}) {
    SCOPED_TRACE(step.now);
    const size_t points = ForEachFailurePoint([&](size_t fail_at) {
      NeighborResolver::Key key_a;
      NeighborResolver::Key key_b;
      auto resolver = ResolvedPair(&key_a, &key_b);
      if (!resolver) {
        return;
      }
      probe_until(*resolver, step.now);
      EXPECT_EQ(NeighborState::kResolved, State(*resolver, key_a).state);
      EXPECT_EQ(NeighborState::kResolved, State(*resolver, key_b).state);
      EXPECT_EQ(2u, resolver->armed());
      bool threw = false;
      bool injected = false;
      NeighborResolver::Progress progress;
      {
        AllocationFaults faults(fail_at);
        try {
          progress = resolver->Poll(step.now, 16);
        } catch (const std::bad_alloc &) {
          threw = true;
        }
        injected = faults.injected();
      }
      if (!injected) {
        EXPECT_FALSE(threw);
        EXPECT_EQ(step.updates, progress.updates.size());
        EXPECT_EQ(step.solicitations, progress.solicitations.size());
        EXPECT_EQ(step.after, State(*resolver, key_a).state);
        EXPECT_EQ(step.after, State(*resolver, key_b).state);
        return;
      }
      EXPECT_TRUE(threw);
      // Nothing committed, and both timers are still live (re-armed at their
      // deadline after the wheel spent their nodes during planning).
      EXPECT_EQ(NeighborState::kResolved, State(*resolver, key_a).state);
      EXPECT_EQ(NeighborState::kResolved, State(*resolver, key_b).state);
      EXPECT_EQ(Mac("02:00:00:00:00:aa"), State(*resolver, key_a).mac);
      EXPECT_EQ(Mac("02:00:00:00:00:bb"), State(*resolver, key_b).mac);
      EXPECT_EQ(2u, resolver->armed());

      // The retry converges to the transitions the refused poll was making,
      // for both entries, so no other due work was lost either.
      progress = resolver->Poll(step.now, 16);
      EXPECT_EQ(step.updates, progress.updates.size());
      EXPECT_EQ(step.solicitations, progress.solicitations.size());
      EXPECT_EQ(step.after, State(*resolver, key_a).state);
      EXPECT_EQ(step.after, State(*resolver, key_b).state);
      EXPECT_EQ(2u, resolver->armed());
    });
    EXPECT_GT(points, 0u);
  }
}

// A refused allocation while ConfigureInterface stages an identity change
// leaves the old interface generation, the learned neighbor, its binding's
// source MAC and its timer untouched; retrying the same call converges.
TEST(NeighborResolverFaultTest, ConfigureAllocationFailureLeavesIdentityWhole) {
  const NeighborResolver::Interface moved = Iface(kIface1, "02:00:00:00:00:09", kMyIp);
  const size_t points = ForEachFailurePoint([&](size_t fail_at) {
    NeighborResolver::Key key;
    auto resolver = ResolvedWithOldMac(&key);
    if (!resolver) {
      return;
    }
    const auto binding_opt = resolver->table().BindingOf(NextHopId(1));
    const NeighborTable::Bound binding =
        binding_opt.value_or(NeighborTable::Bound{});
    if (!binding_opt.has_value()) {
      ADD_FAILURE() << "binding";
      return;
    }
    EXPECT_EQ(1u, resolver->interface_epoch(kIface1));
    bool threw = false;
    bool injected = false;
    std::expected<NeighborResolver::Progress, std::string> progress;
    {
      AllocationFaults faults(fail_at);
      try {
        progress = resolver->ConfigureInterface(moved, 20);
      } catch (const std::bad_alloc &) {
        threw = true;
      }
      injected = faults.injected();
    }
    if (!injected) {
      EXPECT_FALSE(threw);
      ASSERT_TRUE(progress.has_value());
      EXPECT_EQ(2u, resolver->interface_epoch(kIface1));
      EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);
      EXPECT_EQ(1u, progress.value().updates.size());
      EXPECT_EQ(Mac("02:00:00:00:00:09"),
                progress.value().updates[0].second.src_mac);
      return;
    }
    EXPECT_TRUE(threw);
    // The failed call changed neither the interface nor the neighbor.
    EXPECT_EQ(1u, resolver->interface_epoch(kIface1));
    EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
    EXPECT_EQ(Mac("02:00:00:00:00:aa"), State(*resolver, key).mac);
    const auto unchanged = resolver->table().BindingOf(NextHopId(1));
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(binding.src_mac,
              unchanged.value_or(NeighborTable::Bound{}).src_mac);
    EXPECT_EQ(1u, resolver->armed());

    // Retrying the same change (the old identity is still current) commits.
    auto retried = resolver->ConfigureInterface(moved, 20);
    ASSERT_TRUE(retried.has_value());
    EXPECT_EQ(2u, resolver->interface_epoch(kIface1));
    EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);
    EXPECT_EQ(1u,
              retried.value_or(NeighborResolver::Progress{}).updates.size());
    EXPECT_EQ(Mac("02:00:00:00:00:09"),
              retried.value_or(NeighborResolver::Progress{})
                  .updates[0]
                  .second.src_mac);
    const auto rebound = resolver->table().BindingOf(NextHopId(1));
    ASSERT_TRUE(rebound.has_value());
    EXPECT_EQ(Mac("02:00:00:00:00:09"),
              rebound.value_or(NeighborTable::Bound{}).src_mac);
  });
  EXPECT_GT(points, 0u);
}

// RemoveInterface stages every update before erasing the interface or
// touching a neighbor; a refusal leaves all of it in place and the retry
// converges to unreachable.
TEST(NeighborResolverFaultTest, RemoveAllocationFailureLeavesInterfaceWhole) {
  const size_t points = ForEachFailurePoint([&](size_t fail_at) {
    NeighborResolver::Key key;
    auto resolver = ResolvedWithOldMac(&key);
    if (!resolver) {
      return;
    }
    const auto binding_opt = resolver->table().BindingOf(NextHopId(1));
    const NeighborTable::Bound binding =
        binding_opt.value_or(NeighborTable::Bound{});
    if (!binding_opt.has_value()) {
      ADD_FAILURE() << "binding";
      return;
    }
    EXPECT_EQ(1u, resolver->interface_epoch(kIface1));
    bool threw = false;
    bool injected = false;
    NeighborResolver::Progress progress;
    {
      AllocationFaults faults(fail_at);
      try {
        progress = resolver->RemoveInterface(kIface1);
      } catch (const std::bad_alloc &) {
        threw = true;
      }
      injected = faults.injected();
    }
    if (!injected) {
      EXPECT_FALSE(threw);
      EXPECT_EQ(0u, resolver->interface_epoch(kIface1));
      EXPECT_EQ(1u, progress.updates.size());
      EXPECT_EQ(NeighborState::kUnreachable, State(*resolver, key).state);
      EXPECT_EQ(0u, resolver->armed());
      return;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(1u, resolver->interface_epoch(kIface1));
    EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
    EXPECT_EQ(Mac("02:00:00:00:00:aa"), State(*resolver, key).mac);
    const auto unchanged = resolver->table().BindingOf(NextHopId(1));
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(binding.src_mac,
              unchanged.value_or(NeighborTable::Bound{}).src_mac);
    EXPECT_EQ(1u, resolver->armed());

    progress = resolver->RemoveInterface(kIface1);
    EXPECT_EQ(0u, resolver->interface_epoch(kIface1));
    EXPECT_EQ(1u, progress.updates.size());
    EXPECT_EQ(NeighborState::kUnreachable, State(*resolver, key).state);
    EXPECT_EQ(0u, resolver->armed());
  });
  EXPECT_GT(points, 0u);
}

// -- IPv6 neighbor discovery ---------------------------------------------------

constexpr size_t kEthFrameWithoutOption =
    sizeof(Ethernet) + kIpv6HeaderSize + kNdMessageSize;

const Ipv6Address kMy6 = Ipv6("fe80::1");
const Ipv6Address kPeer6 = Ipv6("fe80::2");
const Ipv6Address kOther6 = Ipv6("fe80::3");

// An NA from `sender` (source and target both it) to our address.
std::array<uint8_t, kNdFrameSize> Advertisement(
    const Ipv6Address &address, const Ethernet::Address &sender_mac,
    bool solicited = true, bool override = true,
    const Ipv6Address &destination_ip = kMy6,
    const Ethernet::Address &destination_mac = Mac("02:00:00:00:00:01")) {
  std::array<uint8_t, kNdFrameSize> frame{};
  EXPECT_EQ(kNdFrameSize, BuildNeighborAdvertisement(
                              sender_mac, address, address, destination_mac,
                              destination_ip, solicited, override, frame));
  return frame;
}

// The ND analogue: one IPv6 neighbor resolved at tick 10, aging at 110.
std::unique_ptr<NeighborResolver> NdResolved(NeighborResolver::Key *key) {
  auto resolver = MakeResolver();
  if (!resolver) {
    return nullptr;
  }
  if (!resolver
           ->ConfigureInterface(
               Iface6(kIface1, "02:00:00:00:00:01", "fe80::1"), 0)
           .has_value()) {
    ADD_FAILURE() << "configure";
    return nullptr;
  }
  *key = NeighborResolver::Key(kIface1, kPeer6);
  if (!resolver->Bind(NextHopId(1), *key, 0).has_value()) {
    ADD_FAILURE() << "bind";
    return nullptr;
  }
  if (resolver->Poll(10, 16).solicitations.size() != 1) {
    ADD_FAILURE() << "solicitation";
    return nullptr;
  }
  if (resolver
          ->OnNd(10, kIface1, Advertisement(kPeer6, Mac("02:00:00:00:00:aa")))
          .updates.size() != 1) {
    ADD_FAILURE() << "reply";
    return nullptr;
  }
  return resolver;
}

TEST(NeighborResolverTest, NdSolicitationResolvesAndAges) {
  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(
                  Iface6(kIface1, "02:00:00:00:00:01", "fe80::1"), 0)
                  .has_value());
  const NeighborResolver::Key key(kIface1, kPeer6);
  auto bound = resolver->Bind(NextHopId(1), key, 0);
  ASSERT_TRUE(bound.has_value());
  const NextHop initial = bound.value_or(NextHop{});
  EXPECT_EQ(NeighborState::kIncomplete, initial.neighbor);

  auto progress = resolver->Poll(10, 16);
  ASSERT_EQ(1u, progress.solicitations.size());
  EXPECT_EQ(kNdFrameSize, progress.solicitations[0].length);
  EXPECT_EQ(kIface1, progress.solicitations[0].egress);
  const NdMessage solicitation = Nd(ParseNeighborDiscovery(
      std::span<const uint8_t>(progress.solicitations[0].frame.data(),
                               progress.solicitations[0].length)));
  EXPECT_EQ(NdType::kNeighborSolicitation, solicitation.type);
  EXPECT_EQ(kMy6, solicitation.source_ip);
  EXPECT_EQ(SolicitedNodeAddress(kPeer6), solicitation.destination_ip);
  EXPECT_EQ(kPeer6, solicitation.target_ip);
  EXPECT_EQ(Mac("02:00:00:00:00:01"), solicitation.source_mac);
  const auto *eth = reinterpret_cast<const Ethernet *>(
      progress.solicitations[0].frame.data());
  EXPECT_EQ(SolicitedNodeMac(kPeer6), eth->dst_addr);
  EXPECT_EQ(Mac("02:00:00:00:00:01"), eth->src_addr);

  progress = resolver->OnNd(
      10, kIface1,
      Advertisement(kPeer6, Mac("02:00:00:00:00:aa")));
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kResolved, progress.updates[0].second.neighbor);
  EXPECT_EQ(Mac("02:00:00:00:00:aa"), progress.updates[0].second.dst_mac);
  EXPECT_EQ(Mac("02:00:00:00:00:01"), progress.updates[0].second.src_mac);
  EXPECT_EQ(kIface1, progress.updates[0].second.egress);
  EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);

  // Aging solicits again over ND and keeps the neighbor in service.
  progress = resolver->Poll(110, 16);
  EXPECT_TRUE(progress.updates.empty());
  EXPECT_EQ(Mac("02:00:00:00:00:aa"), State(*resolver, key).mac);
  ASSERT_EQ(1u, progress.solicitations.size());
  EXPECT_EQ(NdType::kNeighborSolicitation,
            Nd(ParseNeighborDiscovery(std::span<const uint8_t>(
                   progress.solicitations[0].frame.data(),
                   progress.solicitations[0].length)))
                .type);

  // Unanswered (probes at 120 and 140), it fails closed at 160.
  EXPECT_TRUE(resolver->Poll(120, 16).updates.empty());
  EXPECT_TRUE(resolver->Poll(140, 16).updates.empty());
  EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
  progress = resolver->Poll(160, 16);
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kUnreachable, progress.updates[0].second.neighbor);
  EXPECT_EQ(Ethernet::Address{}, progress.updates[0].second.dst_mac);
}

TEST(NeighborResolverTest, NdAdvertisementsFollowMacAndOverrideSemantics) {
  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(
                  Iface6(kIface1, "02:00:00:00:00:01", "fe80::1"), 0)
                  .has_value());
  const NeighborResolver::Key key(kIface1, kPeer6);
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key, 0).has_value());
  ASSERT_EQ(1u, resolver->Poll(10, 16).solicitations.size());
  ASSERT_EQ(1u,
            resolver
                ->OnNd(10, kIface1,
                       Advertisement(kPeer6, Mac("02:00:00:00:00:aa")))
                .updates.size());

  // An unsolicited advertisement without override cannot replace a resolved
  // MAC.
  EXPECT_TRUE(resolver
                  ->OnNd(20, kIface1,
                         Advertisement(kPeer6, Mac("02:00:00:00:00:bb"),
                                       /*solicited=*/false,
                                       /*override=*/false))
                  .empty());
  EXPECT_EQ(Mac("02:00:00:00:00:aa"), State(*resolver, key).mac);

  // An all-nodes advertisement with Solicited set is invalid and changes
  // nothing.
  EXPECT_TRUE(resolver
                  ->OnNd(20, kIface1,
                         Advertisement(kPeer6, Mac("02:00:00:00:00:bb"),
                                       /*solicited=*/true, /*override=*/true,
                                       bess::route::AllNodesAddress(),
                                       bess::route::AllNodesMac()))
                  .empty());
  EXPECT_EQ(Mac("02:00:00:00:00:aa"), State(*resolver, key).mac);

  // A solicited (or overriding) one can, including the RFC 4861 unsolicited
  // form sent to all-nodes.
  auto progress = resolver->OnNd(
      20, kIface1,
      Advertisement(kPeer6, Mac("02:00:00:00:00:bb"), false, true,
                    bess::route::AllNodesAddress(),
                    bess::route::AllNodesMac()));
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(Mac("02:00:00:00:00:bb"), progress.updates[0].second.dst_mac);
  EXPECT_EQ(Mac("02:00:00:00:00:bb"), State(*resolver, key).mac);
}

TEST(NeighborResolverTest, NdRetriesFailAndRecover) {
  NeighborResolver::Config config = TestConfig();
  config.timing.max_probes = 2;
  auto resolver = MakeResolver(config);
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(
                  Iface6(kIface1, "02:00:00:00:00:01", "fe80::1"), 0)
                  .has_value());
  const NeighborResolver::Key key(kIface1, kPeer6);
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key, 0).has_value());

  EXPECT_EQ(1u, resolver->Poll(10, 16).solicitations.size());
  EXPECT_EQ(1u, resolver->Poll(20, 16).solicitations.size());
  auto progress = resolver->Poll(40, 16);
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kUnreachable, progress.updates[0].second.neighbor);

  progress = resolver->Poll(90, 16);
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kIncomplete, progress.updates[0].second.neighbor);
  ASSERT_EQ(1u, progress.solicitations.size());
  progress = resolver->OnNd(
      90, kIface1, Advertisement(kPeer6, Mac("02:00:00:00:00:aa")));
  ASSERT_EQ(1u, progress.updates.size());
  EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
}

TEST(NeighborResolverTest, NdIdentityRejectsWrongInterfaceTargetAndSource) {
  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(
                  Iface6(kIface1, "02:00:00:00:00:01", "fe80::1"), 0)
                  .has_value());
  ASSERT_TRUE(resolver->ConfigureInterface(
                  Iface6(kIface2, "02:00:00:00:00:02", "fe80::10"), 0)
                  .has_value());
  const NeighborResolver::Key key(kIface1, kPeer6);
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key, 0).has_value());
  ASSERT_EQ(1u, resolver->Poll(10, 16).solicitations.size());
  const auto good = Advertisement(kPeer6, Mac("02:00:00:00:00:aa"));

  // Another interface, another target, a source that is not the target, the
  // wrong Ethernet destination, and an unusable sender MAC are all ignored.
  std::array<uint8_t, kNdFrameSize> for_iface2{};
  ASSERT_EQ(kNdFrameSize,
            BuildNeighborAdvertisement(Mac("02:00:00:00:00:aa"), kPeer6, kPeer6,
                                       Mac("02:00:00:00:00:02"),
                                       Ipv6("fe80::10"), true, true,
                                       for_iface2));
  EXPECT_TRUE(resolver->OnNd(10, kIface2, for_iface2).empty());
  EXPECT_TRUE(resolver->OnNd(10, kIface2, good).empty());
  EXPECT_TRUE(
      resolver->OnNd(10, kIface1,
                     Advertisement(kOther6, Mac("02:00:00:00:00:aa")))
          .empty());
  EXPECT_TRUE(resolver
                  ->OnNd(10, kIface1,
                         [&] {
                           std::array<uint8_t, kNdFrameSize> frame{};
                           EXPECT_EQ(kNdFrameSize, BuildNeighborAdvertisement(
                                                       Mac("02:00:00:00:00:aa"),
                                                       kPeer6, kOther6,
                                                       Mac("02:00:00:00:00:01"),
                                                       kMy6, true, true,
                                                       frame));
                           return frame;
                         }())
                  .empty());
  auto wrong_dst = good;
  reinterpret_cast<Ethernet *>(wrong_dst.data())->dst_addr =
      Mac("02:00:00:00:00:cc");
  EXPECT_TRUE(resolver->OnNd(10, kIface1, wrong_dst).empty());
  EXPECT_TRUE(
      resolver->OnNd(10, kIface1,
                     Advertisement(kPeer6, Mac("00:00:00:00:00:00")))
          .empty());
  EXPECT_TRUE(
      resolver->OnNd(10, kIface1,
                     Advertisement(kPeer6, Mac("01:00:5e:00:00:01")))
          .empty());
  EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);

  // A moved address starts a new generation; an advertisement in flight for
  // the old identity is stale, and the same frame under the new one is fresh.
  const auto reconfigured = resolver->ConfigureInterface(
      Iface6(kIface1, "02:00:00:00:00:01", "fe80::11"), 20);
  ASSERT_TRUE(reconfigured.has_value());
  EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);
  EXPECT_TRUE(resolver->OnNd(20, kIface1, good).empty());
  EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);
  EXPECT_EQ(1u,
            resolver
                ->OnNd(20, kIface1,
                       Advertisement(kPeer6, Mac("02:00:00:00:00:aa"), true,
                                     true, Ipv6("fe80::11")))
                .updates.size());
  EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
}

TEST(NeighborResolverTest, NdAdvertisementToUnspecifiedAddressIsIgnored) {
  auto resolver = MakeResolver();
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver
                  ->ConfigureInterface(
                      Iface6(kIface1, "02:00:00:00:00:01", "::"), 0)
                  .has_value());
  const NeighborResolver::Key key(kIface1, kPeer6);
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key, 0).has_value());
  // No address means no solicitation...
  EXPECT_TRUE(resolver->Poll(10, 16).solicitations.empty());
  // ...and a destination of :: is not this interface's address.
  EXPECT_TRUE(resolver
                  ->OnNd(10, kIface1,
                         Advertisement(kPeer6, Mac("02:00:00:00:00:aa"),
                                       /*solicited=*/true, /*override=*/true,
                                       Ipv6Address{},
                                       Mac("02:00:00:00:00:01")))
                  .empty());
  EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);
}

TEST(NeighborResolverTest, NdParsingRejectsMalformedAndBadChecksums) {
  Ipv6Address target = kPeer6;
  std::array<uint8_t, kNdFrameSize> frame{};
  ASSERT_EQ(kNdFrameSize,
            BuildNeighborSolicitation(Mac("02:00:00:00:00:01"), kMy6, target,
                                      frame));
  const NdMessage message = Nd(ParseNeighborDiscovery(frame));
  EXPECT_EQ(NdType::kNeighborSolicitation, message.type);
  EXPECT_EQ(kPeer6, message.target_ip);
  ASSERT_TRUE(message.source_ll.has_value());
  EXPECT_EQ(Mac("02:00:00:00:00:01"),
            message.source_ll.value_or(Ethernet::Address{}));

  // Hop limit must be 255.
  auto bad = frame;
  bad[14 + 7] = 64;
  EXPECT_FALSE(ParseNeighborDiscovery(bad).has_value());
  // Next header must be ICMPv6.
  bad = frame;
  bad[14 + 6] = 17;
  EXPECT_FALSE(ParseNeighborDiscovery(bad).has_value());
  // Version must be 6.
  bad = frame;
  bad[14] = 0x40;
  EXPECT_FALSE(ParseNeighborDiscovery(bad).has_value());
  // The checksum covers the pseudo-header and the payload.
  bad = frame;
  bad[14 + 40 + 8] ^= 0x01u;
  EXPECT_FALSE(ParseNeighborDiscovery(bad).has_value());
  // A truncated option.
  bad = frame;
  bad[14 + 40 + 25] = 2;  // option claims 16 bytes, only 8 remain
  EXPECT_FALSE(ParseNeighborDiscovery(bad).has_value());
  // A zero-length option.
  bad = frame;
  bad[14 + 40 + 25] = 0;
  EXPECT_FALSE(ParseNeighborDiscovery(bad).has_value());
  // A link-layer option that disagrees with the Ethernet source.
  bad = frame;
  std::memset(bad.data() + 14 + 40 + 26, 0xee, 6);
  EXPECT_FALSE(ParseNeighborDiscovery(bad).has_value());
  // An advertisement to a multicast destination must not set Solicited.
  std::array<uint8_t, kNdFrameSize> multicast_na{};
  ASSERT_EQ(kNdFrameSize,
            BuildNeighborAdvertisement(Mac("02:00:00:00:00:aa"), kPeer6, kPeer6,
                                       AllNodesMac(), AllNodesAddress(),
                                       /*solicited=*/true, /*override=*/true,
                                       multicast_na));
  EXPECT_FALSE(ParseNeighborDiscovery(multicast_na).has_value());
  EXPECT_FALSE(ParseNeighborDiscovery(
                   std::span<const uint8_t>(frame.data(), frame.size() - 1))
                   .has_value());

  // The multicast mapping is the last 24 bits.
  const Ipv6Address address = Ipv6("2001:db8::1234:5678");
  const Ipv6Address solicited = SolicitedNodeAddress(address);
  EXPECT_EQ(Ipv6("ff02::1:ff34:5678"), solicited);
  EXPECT_EQ(Mac("33:33:ff:34:56:78"), SolicitedNodeMac(address));
  EXPECT_EQ(Mac("33:33:00:00:00:01"), bess::route::AllNodesMac());
  EXPECT_EQ(Ipv6("ff02::1"), bess::route::AllNodesAddress());
}

TEST(NeighborResolverTest, AnswerNeighborSolicitationHandlesUnicastAndDad) {
  const Ethernet::Address my_mac = Mac("02:00:00:00:00:01");
  const Ethernet::Address peer_mac = Mac("02:00:00:00:00:aa");
  std::array<uint8_t, kNdFrameSize> solicitation{};
  ASSERT_EQ(kNdFrameSize,
            BuildNeighborSolicitation(peer_mac, kPeer6, kMy6, solicitation));
  std::array<uint8_t, kNdFrameSize> reply{};
  ASSERT_EQ(kNdFrameSize,
            AnswerNeighborSolicitation(solicitation, my_mac, kMy6, reply));
  NdMessage answer = Nd(ParseNeighborDiscovery(reply));
  EXPECT_EQ(NdType::kNeighborAdvertisement, answer.type);
  EXPECT_TRUE(answer.solicited);
  EXPECT_TRUE(answer.override);
  EXPECT_EQ(kMy6, answer.source_ip);
  EXPECT_EQ(kPeer6, answer.destination_ip);
  EXPECT_EQ(peer_mac, reinterpret_cast<Ethernet *>(reply.data())->dst_addr);
  ASSERT_TRUE(answer.target_ll.has_value());
  EXPECT_EQ(my_mac, answer.target_ll.value_or(Ethernet::Address{}));

  // A solicitation for another address is not answered and the output stays
  // untouched.
  std::array<uint8_t, kNdFrameSize> untouched{};
  EXPECT_EQ(0u, AnswerNeighborSolicitation(solicitation, my_mac,
                                           Ipv6("fe80::99"), untouched));
  const std::array<uint8_t, kNdFrameSize> empty{};
  EXPECT_EQ(empty, untouched);
  // An output buffer that cannot hold the 86-byte answer (with its target
  // link-layer option) is refused rather than truncated.
  std::array<uint8_t, kEthFrameWithoutOption> too_small{};
  EXPECT_EQ(0u, AnswerNeighborSolicitation(solicitation, my_mac, kMy6,
                                           too_small));
  const std::array<uint8_t, kEthFrameWithoutOption> empty_small{};
  EXPECT_EQ(empty_small, too_small);

  // A duplicate-address-detection solicitation (source ::, no source
  // link-layer option) is answered to all-nodes with the Solicited flag
  // clear. The answer still carries the target link-layer option: RFC 4861
  // 7.2.4 requires it because the solicitation's destination is multicast,
  // so the 78-byte solicitation becomes an 86-byte answer.
  std::array<uint8_t, kEthFrameWithoutOption> dad{};
  ASSERT_EQ(kEthFrameWithoutOption,
            internal::WriteIpv6Nd(dad, SolicitedNodeMac(kMy6), peer_mac,
                                  Ipv6Address{}, SolicitedNodeAddress(kMy6),
                                  kNdMessageSize));
  uint8_t *payload = dad.data() + sizeof(Ethernet) + kIpv6HeaderSize;
  payload[0] = static_cast<uint8_t>(NdType::kNeighborSolicitation);
  std::memcpy(payload + 8, kMy6.bytes, 16);
  const uint16_t checksum = internal::Icmpv6ChecksumSum(
      Ipv6Address{}, SolicitedNodeAddress(kMy6),
      std::span<const uint8_t>(payload, kNdMessageSize));
  payload[2] = static_cast<uint8_t>(checksum & 0xffu);
  payload[3] = static_cast<uint8_t>(checksum >> 8u);

  reply = {};
  ASSERT_EQ(kNdFrameSize,
            AnswerNeighborSolicitation(dad, my_mac, kMy6, reply));
  answer = Nd(ParseNeighborDiscovery(reply));
  EXPECT_EQ(NdType::kNeighborAdvertisement, answer.type);
  EXPECT_FALSE(answer.solicited);
  EXPECT_TRUE(answer.override);  // the target option is present
  EXPECT_EQ(AllNodesAddress(), answer.destination_ip);
  EXPECT_EQ(AllNodesMac(),
            reinterpret_cast<Ethernet *>(reply.data())->dst_addr);
  ASSERT_TRUE(answer.target_ll.has_value());
  EXPECT_EQ(my_mac, answer.target_ll.value_or(Ethernet::Address{}));

  // RFC 4861 7.1.1: a solicitation from :: must go to the target's
  // solicited-node multicast address. A unicast (or ::, or another group)
  // destination is not valid duplicate-address detection and is silently
  // discarded. Each rejected frame recomputes a valid ICMPv6 checksum over
  // the new destination, so only the destination guard can be the reason.
  ASSERT_TRUE(ParseNeighborDiscovery(dad).has_value());
  for (const Ipv6Address &bad_destination :
       {kMy6, Ipv6Address{}, AllNodesAddress()}) {
    auto bad_dad = dad;
    uint8_t *bad_ip = bad_dad.data() + sizeof(Ethernet);
    std::memcpy(bad_ip + 24, bad_destination.bytes, 16);
    uint8_t *bad_payload =
        bad_dad.data() + sizeof(Ethernet) + kIpv6HeaderSize;
    bad_payload[2] = 0;
    bad_payload[3] = 0;
    const uint16_t bad_checksum = internal::Icmpv6ChecksumSum(
        Ipv6Address{}, bad_destination,
        std::span<const uint8_t>(bad_payload, kNdMessageSize));
    bad_payload[2] = static_cast<uint8_t>(bad_checksum & 0xffu);
    bad_payload[3] = static_cast<uint8_t>(bad_checksum >> 8u);
    EXPECT_EQ(0u, internal::Icmpv6ChecksumSum(
                      Ipv6Address{}, bad_destination,
                      std::span<const uint8_t>(bad_payload, kNdMessageSize)));
    EXPECT_FALSE(ParseNeighborDiscovery(bad_dad).has_value());
  }
}

TEST(NeighborResolverTest, NdConcurrentPublishAndLookupNeverSeeAHalfUpdate) {
  rcu::RcuDomain &domain = bess::runtime::runtime().rcu();
  LpmRouteTable::Config config;
  auto made = Router::Create("neighbor_nd_concurrent", config, 4, domain);
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

  NeighborResolver::Config resolver_config = TestConfig();
  resolver_config.timing.reachable_lifetime = 1000000;
  auto resolver = MakeResolver(resolver_config);
  ASSERT_NE(nullptr, resolver);
  ASSERT_TRUE(resolver->ConfigureInterface(
                  Iface6(kIface1, "02:00:00:00:00:01", "fe80::1"), 0)
                  .has_value());
  const NeighborResolver::Key key(kIface1, kPeer6);
  ASSERT_TRUE(resolver->Bind(NextHopId(1), key, 0).has_value());
  ASSERT_EQ(1u, resolver->Poll(10, 16).solicitations.size());
  auto initial_updates = resolver->OnNd(
      10, kIface1, Advertisement(kPeer6, mac_a));
  ASSERT_EQ(1u, initial_updates.updates.size());
  ASSERT_TRUE(Publish(router, initial_updates.updates));

  std::atomic<bool> stop{false};
  std::atomic<uint64_t> bad{0}, lookups{0};
  std::vector<std::thread> readers;
  for (int r = 0; r < 2; r++) {
    const uint32_t reader_id = 50 + r;
    ASSERT_TRUE(domain.Register(reader_id).has_value());
    readers.emplace_back([&, reader_id] {
      domain.Online(reader_id);
      uint64_t local = 0;
      while (!stop.load(std::memory_order_relaxed)) {
        const NextHop *hop = router.LookupNextHop(NextHopId(1));
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
      std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
  while (std::chrono::steady_clock::now() < deadline) {
    const Ethernet::Address &mac = (now & uint64_t{1}) ? mac_a : mac_b;
    auto updates = resolver->OnNd(
        now++, kIface1, Advertisement(kPeer6, mac));
    if (!updates.updates.empty()) {
      ASSERT_TRUE(Publish(router, updates.updates));
    }
  }
  stop = true;
  for (auto &t : readers) {
    t.join();
  }
  for (int r = 0; r < 2; r++) {
    domain.Unregister(50 + r);
  }
  EXPECT_EQ(0u, bad.load()) << "of " << lookups.load() << " lookups";
  EXPECT_GT(lookups.load(), 100u);
}

TEST(NeighborResolverFaultTest, NdPollAllocationFailureLosesNoTimer) {
  const size_t points = ForEachFailurePoint([&](size_t fail_at) {
    NeighborResolver::Key key;
    auto resolver = NdResolved(&key);
    if (!resolver) {
      return;
    }
    EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
    EXPECT_EQ(1u, resolver->armed());
    bool threw = false;
    bool injected = false;
    NeighborResolver::Progress progress;
    {
      AllocationFaults faults(fail_at);
      try {
        progress = resolver->Poll(110, 16);
      } catch (const std::bad_alloc &) {
        threw = true;
      }
      injected = faults.injected();
    }
    if (!injected) {
      EXPECT_FALSE(threw);
      EXPECT_TRUE(progress.updates.empty());
      EXPECT_EQ(1u, progress.solicitations.size());
      EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
      return;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
    EXPECT_EQ(Mac("02:00:00:00:00:aa"), State(*resolver, key).mac);
    EXPECT_EQ(1u, resolver->armed());
    progress = resolver->Poll(110, 16);
    EXPECT_TRUE(progress.updates.empty());
    EXPECT_EQ(1u, progress.solicitations.size());
    EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
    EXPECT_EQ(1u, resolver->armed());
  });
  EXPECT_GT(points, 0u);
}

TEST(NeighborResolverFaultTest, NdAdvertisementAllocationFailureKeepsTimer) {
  const size_t points = ForEachFailurePoint([&](size_t fail_at) {
    NeighborResolver::Key key(kIface1, kPeer6);
    auto resolver = MakeResolver();
    if (!resolver) {
      return;
    }
    if (!resolver
             ->ConfigureInterface(
                 Iface6(kIface1, "02:00:00:00:00:01", "fe80::1"), 0)
             .has_value() ||
        !resolver->Bind(NextHopId(1), key, 0).has_value()) {
      ADD_FAILURE() << "setup";
      return;
    }
    if (resolver->Poll(10, 16).solicitations.size() != 1) {
      ADD_FAILURE() << "solicitation";
      return;
    }
    EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);
    EXPECT_EQ(1u, resolver->armed());
    bool threw = false;
    bool injected = false;
    NeighborResolver::Progress progress;
    {
      AllocationFaults faults(fail_at);
      try {
        progress = resolver->OnNd(
            10, kIface1,
            Advertisement(kPeer6, Mac("02:00:00:00:00:aa")));
      } catch (const std::bad_alloc &) {
        threw = true;
      }
      injected = faults.injected();
    }
    if (!injected) {
      EXPECT_FALSE(threw);
      EXPECT_EQ(1u, progress.updates.size());
      EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
      return;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);
    EXPECT_EQ(1u, resolver->armed());
    EXPECT_EQ(1u,
              resolver
                  ->OnNd(10, kIface1,
                         Advertisement(kPeer6, Mac("02:00:00:00:00:aa")))
                  .updates.size());
    EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
  });
  EXPECT_GT(points, 0u);
}

TEST(NeighborResolverFaultTest, NdConfigureAllocationFailureKeepsIdentity) {
  const NeighborResolver::Interface moved =
      Iface6(kIface1, "02:00:00:00:00:09", "fe80::1");
  const size_t points = ForEachFailurePoint([&](size_t fail_at) {
    NeighborResolver::Key key;
    auto resolver = NdResolved(&key);
    if (!resolver) {
      return;
    }
    const auto binding_opt = resolver->table().BindingOf(NextHopId(1));
    const NeighborTable::Bound binding =
        binding_opt.value_or(NeighborTable::Bound{});
    if (!binding_opt.has_value()) {
      ADD_FAILURE() << "binding";
      return;
    }
    EXPECT_EQ(1u, resolver->interface_epoch(kIface1));
    bool threw = false;
    bool injected = false;
    std::expected<NeighborResolver::Progress, std::string> progress;
    {
      AllocationFaults faults(fail_at);
      try {
        progress = resolver->ConfigureInterface(moved, 20);
      } catch (const std::bad_alloc &) {
        threw = true;
      }
      injected = faults.injected();
    }
    if (!injected) {
      EXPECT_FALSE(threw);
      EXPECT_TRUE(progress.has_value());
      EXPECT_EQ(2u, resolver->interface_epoch(kIface1));
      EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);
      return;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(1u, resolver->interface_epoch(kIface1));
    EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
    EXPECT_EQ(Mac("02:00:00:00:00:aa"), State(*resolver, key).mac);
    const auto unchanged = resolver->table().BindingOf(NextHopId(1));
    ASSERT_TRUE(unchanged.has_value());
    EXPECT_EQ(binding.src_mac,
              unchanged.value_or(NeighborTable::Bound{}).src_mac);
    EXPECT_EQ(1u, resolver->armed());

    auto retried = resolver->ConfigureInterface(moved, 20);
    ASSERT_TRUE(retried.has_value());
    const NeighborResolver::Progress retried_progress =
        retried.value_or(NeighborResolver::Progress{});
    EXPECT_EQ(2u, resolver->interface_epoch(kIface1));
    EXPECT_EQ(NeighborState::kIncomplete, State(*resolver, key).state);
    ASSERT_EQ(1u, retried_progress.updates.size());
    EXPECT_EQ(Mac("02:00:00:00:00:09"),
              retried_progress.updates[0].second.src_mac);
  });
  EXPECT_GT(points, 0u);
}

TEST(NeighborResolverFaultTest, NdRemoveAllocationFailureKeepsInterface) {
  const size_t points = ForEachFailurePoint([&](size_t fail_at) {
    NeighborResolver::Key key;
    auto resolver = NdResolved(&key);
    if (!resolver) {
      return;
    }
    EXPECT_EQ(1u, resolver->interface_epoch(kIface1));
    bool threw = false;
    bool injected = false;
    NeighborResolver::Progress progress;
    {
      AllocationFaults faults(fail_at);
      try {
        progress = resolver->RemoveInterface(kIface1);
      } catch (const std::bad_alloc &) {
        threw = true;
      }
      injected = faults.injected();
    }
    if (!injected) {
      EXPECT_FALSE(threw);
      EXPECT_EQ(0u, resolver->interface_epoch(kIface1));
      EXPECT_EQ(1u, progress.updates.size());
      EXPECT_EQ(NeighborState::kUnreachable, State(*resolver, key).state);
      return;
    }
    EXPECT_TRUE(threw);
    EXPECT_EQ(1u, resolver->interface_epoch(kIface1));
    EXPECT_EQ(NeighborState::kResolved, State(*resolver, key).state);
    EXPECT_EQ(Mac("02:00:00:00:00:aa"), State(*resolver, key).mac);
    EXPECT_EQ(1u, resolver->armed());
    progress = resolver->RemoveInterface(kIface1);
    EXPECT_EQ(0u, resolver->interface_epoch(kIface1));
    EXPECT_EQ(1u, progress.updates.size());
    EXPECT_EQ(NeighborState::kUnreachable, State(*resolver, key).state);
  });
  EXPECT_GT(points, 0u);
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
