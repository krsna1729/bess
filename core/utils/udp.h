// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_UDP_H_
#define BESS_UTILS_UDP_H_

#include <type_traits>

namespace bess {
namespace utils {

// A basic UDP header definition.
struct[[gnu::packed]] Udp {
  be16_t src_port;    // Source port.
  be16_t dst_port;    // Destination port.
  be16_t length;      // Length of header and data.
  uint16_t checksum;  // Checksum.
};

static_assert(std::is_standard_layout<Udp>::value &&
                  std::is_trivial<Udp>::value,
              "Udp must be standard-layout and trivial");
static_assert(sizeof(Udp) == 8, "struct Udp is incorrect");

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_UDP_H_
