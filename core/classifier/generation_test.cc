// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor their contributors may be
// used to endorse or promote products derived from this software without
// specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
// ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
// THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>

#include "classifier/generation.h"
#include "classifier/result_plan.h"
#include "rcu/rcu_domain.h"
#include "rcu/rcu_ptr.h"

namespace {

using bess::classifier::BackendInfo;
using bess::classifier::ExtractPlan;
using bess::classifier::Generation;
using bess::classifier::ResultPlan;
using bess::classifier::RuntimeClassifierSchema;
using bess::classifier::RuntimeKeyField;
using bess::classifier::SourceKind;
using bess::rcu::RcuDomain;
using bess::rcu::RcuPtr;

struct FakeBackend {
  int value = 0;
};

using TestGeneration = Generation<FakeBackend>;

std::unique_ptr<const TestGeneration> BuildGeneration(int value) {
  RuntimeClassifierSchema schema{
      .key_size = 1,
      .key_fields = {{SourceKind::kPacket, 0, 0, 1}},
  };
  auto extract = ExtractPlan::Compile(schema);
  auto result = ResultPlan::Compile(schema);
  EXPECT_TRUE(extract);
  EXPECT_TRUE(result);
  if (!extract || !result) {
    return nullptr;
  }
  return std::unique_ptr<const TestGeneration>(new TestGeneration(
      std::move(*extract), FakeBackend{value}, std::move(*result),
      BackendInfo{}));
}

TEST(GenerationTest, RcuPublishesWholeImmutableGeneration) {
  RcuDomain domain(2);
  constexpr uint32_t reader = 0;
  ASSERT_TRUE(domain.Register(reader).has_value());
  domain.Online(reader);

  RcuPtr<TestGeneration> published(domain);
  published.Initialize(BuildGeneration(1));
  const TestGeneration *old_generation = published.Read();
  ASSERT_NE(nullptr, old_generation);
  ASSERT_EQ(1, old_generation->backend().value);

  const auto token = published.Publish(BuildGeneration(2));
  EXPECT_EQ(1, old_generation->backend().value);
  EXPECT_EQ(2, published.Read()->backend().value);
  EXPECT_FALSE(domain.IsComplete(token));

  domain.Quiescent(reader);
  EXPECT_TRUE(domain.IsComplete(token));
  EXPECT_EQ(1u, domain.ReclaimReady());

  domain.Offline(reader);
  domain.Unregister(reader);
}

}  // namespace
