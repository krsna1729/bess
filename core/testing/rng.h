// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_TESTING_RNG_H_
#define BESS_TESTING_RNG_H_

// A generator with a fixed seed, for a case that must produce the same bytes
// every run. The seed is a parameter, so nothing is constant where the
// generator is built: `bugprone-random-generator-seed` protects a program from
// a predictable sequence, and a test wants exactly that.

#include <cstdint>
#include <random>

namespace bess::testing {

inline std::mt19937 SeededRng(uint32_t seed) {
  return std::mt19937(seed);
}

}  // namespace bess::testing

#endif  // BESS_TESTING_RNG_H_
