// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "ip.h"

#include <gtest/gtest.h>

using bess::utils::be32_t;

namespace {

using bess::utils::Ipv4Prefix;

TEST(IPTest, AddressInStr) {
  be32_t a(192 << 24 | 168 << 16 | 100 << 8 | 199);

  std::string str = ToIpv4Address(a);
  EXPECT_EQ(str, "192.168.100.199");

  be32_t b;
  bool ret = ParseIpv4Address(str, &b);
  EXPECT_TRUE(ret);
  EXPECT_EQ(a, b);

  EXPECT_FALSE(ParseIpv4Address("hello", &b));
  EXPECT_FALSE(ParseIpv4Address("1.1.1", &b));
  EXPECT_FALSE(ParseIpv4Address("1.1.256.1", &b));
}

// Check if Ipv4Prefix can be correctly constructed from strings
TEST(IPTest, PrefixInStr) {
  Ipv4Prefix prefix_1("192.168.0.1/24");
  EXPECT_EQ((192 << 24) + (168 << 16) + 1, prefix_1.addr.value());
  EXPECT_EQ(0xffffff00, prefix_1.mask.value());

  Ipv4Prefix prefix_2("0.0.0.0/0");
  EXPECT_EQ(0, prefix_2.addr.value());
  EXPECT_EQ(0, prefix_2.mask.value());

  Ipv4Prefix prefix_3("128.0.0.0/1");
  EXPECT_EQ(128 << 24, prefix_3.addr.value());
  EXPECT_EQ(0x80000000, prefix_3.mask.value());
}

// Malformed prefixes: Parse() rejects them; the constructor yields 0.0.0.0/0
// and never throws (std::stoi used to, on a non-numeric length).
TEST(IPTest, PrefixParseRejectsMalformedInput) {
  for (const char *bad : {"", "10.0.0.0", "10.0.0.0/", "10.0.0.0/x",
                          "10.0.0.0/33", "10.0.0.0/-1", "10.0.0.0/8x",
                          "abc/8", "10.0.0/8", " 10.0.0.0/8", "10.0.0.0x/8",
                          "10..0.0/8", "10.0.0.256/8"}) {
    EXPECT_FALSE(Ipv4Prefix::Parse(bad).has_value()) << bad;
    Ipv4Prefix p(bad);  // must not throw
    EXPECT_EQ(0u, p.addr.value()) << bad;
    EXPECT_EQ(0u, p.mask.value()) << bad;
  }
  const auto ok = Ipv4Prefix::Parse("192.168.1.0/24");
  ASSERT_TRUE(ok.has_value());
  EXPECT_EQ(24u, ok->prefix_length());
  EXPECT_EQ(32u, Ipv4Prefix::Parse("1.2.3.4/32")->prefix_length());
  EXPECT_EQ(0u, Ipv4Prefix::Parse("0.0.0.0/0")->prefix_length());
}

// Check if Ipv4Prefix::Match() behaves correctly
TEST(IPTest, PrefixMatch) {
  Ipv4Prefix prefix_1("192.168.0.1/24");
  EXPECT_TRUE(prefix_1.Match(be32_t((192 << 24) + (168 << 16) + 254)));
  EXPECT_FALSE(
      prefix_1.Match(be32_t((192 << 24) + (168 << 16) + (2 << 8) + 1)));

  Ipv4Prefix prefix_2("0.0.0.0/0");
  EXPECT_TRUE(prefix_2.Match(be32_t((192 << 24) + (168 << 16) + 254)));
  EXPECT_TRUE(prefix_2.Match(be32_t((192 << 24) + (168 << 16) + (2 << 8) + 1)));

  Ipv4Prefix prefix_3("192.168.0.1/32");
  EXPECT_FALSE(prefix_3.Match(be32_t((192 << 24) + (168 << 16) + 254)));
  EXPECT_TRUE(prefix_3.Match(be32_t((192 << 24) + (168 << 16) + 1)));
}

TEST(IPTest, PrefixCalc) {
  Ipv4Prefix prefix_1("192.168.0.1/24");
  EXPECT_EQ(24, prefix_1.prefix_length());
  Ipv4Prefix prefix_2("192.168.0.1/32");
  EXPECT_EQ(32, prefix_2.prefix_length());
  Ipv4Prefix prefix_3("192.168.0.1/16");
  EXPECT_EQ(16, prefix_3.prefix_length());

  // exhaustive test
  for (int i = 0; i <= 32; i++) {
    Ipv4Prefix p("0.0.0.0/" + std::to_string(i));
    EXPECT_EQ(i, p.prefix_length());
  }
}

}  // namespace (unnamed)
