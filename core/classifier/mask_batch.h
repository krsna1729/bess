// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_MASK_BATCH_H_
#define BESS_CLASSIFIER_MASK_BATCH_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

#include "classifier/classifier.h"

namespace bess::classifier {

namespace detail {

// Largest logical key width the masked tables accept (ConcurrentMaskedTable,
// RuntimeMaskedBackend). Bounds the batch scratch the packet path keeps on the
// stack; a wider key is a build error rather than a silent stack growth.
inline constexpr size_t kMaskedMaxKeyBytes = 64;

using MaskBatchFn = void (*)(ConstBytes keys, size_t key_stride,
                             ConstBytes mask, size_t count,
                             MutableBytes out) noexcept;

// One mask over `count` keys of exactly Width bytes: the mask is loaded once
// into words (it may alias `out` as far as the compiler knows, so it would
// otherwise be reloaded per key), and each key is masked in 8-, 4-, 2- and
// 1-byte steps -- the same steps the fixed-width hash then reads them back in,
// so every reload forwards from its store.
template <size_t Width>
void MaskBatchFixed(ConstBytes keys, size_t key_stride, ConstBytes mask,
                    size_t count, MutableBytes out) noexcept {
  constexpr size_t kWords = Width / 8;
  constexpr size_t kTail = Width % 8;
  std::array<uint64_t, kWords> words;
  for (size_t w = 0; w < kWords; w++) {
    std::memcpy(&words[w], mask.data() + 8 * w, 8);
  }
  [[maybe_unused]] uint32_t mask4 = 0;
  [[maybe_unused]] uint16_t mask2 = 0;
  [[maybe_unused]] std::byte mask1{0};
  if constexpr (kTail >= 4) {
    std::memcpy(&mask4, mask.data() + 8 * kWords, 4);
  }
  if constexpr (kTail % 4 >= 2) {
    std::memcpy(&mask2, mask.data() + Width - kTail % 2 - 2, 2);
  }
  if constexpr (kTail % 2 == 1) {
    mask1 = mask[Width - 1];
  }
  for (size_t i = 0; i < count; i++) {
    const std::byte *src = keys.data() + i * key_stride;
    std::byte *dst = out.data() + i * Width;
    for (size_t w = 0; w < kWords; w++) {
      uint64_t v;
      std::memcpy(&v, src + 8 * w, 8);
      v &= words[w];
      std::memcpy(dst + 8 * w, &v, 8);
    }
    if constexpr (kTail >= 4) {
      uint32_t v;
      std::memcpy(&v, src + 8 * kWords, 4);
      v &= mask4;
      std::memcpy(dst + 8 * kWords, &v, 4);
    }
    if constexpr (kTail % 4 >= 2) {
      constexpr size_t kAt = Width - kTail % 2 - 2;
      uint16_t v;
      std::memcpy(&v, src + kAt, 2);
      v &= mask2;
      std::memcpy(dst + kAt, &v, 2);
    }
    if constexpr (kTail % 2 == 1) {
      dst[Width - 1] = src[Width - 1] & mask1;
    }
  }
}

// Bind the masking kernel to the key width once at build time (1..64 bytes).
inline MaskBatchFn SelectMaskBatch(size_t key_bytes) {
  static constexpr auto kTable = []<size_t... I>(std::index_sequence<I...>) {
    return std::array<MaskBatchFn, sizeof...(I)>{&MaskBatchFixed<I + 1>...};
  }(std::make_index_sequence<kMaskedMaxKeyBytes>{});
  return kTable[key_bytes - 1];
}

}  // namespace detail

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_MASK_BATCH_H_
