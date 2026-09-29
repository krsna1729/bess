// SPDX-License-Identifier: BSD-3-Clause

#include <gtest/gtest.h>

#include <type_traits>

#include "classifier/classifier.h"
#include "dataplane/action_id.h"

namespace {

using bess::classifier::ExactBackendKind;
using bess::classifier::ResultMode;
using bess::classifier::ResultSlot;
using bess::classifier::WildcardBackendKind;

static_assert(!std::is_same_v<ResultSlot, bess::dataplane::ActionId>);
static_assert(!std::is_convertible_v<ResultSlot, bess::dataplane::ActionId>);

TEST(ClassifierContractTest, ResultModesAndBackendVocabularyAreExplicit) {
  EXPECT_NE(ResultMode::kGate, ResultMode::kSlot);
  EXPECT_NE(ExactBackendKind::kCuckoo, ExactBackendKind::kRteHash);
  EXPECT_NE(WildcardBackendKind::kTupleSpace, WildcardBackendKind::kRteAcl);
}

}  // namespace
