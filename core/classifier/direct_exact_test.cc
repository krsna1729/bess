// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
//   contributors may be used to endorse or promote products derived from this
//   software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "classifier/classifier.h"
#include "classifier/direct_exact.h"
#include "classifier/typed_exact.h"

namespace {

using bess::classifier::BackendInfo;
using bess::classifier::BatchExactBackend;
using bess::classifier::DirectExactBackend;
using bess::classifier::ExactBackendKind;
using bess::classifier::MeasurableBackend;
using bess::classifier::ScalarExactBackend;

static_assert(ScalarExactBackend<DirectExactBackend<uint8_t, uint32_t>, uint8_t>);
static_assert(BatchExactBackend<DirectExactBackend<uint8_t, uint32_t>, uint8_t>);
static_assert(MeasurableBackend<DirectExactBackend<uint8_t, uint32_t>>);

TEST(DirectExactBackendTest, InsertLookupRemove8Bit) {
  DirectExactBackend<uint8_t, uint32_t> backend;
  EXPECT_EQ(0u, backend.size());

  ASSERT_TRUE(backend.insert(10, 100u));
  ASSERT_TRUE(backend.insert(20, 200u));
  EXPECT_EQ(2u, backend.size());

  // Hits
  const uint32_t *r1 = backend.lookup(10);
  ASSERT_NE(nullptr, r1);
  EXPECT_EQ(100u, *r1);

  const uint32_t *r2 = backend.lookup(20);
  ASSERT_NE(nullptr, r2);
  EXPECT_EQ(200u, *r2);

  // Miss
  EXPECT_EQ(nullptr, backend.lookup(30));

  // Overwrite
  ASSERT_TRUE(backend.insert(10, 999u));
  EXPECT_EQ(2u, backend.size());
  EXPECT_EQ(999u, *backend.lookup(10));

  // Removal
  EXPECT_TRUE(backend.remove(10));
  EXPECT_EQ(1u, backend.size());
  EXPECT_EQ(nullptr, backend.lookup(10));
  EXPECT_FALSE(backend.remove(10));  // already absent
}

TEST(DirectExactBackendTest, BatchLookupHitMask) {
  DirectExactBackend<uint8_t, uint32_t> backend;
  backend.insert(1, 10u);
  backend.insert(3, 30u);

  const std::array<uint8_t, 4> keys = {1, 2, 3, 4};
  std::array<uint32_t, 4> results{};

  uint64_t hits = backend.lookup_batch(keys, results);

  // Bits 0 and 2 set -> 0b0101 = 5
  EXPECT_EQ(0x5ull, hits);
  EXPECT_EQ(10u, results[0]);
  EXPECT_EQ(30u, results[2]);
}

TEST(DirectExactBackendTest, InfoReportsCorrectMetadata) {
  DirectExactBackend<uint8_t, uint16_t> backend;
  backend.insert(42, 7u);

  BackendInfo info = backend.info();
  EXPECT_EQ(ExactBackendKind::kDirect, info.kind);
  EXPECT_EQ(1u, info.rule_count);
  EXPECT_EQ(1u, info.key_size);
  EXPECT_EQ(2u, info.result_size);
}

}  // namespace
