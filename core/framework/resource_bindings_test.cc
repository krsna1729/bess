// SPDX-License-Identifier: BSD-3-Clause

#include "framework/resource_bindings.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "dataplane/resource.h"

namespace {

using bess::dataplane::Op;
using bess::framework::ResourceBinding;
using bess::framework::ResourceBindings;
using bess::framework::ResourceCodec;

class FakeResource final : public bess::dataplane::Resource {
 public:
  explicit FakeResource(std::string name) : Resource(std::move(name)) {}
  bool Contains(const bess::dataplane::ResourceKey &) const override {
    return false;
  }
  std::expected<Reservation, std::string> Reserve(const Op &) override {
    return std::unexpected("unused");
  }
  size_t LiveCount() const override { return 0; }
};

class FakeCodec final : public ResourceCodec {
 public:
  std::string key_type() const override { return "k"; }
  std::string value_type() const override { return "v"; }
  std::expected<bess::dataplane::ResourceKey, std::string> Key(
      std::string_view, const std::string &) const override {
    return std::unexpected("unused");
  }
  std::expected<std::any, std::string> Value(
      std::string_view, const std::string &) const override {
    return std::unexpected("unused");
  }
};

TEST(ResourceBindingsTest, UnboundResourceHasNoCodec) {
  ResourceBindings bindings;
  FakeResource r("r");
  EXPECT_EQ(nullptr, bindings.Find(r));
}

TEST(ResourceBindingsTest, DestroyingTheHandleRemovesTheBinding) {
  ResourceBindings bindings;
  FakeResource r("r");
  auto codec = std::make_shared<FakeCodec>();
  {
    ResourceBinding binding = bindings.Bind(r, codec);
    EXPECT_TRUE(binding.bound());
    EXPECT_EQ(codec.get(), bindings.Find(r));
    EXPECT_EQ(1u, bindings.size());
  }
  EXPECT_EQ(nullptr, bindings.Find(r));
  EXPECT_EQ(0u, bindings.size());
}

TEST(ResourceBindingsTest, MovingAHandleTransfersTheBinding) {
  ResourceBindings bindings;
  FakeResource r("r");
  auto codec = std::make_shared<FakeCodec>();
  ResourceBinding a = bindings.Bind(r, codec);
  ResourceBinding b = std::move(a);
  EXPECT_FALSE(a.bound());
  a.Reset();  // the moved-from handle must not remove b's binding
  EXPECT_EQ(codec.get(), bindings.Find(r));
  b.Reset();
  EXPECT_EQ(nullptr, bindings.Find(r));
  b.Reset();  // idempotent
}

TEST(ResourceBindingsTest, BindingsAreIndependentPerResource) {
  ResourceBindings bindings;
  FakeResource r1("r1"), r2("r2");
  auto c1 = std::make_shared<FakeCodec>();
  auto c2 = std::make_shared<FakeCodec>();
  ResourceBinding b1 = bindings.Bind(r1, c1);
  ResourceBinding b2 = bindings.Bind(r2, c2);
  b1.Reset();
  EXPECT_EQ(nullptr, bindings.Find(r1));
  EXPECT_EQ(c2.get(), bindings.Find(r2));
}

TEST(ResourceBindingsTest, RegistrySharesOwnershipOfTheCodec) {
  // The registry shares ownership of the codec: dropping the caller's
  // reference must not invalidate a live binding.
  ResourceBindings bindings;
  FakeResource r("r");
  ResourceBinding binding = bindings.Bind(r, std::make_shared<FakeCodec>());
  ASSERT_NE(nullptr, bindings.Find(r));
  EXPECT_EQ("k", bindings.Find(r)->key_type());
}

}  // namespace
