// SPDX-License-Identifier: BSD-3-Clause

// UrlFilter in process (was bessctl/module_tests/url_filter.py): an HTTP
// request for a blacklisted host is dropped and answered with a RST to the
// server (gate 0) and a 403 and a RST to the client (gate 1); other
// requests pass. The runtime configuration comes back sorted. D-129:
// "<name>/blacklist" holds the whole blacklist; its commands went with the
// legacy service (D-140).

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "modules/url_filter.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
#include "task.h"
#include "testing/allocation_faults.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;

constexpr char kClientMac[] = "02:1e:67:9f:4d:ae";
constexpr char kServerMac[] = "06:16:3e:1b:72:32";
constexpr char k403[] = "HTTP/1.1 403 Bad Forbidden\r\nConnection: Closed\r\n\r\n";
constexpr uint8_t kAck = 0x10, kRst = 0x04;

PacketBuilder::TcpHdr Tcp(uint16_t sport, uint16_t dport, uint8_t flags, uint32_t seq,
                          uint32_t ack, uint16_t window = 8192) {
  return {.sport = sport, .dport = dport, .flags = flags, .seq = seq, .ack = ack, .window = window};
}

// Client to server.
Bytes ToServer(const char *client_ip, const char *server_ip, PacketBuilder::TcpHdr tcp,
               const char *payload = nullptr) {
  PacketBuilder b;
  b.Ether(kServerMac, kClientMac).IPv4(client_ip, server_ip).Add(tcp);
  if (payload != nullptr) {
    b.Payload(payload);
  }
  return b.Build();
}

TEST(UrlFilterTest, BlacklistedRequestIsRefusedAndReset) {
  ModuleHarness h;
  Module *uf = h.Create("UrlFilter", bess::pb::UrlFilterArg(), "uf");
  ASSERT_NE(nullptr, uf);
  bess::pb::UrlFilterConfig blacklist;
  auto *url = blacklist.add_blacklist();
  url->set_host("www.blacklisted.com");
  url->set_path("/");
  ASSERT_EQ("", h.SetResource("uf/blacklist", blacklist));

  // SYN, then the request (seq + 1, an ACK number, no flags).
  const auto syn = Tcp(10001, 80, 0x02, 12345, 0);
  const auto req = Tcp(10001, 80, 0, 12346, 23456);
  const Bytes syn_good = ToServer("192.168.0.1", "10.0.0.1", syn);
  const Bytes good = ToServer("192.168.0.1", "10.0.0.1", req,
                              "GET / HTTP/1.1\r\nHost: www.google.com\r\n\r\n");
  const Bytes syn_bad = ToServer("192.168.0.2", "10.0.0.2", syn);
  const Bytes bad = ToServer("192.168.0.2", "10.0.0.2", req,
                             "GET / HTTP/1.1\r\nHost: www.blacklisted.com\r\n\r\n");

  // To the server: a RST in place of the request (IP id 2).
  PacketBuilder rst_to_server;
  rst_to_server.Ether(kServerMac, kClientMac)
      .IPv4("192.168.0.2", "10.0.0.2")
      .Add(Tcp(10001, 80, kRst | kAck, 12346, 23456, 0));
  rst_to_server.Last<PacketBuilder::IPv4Hdr>().id = 2;
  // To the client: the 403 (IP id 1, ACK, window shut), then a RST after it.
  const Bytes forbidden = PacketBuilder()
                              .Ether(kClientMac, kServerMac)
                              .IPv4("10.0.0.2", "192.168.0.2")
                              .Add(Tcp(80, 10001, kAck, 23456, 12346, 0))
                              .Payload(k403)
                              .Build();
  PacketBuilder rst_to_client;
  rst_to_client.Ether(kClientMac, kServerMac)
      .IPv4("10.0.0.2", "192.168.0.2")
      .Add(Tcp(80, 10001, kRst | kAck, static_cast<uint32_t>(23456 + std::strlen(k403)), 12346, 0));
  rst_to_client.Last<PacketBuilder::IPv4Hdr>().id = 2;

  auto out = h.Run(uf, 0, {syn_good, good, syn_bad, bad}, {0, 1});
  EXPECT_EQ((std::vector<Bytes>{syn_good, good, syn_bad, rst_to_server.Build()}), out[0]);
  EXPECT_EQ((std::vector<Bytes>{forbidden, rst_to_client.Build()}), out[1]);
}

// The blacklist reads back as given, in the order it was given.
TEST(UrlFilterTest, BlacklistReadsBackAsGiven) {
  ModuleHarness h;
  Module *uf = h.Create("UrlFilter", bess::pb::UrlFilterArg(), "uf");
  ASSERT_NE(nullptr, uf);
  bess::pb::UrlFilterConfig add;
  for (const auto &[host, path] : {std::pair{"www.bluelisted.com", "/b"},
                                   std::pair{"www.bluelisted.com", "/a"},
                                   std::pair{"www.blacklisted.com", "/"}}) {
    auto *url = add.add_blacklist();
    url->set_host(host);
    url->set_path(path);
  }
  ASSERT_EQ("", h.SetResource("uf/blacklist", add));

  bess::pb::UrlFilterConfig got;
  const auto entries = h.ReadResource("uf/blacklist");
  ASSERT_EQ(1u, entries.size());
  ASSERT_TRUE(entries[0].second.UnpackTo(&got));
  EXPECT_EQ(add.ShortDebugString(), got.ShortDebugString());
}

// Whether the codec bound to `resource` requires paused workers.
bool RequiresPausedWorkers(const std::string &resource) {
  const bess::dataplane::Resource *r =
      bess::runtime::runtime().transactions().FindResource(resource);
  EXPECT_NE(nullptr, r) << resource;
  if (r == nullptr) {
    return false;
  }
  const bess::framework::ResourceCodec *codec =
      bess::framework::ResourceBindings::ProcessDefault().Find(*r);
  EXPECT_NE(nullptr, codec) << resource;
  return codec != nullptr && codec->requires_paused_workers();
}

bess::pb::UrlFilterConfig Blacklist(
    std::initializer_list<std::pair<const char *, const char *>> urls) {
  bess::pb::UrlFilterConfig config;
  for (const auto &[host, path] : urls) {
    auto *url = config.add_blacklist();
    url->set_host(host);
    url->set_path(path);
  }
  return config;
}

// Whether a request for `host` from `client_ip` (a new flow) is refused:
// it does not reach the server, and the client gets the 403 and a RST.
bool Refused(ModuleHarness &h, Module *uf, const char *client_ip, const std::string &host) {
  const Bytes syn = ToServer(client_ip, "10.0.0.1", Tcp(10001, 80, 0x02, 12345, 0));
  const std::string request = "GET / HTTP/1.1\r\nHost: " + host + "\r\n\r\n";
  const Bytes req = ToServer(client_ip, "10.0.0.1", Tcp(10001, 80, 0, 12346, 23456), request.c_str());
  auto out = h.Run(uf, 0, {syn, req}, {0, 1});
  const bool passed = out[0] == std::vector<Bytes>{syn, req};
  EXPECT_EQ(passed, out[1].empty());
  return !passed;
}

// "<name>/blacklist" is the whole blacklist (paused workers): setting it
// replaces the list (as set_runtime_config does), it reads back as given,
// and an empty list lets everything through. UrlFilter refuses no URL (nor
// did add): only a value of the wrong type is refused.
TEST(UrlFilterTest, BlacklistResourceReplacesTheList) {
  ModuleHarness h;
  bess::pb::UrlFilterArg arg;
  auto *url = arg.add_blacklist();
  url->set_host("www.initial.com");
  url->set_path("/");
  Module *uf = h.Create("UrlFilter", arg, "uf_res");
  ASSERT_NE(nullptr, uf);
  const std::string blacklist = "uf_res/blacklist";
  EXPECT_TRUE(RequiresPausedWorkers(blacklist));
  const auto initial = Blacklist({{"www.initial.com", "/"}});
  EXPECT_EQ("", h.ReadBackDiffers(blacklist, {{nullptr, &initial}})) << "Init's blacklist";

  // Not sorted: it reads back in this order.
  const auto two = Blacklist({{"www.zeta.com", "/"}, {"www.alpha.com", "/"}});
  EXPECT_EQ("", h.SetResource(blacklist, two));
  EXPECT_TRUE(Refused(h, uf, "192.168.1.1", "www.zeta.com"));
  EXPECT_TRUE(Refused(h, uf, "192.168.1.2", "www.alpha.com"));
  EXPECT_FALSE(Refused(h, uf, "192.168.1.3", "www.initial.com")) << "replaced, not added";
  EXPECT_EQ("", h.ReadBackDiffers(blacklist, {{nullptr, &two}}));

  EXPECT_NE("", h.SetResource(blacklist, arg)) << "a UrlFilterArg is not the value type";
  EXPECT_EQ("", h.ReadBackDiffers(blacklist, {{nullptr, &two}}));

  const bess::pb::UrlFilterConfig none;
  EXPECT_EQ("", h.SetResource(blacklist, none));
  EXPECT_FALSE(Refused(h, uf, "192.168.1.4", "www.initial.com"));
  EXPECT_EQ("", h.ReadBackDiffers(blacklist, {{nullptr, &none}}));
}

// A request whose payload continues in a second segment is read across the
// chain and refused. Before, the whole payload was read from the first
// segment, past its data_len, and the request passed.
TEST(UrlFilterTest, ChainedRequestIsReadAcrossSegments) {
  ModuleHarness h;
  Module *uf = h.Create("UrlFilter", bess::pb::UrlFilterArg(), "uf_chain");
  ASSERT_NE(nullptr, uf);
  ASSERT_EQ("", h.SetResource("uf_chain/blacklist", Blacklist({{"www.blacklisted.com", "/"}})));

  const Bytes syn = ToServer("192.168.0.2", "10.0.0.2", Tcp(10001, 80, 0x02, 12345, 0));
  const Bytes req = ToServer("192.168.0.2", "10.0.0.2", Tcp(10001, 80, 0, 12346, 23456),
                             "GET / HTTP/1.1\r\nHost: www.blacklisted.com\r\n\r\n");
  EXPECT_EQ((std::vector<Bytes>{syn}), h.Run(uf, 0, {syn}, {0})[0]);
  constexpr size_t kHeaders = 14 + 20 + 20;
  auto out = h.RunPackets(uf, 0, {bess::testing::MakePacket(req, kHeaders + 8)}, uf, {0, 1});
  ASSERT_EQ(1u, out[0].size()) << "the RST, not the request";
  EXPECT_NE(req, out[0][0]);
  EXPECT_EQ(2u, out[1].size()) << "the 403 and the RST";
}

// A frame the checked parse refuses is dropped: headers longer than the IPv4
// total length, a total length past the frame, a runt, headers cut by the
// first segment's end. Frames that are not untagged IPv4 TCP pass. Before,
// the lying lengths wrapped the payload length to about 4 GiB (a 4 GiB
// resize and a copy that long), the others were read past data_len, and
// every frame was read as untagged IPv4.
TEST(UrlFilterTest, MalformedFramesAreDroppedOthersPass) {
  ModuleHarness h;
  Module *uf = h.Create("UrlFilter", bess::pb::UrlFilterArg(), "uf_bounds");
  ASSERT_NE(nullptr, uf);

  const Bytes syn = ToServer("192.168.0.3", "10.0.0.3", Tcp(10001, 80, 0x02, 12345, 0));
  const auto data = Tcp(10001, 80, 0, 12346, 23456);
  Bytes long_tcp = ToServer("192.168.0.3", "10.0.0.3", data, std::string(40, 'x').c_str());
  long_tcp[46] = 0xf0;            // 60 B of TCP header ...
  long_tcp[16] = 0, long_tcp[17] = 60;  // ... in 40 B of L4
  Bytes past_frame = ToServer("192.168.0.3", "10.0.0.3", data, "GET");
  past_frame[16] = 0x03, past_frame[17] = 0xe8;  // 1000 B
  Bytes runt = syn;
  runt.resize(20);

  const Bytes arp = PacketBuilder()
                        .Ether("ff:ff:ff:ff:ff:ff", kClientMac)
                        .Arp(1, kClientMac, "192.168.0.3", "00:00:00:00:00:00", "10.0.0.3")
                        .Build();
  const Bytes udp = PacketBuilder()
                        .Ether(kServerMac, kClientMac)
                        .IPv4("192.168.0.3", "10.0.0.3")
                        .Udp(1000, 53)
                        .Payload("q")
                        .Build();
  const Bytes tcp6 = PacketBuilder()
                         .Ether(kServerMac, kClientMac)
                         .IPv6("fd00::3", "fd00::4")
                         .Add(Tcp(10001, 80, 0x02, 12345, 0))
                         .Build();
  const Bytes tagged = PacketBuilder()
                           .Ether(kServerMac, kClientMac)
                           .Dot1Q(7)
                           .IPv4("192.168.0.3", "10.0.0.3")
                           .Add(Tcp(10001, 80, 0x02, 12345, 0))
                           .Build();

  auto out = h.Run(uf, 0, {syn, long_tcp, past_frame, runt, arp, udp, tcp6, tagged}, {0, 1});
  EXPECT_EQ((std::vector<Bytes>{syn, arp, udp, tcp6, tagged}), out[0]);
  EXPECT_TRUE(out[1].empty());

  const Bytes req = ToServer("192.168.0.3", "10.0.0.3", data, "GET / HTTP/1.1\r\n");
  out = h.RunPackets(uf, 0, {bess::testing::MakePacket(req, 30)}, uf, {0, 1});
  EXPECT_TRUE(out[0].empty()) << "headers past the first segment (D-121)";
}

// Data far past the SYN's sequence number lies outside the reconstruction
// window: ignored, the segment passes while the flow is still analyzed.
// Before, (offset + length) * 2 wrapped in 32 bits, the buffer shrank to
// 32 B and the copy landed 2 GiB past it.
TEST(UrlFilterTest, DataPastTheWindowIsIgnored) {
  ModuleHarness h;
  Module *uf = h.Create("UrlFilter", bess::pb::UrlFilterArg(), "uf_window");
  ASSERT_NE(nullptr, uf);

  const Bytes syn = ToServer("192.168.0.4", "10.0.0.4", Tcp(10001, 80, 0x02, 12345, 0));
  const Bytes far = ToServer("192.168.0.4", "10.0.0.4",
                             Tcp(10001, 80, 0, 12346 + 0x7ffffff0u, 23456),
                             std::string(32, 'x').c_str());
  EXPECT_EQ((std::vector<Bytes>{syn, far}), h.Run(uf, 0, {syn, far}, {0})[0]);
}

// MZ-11: a flood of 1M SYNs, each a new flow. The table holds at most
// max_flows flows, a SYN past them is dropped and counted, and tracking a flow
// allocates nothing on the worker; once the flows are idle past the timeout,
// one walk of the table (a sweep per batch) empties it.
TEST(UrlFilterTest, ASynFloodStaysWithinTheFlowTable) {
  constexpr size_t kMaxFlows = 1024;
  constexpr size_t kSyns = 1'000'000;
  constexpr size_t kBurst = bess::PacketBatch::kMaxBurst;
  ModuleHarness h;
  bess::pb::UrlFilterArg arg;
  arg.set_max_flows(kMaxFlows);
  auto *uf = dynamic_cast<UrlFilter *>(h.Create("UrlFilter", arg, "uf_flood"));
  ASSERT_NE(nullptr, uf);

  const Bytes syn = ToServer("192.168.0.4", "10.0.0.4", Tcp(10001, 80, 0x02, 12345, 0));
  constexpr size_t kSrcIp = 14 + 12;  // Ethernet, then the IPv4 source
  bess::PlainPacketPool pool(2 * kBurst);
  Task task(uf, nullptr);
  Context ctx{};
  ctx.wid = bess::dataplane::WorkerId(0);
  ctx.task = &task;
  ctx.current_igate = 0;
  ctx.current_ns = 1;
  size_t allocations = 0;
  for (size_t i = 0; i < kSyns; i += kBurst) {
    bess::PacketBatch batch;
    batch.clear();
    for (size_t j = i; j < i + kBurst; j++) {
      bess::PacketHandle pkt = pool.Alloc(syn.size());
      ASSERT_NE(nullptr, pkt);
      auto *frame = bess::PacketRef(pkt).head_data<uint8_t *>();
      std::memcpy(frame, syn.data(), syn.size());
      const bess::utils::be32_t src(static_cast<uint32_t>(0x0a000000u + j));
      std::memcpy(frame + kSrcIp, &src, sizeof(src));
      batch.add(bess::PacketRef(pkt));
    }
    // Every packet ends dropped (a refused SYN, or one emitted to the
    // unconnected gate 0): the dead batch frees them.
    const bess::fault_injection::AllocationFaults window;
    uf->ProcessBatch(&ctx, &batch);
    allocations += window.allocations();
  }
  EXPECT_EQ(kMaxFlows, uf->flows());
  EXPECT_EQ(kSyns - kMaxFlows, uf->refused_flows());
  EXPECT_EQ(0u, allocations);

  ctx.current_ns = 1 + 10'000'000'000ull;  // the 10 s timeout
  for (size_t sweep = 0; sweep < kMaxFlows / UrlFilter::kSweepSlots; sweep++) {
    bess::PacketBatch empty;
    empty.clear();
    uf->ProcessBatch(&ctx, &empty);
  }
  EXPECT_EQ(0u, uf->flows());
}

}  // namespace
