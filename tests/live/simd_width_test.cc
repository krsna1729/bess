// SPDX-License-Identifier: BSD-3-Clause

// --simd_width under traffic (D-126): bessd started with auto, 256 and 512
// forwards through the modules whose paths depend on the width -- CuckooMap's
// runtime-dispatched tag compare (DRR's flow table, past 1,024 buckets),
// rte_hash (ExactMatch), rte_lpm (the Router's route table), the AVX2 checksum kernels
// (IPChecksum, L4Checksum), HashLB -- and must neither die nor stop on a
// signal: --simd_width=512 on a CPU without AVX-512 (this development host)
// is clamped to the CPU's width, never executed. The expected width comes
// from this CPU's features, so on an AVX-512 runner the 512 case runs at 512.

#include <signal.h>
#include <sys/wait.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "arch/simd_dispatch.h"
#include "live_daemon.h"
#include "packet_builder.h"

namespace {

namespace v2 = bess::pb::v2;
namespace gp = google::protobuf;
using bess::live::Daemon;
using bess::live::Types;
using bess::testing::Bytes;
using bess::testing::PacketBuilder;

// UDP: L4Checksum computes a TCP checksum without forwarding the packet when
// not verifying, so TCP traffic would stop there.
Bytes Udp() {
  return PacketBuilder()
      .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
      .IPv4("10.0.0.1", "10.0.0.2")
      .Udp(10001, 10002)
      .PadTo(60)
      .Build();
}

bool Contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}

// The width bessd must report for `requested` on this CPU: the request (auto:
// DPDK's x86 default, 256), never more than the CPU's widest vectors.
uint16_t ExpectedWidth(const std::string &requested) {
  const uint16_t cpu = bess::arch::MaxSimdWidth(bess::arch::DetectCpuVectorFeatures());
  const uint16_t want = requested == "512" ? 512 : 256;
  return cpu < want ? cpu : want;
}

std::optional<double> GatePackets(const std::vector<v2::MetricSample> &samples,
                                  const std::string &module, int gate) {
  for (const v2::MetricSample &s : samples) {
    const auto &l = s.labels();
    if (s.name() == "bess_gate_packets_total" && l.contains("module") &&
        l.at("module") == module && l.contains("gate") && l.at("gate") == std::to_string(gate) &&
        l.contains("direction") && l.at("direction") == "out") {
      return s.value();
    }
  }
  return std::nullopt;
}

class SimdWidthLiveTest : public ::testing::TestWithParam<const char *> {};

TEST_P(SimdWidthLiveTest, ForwardsAndShutsDownWithoutSigill) {
  const std::string requested = GetParam();
  const uint16_t expected = ExpectedWidth(requested);
  const uint16_t cpu = bess::arch::MaxSimdWidth(bess::arch::DetectCpuVectorFeatures());
  std::unique_ptr<Daemon> d = Daemon::Launch({"--simd_width=" + requested, "--logtostderr"});
  ASSERT_TRUE(d->Alive()) << d->Log();
  bess::sdk::Client &client = d->client();

  // The width it decided, as GetCapabilities and its startup log say.
  auto caps = client.Capabilities();
  ASSERT_TRUE(caps) << caps.error().message;
  EXPECT_EQ(expected, caps->simd_width()) << caps->simd();
  EXPECT_TRUE(Contains(caps->simd(), "simd: width " + std::to_string(expected) + " (requested " +
                                         requested + ";"))
      << caps->simd();
  const std::string startup = d->Log();
  EXPECT_TRUE(Contains(startup, caps->simd())) << startup;
  // DPDK is forced only where the CPU's widths are known (x86); on arm64 it
  // keeps its default, which is never narrower than the kernels' width.
  const bool forced = requested != "auto" && bess::arch::kCpuSimdWidthKnown;
  EXPECT_EQ(forced, Contains(startup, R"("--force-max-simd-bitwidth", ")" +
                                          std::to_string(expected) + R"(")"))
      << startup;
  if (forced) {
    EXPECT_EQ(expected, caps->dpdk_simd_width()) << "DPDK's own limit";
  } else {
    EXPECT_GE(caps->dpdk_simd_width(), caps->simd_width()) << "DPDK's own limit";
  }
  const bool clamped = requested == "512" && cpu < 512 && bess::arch::kCpuSimdWidthKnown;
  EXPECT_EQ(clamped, Contains(startup, "--simd_width=512 requested, but this CPU lacks AVX-512"))
      << startup;
  if (clamped) {
    EXPECT_TRUE(Contains(startup, "using " + std::to_string(cpu) + " bits")) << startup;
  }

  // Source -> Rewrite -> RandomUpdate -> ExactMatch -> WildcardMatch ->
  // Router -> IPChecksum -> L4Checksum -> HashLB -> DRR -> Sink. 4,096
  // source addresses and a rate-limited DRR task: thousands of flows queue up
  // at once, and DRR's CuckooMap (four entries per bucket, a power-of-two
  // bucket count) grows to the 1,024 buckets from which its lookups take the
  // dispatched compare once more than 2,048 are queued.
  Types types(client);
  auto rewrite = types.Make("bess.pb.RewriteArg", "{}");
  const Bytes udp = Udp();
  rewrite->GetReflection()->AddString(rewrite.get(),
                                      rewrite->GetDescriptor()->FindFieldByName("templates"),
                                      std::string(udp.begin(), udp.end()));
  auto random = types.Make("bess.pb.RandomUpdateArg",
                           R"({"fields": [{"offset": 26, "size": 4, "min": 1, "max": 4096},
                                          {"offset": 30, "size": 4, "min": 1, "max": 4294967295}]})");
  auto em = types.Make("bess.pb.ExactMatchArg",
                       R"({"fields": [{"offset": 30, "num_bytes": 4}], "default_gate": 0})");
  auto wm = types.Make("bess.pb.WildcardMatchArg",
                       R"({"fields": [{"offset": 26, "num_bytes": 4}], "default_gate": 0})");
  auto ipc = types.Make("bess.pb.IPChecksumArg", "{}");
  auto l4c = types.Make("bess.pb.L4ChecksumArg", "{}");
  auto lb = types.Make("bess.pb.HashLBArg", R"({"gates": [0]})");
  auto drr = types.Make("bess.pb.DRRArg", R"({"num_flows": 4096})");
  v2::TaskPlacement src_task;
  src_task.set_module("src");
  src_task.set_wid(0);
  v2::TrafficClass rl;
  rl.set_name("drr_rl");
  rl.set_policy("rate_limit");
  rl.set_resource("packet");
  rl.set_wid(0);
  (*rl.mutable_limit())["packet"] = 1000000;
  v2::TaskPlacement drr_task;
  drr_task.set_module("drr");
  drr_task.set_parent("drr_rl");
  // The Router in destination mode (D-155) with the default route: its route
  // table is rte_lpm, one of the width-sensitive libraries this pipeline runs.
  auto router = types.Make(
      "bess.pb.RouterArg",
      R"({"mode": "ROUTE_MODE_DESTINATION", "max_ipv4_routes": 16, "max_next_hops": 2})");
  auto hop_key = types.Make("bess.pb.RouterNextHopIdKey", R"({"id": 1})");
  auto hop_value =
      types.Make("bess.pb.RouterNextHopValue",
                 R"({"egress_gate": 0, "dst_mac": "AgAAAAAC", "src_mac": "AgAAAAAB"})");
  auto route_key = types.Make("bess.pb.RouterRouteKey", R"({"ipv4": "0.0.0.0", "prefix_length": 0})");
  auto route_value = types.Make("bess.pb.RouterRouteValue", R"({"next_hop_id": 1})");
  ASSERT_TRUE(router && hop_key && hop_value && route_key && route_value);
  auto pipeline = bess::sdk::PipelineBuilder()
                      .Worker(0, std::nullopt, "")
                      .TrafficClass(rl)
                      .Module("src", "Source", nullptr)
                      .Module("rewrite", "Rewrite", rewrite.get())
                      .Module("rnd", "RandomUpdate", random.get())
                      .Module("em", "ExactMatch", em.get())
                      .Module("wm", "WildcardMatch", wm.get())
                      .Module("rt", "Router", router.get())
                      .Module("ipc", "IPChecksum", ipc.get())
                      .Module("l4c", "L4Checksum", l4c.get())
                      .Module("lb", "HashLB", lb.get())
                      .Module("drr", "DRR", drr.get())
                      .Module("sink", "Sink", nullptr)
                      .Chain({"src", "rewrite", "rnd", "em", "wm", "rt", "ipc", "l4c", "lb",
                              "drr", "sink"})
                      .Task(src_task)
                      .Task(drr_task)
                      .Entry("rt/next_hops", *hop_key, *hop_value)
                      .Entry("rt/routes", *route_key, *route_value)
                      .Build();
  ASSERT_TRUE(pipeline) << pipeline.error().message;
  auto applied = client.ApplyPipeline(*pipeline);
  ASSERT_TRUE(applied) << applied.error().message << "\n" << d->Log();

  ASSERT_TRUE(client.ResumeWorkers());
  std::this_thread::sleep_for(std::chrono::milliseconds(1500));
  ASSERT_TRUE(client.PauseWorkers());
  ASSERT_TRUE(d->Alive()) << "bessd died under traffic:\n" << d->Log();

  auto samples = client.Metrics();
  ASSERT_TRUE(samples) << samples.error().message;
  std::string counts;
  for (const char *module : {"rnd", "em", "wm", "rt", "ipc", "l4c", "lb", "drr"}) {
    const auto n = GatePackets(*samples, module, 0);
    ASSERT_TRUE(n) << module << " has no gate 0 counter";
    EXPECT_GT(*n, 0) << module << " forwarded nothing";
    counts += std::string(" ") + module + "=" + std::to_string(static_cast<uint64_t>(*n));
  }

  // A clean exit: an illegal instruction anywhere (an AVX-512 path taken on
  // a CPU without it) would end bessd on SIGILL.
  const std::string log = d->Log();
  const int status = d->Shutdown();
  ASSERT_NE(-1, status);
  ASSERT_FALSE(WIFSIGNALED(status) && WTERMSIG(status) == SIGILL)
      << "bessd --simd_width=" << requested << " died of SIGILL (illegal instruction):\n"
      << log;
  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
      << "bessd --simd_width=" << requested << " ended with wait status " << status
      << (WIFSIGNALED(status) ? " (signal " + std::to_string(WTERMSIG(status)) + ")" : "")
      << ":\n"
      << log;
  // The run, for the record (meson's test log).
  std::cout << "--simd_width=" << requested << ": " << caps->simd() << "; packets" << counts
            << "; exit " << (WIFEXITED(status) ? WEXITSTATUS(status) : -1) << ", signal "
            << (WIFSIGNALED(status) ? WTERMSIG(status) : 0) << "\n";
  if (const size_t at = startup.find("--simd_width=512 requested"); at != std::string::npos) {
    std::cout << "  warned: " << startup.substr(at, startup.find('\n', at) - at) << "\n";
  }
}

INSTANTIATE_TEST_SUITE_P(Requests, SimdWidthLiveTest, ::testing::Values("auto", "256", "512"),
                         [](const ::testing::TestParamInfo<const char *> &info) {
                           return std::string("simd_width_") + info.param;
                         });

}  // namespace
