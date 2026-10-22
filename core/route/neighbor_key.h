// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ROUTE_NEIGHBOR_KEY_H_
#define BESS_ROUTE_NEIGHBOR_KEY_H_

#include <compare>
#include <cstdint>
#include <cstring>
#include <type_traits>

#include "dataplane/interface_id.h"
#include "utils/ip.h"

namespace bess::route {

// The address family of a neighbor. The ordering is the enum's, so a map keyed
// by a neighbor groups the families without comparing an address across them.
enum class IpFamily : uint8_t { kIpv4 = 0, kIpv6 = 1 };

// A neighbor's protocol address, kept family-tagged so IPv4 and IPv6 share one
// table without converting between them. IPv4 is stored as its four network-
// order bytes (so the same memcmp order as IPv6), the remaining bytes zero;
// IPv6 uses all sixteen. Ordering is family, then memcmp over the bytes: never
// a multi-word integer comparison or a bitwise address parse.
struct NeighborAddress {
  IpFamily family = IpFamily::kIpv4;
  uint8_t bytes[16] = {};

  static NeighborAddress Ipv4(uint32_t address) {
    NeighborAddress out;
    out.family = IpFamily::kIpv4;
    out.bytes[0] = static_cast<uint8_t>(address >> 24u);
    out.bytes[1] = static_cast<uint8_t>(address >> 16u);
    out.bytes[2] = static_cast<uint8_t>(address >> 8u);
    out.bytes[3] = static_cast<uint8_t>(address);
    return out;
  }
  static NeighborAddress Ipv6(const utils::Ipv6Address &address) {
    NeighborAddress out;
    out.family = IpFamily::kIpv6;
    std::memcpy(out.bytes, address.bytes, sizeof(out.bytes));
    return out;
  }

  bool is_ipv4() const { return family == IpFamily::kIpv4; }
  bool is_ipv6() const { return family == IpFamily::kIpv6; }
  uint32_t ipv4() const {
    return static_cast<uint32_t>(bytes[0]) << 24u |
           static_cast<uint32_t>(bytes[1]) << 16u |
           static_cast<uint32_t>(bytes[2]) << 8u | bytes[3];
  }
  utils::Ipv6Address ipv6() const {
    utils::Ipv6Address out;
    std::memcpy(out.bytes, bytes, sizeof(out.bytes));
    return out;
  }

  friend bool operator==(const NeighborAddress &, const NeighborAddress &) =
      default;
  friend auto operator<=>(const NeighborAddress &a, const NeighborAddress &b) {
    if (a.family != b.family) {
      return a.family <=> b.family;
    }
    const int c = std::memcmp(a.bytes, b.bytes, sizeof(a.bytes));
    return c < 0 ? std::strong_ordering::less
                 : c > 0 ? std::strong_ordering::greater
                         : std::strong_ordering::equal;
  }
};
static_assert(std::is_trivially_copyable_v<NeighborAddress>);

// The general neighbor key: (logical interface, family-tagged address). The
// IPv4 constructor keeps existing callers (`Key{interface, 0x0a000001}`)
// source-compatible.
struct NeighborKey {
  dataplane::InterfaceId interface;
  NeighborAddress address;

  NeighborKey() = default;
  NeighborKey(dataplane::InterfaceId interface, uint32_t ipv4)
      : interface(interface), address(NeighborAddress::Ipv4(ipv4)) {}
  NeighborKey(dataplane::InterfaceId interface,
              const utils::Ipv6Address &ipv6)
      : interface(interface), address(NeighborAddress::Ipv6(ipv6)) {}
  NeighborKey(dataplane::InterfaceId interface, NeighborAddress address)
      : interface(interface), address(address) {}

  friend bool operator==(const NeighborKey &, const NeighborKey &) = default;
  friend auto operator<=>(const NeighborKey &, const NeighborKey &) = default;
};
static_assert(std::is_trivially_copyable_v<NeighborKey>);

}  // namespace bess::route

#endif  // BESS_ROUTE_NEIGHBOR_KEY_H_
