// SPDX-License-Identifier: BSD-3-Clause

// The sample plugin in a daemon started with --modules: it registers
// SequentialUpdate, the incompatible probe is refused and leaves nothing, the
// shipped manifest (sample_plugin/sequential_update.yaml) runs, and a plugin is
// not unloaded while one of its modules lives (D-090). Replaces
// tools/check_sample_plugin.py. BESS_PLUGIN_DIR: the build's sample_plugin/.

#include <cerrno>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "live_daemon.h"

namespace {

using bess::live::Daemon;
namespace v2 = bess::pb::v2;

bool Contains(const std::string &s, const std::string &part) {
  return s.find(part) != std::string::npos;
}

bool HasClass(const v2::GetCapabilitiesResponse &caps, const std::string &mclass) {
  for (const std::string &c : caps.module_classes()) {
    if (c == mclass) {
      return true;
    }
  }
  return false;
}

std::optional<std::string> PluginPath(const v2::GetCapabilitiesResponse &caps,
                                      const std::string &part) {
  for (const v2::PluginInfo &p : caps.plugins()) {
    if (Contains(p.path(), part)) {
      return p.path();
    }
  }
  return std::nullopt;
}

double GatePackets(const std::vector<v2::MetricSample> &samples, const std::string &module) {
  double n = 0;
  for (const v2::MetricSample &s : samples) {
    const auto &l = s.labels();
    if (s.name() == "bess_gate_packets_total" && l.contains("module") &&
        l.at("module") == module && l.contains("direction") && l.at("direction") == "out") {
      n += s.value();
    }
  }
  return n;
}

TEST(SamplePluginLive, LoadsRunsAndUnloadsOnlyWhenUnused) {
  std::unique_ptr<Daemon> d =
      Daemon::Launch({"-modules=" + bess::live::Env("BESS_PLUGIN_DIR"), "--logtostderr"});
  ASSERT_TRUE(d->Alive()) << d->Log();
  bess::sdk::Client &client = d->client();

  auto caps = client.Capabilities();
  ASSERT_TRUE(caps) << caps.error().message;
  EXPECT_TRUE(HasClass(*caps, "SequentialUpdate"));
  // The probe declares an API range this daemon does not serve: refused,
  // nothing of it registered, and the daemon says so.
  EXPECT_FALSE(HasClass(*caps, "IncompatibleProbe"));
  EXPECT_FALSE(PluginPath(*caps, "incompatible_probe"));
  const std::string log = d->Log();
  EXPECT_TRUE(Contains(log, "incompatible_probe") && Contains(log, "refused")) << log;
  const std::optional<std::string> plugin = PluginPath(*caps, "sequential_update");
  ASSERT_TRUE(plugin);

  // The shipped manifest: SequentialUpdate takes every other batch.
  bess::live::Output applied =
      d->Bessctl({"apply", bess::live::SourcePath("sample_plugin/sequential_update.yaml")});
  ASSERT_EQ(0, applied.status) << applied.err;
  ASSERT_TRUE(client.ResumeWorkers());
  double through_su = 0;
  for (int i = 0; i < 50 && through_su == 0; i++) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto samples = client.Metrics();
    ASSERT_TRUE(samples) << samples.error().message;
    through_su = GatePackets(*samples, "su");
  }
  EXPECT_GT(through_su, 0) << "no packet left SequentialUpdate";
  ASSERT_TRUE(client.PauseWorkers());

  // Module su lives: the unload is refused, names it, and keeps the class.
  auto refused = client.UnloadPlugin(*plugin);
  ASSERT_FALSE(refused);
  ASSERT_TRUE(refused.error().detail) << refused.error().message;
  EXPECT_EQ(EBUSY, refused.error().detail->errno_value());
  EXPECT_TRUE(Contains(refused.error().message, "module su")) << refused.error().message;
  caps = client.Capabilities();
  ASSERT_TRUE(caps) << caps.error().message;
  EXPECT_TRUE(HasClass(*caps, "SequentialUpdate"));

  // Gone with its last module: the class and the listing too.
  d->Reset();
  auto unloaded = client.UnloadPlugin(*plugin);
  ASSERT_TRUE(unloaded) << unloaded.error().message;
  caps = client.Capabilities();
  ASSERT_TRUE(caps) << caps.error().message;
  EXPECT_FALSE(HasClass(*caps, "SequentialUpdate"));
  EXPECT_FALSE(PluginPath(*caps, "sequential_update"));
}

}  // namespace
