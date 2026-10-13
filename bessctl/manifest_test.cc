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

// A daemon's schema: module class "Fake" takes a Worker, has the action "go"
// (a Worker) and the resource tables "rules" (a map, Worker ->
// ApplyPipelineResponse) and "settings" (a singleton, a Connection); driver
// "FakePort" takes a Connection; "Opaque" declares nothing. Descriptors as
// GetSchema sends them.
pb::GetSchemaResponse FakeSchema() {
  pb::GetSchemaResponse s;
  auto *m = s.add_module_classes();
  m->set_name("Fake");
  m->set_init_arg_type("bess.pb.v2.Worker");
  auto *go = m->add_actions();
  go->set_name("go");
  go->set_arg_type("bess.pb.v2.Worker");
  auto *rules = m->add_resources();
  rules->set_table("rules");
  rules->set_kind(pb::TransactionResource::KIND_MAP);
  rules->set_key_type("bess.pb.v2.Worker");
  rules->set_value_type("bess.pb.v2.ApplyPipelineResponse");
  auto *settings = m->add_resources();
  settings->set_table("settings");
  settings->set_kind(pb::TransactionResource::KIND_SINGLETON);
  settings->set_key_type("google.protobuf.Empty");
  settings->set_value_type("bess.pb.v2.Connection");
  settings->set_requires_paused_workers(true);
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
  return schema.PipelineFrom(text, FormatOf("", text));
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

// A YAML manifest is the JSON one: the same pipeline, resources included,
// 64-bit numbers as written.
TEST(Manifest, YamlMeansWhatTheSameJsonMeans) {
  auto schema = MakeSchema();
  auto json = schema->PipelineFrom(R"({
    "modules": [{"name": "m", "mclass": "Fake", "arg": {"wid": 3}}],
    "traffic_classes": [{"name": "t", "limit": {"count": 9223372036854775807}}],
    "resources": {"m/rules": [{"key": {"wid": 2}, "value": {"generation": 9007199254740993}}],
                  "m/settings": {"upstream": "u"}}
  })", Format::kJson);
  ASSERT_TRUE(json) << json.error();
  auto yaml = schema->PipelineFrom(R"(# a comment
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
  m/settings: {upstream: u}
)", Format::kYaml);
  ASSERT_TRUE(yaml) << yaml.error();
  EXPECT_TRUE(gp::util::MessageDifferencer::Equals(*json, *yaml));
  EXPECT_EQ(2, yaml->resources_size());
  auto list = schema->PipelineFrom("- a\n", Format::kYaml);
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

// "resources" names each resource "<module>/<table>": its kind and types are
// the table's that the class of the manifest's module of that name declares.
// A map's entries are a list, a singleton is its value (an entry without a
// key); a map listed with no entries is listed (an apply clears it). File
// order is kept: the daemon sorts.
TEST(Manifest, ResourcesAreTypedByTheirModulesTable) {
  auto schema = MakeSchema();
  auto p = PipelineOf(*schema, R"({
    "modules": [{"name": "z", "mclass": "Fake"}, {"name": "a", "mclass": "Fake"}],
    "resources": {"z/rules": [{"key": {"wid": 2}, "value": {"generation": 9007199254740993}},
                              {"key": {}, "value": {}}],
                  "z/settings": {"upstream": "u", "ogate": 1},
                  "a/rules": []}
  })");
  ASSERT_TRUE(p) << p.error();
  ASSERT_EQ(3, p->resources_size());
  EXPECT_EQ("z/rules", p->resources(0).name());
  EXPECT_EQ("z/settings", p->resources(1).name());
  EXPECT_EQ("a/rules", p->resources(2).name());
  const pb::ResourceContents &rules = p->resources(0);
  ASSERT_EQ(2, rules.entries_size());
  pb::Worker key;
  ASSERT_TRUE(rules.entries(0).key().UnpackTo(&key)) << rules.entries(0).key().type_url();
  EXPECT_EQ(2, key.wid());
  pb::ApplyPipelineResponse value;
  ASSERT_TRUE(rules.entries(0).value().UnpackTo(&value));
  EXPECT_EQ(9007199254740993u, value.generation()) << "2^53 + 1, as written";
  EXPECT_TRUE(rules.entries(1).key().Is<pb::Worker>());
  EXPECT_TRUE(rules.entries(1).value().Is<pb::ApplyPipelineResponse>());
  const pb::ResourceContents &settings = p->resources(1);
  ASSERT_EQ(1, settings.entries_size());
  EXPECT_FALSE(settings.entries(0).has_key()) << "a singleton's entry has no key";
  pb::Connection c;
  ASSERT_TRUE(settings.entries(0).value().UnpackTo(&c));
  EXPECT_EQ("u", c.upstream());
  EXPECT_EQ(1u, c.ogate());
  EXPECT_EQ(0, p->resources(2).entries_size());

  // An explicit type is kept, as an arg's is.
  auto typed = PipelineOf(*schema, R"({"modules": [{"name": "m", "mclass": "Fake"}],
      "resources": {"m/settings": {"@type": "type.googleapis.com/bess.pb.v2.Worker", "wid": 4}}})");
  ASSERT_TRUE(typed) << typed.error();
  EXPECT_TRUE(typed->resources(0).entries(0).value().Is<pb::Worker>());
  // Without "resources" a manifest lists none: an apply leaves them alone.
  auto none = PipelineOf(*schema, R"({"modules": [{"name": "m", "mclass": "Fake"}]})");
  ASSERT_TRUE(none) << none.error();
  EXPECT_EQ(0, none->resources_size());
}

TEST(Manifest, BadResourcesAreNamed) {
  auto schema = MakeSchema();
  const std::string m = R"("modules": [{"name": "m", "mclass": "Fake"}, {"name": "o", "mclass": "Opaque"}], )";
  const std::pair<std::string, const char *> bad[] = {
      {R"({"resources": []})", "\"resources\" is not an object"},
      {R"({"resources": {"r": []}})", "resource \"r\": a resource is named \"<module>/<table>\""},
      {R"({"resources": {"x/rules": []}})", "resource \"x/rules\": no module \"x\" in the pipeline"},
      {"{" + m + R"("resources": {"m/nope": []}})",
       "resource \"m/nope\": module class Fake has no table \"nope\" (it has: rules, settings)"},
      {"{" + m + R"("resources": {"o/rules": []}})",
       "module class Opaque has no table \"rules\" (it has none)"},
      {"{" + m + R"("resources": {"m/rules": {}}})", "resource \"m/rules\": not a list of entries"},
      {"{" + m + R"("resources": {"m/settings": []}})", "resource \"m/settings\": a singleton"},
      {"{" + m + R"("resources": {"m/settings": 3}})", "resource \"m/settings\": a singleton"},
      {"{" + m + R"("resources": {"m/rules": [{"key": {}}]}})",
       "resource \"m/rules\" entry 0: needs exactly"},
      {"{" + m + R"("resources": {"m/rules": [{"key": {}, "value": 1}]}})", "entry 0: needs exactly"},
      {"{" + m + R"("resources": {"m/rules": [{"key": {}, "value": {}, "x": 1}]}})",
       "entry 0: needs exactly"},
      {"{" + m + R"("resources": {"m/rules": [{"key": {}, "value": {}}, 1]}})",
       "entry 1: needs exactly"},
  };
  for (const auto &[text, error] : bad) {
    auto p = PipelineOf(*schema, text);
    ASSERT_FALSE(p) << text;
    EXPECT_NE(std::string::npos, p.error().find(error)) << text << ": " << p.error();
  }
  // A key of the wrong shape for its type: protobuf's own error.
  EXPECT_FALSE(PipelineOf(*schema,
      "{" + m + R"("resources": {"m/rules": [{"key": {"wid": "x"}, "value": {}}]}})"));
}

// What `get` prints, `apply` reads back: resources keyed by name, a map's
// entries and a singleton's value without the table's "@type", a map with no
// entries as []. A resource bessctl cannot place in a table keeps the
// entries the daemon sent (the daemon would refuse it back).
TEST(Manifest, ResourcesReadBackAsTheyAreWritten) {
  auto schema = MakeSchema();
  auto p = PipelineOf(*schema, R"({
    "modules": [{"name": "m", "mclass": "Fake"}, {"name": "n", "mclass": "Fake"}],
    "resources": {"m/rules": [{"key": {"wid": 2}, "value": {"generation": 9007199254740993}}],
                  "m/settings": {"upstream": "u"},
                  "n/rules": [],
                  "n/settings": {}}
  })");
  ASSERT_TRUE(p) << p.error();
  for (const Format format : {Format::kJson, Format::kYaml}) {
    auto text = schema->PipelineTo(*p, format);
    ASSERT_TRUE(text) << text.error();
    EXPECT_EQ(std::string::npos, text->find("@type")) << *text;
    EXPECT_EQ(std::string::npos, text->find("entries")) << *text;
    auto again = PipelineOf(*schema, *text);
    ASSERT_TRUE(again) << again.error() << "\n" << *text;
    EXPECT_TRUE(gp::util::MessageDifferencer::Equals(*p, *again)) << *text;
  }
  auto text = schema->PipelineTo(*p, Format::kJson);
  ASSERT_TRUE(text) << text.error();
  auto doc = json::Parse(*text);
  ASSERT_TRUE(doc) << doc.error();
  const json::Value *resources = json::Find(*doc, "resources");
  ASSERT_NE(nullptr, resources) << *text;
  EXPECT_EQ(Compact(R"({"m/rules": [{"key": {"wid": 2}, "value": {"generation": "9007199254740993"}}],
                       "m/settings": {"upstream": "u"}, "n/rules": [], "n/settings": {}})"),
            json::Dump(*resources));

  pb::Pipeline gone;
  pb::ResourceEntry *entry = gone.add_resources()->add_entries();
  gone.mutable_resources(0)->set_name("gone/rules");
  pb::Worker w;
  w.set_wid(1);
  ASSERT_TRUE(entry->mutable_key()->PackFrom(w));
  ASSERT_TRUE(entry->mutable_value()->PackFrom(w));
  auto kept = schema->PipelineTo(gone, Format::kJson);
  ASSERT_TRUE(kept) << kept.error();
  EXPECT_NE(std::string::npos, kept->find("\"gone/rules\": [")) << *kept;
  EXPECT_NE(std::string::npos, kept->find("type.googleapis.com/bess.pb.v2.Worker")) << *kept;
}

// Actions (D-129) are what GetSchema lists for the class: an argument type
// each; a command that edits state, and get_initial_arg, are none.
TEST(Schema, ActionsAreTheClassesDeclaredOnes) {
  auto schema = MakeSchema();
  auto type = schema->ActionArgType("Fake", "go");
  ASSERT_TRUE(type) << type.error();
  EXPECT_EQ("bess.pb.v2.Worker", *type);
  const std::pair<std::pair<const char *, const char *>, const char *> bad[] = {
      {{"Fake", "add"}, "Fake has no action add (it has: go)"},
      {{"Fake", "get_initial_arg"}, "Fake has no action get_initial_arg"},
      {{"Opaque", "go"}, "Opaque has no action go (it has none)"},
      {{"Nope", "go"}, "the daemon has no module class Nope"},
  };
  for (const auto &[call, error] : bad) {
    auto t = schema->ActionArgType(call.first, call.second);
    ASSERT_FALSE(t) << call.first << " " << call.second;
    EXPECT_NE(std::string::npos, t.error().find(error)) << t.error();
  }
  // An argument as JSON or YAML text, a result back in either.
  for (const char *text : {R"({"wid": 5, "core": 2})", "{wid: 5, core: 2}"}) {
    auto arg = schema->MessageFromText(*type, text);
    ASSERT_TRUE(arg) << arg.error();
    pb::Worker w;
    ASSERT_TRUE(w.ParseFromString((*arg)->SerializeAsString()));
    EXPECT_EQ(5, w.wid());
    EXPECT_EQ(2, w.core());
  }
  EXPECT_FALSE(schema->MessageFromJson("bess.pb.v2.Worker", R"({"nope": 1})"));
  EXPECT_FALSE(schema->MessageFromJson("no.such.Type", "{}"));
  pb::Worker w;
  w.set_wid(7);
  gp::Any result;
  ASSERT_TRUE(result.PackFrom(w));
  auto json = schema->AnyTo(result, Format::kJson);
  ASSERT_TRUE(json) << json.error();
  EXPECT_NE(std::string::npos, json->find("\"wid\": 7")) << *json;
  auto yaml = schema->AnyTo(result, Format::kYaml);
  ASSERT_TRUE(yaml) << yaml.error();
  EXPECT_NE(std::string::npos, yaml->find("wid: 7")) << *yaml;
}

}  // namespace
}  // namespace bessctl
