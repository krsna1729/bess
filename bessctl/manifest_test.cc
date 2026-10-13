// SPDX-License-Identifier: BSD-3-Clause

// bessctl's manifests: the JSON tree keeps numbers exactly, YAML reads into
// and writes from the same tree, and a manifest's args get their type from
// the schema. control_v2's own messages stand in for
// module and driver argument types, so no module code is linked.

#include "manifest.h"

#include <google/protobuf/descriptor.pb.h>
#include <google/protobuf/util/message_differencer.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <functional>
#include <limits>
#include <set>
#include <string>

#include "json.h"
#include "yaml_io.h"

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

// -- YAML ---------------------------------------------------------------------

// The JSON a YAML document reads as, compact.
std::string YamlAsJson(std::string_view text) {
  auto v = yaml::Parse(text);
  return v ? json::Dump(*v) : "error: " + v.error();
}

std::string Compact(std::string_view json_text) {
  auto v = json::Parse(json_text);
  EXPECT_TRUE(v) << v.error();
  return v ? json::Dump(*v) : "";
}

TEST(Yaml, PlainScalarsResolveAsTheCoreSchemaDoes) {
  EXPECT_EQ(Compact(R"({"u64": 18446744073709551615, "neg": -9223372036854775808,
      "plus": 7, "lead": 7, "half": 0.5, "dot": 1, "exp": -1.5e+300, "hex": 31,
      "oct": 15, "hexmax": 18446744073709551615, "t": true, "f": false, "n1": null,
      "n2": null, "n3": null, "inf": "Infinity", "ninf": "-Infinity", "nan": "NaN",
      "qnum": "123", "qhex": "0x1F", "qtrue": "true", "tagged": "42", "under": "1_000",
      "ip": "10.0.0.1", "yes": "yes", "word": "Bypass", "block": "two\nlines\n",
      "folded": "one line\n", "flow": [1, "a", {"k": null}]})"),
            YamlAsJson(R"(
u64: 18446744073709551615
neg: -9223372036854775808
plus: +7
lead: 007
half: .5
dot: 1.
exp: -1.5e+300
hex: 0x1F
oct: 0o17
hexmax: 0xFFFFFFFFFFFFFFFF
t: True
f: FALSE
n1: ~
n2:
n3: Null
inf: .inf
ninf: -.Inf
nan: .NaN
qnum: "123"
qhex: '0x1F'
qtrue: "true"
tagged: !!str 42
under: 1_000
ip: 10.0.0.1
yes: yes
word: Bypass
block: |
  two
  lines
folded: >
  one
  line
flow: [1, a, {k: }]
)"));
  EXPECT_EQ("null", YamlAsJson(""));
  EXPECT_EQ("\"x\"", YamlAsJson("--- x\n...\n"));
}

TEST(Yaml, AliasesAreCopiesWithinABudget) {
  EXPECT_EQ(Compact(R"({"a": {"k": [1, 2]}, "b": {"k": [1, 2]}, "c": [1, 2]})"),
            YamlAsJson("a: &x {k: &y [1, 2]}\nb: *x\nc: *y\n"));
  // Ten levels of ten aliases: 10^10 values.
  std::string bomb = "l0: &l0 [x, x, x, x, x, x, x, x, x, x]\n";
  for (int i = 1; i <= 9; i++) {
    bomb += std::format("l{}: &l{} [", i, i);
    for (int j = 0; j < 10; j++) {
      bomb += std::format("{}*l{}", j == 0 ? "" : ", ", i - 1);
    }
    bomb += "]\n";
  }
  EXPECT_NE(std::string::npos, YamlAsJson(bomb).find("expands beyond 256 MiB")) << YamlAsJson(bomb);
  // Few values, long text: 5,000 copies of a 64 KiB string.
  std::string wide = std::format("s: &s \"{}\"\nl: [", std::string(65536, 'x'));
  for (int i = 0; i < 5000; i++) {
    wide += i == 0 ? "*s" : ", *s";
  }
  wide += "]\n";
  EXPECT_NE(std::string::npos, YamlAsJson(wide).find("expands beyond")) << YamlAsJson(wide).substr(0, 200);
  // No alias at all: 200 nested anchors each keep a copy of 400,000 values.
  std::string nested;
  for (int i = 0; i < 200; i++) {
    nested += std::format("&a{} [", i);
  }
  for (int i = 0; i < 400000; i++) {
    nested += i == 0 ? "x" : ",x";
  }
  nested += std::string(200, ']');
  EXPECT_NE(std::string::npos, YamlAsJson(nested).find("expands beyond")) << YamlAsJson(nested).substr(0, 200);
  // An alias's subtree counts toward the depth: 200 levels placed at 100.
  const std::string tall = "a: &t " + std::string(200, '[') + std::string(200, ']') + "\nb: " +
                           std::string(100, '[') + "*t" + std::string(100, ']') + "\n";
  EXPECT_NE(std::string::npos, YamlAsJson(tall).find("nested deeper than")) << YamlAsJson(tall);
  const std::string fits = "a: &t " + std::string(100, '[') + std::string(100, ']') + "\nb: " +
                           std::string(100, '[') + "*t" + std::string(100, ']') + "\n";
  EXPECT_EQ(0u, YamlAsJson(fits).find("{")) << YamlAsJson(fits);
  EXPECT_NE(std::string::npos, YamlAsJson("a: &x [1, *x]\n").find("names no complete node"));
  EXPECT_NE(std::string::npos, YamlAsJson("a: *nope\n").find("names no complete node"));
}

TEST(Yaml, RefusedInputIsNamedWithItsPosition) {
  const std::pair<const char *, const char *> bad[] = {
      {"a: !!int 3\n", "line 1, column 4: tag tag:yaml.org,2002:int is not supported"},
      {"a: !custom {b: 1}\n", "line 1, column 4: tag !custom is not supported"},
      {"base: &b {x: 1}\nc:\n  <<: *b\n", "line 3, column 3: merge keys (<<) are not supported"},
      {"a: 1\nb: 2\na: 3\n", "line 3, column 1: duplicate key \"a\""},
      {"? [k]\n: 1\n", "line 1, column 3: a key must be a scalar"},
      {"a: 1\n---\nb: 2\n", "line 2, column 1: a manifest is one YAML document"},
      {"a: 0x10000000000000000\n", "line 1, column 4: 0x10000000000000000 does not fit in 64 bits"},
      {"a: [1\n", "line 2, column 1:"},
      {"a: \"x\n", "line"},
  };
  for (const auto &[text, error] : bad) {
    const std::string got = YamlAsJson(text);
    EXPECT_EQ(0u, got.find(std::string("error: ") + error)) << text << " -> " << got;
  }
  std::string deep;
  for (int i = 0; i <= json::kMaxDepth + 1; i++) {
    deep += "[";
  }
  EXPECT_NE(std::string::npos, YamlAsJson(deep).find("nested deeper than"));
}

// What Dump writes, Parse reads back as the same tree: strings that look
// like other scalars, or hold YAML's indicators, are quoted.
TEST(Yaml, DumpReadsBackAsTheSameTree) {
  const char *const docs[] = {
      R"({"strings": ["", "true", "yes", "No", "null", "~", "123", "-1", "0x1F", ".inf", "1_000",
         "@type", "a: b", "a #b", "-x", "- x", "[x]", "{x}", "*x", "&x", "!x", "%x", "|", ">",
         "'q'", "\"dq\"", "line\nbreak", "tab\there", "\u007f", "\u0001", " lead", "trail ",
         "ünïcödé", "a\u0085b", "a \u2028b", "a\u2029 b", "a\u0080b", "\u009f", "\ufeff",
         "\ufffe\uffff", "€\u00a0", "type.googleapis.com/bess.pb.v2.Connection", "10.0.0.1", "Bypass"],
         "@type": "x", "a: b": 1, "": 2, "123": 3,
         "numbers": [18446744073709551615, -0.5e-10, 0],
         "empty": {"o": {}, "a": [], "n": null, "f": false},
         "nested": [[1, [2, []]], [{"a": [{"b": {}}]}], {"x": [[{"y": 1}]]}]})",
      "[]",
      "{}",
      "\"top\"",
      "42",
      "[[1], {\"a\": 1}]",
  };
  for (const char *doc : docs) {
    auto v = json::Parse(doc);
    ASSERT_TRUE(v) << v.error();
    const std::string text = yaml::Dump(*v);
    auto back = yaml::Parse(text);
    ASSERT_TRUE(back) << back.error() << "\n" << text;
    EXPECT_EQ(json::Dump(*v), json::Dump(*back)) << text;
  }
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

std::expected<pb::Pipeline, std::string> PipelineOf(const Schema &schema, std::string_view text) {
  auto m = schema.ManifestFrom(text, FormatOf("", text));
  if (!m) {
    return std::unexpected(m.error());
  }
  return std::move(m->pipeline);
}

TEST(Manifest, AnArgWithoutTypeGetsItsClassesInitType) {
  auto schema = MakeSchema();
  auto p = PipelineOf(*schema, R"({
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

// D-123: a worker the manifest gives no core is automatic -- the field stays
// unset, so the daemon picks the CPU -- and an explicit core 0 stays
// explicit, in the manifest read and in the one written back.
TEST(Manifest, AWorkerWithoutCoreLeavesItUnsetAndCoreZeroStaysExplicit) {
  auto schema = MakeSchema();
  auto p = PipelineOf(*schema, "workers:\n  - {wid: 0}\n  - {wid: 1, core: 0}\n");
  ASSERT_TRUE(p) << p.error();
  ASSERT_EQ(2, p->workers_size());
  EXPECT_FALSE(p->workers(0).has_core());
  ASSERT_TRUE(p->workers(1).has_core());
  EXPECT_EQ(0, p->workers(1).core());

  auto text = schema->PipelineTo(*p, Format::kJson);
  ASSERT_TRUE(text) << text.error();
  auto back = PipelineOf(*schema, *text);
  ASSERT_TRUE(back) << back.error() << "\n" << *text;
  ASSERT_EQ(2, back->workers_size());
  EXPECT_FALSE(back->workers(0).has_core()) << *text;
  ASSERT_TRUE(back->workers(1).has_core()) << *text;
  EXPECT_EQ(0, back->workers(1).core());
}

TEST(Manifest, AnExplicitTypeIsKeptEvenIfItDiffers) {
  auto schema = MakeSchema();
  auto p = PipelineOf(*schema, R"({"modules": [{"name": "m", "mclass": "Fake",
      "arg": {"@type": "type.googleapis.com/bess.pb.v2.Connection", "upstream": "x"}}]})");
  ASSERT_TRUE(p) << p.error();
  EXPECT_TRUE(p->modules(0).arg().Is<pb::Connection>());
}

TEST(Manifest, SixtyFourBitValuesAreNotRounded) {
  auto schema = MakeSchema();
  auto p = PipelineOf(*schema,
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
      {R"([])", "a manifest is an object"},
  };
  for (const auto &[text, error] : cases) {
    auto p = PipelineOf(*schema, text);
    ASSERT_FALSE(p) << text;
    EXPECT_NE(std::string::npos, p.error().find(error)) << text << ": " << p.error();
  }
  // A value of the wrong type for the class's argument: protobuf's own error.
  EXPECT_FALSE(PipelineOf(*schema,
      R"({"modules": [{"name": "m", "mclass": "Fake", "arg": {"wid": "x"}}]})"));
  // An untyped class without an arg is fine: nothing to type.
  EXPECT_TRUE(PipelineOf(*schema, R"({"modules": [{"name": "m", "mclass": "Opaque"}]})"));
}

TEST(Manifest, WhatGetPrintsApplyReadsBack) {
  auto schema = MakeSchema();
  auto p = PipelineOf(*schema, R"({
    "modules": [{"name": "m", "mclass": "Fake", "arg": {"wid": 3}},
                {"name": "n", "mclass": "Fake",
                 "arg": {"@type": "type.googleapis.com/bess.pb.v2.Connection", "upstream": "x"}}],
    "connections": [{"upstream": "m", "ogate": 1, "downstream": "n"}],
    "traffic_classes": [{"name": "t", "limit": {"count": 9223372036854775807}}]
  })");
  ASSERT_TRUE(p) << p.error();
  for (const Format format : {Format::kJson, Format::kYaml}) {
    auto text = schema->PipelineTo(*p, format);
    ASSERT_TRUE(text) << text.error();
    // The implied type is left out, a different one kept; names as in the .proto.
    EXPECT_EQ(1u, [&] {
      size_t n = 0;
      for (size_t at = text->find("@type"); at != std::string::npos; at = text->find("@type", at + 1)) {
        n++;
      }
      return n;
    }()) << *text;
    EXPECT_NE(std::string::npos, text->find("traffic_classes")) << *text;
    EXPECT_EQ(format, FormatOf("-", *text)) << *text;
    auto again = PipelineOf(*schema, *text);
    ASSERT_TRUE(again) << again.error();
    EXPECT_TRUE(gp::util::MessageDifferencer::Equals(*p, *again)) << *text;
  }
}

// A YAML manifest is the JSON one: the same pipeline and resource entries,
// 64-bit numbers as written.
TEST(Manifest, YamlMeansWhatTheSameJsonMeans) {
  auto schema = MakeSchema();
  auto json = schema->ManifestFrom(R"({
    "modules": [{"name": "m", "mclass": "Fake", "arg": {"wid": 3}}],
    "traffic_classes": [{"name": "t", "limit": {"count": 9223372036854775807}}],
    "resources": {"m/rules": [{"key": {"wid": 2}, "value": {"generation": 9007199254740993}}]}
  })", Format::kJson);
  ASSERT_TRUE(json) << json.error();
  auto yaml = schema->ManifestFrom(R"(# a comment
modules:
  - name: m
    mclass: Fake
    arg: {wid: 3}
traffic_classes:
  - name: t
    limit:
      count: 9223372036854775807
resources:
  m/rules:
    - key: {wid: 2}
      value: {generation: 9007199254740993}
)", Format::kYaml);
  ASSERT_TRUE(yaml) << yaml.error();
  EXPECT_TRUE(gp::util::MessageDifferencer::Equals(json->pipeline, yaml->pipeline));
  ASSERT_EQ(1u, yaml->resources.size());
  EXPECT_EQ(json->resources[0].first, yaml->resources[0].first);
  EXPECT_EQ(json->resources[0].second[0].key, yaml->resources[0].second[0].key);
  EXPECT_EQ(json->resources[0].second[0].value, yaml->resources[0].second[0].value);
  auto list = schema->ManifestFrom("- a\n", Format::kYaml);
  ASSERT_FALSE(list);
  EXPECT_NE(std::string::npos, list.error().find("a manifest is an object")) << list.error();
}

TEST(Manifest, FormatIsTheExtensionElseTheFirstCharacter) {
  EXPECT_EQ(Format::kJson, FormatOf("p.json", "modules: []"));
  EXPECT_EQ(Format::kYaml, FormatOf("p.yaml", "{}"));
  EXPECT_EQ(Format::kYaml, FormatOf("p.yml", "{}"));
  EXPECT_EQ(Format::kJson, FormatOf("-", " \n\t{\"modules\": []}"));
  EXPECT_EQ(Format::kYaml, FormatOf("-", "modules: []"));
  EXPECT_EQ(Format::kYaml, FormatOf("p.txt", ""));
}

TEST(Manifest, ResourceEntriesAreTakenOutInFileOrderAndTypedLater) {
  auto schema = MakeSchema();
  auto m = schema->ManifestFrom(R"({
    "modules": [{"name": "m", "mclass": "Fake", "arg": {"wid": 1}}],
    "resources": {"z/rules": [{"key": {"wid": 2}, "value": {"generation": 9007199254740993}}],
                  "a/rules": [{"key": {"wid": 3}, "value": {}}, {"key": {}, "value": {}}]}
  })", Format::kJson);
  ASSERT_TRUE(m) << m.error();
  EXPECT_EQ(1, m->pipeline.modules_size());
  ASSERT_EQ(2u, m->resources.size());
  EXPECT_EQ("z/rules", m->resources[0].first);
  EXPECT_EQ("a/rules", m->resources[1].first);
  EXPECT_EQ(3u, m->entries());
  // An entry becomes the resource's types once the daemon names them; its
  // numbers are as written (2^53 + 1 survives).
  auto value = schema->MessageFromJson("bess.pb.v2.ApplyPipelineResponse",
                                       m->resources[0].second[0].value);
  ASSERT_TRUE(value) << value.error();
  pb::ApplyPipelineResponse r;
  ASSERT_TRUE(r.ParseFromString((*value)->SerializeAsString()));
  EXPECT_EQ(9007199254740993u, r.generation());
  EXPECT_FALSE(schema->MessageFromJson("bess.pb.v2.Worker", R"({"nope": 1})"));
  EXPECT_FALSE(schema->MessageFromJson("no.such.Type", "{}"));

  const std::pair<const char *, const char *> bad[] = {
      {R"({"resources": []})", "\"resources\" is not an object"},
      {R"({"resources": {"r": {}}})", "resource \"r\": not a list"},
      {R"({"resources": {"r": [{"key": {}}]}})", "resource \"r\" entry 0: needs exactly"},
      {R"({"resources": {"r": [{"key": {}, "value": 1}]}})", "entry 0: needs exactly"},
      {R"({"resources": {"r": [{"key": {}, "value": {}, "x": 1}]}})", "entry 0: needs exactly"},
      {R"({"resources": {"r": [1]}})", "entry 0: needs exactly"},
  };
  for (const auto &[text, error] : bad) {
    auto r = schema->ManifestFrom(text, Format::kJson);
    ASSERT_FALSE(r) << text;
    EXPECT_NE(std::string::npos, r.error().find(error)) << text << ": " << r.error();
  }
}

}  // namespace
}  // namespace bessctl
