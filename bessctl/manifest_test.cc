// SPDX-License-Identifier: BSD-3-Clause

// bessctl's manifests: the JSON tree keeps numbers exactly, and a manifest's
// args get their type from the schema. control_v2's own messages stand in for
// module and driver argument types, so no module code is linked.

#include "manifest.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <string>

#include "json.h"

namespace bessctl {
namespace {

namespace gp = google::protobuf;

// -- JSON ---------------------------------------------------------------------

TEST(Json, NumbersSurviveExactly) {
  // A double holds neither: 2^64 - 1 and 2^53 + 1.
  const std::string text = R"({"a":18446744073709551615,"b":9007199254740993,"c":-0.5e-10})";
  auto v = json::Parse(text);
  ASSERT_TRUE(v) << v.error();
  EXPECT_EQ(text, json::Dump(*v));
}

TEST(Json, StringsAreDecodedAndReencoded) {
  auto v = json::Parse(R"(["\u00e9\ud83d\ude00", "a\"b\\c\n\u0001"])");
  ASSERT_TRUE(v) << v.error();
  const json::Array &a = *v->array();
  EXPECT_EQ("\xC3\xA9\xF0\x9F\x98\x80", *a[0].string());
  EXPECT_EQ("a\"b\\c\n\x01", *a[1].string());
  EXPECT_EQ(R"(["é😀","a\"b\\c\n\u0001"])", json::Dump(*v));
}

TEST(Json, MalformedInputIsRefusedWithItsPosition) {
  const std::pair<const char *, const char *> cases[] = {
      {"[1,]", "line 1, column 4: expected a value"},
      {"{\"a\":1,\n\"a\":2}", "line 2, column 4: duplicate member \"a\""},
      {"01", "line 1, column 2: unexpected text after the value"},
      {"\"\\ud800\"", "unpaired surrogate"},
      {"\"a", "unterminated string"},
      {"{\"a\" 1}", "expected ':'"},
      {"1.", "expected digits after '.'"},
      {"nul", "invalid literal"},
      {"\"\x01\"", "control character"},
  };
  for (const auto &[input, error] : cases) {
    auto v = json::Parse(input);
    ASSERT_FALSE(v) << input;
    EXPECT_NE(std::string::npos, v.error().find(error)) << input << ": " << v.error();
  }
  EXPECT_FALSE(json::Parse(std::string(json::kMaxDepth + 2, '[') +
                           std::string(json::kMaxDepth + 2, ']')));
  EXPECT_TRUE(json::Parse(std::string(json::kMaxDepth, '[') + std::string(json::kMaxDepth, ']')));
}

// -- manifests ----------------------------------------------------------------

// A daemon's schema: module class "Fake" takes a Worker, driver "FakePort" a
// Connection, "Opaque" declares nothing; descriptors as GetSchema sends them.
pb::GetSchemaResponse FakeSchema() {
  pb::GetSchemaResponse s;
  auto *m = s.add_module_classes();
  m->set_name("Fake");
  m->set_init_arg_type("bess.pb.v2.Worker");
  s.add_module_classes()->set_name("Opaque");
  auto *d = s.add_port_drivers();
  d->set_name("FakePort");
  d->set_init_arg_type("bess.pb.v2.Connection");
  std::set<std::string> added;
  std::function<void(const gp::FileDescriptor *)> add = [&](const gp::FileDescriptor *f) {
    if (!added.emplace(f->name()).second) {
      return;
    }
    for (int i = 0; i < f->dependency_count(); i++) {
      add(f->dependency(i));
    }
    f->CopyTo(s.mutable_descriptors()->add_file());
  };
  add(pb::Worker::descriptor()->file());
  return s;
}

std::unique_ptr<Schema> MakeSchema() {
  auto s = Schema::From(FakeSchema());
  EXPECT_TRUE(s) << s.error();
  return std::move(*s);
}

TEST(Manifest, AnArgWithoutTypeGetsItsClassesInitType) {
  auto schema = MakeSchema();
  auto p = schema->PipelineFromJson(R"({
    "modules": [{"name": "m", "mclass": "Fake", "arg": {"wid": 3, "core": "7"}}],
    "ports": [{"name": "p", "driver": "FakePort", "arg": {"upstream": "u", "ogate": 2}}]
  })");
  ASSERT_TRUE(p) << p.error();
  pb::Worker w;
  ASSERT_TRUE(p->modules(0).arg().UnpackTo(&w)) << p->modules(0).arg().type_url();
  EXPECT_EQ(3, w.wid());
  EXPECT_EQ(7, w.core());
  pb::Connection c;
  ASSERT_TRUE(p->ports(0).arg().UnpackTo(&c));
  EXPECT_EQ("u", c.upstream());
  EXPECT_EQ(2u, c.ogate());
}

TEST(Manifest, AnExplicitTypeIsKeptEvenIfItDiffers) {
  auto schema = MakeSchema();
  auto p = schema->PipelineFromJson(R"({"modules": [{"name": "m", "mclass": "Fake",
      "arg": {"@type": "type.googleapis.com/bess.pb.v2.Connection", "upstream": "x"}}]})");
  ASSERT_TRUE(p) << p.error();
  EXPECT_TRUE(p->modules(0).arg().Is<pb::Connection>());
}

TEST(Manifest, SixtyFourBitValuesAreNotRounded) {
  auto schema = MakeSchema();
  auto p = schema->PipelineFromJson(
      R"({"traffic_classes": [{"name": "t", "limit": {"count": 9223372036854775807}}]})");
  ASSERT_TRUE(p) << p.error();
  EXPECT_EQ(std::numeric_limits<int64_t>::max(), p->traffic_classes(0).limit().at("count"));
}

TEST(Manifest, UnknownOrUntypedClassesAreNamed) {
  auto schema = MakeSchema();
  const std::pair<const char *, const char *> cases[] = {
      {R"({"modules": [{"name": "m", "mclass": "Nope", "arg": {}}]})",
       "module \"m\": the daemon has no module class \"Nope\""},
      {R"({"modules": [{"name": "m", "mclass": "Opaque", "arg": {}}]})",
       "module \"m\": module class \"Opaque\" declares no argument type"},
      {R"({"ports": [{"name": "p", "arg": {}}]})", "port \"p\": an \"arg\" needs \"driver\""},
      {R"({"modules": {}})", "\"modules\" is not a list"},
      {R"([])", "a manifest is a JSON object"},
  };
  for (const auto &[text, error] : cases) {
    auto p = schema->PipelineFromJson(text);
    ASSERT_FALSE(p) << text;
    EXPECT_NE(std::string::npos, p.error().find(error)) << text << ": " << p.error();
  }
  // A value of the wrong type for the class's argument: protobuf's own error.
  EXPECT_FALSE(schema->PipelineFromJson(
      R"({"modules": [{"name": "m", "mclass": "Fake", "arg": {"wid": "x"}}]})"));
  // An untyped class without an arg is fine: nothing to type.
  EXPECT_TRUE(schema->PipelineFromJson(R"({"modules": [{"name": "m", "mclass": "Opaque"}]})"));
}

TEST(Manifest, WhatGetPrintsApplyReadsBack) {
  auto schema = MakeSchema();
  auto p = schema->PipelineFromJson(R"({
    "modules": [{"name": "m", "mclass": "Fake", "arg": {"wid": 3}},
                {"name": "n", "mclass": "Fake",
                 "arg": {"@type": "type.googleapis.com/bess.pb.v2.Connection", "upstream": "x"}}],
    "connections": [{"upstream": "m", "ogate": 1, "downstream": "n"}],
    "traffic_classes": [{"name": "t", "limit": {"count": 9223372036854775807}}]
  })");
  ASSERT_TRUE(p) << p.error();
  auto text = schema->PipelineToJson(*p);
  ASSERT_TRUE(text) << text.error();
  // The implied type is left out, a different one kept; names as in the .proto.
  EXPECT_EQ(1u, [&] {
    size_t n = 0;
    for (size_t at = text->find("@type"); at != std::string::npos; at = text->find("@type", at + 1)) {
      n++;
    }
    return n;
  }()) << *text;
  EXPECT_NE(std::string::npos, text->find("\"traffic_classes\"")) << *text;
  auto again = schema->PipelineFromJson(*text);
  ASSERT_TRUE(again) << again.error();
  EXPECT_TRUE(gp::util::MessageDifferencer::Equals(*p, *again)) << *text;
}

}  // namespace
}  // namespace bessctl
