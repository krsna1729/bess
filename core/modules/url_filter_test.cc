// SPDX-License-Identifier: BSD-3-Clause

// UrlFilter in process (was bessctl/module_tests/url_filter.py): an HTTP
// request for a blacklisted host is dropped and answered with a RST to the
// server (gate 0) and a 403 and a RST to the client (gate 1); other
// requests pass. The runtime configuration comes back sorted.

#include <gtest/gtest.h>

#include <cstring>
#include <vector>

#include "pb/module_msg.pb.h"
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
  Module *uf = h.Create("UrlFilter", bess::pb::UrlFilterArg());
  ASSERT_NE(nullptr, uf);
  bess::pb::UrlFilterArg blacklist;
  auto *url = blacklist.add_blacklist();
  url->set_host("www.blacklisted.com");
  url->set_path("/");
  h.MustCommand(uf, "add", blacklist);

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

TEST(UrlFilterTest, RuntimeConfigIsSortedByHostThenPath) {
  ModuleHarness h;
  Module *uf = h.Create("UrlFilter", bess::pb::UrlFilterArg());
  ASSERT_NE(nullptr, uf);
  bess::pb::UrlFilterArg add;
  for (const auto &[host, path] : {std::pair{"www.bluelisted.com", "/b"},
                                   std::pair{"www.bluelisted.com", "/a"},
                                   std::pair{"www.blacklisted.com", "/"}}) {
    auto *url = add.add_blacklist();
    url->set_host(host);
    url->set_path(path);
  }
  h.MustCommand(uf, "add", add);

  bess::pb::UrlFilterArg initial;
  ASSERT_TRUE(h.MustCommand(uf, "get_initial_arg", bess::pb::EmptyArg()).UnpackTo(&initial));
  EXPECT_EQ(0u, initial.ByteSizeLong()) << "the arg it was created with: none";

  // Blue sorts after black ('u' > 'a').
  bess::pb::UrlFilterConfig config;
  ASSERT_TRUE(h.MustCommand(uf, "get_runtime_config", bess::pb::EmptyArg()).UnpackTo(&config));
  ASSERT_EQ(3, config.blacklist_size());
  EXPECT_EQ("www.blacklisted.com", config.blacklist(0).host());
  EXPECT_EQ("/", config.blacklist(0).path());
  EXPECT_EQ("www.bluelisted.com", config.blacklist(1).host());
  EXPECT_EQ("/a", config.blacklist(1).path());
  EXPECT_EQ("www.bluelisted.com", config.blacklist(2).host());
  EXPECT_EQ("/b", config.blacklist(2).path());
}

}  // namespace
