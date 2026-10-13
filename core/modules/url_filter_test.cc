// SPDX-License-Identifier: BSD-3-Clause

// UrlFilter in process (was bessctl/module_tests/url_filter.py): an HTTP
// request for a blacklisted host is dropped and answered with a RST to the
// server (gate 0) and a 403 and a RST to the client (gate 1); other
// requests pass. The runtime configuration comes back sorted. D-129:
// "<name>/blacklist" holds the whole blacklist, and the commands are
// legacy-only.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <vector>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "pb/module_msg.pb.h"
#include "runtime/runtime_state.h"
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

  // The legacy commands edit the same list.
  bess::pb::UrlFilterArg more;
  url = more.add_blacklist();
  url->set_host("www.beta.com");
  url->set_path("/");
  h.MustCommand(uf, "add", more);
  const auto three = Blacklist({{"www.zeta.com", "/"}, {"www.alpha.com", "/"}, {"www.beta.com", "/"}});
  EXPECT_EQ("", h.ReadBackDiffers(blacklist, {{nullptr, &three}}));
  h.MustCommand(uf, "set_runtime_config", initial);
  EXPECT_EQ("", h.ReadBackDiffers(blacklist, {{nullptr, &initial}}));

  const bess::pb::UrlFilterConfig none;
  EXPECT_EQ("", h.SetResource(blacklist, none));
  EXPECT_FALSE(Refused(h, uf, "192.168.1.4", "www.initial.com"));
  EXPECT_EQ("", h.ReadBackDiffers(blacklist, {{nullptr, &none}}));
}

// Every command edits or reads back "<name>/blacklist": control_v2's
// InvokeAction refuses them.
TEST(UrlFilterTest, CommandsAreLegacyOnly) {
  ModuleHarness h;
  Module *uf = h.Create("UrlFilter", bess::pb::UrlFilterArg());
  ASSERT_NE(nullptr, uf);
  const bess::pb::EmptyArg empty;
  EXPECT_EQ(ENOTSUP, h.Action(uf, "add", bess::pb::UrlFilterArg()).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(uf, "clear", empty).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(uf, "get_initial_arg", empty).error().code());
  EXPECT_EQ(ENOTSUP, h.Action(uf, "get_runtime_config", empty).error().code());
  EXPECT_EQ(ENOTSUP,
            h.Action(uf, "set_runtime_config", bess::pb::UrlFilterConfig()).error().code());
  EXPECT_EQ(0, h.Command(uf, "get_runtime_config", empty).error().code());
}

}  // namespace
