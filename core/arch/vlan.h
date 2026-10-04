// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_ARCH_VLAN_H_
#define BESS_ARCH_VLAN_H_

// 802.1Q / 802.1ad tag removal and insertion at the front of an Ethernet
// frame (VLANPop, VLANSplit, VLANPush, Vif) (M21, D-071). Each is one 16-byte
// load, one byte shift or lane insert, and one 16-byte store, written with
// GCC/Clang vector extensions rather than intrinsics: the compiler emits
// SSE2/SSE4.1 on x86 (the same instructions as the former intrinsics) and
// NEON on arm64, and plain stores where there is no vector unit. One path
// for every target; nothing here depends on BESS_ARCH_*.

#include <cstdint>
#include <cstring>

namespace bess::arch {

namespace detail {
using VlanBytes16 = uint8_t __attribute__((vector_size(16)));
using VlanWords4 = uint32_t __attribute__((vector_size(16)));
}  // namespace detail

// `p` points at a tagged frame: [dst MAC | src MAC | TPID | TCI], 16 bytes.
// Rewrites those bytes as [0 0 0 0 | dst MAC | src MAC]: the addresses move
// 4 bytes forward over the tag, so the untagged frame starts at p + 4.
inline void RemoveVlanTag(void *p) noexcept {
  detail::VlanBytes16 v;
  std::memcpy(&v, p, sizeof(v));
  v = __builtin_shufflevector(v, detail::VlanBytes16{}, 16, 16, 16, 16, 0, 1,
                              2, 3, 4, 5, 6, 7, 8, 9, 10, 11);
  std::memcpy(p, &v, sizeof(v));
}

// `p` points 4 bytes before an untagged frame. Writes [dst MAC | src MAC |
// tag] to p[0..15]: the addresses move 4 bytes back and the tag (TPID then
// TCI, `tag_raw` holding them in network byte order as stored) fills the gap
// before the frame's EtherType, which stays where it was.
inline void InsertVlanTag(void *p, uint32_t tag_raw) noexcept {
  detail::VlanWords4 v;
  std::memcpy(&v, static_cast<char *>(p) + 4, sizeof(v));
  v[3] = tag_raw;
  std::memcpy(p, &v, sizeof(v));
}

}  // namespace bess::arch

#endif  // BESS_ARCH_VLAN_H_
