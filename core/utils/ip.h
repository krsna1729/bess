// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_IP_H_
#define BESS_UTILS_IP_H_

#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>

#include "endian.h"

namespace bess {
namespace utils {

// "a.b.c.d" with decimal parts 0-255, each optionally preceded by whitespace;
// input after the fourth part is ignored. Returns false if the conversion
// failed (*addr is unmodified).
bool ParseIpv4Address(const std::string &str, be32_t *addr);

// be32 -> string
std::string ToIpv4Address(be32_t addr);

// A 128-bit IPv6 address, as network-order bytes. Ordering is memcmp over the
// bytes (never bit twiddling of a multi-word integer): the same order for
// IPv4-mapped and native addresses, and no endian assumption. Zero is the
// unspecified address (::), exactly as a zero IPv4 address is 0.0.0.0.
struct Ipv6Address {
  uint8_t bytes[16] = {};

  friend bool operator==(const Ipv6Address &, const Ipv6Address &) = default;
  friend auto operator<=>(const Ipv6Address &a, const Ipv6Address &b) {
    const int c = std::memcmp(a.bytes, b.bytes, sizeof(a.bytes));
    return c < 0 ? std::strong_ordering::less
                 : c > 0 ? std::strong_ordering::greater
                         : std::strong_ordering::equal;
  }

  bool IsUnspecified() const {
    for (const uint8_t byte : bytes) {
      if (byte != 0) {
        return false;
      }
    }
    return true;
  }
  bool IsMulticast() const { return bytes[0] == 0xff; }
  bool IsLinkLocal() const {
    return bytes[0] == 0xfe &&
           (static_cast<unsigned>(bytes[1]) & 0xc0u) == 0x80u;
  }
  bool IsLoopback() const {
    for (size_t i = 0; i < 15; i++) {
      if (bytes[i] != 0) {
        return false;
      }
    }
    return bytes[15] == 1;
  }
};

static_assert(sizeof(Ipv6Address) == 16, "an IPv6 address is 16 bytes");
static_assert(std::is_trivially_copyable_v<Ipv6Address>);

// Strict textual IPv6 parse: eight groups of 1..4 hex digits, with one "::"
// standing for one or more zero groups. No zone id, no embedded IPv4, no
// whitespace. Returns nullopt for anything else.
std::optional<Ipv6Address> ParseIpv6Address(const std::string &str);
// The canonical compressed lower-case form ("2001:db8::1", "::", "::1").
std::string ToIpv6Address(const Ipv6Address &addr);

// An IPv4 header definition loosely based on the BSD version.
struct[[gnu::packed]] Ipv4 {
  enum Flag : uint16_t {
    kMF = 1 << 13,  // More fragments
    kDF = 1 << 14,  // Do not fragment
  };

  enum Proto : uint8_t {
    kIcmp = 1,
    kIgmp = 2,
    kIpIp = 4,  // IPv4-in-IPv4
    kTcp = 6,
    kUdp = 17,
    kIpv6 = 41,  // IPv6-in-IPv4
    kGre = 47,
    kEsp = 50,  // IPsec ESP (Encapsulating Security Payload)
    kAh = 51,   // IPsec AH (Authentication Header)
    kSctp = 132,
    kUdpLite = 136,
    kMpls = 137,  // MPLS-in-IPv4
    kRaw = 255,
  };

#if __BYTE_ORDER == __LITTLE_ENDIAN
  uint8_t header_length : 4;  // Header length.
  uint8_t version : 4;        // Version.
#elif __BYTE_ORDER == __BIG_ENDIAN
  uint8_t version : 4;        // Version.
  uint8_t header_length : 4;  // Header length.
#else
#error __BYTE_ORDER must be defined.
#endif
  uint8_t type_of_service;  // Type of service.
  be16_t length;            // Length.
  be16_t id;                // ID.
  be16_t fragment_offset;   // Fragment offset.
  uint8_t ttl;              // Time to live.
  uint8_t protocol;         // Protocol.
  uint16_t checksum;        // Checksum.
  be32_t src;               // Source address.
  be32_t dst;               // Destination address.
};

static_assert(std::is_standard_layout<Ipv4>::value &&
                  std::is_trivial<Ipv4>::value,
              "Ipv4 must be standard-layout and trivial");
static_assert(sizeof(Ipv4) == 20, "struct Ipv4 is incorrect");

struct Ipv4Prefix {
  // Implicit default constructor is not allowed
  Ipv4Prefix() = delete;

  // Construct Ipv4Prefix from a string like "192.168.0.1/24". Malformed
  // input yields 0.0.0.0/0 (never throws); callers that must reject bad
  // input use Parse().
  explicit Ipv4Prefix(const std::string &prefix);

  // Strict parse of "a.b.c.d/len" with 0 <= len <= 32; nullopt for anything
  // else (a missing or non-numeric length, a bad address, trailing garbage).
  static std::optional<Ipv4Prefix> Parse(const std::string &prefix);

  // Returns true if ip is within the range of Ipv4Prefix
  bool Match(const be32_t &ip) const { return (addr & mask) == (ip & mask); }

  // Returns the prefix length
  uint32_t prefix_length() const {
    uint32_t mask_val = mask.value();
    if (mask_val == 0) {
      return 0;
    } else {
      return 32 - __builtin_ctz(mask_val);
    }
  }

  be32_t addr;
  be32_t mask;
};

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_IP_ADDRESS_H_
