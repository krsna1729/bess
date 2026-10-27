// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_FIREWALL_KEY_KERNELS_H_
#define BESS_MODULES_FIREWALL_KEY_KERNELS_H_

#include <bit>
#include <cstdint>
#include <cstring>

#include "arch/cpu.h"

namespace bess::firewall_key {

// Preconditions: an untagged IPv4 frame with IHL 5, at least 38 contiguous
// readable bytes, and checked TCP/UDP ports. src and dst each hold 16 writable
// bytes; neither output overlaps the input. IPv4 addresses occupy bytes 0..3
// of each output, with the other bytes zero; ports are returned in host order.
inline void GatherScalar(const uint8_t *frame, void *src, void *dst,
                         uint16_t &src_port, uint16_t &dst_port) noexcept {
  std::memset(src, 0, 16);
  std::memset(dst, 0, 16);
  std::memcpy(src, frame + 26, 4);
  std::memcpy(dst, frame + 30, 4);
  src_port = static_cast<uint16_t>((unsigned{frame[34]} << 8u) | frame[35]);
  dst_port = static_cast<uint16_t>((unsigned{frame[36]} << 8u) | frame[37]);
}

// One bounded 16-byte input load followed by address/port byte shuffles.
// The load is frame[22..37], never beyond the checked ports. GCC/Clang vector
// extensions select the target's operations. The family and protocol remain
// the caller's fields.
inline void GatherPortable(const uint8_t *frame, void *src, void *dst,
                           uint16_t &src_port, uint16_t &dst_port) noexcept {
  using Bytes16 = uint8_t __attribute__((vector_size(16)));
  using Ports8 = uint16_t __attribute__((vector_size(16)));
  Bytes16 v;
  std::memcpy(&v, frame + 22, sizeof(v));
  const Bytes16 zero{};
  const Bytes16 s = __builtin_shufflevector(v, zero, 4, 5, 6, 7, 16, 16, 16, 16,
                                            16, 16, 16, 16, 16, 16, 16, 16);
  const Bytes16 d = __builtin_shufflevector(v, zero, 8, 9, 10, 11, 16, 16, 16,
                                            16, 16, 16, 16, 16, 16, 16, 16, 16);
  std::memcpy(src, &s, sizeof(s));
  std::memcpy(dst, &d, sizeof(d));
  Bytes16 bytes;
  if constexpr (std::endian::native == std::endian::little) {
    bytes = __builtin_shufflevector(v, v, 13, 12, 15, 14, 0, 0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0);
  } else {
    bytes = __builtin_shufflevector(v, v, 12, 13, 14, 15, 0, 0, 0, 0, 0, 0, 0,
                                    0, 0, 0, 0, 0);
  }
  Ports8 ports;
  std::memcpy(&ports, &bytes, sizeof(ports));
  src_port = ports[0];
  dst_port = ports[1];
}

// Same preconditions as GatherScalar. The x86-v3 form emits one load, three
// byte shuffles, two address stores and two port-extract stores. GCC 16.2
// adds three instructions to the portable form's wrapper. Eight paired,
// CPU-isolated dependency-probe rounds measured portable 12.57 ns vs native
// 11.57 ns (paired ratio 0.921, all eight lower). Turbo off, balanced profile,
// powersave/EPP balance_performance; IRQ/softirq activity was observed and
// the busy-process preflight was overridden. This is a kernel latency probe,
// not a whole-Firewall timing claim. Live Firewall rows used 5.5-5.9 fewer
// instructions per packet than the portable form, all eight pairs lower.
inline void Gather(const uint8_t *frame, void *src, void *dst,
                   uint16_t &src_port, uint16_t &dst_port) noexcept {
#if defined(BESS_ARCH_AVX2)
  const __m128i v =
      _mm_loadu_si128(reinterpret_cast<const __m128i *>(frame + 22));
  const __m128i s =
      _mm_shuffle_epi8(v, _mm_setr_epi8(4, 5, 6, 7, -1, -1, -1, -1, -1, -1, -1,
                                        -1, -1, -1, -1, -1));
  const __m128i d =
      _mm_shuffle_epi8(v, _mm_setr_epi8(8, 9, 10, 11, -1, -1, -1, -1, -1, -1,
                                        -1, -1, -1, -1, -1, -1));
  const __m128i ports =
      _mm_shuffle_epi8(v, _mm_setr_epi8(13, 12, 15, 14, -1, -1, -1, -1, -1, -1,
                                        -1, -1, -1, -1, -1, -1));
  std::memcpy(src, &s, sizeof(s));
  std::memcpy(dst, &d, sizeof(d));
  src_port = static_cast<uint16_t>(_mm_extract_epi16(ports, 0));
  dst_port = static_cast<uint16_t>(_mm_extract_epi16(ports, 1));
#elif defined(BESS_ARCH_GENERIC)
  GatherScalar(frame, src, dst, src_port, dst_port);
#else
  GatherPortable(frame, src, dst, src_port, dst_port);
#endif
}

}  // namespace bess::firewall_key

#endif  // BESS_MODULES_FIREWALL_KEY_KERNELS_H_
