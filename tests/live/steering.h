// SPDX-License-Identifier: BSD-3-Clause

// Whether an ExactMatch's rules steer packets, seen from a live daemon's gate
// counters (D-116): src (Source) -> upd (Update writes the IPv4 source and
// destination at 26 and 30) -> em (ExactMatch on those two fields, default
// gate 3) -> gates 0-3 -> sink0..sink3, src's task on worker 0. Every packet
// carries the same addresses, so all of them leave em by one gate.

#ifndef BESS_TESTS_LIVE_STEERING_H_
#define BESS_TESTS_LIVE_STEERING_H_

#include <array>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include "live_daemon.h"

namespace bess::live {

inline constexpr char kSteeringResource[] = "em/rules";

// em's rules key for `sip` -> `dip` (host order), as protobuf JSON.
inline std::string RuleKeyJson(uint32_t sip, uint32_t dip) {
  const auto b64 = [](uint32_t v) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const uint8_t b[4] = {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                          static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)};
    std::string out;
    out += kAlphabet[b[0] >> 2];
    out += kAlphabet[((b[0] & 3) << 4) | (b[1] >> 4)];
    out += kAlphabet[((b[1] & 15) << 2) | (b[2] >> 6)];
    out += kAlphabet[b[2] & 63];
    out += kAlphabet[b[3] >> 2];
    out += kAlphabet[(b[3] & 3) << 4];
    return out + "==";
  };
  return std::format(R"({{"fields": [{{"value_bin": "{}"}}, {{"value_bin": "{}"}}]}})", b64(sip),
                     b64(dip));
}

// Applies the pipeline; its packets carry `sip` -> `dip`.
inline void ApplySteeringPipeline(Types &types, bess::sdk::Client &client, uint32_t sip,
                                  uint32_t dip) {
  auto src = types.Make("bess.pb.SourceArg", R"({"pkt_size": 60})");
  auto upd = types.Make("bess.pb.UpdateArg",
                        std::format(R"({{"fields": [{{"offset": 26, "size": 4, "value": {}}},
                                                    {{"offset": 30, "size": 4, "value": {}}}]}})",
                                    sip, dip));
  auto em = types.Make(
      "bess.pb.ExactMatchArg",
      R"({"fields": [{"offset": 26, "num_bytes": 4}, {"offset": 30, "num_bytes": 4}],
          "default_gate": 3})");
  bess::pb::v2::TaskPlacement task;
  task.set_module("src");
  task.set_wid(0);
  bess::sdk::PipelineBuilder b;
  b.Worker(0, 0, "")
      .Module("src", "Source", src.get())
      .Module("upd", "Update", upd.get())
      .Module("em", "ExactMatch", em.get())
      .Chain({"src", "upd", "em"})
      .Task(task);
  for (uint32_t gate = 0; gate < 4; gate++) {
    const std::string sink = "sink" + std::to_string(gate);
    b.Module(sink, "Sink", nullptr).Connect("em", gate, sink, 0);
  }
  auto pipeline = b.Build();
  ASSERT_TRUE(pipeline) << pipeline.error().message;
  auto applied = client.ApplyPipeline(*pipeline);
  ASSERT_TRUE(applied) << applied.error().message;
}

// em's output gate counters, gates 0-3.
inline std::array<double, 4> EmGateCounts(bess::sdk::Client &client) {
  std::array<double, 4> counts{};
  auto samples = client.Metrics();
  EXPECT_TRUE(samples) << samples.error().message;
  if (!samples) {
    return counts;
  }
  for (const bess::pb::v2::MetricSample &s : *samples) {
    const auto module = s.labels().find("module");
    const auto gate = s.labels().find("gate");
    const auto direction = s.labels().find("direction");
    if (s.name() == "bess_gate_packets_total" && module != s.labels().end() &&
        module->second == "em" && direction != s.labels().end() && direction->second == "out" &&
        gate != s.labels().end()) {
      const int g = std::stoi(gate->second);
      if (g >= 0 && g < 4) {
        counts[g] = s.value();
      }
    }
  }
  return counts;
}

// Runs the workers for a moment: what left em by each of gates 0-3 meanwhile.
inline std::array<double, 4> Steer(bess::sdk::Client &client) {
  const auto before = EmGateCounts(client);
  EXPECT_TRUE(client.ResumeWorkers());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_TRUE(client.PauseWorkers());
  auto after = EmGateCounts(client);
  for (size_t i = 0; i < after.size(); i++) {
    after[i] -= before[i];
  }
  return after;
}

// Every packet of a moment's traffic left em by `gate`.
inline ::testing::AssertionResult SteeredTo(bess::sdk::Client &client, int gate) {
  const auto d = Steer(client);
  const std::string seen = std::format("gates 0-3 forwarded {} {} {} {}", d[0], d[1], d[2], d[3]);
  for (int g = 0; g < 4; g++) {
    if ((g == gate) != (d[g] > 0)) {
      return ::testing::AssertionFailure() << "want every packet on gate " << gate << "; " << seen;
    }
  }
  return ::testing::AssertionSuccess() << seen;
}

}  // namespace bess::live

#endif  // BESS_TESTS_LIVE_STEERING_H_
