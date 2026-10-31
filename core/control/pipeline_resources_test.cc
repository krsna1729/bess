// SPDX-License-Identifier: BSD-3-Clause

#include "control/pipeline_resources.h"

#include <malloc.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <google/protobuf/wrappers.pb.h>

#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "gtest/gtest.h"
#include "rcu/rcu_domain.h"

namespace bess::control {
namespace {

using google::protobuf::BytesValue;
using google::protobuf::UInt32Value;

// A resource of `count` entries the codec below generates; it stores none.
class GeneratedResource final : public dataplane::Resource {
 public:
  GeneratedResource(std::string name, const uint32_t *count)
      : Resource(std::move(name)), count_(count) {}
  size_t LiveCount() const override { return *count_; }
  bool Contains(const dataplane::ResourceKey &) const override { return false; }
  std::expected<Reservation, std::string> Reserve(const dataplane::Op &) override {
    return std::unexpected("read only");
  }

 private:
  const uint32_t *count_;
};

// Reads never decode. "gen/table" has only a whole walk (Entries), so it is
// read by key; "gen/paged" gives the same entries from a cursor (PageFn).
// Entries come in a scrambled key order, each value `value_bytes_` long;
// every visit is counted and the heap in use is sampled after it, so a test
// sees the most the read held.
class PipelineResourcesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(engine_.Register(&resource_).has_value());
    binding_ = bindings_.Bind(
        resource_,
        std::make_shared<framework::TypedCodec<UInt32Value, BytesValue>>(
            [](const UInt32Value &) -> std::expected<dataplane::ResourceKey, std::string> {
              return std::unexpected("unused");
            },
            [](const BytesValue &) -> std::expected<std::any, std::string> {
              return std::unexpected("unused");
            },
            [this](const auto &visit) {
              UInt32Value key;
              BytesValue value;
              value.set_value(std::string(value_bytes_, 'v'));
              for (uint32_t i = 0; i < count_; i++) {
                key.set_value((i * 7919u) % count_);  // a permutation: 7919 is prime
                visit(key, value);
                visits_++;
                high_water_ = std::max(high_water_, InUse());
              }
            }));
    ASSERT_TRUE(engine_.Register(&paged_).has_value());
    paged_binding_ = bindings_.Bind(
        paged_,
        std::make_shared<framework::TypedCodec<UInt32Value, BytesValue>>(
            [](const UInt32Value &) -> std::expected<dataplane::ResourceKey, std::string> {
              return std::unexpected("unused");
            },
            [](const BytesValue &) -> std::expected<std::any, std::string> {
              return std::unexpected("unused");
            },
            [this](std::string_view cursor, const auto &visit) -> std::string {
              std::array<uint32_t, 1> from{};
              (void)framework::CursorFields(cursor, &from);
              UInt32Value key;
              BytesValue value;
              value.set_value(std::string(value_bytes_, 'v'));
              for (uint32_t i = from[0]; i < count_; i++) {
                key.set_value((i * 7919u) % count_);
                const bool more = visit(key, value);
                visits_++;
                high_water_ = std::max(high_water_, InUse());
                if (!more) {
                  return i + 1 < count_ ? framework::CursorOf<1>({i + 1}) : std::string();
                }
              }
              return {};
            }));
  }

  // A paged read of `name` at `page` entries per page into `keys` (reserved
  // by the caller, so they are not counted as held); returns the pages.
  size_t ReadAll(const std::string &name, size_t page, std::vector<uint32_t> *keys) {
    size_t pages = 0;
    std::string cursor;
    for (bool more = true; more;) {
      auto read = ReadResourcePage(engine_, bindings_, name, cursor, page, 3 << 20);
      EXPECT_TRUE(read.has_value()) << read.error().message;
      if (!read) {
        break;
      }
      EXPECT_LE(read->entries.size(), page);
      for (const ResourceEntrySpec &entry : read->entries) {
        UInt32Value key;
        EXPECT_TRUE(entry.key.UnpackTo(&key));
        keys->push_back(key.value());
      }
      pages++;
      more = read->next.has_value();
      cursor = read->next.value_or("");
    }
    return pages;
  }

  static size_t InUse() { return mallinfo2().uordblks; }

  // Entries of `value_bytes_` the read held at most, above `base` bytes.
  size_t HeldEntries(size_t base) const {
    return high_water_ > base ? (high_water_ - base) / value_bytes_ : 0;
  }

  uint32_t count_ = 0;
  size_t value_bytes_ = 0;
  size_t visits_ = 0;
  size_t high_water_ = 0;
  rcu::RcuDomain domain_{1, 8};
  dataplane::TransactionEngine engine_{domain_};
  GeneratedResource resource_{"gen/table", &count_};
  GeneratedResource paged_{"gen/paged", &count_};
  framework::ResourceBindings bindings_;
  framework::ResourceBinding binding_;
  framework::ResourceBinding paged_binding_;
  const std::vector<std::string> names_{"gen/table"};
};

// 200,000 live entries at no less than ~100 bytes each cannot fit 1 MiB
// however small their values: refused before the walk, with no entry built.
TEST_F(PipelineResourcesTest, ReadResourcesRefusesBeforeTheWalkWhenTheCountCannotFit) {
  count_ = 200000;
  constexpr size_t kBudget = 1 << 20;
  auto read = ReadResources(engine_, bindings_, names_, kBudget);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(ControlErrorCode::kResourceExhausted, read.error().code);
  EXPECT_EQ(0u, visits_) << "the walk ran for a read the count already refused";
}

// 2,000 entries pass the count check (their smallest form fits) but their
// 4 KiB values do not: the read stops building at the budget, holding about
// 1 MiB of entries rather than 8 MiB.
TEST_F(PipelineResourcesTest, ReadResourcesStopsBuildingAtTheBudget) {
  count_ = 2000;
  value_bytes_ = 4096;
  constexpr size_t kBudget = 1 << 20;
  const size_t base = InUse();
  auto read = ReadResources(engine_, bindings_, names_, kBudget);
  ASSERT_FALSE(read.has_value());
  EXPECT_EQ(ControlErrorCode::kResourceExhausted, read.error().code);
  EXPECT_LE(HeldEntries(base), 2 * kBudget / value_bytes_)
      << "entries built past the budget";
}

// Read by key (no cursor), 10,000 4 KiB entries at 128 per page: the read
// holds at most two pages' worth at once, never the resource, and still
// returns every key once.
TEST_F(PipelineResourcesTest, ReadResourcePageByKeyHoldsAtMostTwoPages) {
  count_ = 10000;
  value_bytes_ = 4096;
  constexpr size_t kPage = 128;
  std::vector<uint32_t> keys;
  keys.reserve(count_);
  const size_t base = InUse();
  (void)ReadAll("gen/table", kPage, &keys);
  EXPECT_LE(HeldEntries(base), 2 * kPage) << "the read held more than two pages";
  ASSERT_EQ(count_, keys.size());
  std::vector<uint32_t> sorted = keys;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(sorted.end(), std::adjacent_find(sorted.begin(), sorted.end())) << "a key twice";
}

// From the codec's cursor, 100,000 entries at 128 per page: a page costs its
// own entries (the whole read visits N + pages * 1, not N per page), holds at
// most two pages, and returns every key once.
TEST_F(PipelineResourcesTest, ReadResourcePageFromACursorVisitsEachEntryOnce) {
  count_ = 100000;
  value_bytes_ = 4096;
  constexpr size_t kPage = 128;
  std::vector<uint32_t> keys;
  keys.reserve(count_);
  const size_t base = InUse();
  const size_t pages = ReadAll("gen/paged", kPage, &keys);
  EXPECT_LE(visits_, count_ + pages) << "a page walked more than its own entries";
  EXPECT_LE(HeldEntries(base), 2 * kPage) << "the read held more than two pages";
  ASSERT_EQ(count_, keys.size());
  std::vector<uint32_t> sorted = keys;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(sorted.end(), std::adjacent_find(sorted.begin(), sorted.end())) << "a key twice";
}

}  // namespace
}  // namespace bess::control
