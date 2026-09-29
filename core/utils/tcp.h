// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_TCP_H_
#define BESS_UTILS_TCP_H_

#include <type_traits>

namespace bess {
namespace utils {

// A basic TCP header definition loosely based on the BSD version.
struct[[gnu::packed]] Tcp {
  enum Flag : uint8_t {
    kFin = 0x01,
    kSyn = 0x02,
    kRst = 0x04,
    kPsh = 0x08,
    kAck = 0x10,
    kUrg = 0x20,
  };

  be16_t src_port;  // Source port.
  be16_t dst_port;  // Destination port.
  be32_t seq_num;   // Sequence number.
  be32_t ack_num;   // Acknowledgement number.
#if __BYTE_ORDER == __LITTLE_ENDIAN
  uint8_t reserved : 4;  // Unused reserved bits.
  uint8_t offset : 4;    // Data offset.
#elif __BYTE_ORDER == __BIG_ENDIAN
  uint8_t offset : 4;    // Data offset.
  uint8_t reserved : 4;  // Unused reserved bits.
#else
#error __BYTE_ORDER must be defined.
#endif
  uint8_t flags;      // Flags.
  be16_t window;      // Receive window.
  uint16_t checksum;  // Checksum.
  be16_t urgent_ptr;  // Urgent pointer.
};

static_assert(std::is_standard_layout<Tcp>::value &&
                  std::is_trivial<Tcp>::value,
              "Tcp must be standard-layout and trivial");
static_assert(sizeof(Tcp) == 20, "struct Tcp is incorrect");

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_TCP_H_
