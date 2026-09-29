// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// Copyright (c) 2017, Cloudigo.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_ETHER_H_
#define BESS_UTILS_ETHER_H_

#include <cstdint>
#include <string>
#include <type_traits>

#include "endian.h"

#include "copy.h"

namespace bess {
namespace utils {

struct [[gnu::packed]] Ethernet {
  struct [[gnu::packed]] Address {
    Address() = default;
    Address(const uint8_t *addr) { bess::utils::Copy(bytes, addr, kSize); }
    Address(const std::string &str);

    static const size_t kSize = 6;

    // Parses str in "aA:Bb:00:11:22:33" format and saves the address in parsed
    // Returns false if the format is incorrect.
    //   (in that case, the content of parsed is undefined.)
    bool FromString(const std::string &str);

    // Returns "aa:bb:00:11:22:33" (all in lower case)
    std::string ToString() const;

    void Randomize();

    bool IsBroadcast() const {
      return bytes[0] == 0xff && bytes[1] == 0xff && bytes[2] == 0xff &&
             bytes[3] == 0xff && bytes[4] == 0xff && bytes[5] == 0xff;
    }

    bool IsZero() const {
      return bytes[0] == 0x00 && bytes[1] == 0x00 && bytes[2] == 0x00 &&
             bytes[3] == 0x00 && bytes[4] == 0x00 && bytes[5] == 0x00;
    }

    bool operator<(const Address &o) const {
      for (size_t i = 0; i < kSize; i++) {
        if (bytes[i] < o.bytes[i]) {
          return true;
        }
      }
      return false;
    }

    bool operator==(const Address &o) const {
      for (size_t i = 0; i < kSize; i++) {
        if (bytes[i] != o.bytes[i]) {
          return false;
        }
      }
      return true;
    }

    bool operator!=(const Address &o) const {
      for (size_t i = 0; i < kSize; i++) {
        if (bytes[i] != o.bytes[i]) {
          return true;
        }
      }
      return false;
    }

    uint8_t bytes[kSize];
  };

  enum Type : uint16_t {
    kIpv4 = 0x0800,
    kArp = 0x0806,
    kVlan = 0x8100,
    kQinQ = 0x88a8,  // 802.1ad double-tagged VLAN packets
    kIpv6 = 0x86DD,
    kMpls = 0x8847,
  };

  Address dst_addr;
  Address src_addr;
  be16_t ether_type;
};

struct [[gnu::packed]] Vlan {
  be16_t tci;
  be16_t ether_type;
};

static_assert(std::is_standard_layout<Ethernet>::value &&
                  std::is_trivial<Ethernet>::value,
              "Ethernet must be standard-layout and trivial");
static_assert(std::is_standard_layout<Ethernet::Address>::value &&
                  std::is_trivial<Ethernet::Address>::value,
              "Ethernet::Address must be standard-layout and trivial");
static_assert(sizeof(Ethernet) == 14, "struct Ethernet is incorrect");
static_assert(std::is_standard_layout<Vlan>::value &&
                  std::is_trivial<Vlan>::value,
              "Vlan must be standard-layout and trivial");
static_assert(sizeof(Vlan) == 4, "struct Vlan is incorrectly sized");

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_ETHER_H_
