// SPDX-License-Identifier: BSD-3-Clause

// The C++ bessctl against a live bessd (was bessctl/module_tests/bessctl_cpp.py):
// the manifest lifecycle (validate, plan, apply with resources, get, diff,
// generation, resource generation and epoch conflicts), resources converging
// to what a manifest lists, the Admin verbs, module actions, and every sample
// manifest.

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "live_daemon.h"

namespace {

namespace fs = std::filesystem;
namespace gp = google::protobuf;
using bess::live::Child;
using bess::live::Env;
using bess::live::LiveTest;
using bess::live::Output;
using bess::live::SourcePath;
using std::chrono::milliseconds;

constexpr std::string_view kManifest = R"({
  "modules": [
    {"name": "ctl_src", "mclass": "Source", "arg": {"pkt_size": 60}},
    {"name": "ctl_em", "mclass": "ExactMatch", "arg": {"fields": [{"offset": 26, "num_bytes": 4}]}},
    {"name": "ctl_sink", "mclass": "Sink"}
  ],
  "connections": [
    {"upstream": "ctl_src", "downstream": "ctl_em"},
    {"upstream": "ctl_em", "downstream": "ctl_sink"}
  ],
  "resources": {
    "ctl_em/rules": [
      {"key": {"fields": [{"value_bin": "CgAAAQ=="}]}, "value": {"gate": 1}},
      {"key": {"fields": [{"value_int": "167772162"}]}, "value": {"gate": "0"}}
    ],
    "ctl_em/settings": {"default_gate": 0}
  }
})";

gp::Struct Json(const std::string &text) {
  gp::Struct s;
  const auto status = gp::util::JsonStringToMessage(text, &s);
  EXPECT_TRUE(status.ok()) << status.message() << "\n" << text;
  return s;
}

std::string Dump(const gp::Struct &s) {
  std::string text;
  EXPECT_TRUE(gp::util::MessageToJsonString(s, &text).ok());
  return text;
}

// `key` is a C string: a std::string built at the call site would be a
// temporary bound to a reference parameter of a function that returns a
// reference, which g++ 14 flags (-Wdangling-reference) in range-for loops.
const gp::Value &Get(const gp::Struct &s, const char *key) {
  static const gp::Value missing;
  const auto it = s.fields().find(key);
  return it == s.fields().end() ? missing : it->second;
}

std::vector<std::string> ModuleNames(const gp::Struct &pipeline) {
  std::vector<std::string> names;
  for (const gp::Value &m : Get(pipeline, "modules").list_value().values()) {
    names.push_back(Get(m.struct_value(), "name").string_value());
  }
  std::sort(names.begin(), names.end());
  return names;
}

bool Contains(const std::string &haystack, std::string_view needle) {
  return haystack.find(needle) != std::string::npos;
}

// The listed resource `name` of a manifest `get` printed: a map's entries
// (a list) or a singleton's value (an object).
gp::Value *Resource(gp::Struct &manifest, const char *name) {
  return &(*(*manifest.mutable_fields())["resources"].mutable_struct_value()->mutable_fields())
      [name];
}

// A version as bessctl prints it, "G@E" or "G/R@E".
struct Version {
  uint64_t generation = 0;
  std::optional<uint64_t> resource_generation;
  uint64_t epoch = 0;
};

Version ParseVersion(const std::string &text) {
  Version v;
  const size_t at = text.find('@');
  const size_t slash = text.find('/');
  EXPECT_NE(std::string::npos, at) << text;
  v.generation = std::stoull(text.substr(0, std::min(slash, at)));
  if (slash < at) {
    v.resource_generation = std::stoull(text.substr(slash + 1, at - slash - 1));
  }
  v.epoch = std::stoull(text.substr(at + 1));
  return v;
}

// The version an `apply` printed: "version V: ...".
std::string AppliedVersion(const std::string &applied) {
  EXPECT_TRUE(applied.starts_with("version ")) << applied;
  return applied.substr(8, applied.find_first_of(": \n", 8) - 8);
}

class TempDir {
 public:
  TempDir() {
    char tmpl[] = "/tmp/bessctl-live-XXXXXX";
    path_ = mkdtemp(tmpl);
  }
  ~TempDir() { fs::remove_all(path_); }
  std::string Write(const std::string &name, std::string_view text) const {
    const std::string p = (path_ / name).string();
    std::ofstream(p) << text;
    return p;
  }

 private:
  fs::path path_;
};

class BessctlTest : public LiveTest {
 protected:
  // bessctl ARGS; fails the test unless it exits 0.
  Output Ok(const std::vector<std::string> &args) {
    Output o = daemon().Bessctl(args);
    EXPECT_EQ(0, o.status) << "bessctl " << ::testing::PrintToString(args) << ": " << o.err;
    return o;
  }
  std::vector<std::string> Modules() { return ModuleNames(Json(Ok({"get"}).out)); }
  // The active pipeline's resources, as `get` prints them.
  gp::Struct Resources() { return Get(Json(Ok({"get"}).out), "resources").struct_value(); }
  size_t EmRules() {
    return static_cast<size_t>(Get(Resources(), "ctl_em/rules").list_value().values_size());
  }
};

TEST_F(BessctlTest, ManifestLifecycle) {
  TempDir tmp;
  const std::string path = tmp.Write("pipeline.json", kManifest);

  const gp::Struct normalized = Json(Ok({"validate", path}).out);
  EXPECT_EQ((std::vector<std::string>{"ctl_em", "ctl_sink", "ctl_src"}), ModuleNames(normalized));
  EXPECT_EQ(2, Get(Get(normalized, "resources").struct_value(), "ctl_em/rules")
                   .list_value()
                   .values_size());
  const std::string plan = Ok({"plan", path}).out;
  EXPECT_TRUE(Contains(plan, "ctl_em/rules")) << plan;
  EXPECT_TRUE(Contains(plan, "RESOURCE")) << plan;
  // A resource whose module the same apply creates: not checked until then.
  const std::string first_diff = Ok({"diff", path}).out;
  EXPECT_TRUE(Contains(first_diff, "ctl_em/rules  +2 ~0 -0 (unchecked)")) << first_diff;

  const std::string applied = Ok({"apply", path}).out;
  std::string version = AppliedVersion(applied);
  const Version v = ParseVersion(version);
  ASSERT_TRUE(v.resource_generation) << "the manifest lists resources: " << applied;
  auto caps = client().Capabilities();
  ASSERT_TRUE(caps) << caps.error().message;
  EXPECT_EQ(caps->daemon_epoch(), v.epoch);
  EXPECT_TRUE(Contains(applied, " resource entr")) << applied;
  EXPECT_EQ(2u, EmRules());
  const std::string listed = Ok({"resources"}).out;
  EXPECT_TRUE(Contains(listed, "map bess.pb.ExactMatchRuleKey -> bess.pb.ExactMatchRuleValue"))
      << listed;
  EXPECT_TRUE(Contains(listed, "singleton bess.pb.ExactMatchSettings")) << listed;

  const Output got_output = Ok({"get"});
  EXPECT_TRUE(Contains(got_output.err, "# version " + version)) << got_output.err;
  const std::string &got_text = got_output.out;
  const gp::Struct got = Json(got_text);
  for (const gp::Value &m : Get(got, "modules").list_value().values()) {
    if (Get(m.struct_value(), "name").string_value() == "ctl_em") {
      const gp::Struct arg = Get(m.struct_value(), "arg").struct_value();
      EXPECT_EQ(0u, arg.fields().count("@type")) << "the implied type is left out";
      EXPECT_EQ(26, Get(Get(arg, "fields").list_value().values(0).struct_value(), "offset")
                        .number_value());
    }
  }
  EXPECT_FALSE(Contains(got_text, "@type")) << "every type is implied:\n" << got_text;
  const gp::Struct got_resources = Get(got, "resources").struct_value();
  EXPECT_TRUE(Get(got_resources, "ctl_em/settings").has_struct_value()) << got_text;

  // What `get` printed applies as a no-op, as JSON and as YAML (D-108), and
  // both validate to the same canonical form.
  const std::string again = tmp.Write("again.json", got_text);
  const std::string no_change = Ok({"diff", again}).out;
  EXPECT_TRUE(Contains(no_change, "0 change(s)")) << no_change;
  EXPECT_TRUE(Contains(no_change, "against version " + version)) << no_change;
  const std::string again_yaml = tmp.Write("again.yaml", Ok({"get", "-o", "yaml"}).out);
  EXPECT_TRUE(Contains(Ok({"diff", again_yaml}).out, "0 change(s)"));
  EXPECT_EQ(Ok({"validate", again_yaml}).out, Ok({"validate", again}).out);

  // Bad entries: nothing applies, all or nothing. Entry 1 is refused by
  // bessctl (no such field), then by the daemon (two fields for a one-field
  // key), naming the entry.
  const std::string good = R"({"key": {"fields": [{"value_bin": "CgAAAw=="}]}, "value": {"gate": 1}})";
  const std::pair<std::string, std::string> bad_entries[] = {
      {R"({"key": {"fields": [{"value_bin": "CgAAAw=="}]}, "value": {"nope": 1}})", "nope"},
      {R"({"key": {"fields": [{"value_int": 1}, {"value_int": 2}]}, "value": {}})",
       "resource 'ctl_em/rules' entry 1"},
  };
  for (const auto &[entry, error] : bad_entries) {
    gp::Struct bad = got;
    *Resource(bad, "ctl_em/rules") =
        Get(Json(std::format(R"({{"r": [{}, {}]}})", good, entry)), "r");
    const Output failed = daemon().Bessctl({"apply", tmp.Write("bad.json", Dump(bad))});
    EXPECT_EQ(1, failed.status) << failed.out;
    EXPECT_TRUE(Contains(failed.err, error)) << failed.err;
    EXPECT_EQ(2u, EmRules());
  }

  // Resource contents moved since a version: its G/R@E is refused, G@E (the
  // topology only) is not. A resources-only apply keeps the generation.
  gp::Struct fewer = got;
  Resource(fewer, "ctl_em/rules")->mutable_list_value()->mutable_values()->RemoveLast();
  const Version moved =
      ParseVersion(AppliedVersion(Ok({"apply", tmp.Write("fewer.json", Dump(fewer))}).out));
  EXPECT_EQ(v.generation, moved.generation);
  ASSERT_TRUE(moved.resource_generation);
  EXPECT_GT(*moved.resource_generation, *v.resource_generation);
  EXPECT_EQ(1u, EmRules());
  Output stale = daemon().Bessctl({"apply", again, "--generation", version});
  EXPECT_EQ(1, stale.status);
  EXPECT_TRUE(Contains(stale.err, "resource generation")) << stale.err;
  EXPECT_EQ(1u, EmRules());
  Ok({"apply", again, "--generation", std::format("{}@{}", v.generation, v.epoch)});
  EXPECT_EQ(2u, EmRules());

  // A generation that is no longer active is refused, nothing changes.
  // Desired state is the whole pipeline: cut down what `get` printed (its
  // resources too: those of a removed module cannot be listed).
  gp::Struct reduced = got;
  reduced.mutable_fields()->erase("resources");
  auto *modules = (*reduced.mutable_fields())["modules"].mutable_list_value()->mutable_values();
  modules->erase(std::remove_if(modules->begin(), modules->end(),
                                [](const gp::Value &m) {
                                  const std::string n = Get(m.struct_value(), "name").string_value();
                                  return n == "ctl_em" || n == "ctl_sink";
                                }),
                 modules->end());
  auto *connections =
      (*reduced.mutable_fields())["connections"].mutable_list_value()->mutable_values();
  connections->erase(std::remove_if(connections->begin(), connections->end(),
                                    [](const gp::Value &c) {
                                      const gp::Struct &s = c.struct_value();
                                      return Get(s, "upstream").string_value() == "ctl_em" ||
                                             Get(s, "downstream").string_value() == "ctl_em";
                                    }),
                     connections->end());
  Ok({"apply", tmp.Write("pipeline.json", Dump(reduced))});
  stale = daemon().Bessctl({"apply", again, "--generation", std::format("{}@{}", v.generation,
                                                                         v.epoch)});
  EXPECT_EQ(1, stale.status);
  EXPECT_TRUE(Contains(stale.err, "conflict")) << stale.err;
  auto names = Modules();
  EXPECT_EQ(names.end(), std::find(names.begin(), names.end(), "ctl_em"));
  // The same generation of another daemon epoch: refused for the epoch,
  // before the generation is compared (D-106).
  stale = daemon().Bessctl(
      {"apply", again, "--generation", std::format("{}@{}", v.generation, v.epoch + 1)});
  EXPECT_EQ(1, stale.status);
  EXPECT_TRUE(Contains(stale.err, "restarted")) << stale.err;
  names = Modules();
  EXPECT_EQ(names.end(), std::find(names.begin(), names.end(), "ctl_em"));
}

// Module state is resources (D-129): `get` prints them, a manifest listing
// one makes it exactly what it lists ([] clears a map), and one it leaves out
// is left alone.
TEST_F(BessctlTest, ResourcesConverge) {
  TempDir tmp;
  const std::string sample = SourcePath("bessctl/conf/samples/l2forward.yaml");
  Ok({"apply", sample});
  gp::Struct got = Json(Ok({"get"}).out);
  EXPECT_EQ(4, Resource(got, "fib/macs")->list_value().values_size());
  ASSERT_TRUE(Resource(got, "fib/settings")->has_struct_value());
  const std::string listed = Ok({"resources"}).out;
  EXPECT_TRUE(Contains(listed, "map bess.pb.L2ForwardMacKey -> bess.pb.L2ForwardMacValue"))
      << listed;
  EXPECT_TRUE(Contains(listed, "singleton bess.pb.L2ForwardSettings")) << listed;

  gp::Struct changed = got;
  *Resource(changed, "fib/macs") = Get(Json(R"({"v": []})"), "v");
  *Resource(changed, "fib/settings") = Get(Json(R"({"v": {"default_gate": 2}})"), "v");
  const std::string path = tmp.Write("changed.json", Dump(changed));
  const std::string diff = Ok({"diff", path}).out;
  EXPECT_TRUE(Contains(diff, "fib/macs  +0 ~0 -4")) << diff;
  EXPECT_TRUE(Contains(diff, " fib/settings")) << diff;
  const size_t against = diff.find("against version ");
  ASSERT_NE(std::string::npos, against) << diff;
  EXPECT_TRUE(ParseVersion(diff.substr(against + 16, diff.find(':', against) - against - 16))
                  .resource_generation)
      << "the version guards the listed resources: " << diff;
  const std::string plan = Ok({"plan", path}).out;
  EXPECT_TRUE(Contains(plan, "fib/macs")) << plan;
  Ok({"apply", path});
  EXPECT_TRUE(Contains(Ok({"diff", path}).out, "0 change(s)"));
  gp::Struct after = Json(Ok({"get"}).out);
  EXPECT_EQ(0, Resource(after, "fib/macs")->list_value().values_size());
  EXPECT_EQ("2", Get(Resource(after, "fib/settings")->struct_value(), "default_gate")
                     .string_value());

  // The sample lists fib/macs only: its entries come back, the default gate
  // stays.
  Ok({"apply", sample});
  after = Json(Ok({"get"}).out);
  EXPECT_EQ(4, Resource(after, "fib/macs")->list_value().values_size());
  EXPECT_EQ("2", Get(Resource(after, "fib/settings")->struct_value(), "default_gate")
                     .string_value());

  // A singleton takes its value, not a list.
  *Resource(changed, "fib/settings") = Get(Json(R"({"v": []})"), "v");
  const Output bad = daemon().Bessctl({"validate", tmp.Write("bad.json", Dump(changed))});
  EXPECT_EQ(1, bad.status);
  EXPECT_TRUE(Contains(bad.err, "resource \"fib/settings\": a singleton")) << bad.err;
}

TEST_F(BessctlTest, SchemaStatusAndUsage) {
  const std::string schema = Ok({"schema", "ExactMatch"}).out;
  EXPECT_TRUE(Contains(schema, "bess.pb.ExactMatchArg")) << schema;
  EXPECT_TRUE(Contains(schema, "map bess.pb.ExactMatchRuleKey -> bess.pb.ExactMatchRuleValue"))
      << schema;
  EXPECT_TRUE(Contains(schema, "singleton bess.pb.ExactMatchSettings")) << schema;
  EXPECT_FALSE(Contains(schema, "\n  action ")) << "its commands edit state: " << schema;
  const std::string l2 = Ok({"schema", "L2Forward"}).out;
  EXPECT_TRUE(Contains(l2, "bess.pb.L2ForwardCommandLookupArg -> "
                           "bess.pb.L2ForwardCommandLookupResponse  (no pause)"))
      << l2;
  EXPECT_FALSE(Contains(l2, "set_default_gate")) << l2;
  EXPECT_TRUE(Contains(Ok({"schema", "ArpResponder"}).out, "  (paused)"));
  Ok({"status"});
  Ok({"metrics"});
  EXPECT_EQ(2, daemon().Bessctl({"nonsense"}).status);
  EXPECT_EQ(2, daemon().Bessctl({"command", "m", "c"}).status) << "no verb `command`";
  EXPECT_EQ(2, daemon().Bessctl({"events", "--from", "x"}).status);
  // A generation alone is ambiguous; a resource generation needs a number.
  for (const char *bad : {"-1", "5", "5@0", "5@x", "5/@1", "5/x@1", "/3@1"}) {
    EXPECT_EQ(2, daemon().Bessctl({"apply", "f", "--generation", bad}).status) << bad;
  }
}

// `events --from 1` replays what the daemon holds at once, then follows;
// Ctrl-C ends it cleanly. A bess.gap comes first when sequence 1 is no
// longer held.
TEST_F(BessctlTest, EventsReplayAndInterrupt) {
  TempDir tmp;
  Ok({"apply", tmp.Write("pipeline.json", kManifest)});  // a transaction to replay
  Child watch({Env("BESSCTL"), "-s", daemon().address(), "events", "--from", "1",
               "bess.transaction"});
  auto first = watch.ReadLine(milliseconds(10000));
  ASSERT_TRUE(first) << "events --from 1 printed nothing";
  if (Contains(*first, " bess.gap")) {
    first = watch.ReadLine(milliseconds(10000));
    ASSERT_TRUE(first);
  }
  EXPECT_TRUE(Contains(*first, " bess.transaction")) << *first;
  EXPECT_TRUE(Contains(*first, "outcome=applied")) << *first;
  const Output stopped = watch.Stop(SIGINT);
  EXPECT_EQ(0, stopped.status) << stopped.err;
}

// The Admin service through bessctl (D-107): pools, ports, a refused plugin,
// a capture that streams pcap and is stopped on Ctrl-C, pause and resume.
TEST_F(BessctlTest, Admin) {
  EXPECT_TRUE(Contains(Ok({"pools"}).out, "socket 0: "));

  TempDir tmp;
  const std::string pipeline = std::format(R"({{
    "ports": [{{"name": "ctl_p0", "driver": "UnixSocketPort", "arg": {{"path": "@bessctl_live_{}"}}}}],
    "modules": [{{"name": "cap_a", "mclass": "Bypass"}}, {{"name": "cap_b", "mclass": "Bypass"}}],
    "connections": [{{"upstream": "cap_a", "downstream": "cap_b"}}]
  }})",
                                           getpid());
  Ok({"apply", tmp.Write("admin.json", pipeline)});
  const std::string ports = Ok({"ports", "ctl_p0"}).out;
  EXPECT_TRUE(ports.starts_with("ctl_p0 (UnixSocketPort): link ")) << ports;
  EXPECT_TRUE(Contains(ports, "rx  packets 0")) << ports;
  const Output missing = daemon().Bessctl({"ports", "ctl_nosuch"});
  EXPECT_EQ(1, missing.status);
  EXPECT_TRUE(Contains(missing.err, "ctl_nosuch"));

  EXPECT_EQ(1, daemon().Bessctl({"plugin", "import", "/nonexistent/plugin.so"}).status);
  EXPECT_EQ(2, daemon().Bessctl({"plugin", "load", "x"}).status);

  for (int i = 0; i < 2; i++) {  // a stopped capture can start again
    Child cap({Env("BESSCTL"), "-s", daemon().address(), "capture", "cap_a", "out:0"});
    std::string header;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (header.size() < 4 && std::chrono::steady_clock::now() < deadline) {
      header += cap.Read(24 - header.size(), milliseconds(1000));
    }
    ASSERT_GE(header.size(), 4u) << "capture printed nothing";
    EXPECT_EQ(std::string("\xd4\xc3\xb2\xa1", 4), header.substr(0, 4)) << "a libpcap header";
    const Output stopped = cap.Stop(SIGINT);
    EXPECT_EQ(0, stopped.status) << stopped.err;
  }
  EXPECT_EQ(2, daemon().Bessctl({"capture", "cap_a", "sideways"}).status);

  Ok({"resume"});
  Ok({"pause"});
}

// Module actions (D-129): the argument as JSON or YAML text (a flow mapping
// starts with "{" too), the result printed; a command that edits state is no
// action (the module's resources hold that state), and an action not marked
// thread-safe is refused while a worker runs the module.
TEST_F(BessctlTest, Action) {
  Ok({"apply", SourcePath("bessctl/conf/samples/l2forward.yaml")});
  const gp::Struct found =
      Json(Ok({"action", "fib", "lookup", R"({addrs: ["00:00:00:00:00:02"]})"}).out);
  ASSERT_EQ(1, Get(found, "gates").list_value().values_size());
  EXPECT_EQ("2", Get(found, "gates").list_value().values(0).string_value());
  EXPECT_TRUE(Contains(
      Ok({"action", "fib", "lookup", R"({"addrs": ["00:00:00:00:00:03"]})", "-o", "yaml"}).out,
      "\"3\""));
  for (const char *legacy : {"add", "set_default_gate", "get_initial_arg"}) {
    const Output refused = daemon().Bessctl({"action", "fib", legacy});
    EXPECT_EQ(1, refused.status) << legacy;
    EXPECT_TRUE(Contains(refused.err, "it has: lookup, populate")) << refused.err;
  }
  EXPECT_EQ(1, daemon().Bessctl({"action", "nosuch", "lookup"}).status);
  EXPECT_EQ(2, daemon().Bessctl({"action", "fib"}).status);

  TempDir tmp;
  Ok({"apply", tmp.Write("bridge.yaml", R"(modules:
  - {name: src, mclass: Source}
  - {name: br, mclass: Bridge, arg: {size: 1024, aging_time: 300}}
  - {name: sink, mclass: Sink}
connections:
  - {upstream: src, downstream: br}
  - {upstream: br, downstream: sink}
workers:
  - {wid: 0}
tasks:
  - {module: src, wid: 0}
)")});
  Ok({"resume"});
  const Output busy = daemon().Bessctl({"action", "br", "flush"});
  EXPECT_EQ(1, busy.status);
  EXPECT_TRUE(Contains(busy.err, "is not MT safe")) << busy.err;
  Ok({"pause"});
  Ok({"action", "br", "flush"});
}

// Every sample manifest (conf/samples, D-104): applies on a reset daemon,
// converges (diff reports nothing left), and runs briefly.
TEST_F(BessctlTest, Samples) {
  std::vector<std::string> paths;
  for (const auto &e : fs::recursive_directory_iterator(SourcePath("bessctl/conf/samples"))) {
    if (e.path().extension() == ".yaml") {
      paths.push_back(e.path().string());
    }
  }
  std::sort(paths.begin(), paths.end());
  ASSERT_FALSE(paths.empty());
  for (const std::string &path : paths) {
    SCOPED_TRACE(path);
    daemon().Reset();
    Ok({"apply", path});
    EXPECT_TRUE(Contains(Ok({"diff", path}).out, "0 change(s)"));
    Ok({"resume"});
    std::this_thread::sleep_for(milliseconds(200));
    Ok({"pause"});
    Ok({"status"});
  }
}

}  // namespace
