// SPDX-License-Identifier: BSD-3-Clause

// The C++ bessctl against a live bessd (was bessctl/module_tests/bessctl_cpp.py):
// the manifest lifecycle (validate, plan, apply with resource entries, get,
// diff, generation and epoch conflicts), the Admin verbs, module commands,
// and every sample manifest.

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
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
    ]
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
  size_t EmRules() {
    const gp::Struct config = Json(Ok({"command", "ctl_em", "get_runtime_config"}).out);
    return static_cast<size_t>(Get(config, "rules").list_value().values_size());
  }
};

TEST_F(BessctlTest, ManifestLifecycle) {
  TempDir tmp;
  const std::string path = tmp.Write("pipeline.json", kManifest);

  const gp::Struct normalized = Json(Ok({"validate", path}).out);
  EXPECT_EQ((std::vector<std::string>{"ctl_em", "ctl_sink", "ctl_src"}), ModuleNames(normalized));
  EXPECT_TRUE(Contains(Ok({"plan", path}).out, "ctl_em"));

  const std::string applied = Ok({"apply", path}).out;
  ASSERT_TRUE(applied.starts_with("version ")) << applied;
  std::string version = applied.substr(8, applied.find_first_of(": \n", 8) - 8);
  const size_t at = version.find('@');
  ASSERT_NE(std::string::npos, at) << version;
  const uint64_t generation = std::stoull(version.substr(0, at));
  const uint64_t epoch = std::stoull(version.substr(at + 1));
  auto caps = client().Capabilities();
  ASSERT_TRUE(caps) << caps.error().message;
  EXPECT_EQ(caps->daemon_epoch(), epoch);
  EXPECT_TRUE(Contains(applied, "resources: 2 entries in 1 resource(s)")) << applied;
  EXPECT_EQ(2u, EmRules());

  const std::string got_text = Ok({"get"}).out;
  const gp::Struct got = Json(got_text);
  for (const gp::Value &m : Get(got, "modules").list_value().values()) {
    if (Get(m.struct_value(), "name").string_value() == "ctl_em") {
      const gp::Struct arg = Get(m.struct_value(), "arg").struct_value();
      EXPECT_EQ(0u, arg.fields().count("@type")) << "the implied type is left out";
      EXPECT_EQ(26, Get(Get(arg, "fields").list_value().values(0).struct_value(), "offset")
                        .number_value());
    }
  }

  // What `get` printed applies as a no-op, as JSON and as YAML (D-108), and
  // both validate to the same canonical form.
  const std::string again = tmp.Write("again.json", got_text);
  EXPECT_TRUE(Contains(Ok({"diff", again}).out, "0 change(s)"));
  const std::string again_yaml = tmp.Write("again.yaml", Ok({"get", "-o", "yaml"}).out);
  EXPECT_TRUE(Contains(Ok({"diff", again_yaml}).out, "0 change(s)"));
  EXPECT_EQ(Ok({"validate", again_yaml}).out, Ok({"validate", again}).out);

  // Bad entries: the pipeline (unchanged) applies, the entry is named, and
  // nothing of the transaction is committed. Entry 1 is refused by bessctl
  // (no such field), then by the daemon (two fields for a one-field key).
  const std::string good = R"({"key": {"fields": [{"value_bin": "CgAAAw=="}]}, "value": {"gate": 1}})";
  const std::pair<std::string, std::string> bad_entries[] = {
      {R"({"key": {"fields": [{"value_bin": "CgAAAw=="}]}, "value": {"nope": 1}})",
       "resource \"ctl_em/rules\" entry 1: value: "},
      {R"({"key": {"fields": [{"value_int": 1}, {"value_int": 2}]}, "value": {}})",
       "resource \"ctl_em/rules\" entry 1: "},
  };
  for (const auto &[entry, error] : bad_entries) {
    gp::Struct bad = got;
    (*bad.mutable_fields())["resources"] =
        Get(Json(std::format(R"({{"r": {{"ctl_em/rules": [{}, {}]}}}})", good, entry)), "r");
    const Output failed = daemon().Bessctl({"apply", tmp.Write("bad.json", Dump(bad))});
    EXPECT_EQ(1, failed.status) << failed.out;
    EXPECT_TRUE(Contains(failed.err, "the pipeline is applied, its resource entries are not"))
        << failed.err;
    EXPECT_TRUE(Contains(failed.err, error)) << failed.err;
    EXPECT_EQ(2u, EmRules());
  }

  // A generation that is no longer active is refused, nothing changes.
  // Desired state is the whole pipeline: cut down what `get` printed.
  gp::Struct reduced = got;
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
  Output stale = daemon().Bessctl({"apply", again, "--generation", version});
  EXPECT_EQ(1, stale.status);
  EXPECT_TRUE(Contains(stale.err, "conflict")) << stale.err;
  auto names = Modules();
  EXPECT_EQ(names.end(), std::find(names.begin(), names.end(), "ctl_em"));
  // The same generation of another daemon epoch: refused for the epoch,
  // before the generation is compared (D-106).
  stale = daemon().Bessctl(
      {"apply", again, "--generation", std::format("{}@{}", generation, epoch + 1)});
  EXPECT_EQ(1, stale.status);
  EXPECT_TRUE(Contains(stale.err, "restarted")) << stale.err;
  names = Modules();
  EXPECT_EQ(names.end(), std::find(names.begin(), names.end(), "ctl_em"));
}

TEST_F(BessctlTest, SchemaStatusAndUsage) {
  const std::string schema = Ok({"schema", "ExactMatch"}).out;
  EXPECT_TRUE(Contains(schema, "bess.pb.ExactMatchArg"));
  EXPECT_TRUE(Contains(schema, "bess.pb.ExactMatchCommandAddArg"));
  Ok({"status"});
  Ok({"metrics"});
  EXPECT_EQ(2, daemon().Bessctl({"nonsense"}).status);
  EXPECT_EQ(2, daemon().Bessctl({"events", "--from", "x"}).status);
  for (const char *bad : {"-1", "5", "5@0", "5@x"}) {  // a generation alone is ambiguous
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

// Module commands (D-115): the argument as JSON or YAML text (a flow mapping
// starts with "{" too), the answer printed; a command not marked thread-safe
// is refused while a worker runs the module.
TEST_F(BessctlTest, Command) {
  Ok({"apply", SourcePath("bessctl/conf/samples/arp.yaml")});
  Ok({"resume"});
  const Output busy =
      daemon().Bessctl({"command", "arp", "add", R"({ip: 10.9.8.7, mac_addr: "02:00:00:00:00:07"})"});
  EXPECT_EQ(1, busy.status);
  EXPECT_TRUE(Contains(busy.err, "is not MT safe")) << busy.err;
  Ok({"pause"});
  Ok({"command", "arp", "add", R"({ip: 10.9.8.7, mac_addr: "02:00:00:00:00:07"})"});
  Ok({"command", "arp", "add", R"({"ip": "10.9.8.6", "mac_addr": "02:00:00:00:00:06"})"});
  const gp::Struct initial = Json(Ok({"command", "arp", "get_initial_arg"}).out);
  EXPECT_EQ("1.2.3.4", Get(initial, "ip").string_value());
  EXPECT_TRUE(Contains(Ok({"command", "arp", "get_initial_arg", "-o", "yaml"}).out,
                       "ip: \"1.2.3.4\""));
  const Output unknown = daemon().Bessctl({"command", "arp", "nope"});
  EXPECT_EQ(1, unknown.status);
  EXPECT_TRUE(Contains(unknown.err, "it has: add")) << unknown.err;
  EXPECT_EQ(2, daemon().Bessctl({"command", "arp"}).status);
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
