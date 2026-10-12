// SPDX-License-Identifier: BSD-3-Clause

// The control SDK conformance suite (D-104, docs/control-sdk.md) against a
// live bessd (was bessctl/module_tests/control_sdk_conformance.py): with an
// ExactMatch's rules as $resource, sdk/conformance/cases.json runs through
// the C++ driver ($BESS_SDK_CPP_CONFORMANCE) and the Go driver
// (`go test -tags live -run TestConformance` in sdk/go); every case must pass
// in each, and the rule each leaves behind must steer packets.
//
// The Go command is `go` on PATH, or $BESS_GO split at spaces (a container,
// for example `sudo docker run --rm --network host -v $PWD:/src -w /src/sdk/go
// golang go`). Without Go the test fails unless BESS_SKIP_GO=1.

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <google/protobuf/struct.pb.h>
#include <google/protobuf/util/json_util.h>

#include "live_daemon.h"
#include "steering.h"

namespace {

using bess::live::LiveTest;
using bess::live::Types;

// 65.43.21.0 -> 12.34.56.78: the rule the commit cases leave behind, gate 1.
constexpr uint32_t kSip = 0x412B1500;
constexpr uint32_t kDip = 0x0C22384E;

std::vector<std::string> CaseNames() {
  std::ifstream in(bess::live::SourcePath("sdk/conformance/cases.json"));
  const std::string text(std::istreambuf_iterator<char>(in), {});
  google::protobuf::Struct cases;
  EXPECT_TRUE(google::protobuf::util::JsonStringToMessage(text, &cases).ok());
  std::vector<std::string> names;
  for (const auto &c : cases.fields().at("cases").list_value().values()) {
    names.push_back(c.struct_value().fields().at("name").string_value());
  }
  EXPECT_FALSE(names.empty());
  return names;
}

// `name` on PATH (or a path), if it is an executable.
std::optional<std::string> Executable(const std::string &name) {
  if (name.find('/') != std::string::npos) {
    return access(name.c_str(), X_OK) == 0 ? std::optional(name) : std::nullopt;
  }
  const char *path = std::getenv("PATH");
  std::stringstream dirs(path != nullptr ? path : "");
  for (std::string dir; std::getline(dirs, dir, ':');) {
    const std::string candidate = dir + "/" + name;
    if (!dir.empty() && access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
  }
  return std::nullopt;
}

// The Go command: $BESS_GO, or a `go` that runs (an unconfigured
// version-manager shim on PATH is no toolchain).
std::optional<std::vector<std::string>> GoCommand() {
  std::vector<std::string> argv;
  if (const char *custom = std::getenv("BESS_GO"); custom != nullptr && *custom != '\0') {
    std::stringstream words(custom);
    for (std::string w; words >> w;) {
      argv.push_back(w);
    }
  } else {
    argv = {"go"};
  }
  auto program = Executable(argv[0]);
  if (!program) {
    return std::nullopt;
  }
  argv[0] = *program;
  if (std::getenv("BESS_GO") == nullptr &&
      bess::live::RunProgram({argv[0], "version"}).status != 0) {
    return std::nullopt;
  }
  return argv;
}

class ConformanceTest : public LiveTest {
 protected:
  void SetUp() override {
    LiveTest::SetUp();
    Types types(client());
    ASSERT_NO_FATAL_FAILURE(bess::live::ApplySteeringPipeline(types, client(), kSip, kDip));
    // No rule yet: the default gate.
    ASSERT_TRUE(bess::live::SteeredTo(client(), 3));
  }

  // Every case passed: `passed(name)` is in the output for each.
  void ExpectAllPassed(const std::string &output,
                       const std::function<std::string(const std::string &)> &passed) {
    for (const std::string &name : CaseNames()) {
      EXPECT_NE(std::string::npos, output.find(passed(name)))
          << "case " << name << " did not pass:\n"
          << output;
    }
  }

  // The rule the driver left steers the packets to gate 1; then erased.
  void ExpectRuleSteersAndErase(const char *who) {
    EXPECT_TRUE(bess::live::SteeredTo(client(), 1)) << "the " << who << " client's rule";
    Types types(client());
    auto rules = client().Resource(bess::live::kSteeringResource);
    ASSERT_TRUE(rules) << rules.error().message;
    auto key = types.Make(rules->key_type, bess::live::RuleKeyJson(kSip, kDip));
    auto tx = client().NewTransaction();
    ASSERT_TRUE(tx.Erase(*rules, *key));
    auto done = tx.Commit();
    ASSERT_TRUE(done) << done.error().message;
    EXPECT_TRUE(bess::live::SteeredTo(client(), 3));
  }
};

TEST_F(ConformanceTest, Cpp) {
  const auto run = bess::live::RunProgram(
      {bess::live::Env("BESS_SDK_CPP_CONFORMANCE"), daemon().address(),
       bess::live::kSteeringResource, bess::live::SourcePath("sdk/conformance/cases.json")},
      "", std::chrono::seconds(300));
  ASSERT_EQ(0, run.status) << run.out << run.err;
  ExpectAllPassed(run.out, [](const std::string &n) { return "PASS " + n + "\n"; });
  ExpectRuleSteersAndErase("C++");
}

TEST_F(ConformanceTest, Go) {
  const auto go = GoCommand();
  if (!go) {
    const char *skip = std::getenv("BESS_SKIP_GO");
    if (skip != nullptr && std::string(skip) == "1") {
      GTEST_SKIP() << "no Go toolchain (BESS_SKIP_GO=1)";
    }
    FAIL() << "no Go toolchain: install Go, set BESS_GO, or BESS_SKIP_GO=1";
  }
  // A file, so that a Go run in a container sees it too.
  const std::string sdk_go = bess::live::SourcePath("sdk/go");
  const std::filesystem::path testdata = sdk_go + "/bess/testdata";
  std::filesystem::create_directories(testdata);
  const std::filesystem::path config = testdata / "conformance.json";
  {
    std::ofstream out(config);
    out << std::format(R"({{"address": "{}", "resource": "{}"}})", daemon().address(),
                       bess::live::kSteeringResource);
  }
  std::vector<std::string> argv = *go;
  for (const char *a : {"test", "-count=1", "-v", "-tags", "live", "-run", "TestConformance",
                        "./bess"}) {
    argv.push_back(a);
  }
  const auto run = bess::live::RunProgramIn(sdk_go, argv, "", std::chrono::seconds(600));
  std::filesystem::remove(config);
  ASSERT_EQ(0, run.status) << run.out << run.err;
  ExpectAllPassed(run.out,
                  [](const std::string &n) { return "--- PASS: TestConformance/" + n + " "; });
  ExpectRuleSteersAndErase("Go");
}

}  // namespace
