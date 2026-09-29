// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_ICMP_H_
#define BESS_UTILS_ICMP_H_

#include <type_traits>

namespace bess {
namespace utils {

// A basic ICMP header definition.
struct[[gnu::packed]] Icmp {
  uint8_t type;       // ICMP packet type.
  uint8_t code;       // ICMP packet code.
  uint16_t checksum;  // ICMP packet checksum.
  be16_t ident;       // ICMP packet identifier.
  be16_t seq_num;     // ICMP packet sequence number
};

static_assert(std::is_standard_layout<Icmp>::value &&
                  std::is_trivial<Icmp>::value,
              "Icmp must be standard-layout and trivial");
static_assert(sizeof(Icmp) == 8, "struct Icmp is incorrect");

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_ICMP_H_
