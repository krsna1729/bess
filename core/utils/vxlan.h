// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_VXLAN_H_
#define BESS_UTILS_VXLAN_H_

#include <type_traits>

namespace bess {
namespace utils {

// 8-byte basic VXLAN header
// +-------+-------+-------+--------+
// | flags |       Reserved         |
// +-------+-------+-------+--------+
// |        VNI            | Rsvd.  |
// +-------+-------+-------+--------+
struct[[gnu::packed]] Vxlan {
  be32_t vx_flags;
  be32_t vx_vni;
};

static_assert(std::is_standard_layout<Vxlan>::value &&
                  std::is_trivial<Vxlan>::value,
              "Vxlan must be standard-layout and trivial");
static_assert(sizeof(Vxlan) == 8, "struct Vxlan is incorrect");

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_VXLAN_H_
