// SPDX-License-Identifier: BSD-3-Clause

// The strict memory budget (D-207) end to end: bessd alone in a cgroup with
// memory.max 512 MiB and no swap, normal pages, no --mem_limit_mb (the limit
// comes from the cgroup, as in a Kubernetes pod). Rules are loaded until the
// daemon refuses; then it must still answer and take changes, the kernel must
// never have OOM-killed anything in the cgroup, and the memory the daemon
// cannot attribute (used - admitted - baseline) must stay under a stated
// bound. Skipped where `systemd-run --user --scope` cannot make the cgroup.

#include <grpcpp/grpcpp.h>

#include <format>
#include <fstream>
#include <map>
#include <string>

#include "control_v2.grpc.pb.h"
#include "live_daemon.h"
#include "steering.h"

namespace {

namespace v2 = bess::pb::v2;
using bess::live::Daemon;
using bess::live::Types;

constexpr uint64_t kCapMiB = 512;
// What the daemon may hold that no module admitted, after the refusal and a
// 20K-rule erase and insert: half the 128 MiB reserve. Mostly what glibc
// keeps free in its two arenas (RT-1) from the last large transactions'
// decoded copies, given back before any refusal (D-207); the other half of
// the reserve is the margin for a request in flight. Measured: 52 MiB.
constexpr int64_t kUnattributedBound = int64_t{64} * 1024 * 1024;
constexpr uint32_t kBatch = 20000;

std::map<std::string, uint64_t> ReadKeyed(const std::string &path) {
  std::map<std::string, uint64_t> out;
  std::ifstream in(path);
  std::string key;
  uint64_t value = 0;
  while (in >> key >> value) {
    out[key] = value;
  }
  return out;
}

std::string CgroupOf(int pid) {
  std::ifstream in("/proc/" + std::to_string(pid) + "/cgroup");
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("0::", 0) == 0) {
      return "/sys/fs/cgroup" + line.substr(3);
    }
  }
  return "";
}

v2::GetResourceStatusResponse Status(const std::string &address) {
  auto stub = v2::Control::NewStub(
      grpc::CreateChannel(address, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  v2::GetResourceStatusResponse status;
  const grpc::Status s = stub->GetResourceStatus(&context, {}, &status);
  EXPECT_TRUE(s.ok()) << s.error_message();
  return status;
}

TEST(StrictBudgetTest, RefusesInsteadOfBeingKilledAndAccountsEveryByte) {
  const std::string unit = std::format("bess-strict-budget-{}", getpid());
  if (bess::live::RunProgram({"systemd-run", "--user", "--scope", "-q", "true"}).status != 0) {
    GTEST_SKIP() << "systemd-run --user --scope cannot make a cgroup here";
  }
  auto daemon = Daemon::LaunchUnder(
      {"systemd-run", "--user", "--scope", "-q", "--unit=" + unit,
       "-p", std::format("MemoryMax={}M", kCapMiB), "-p", "MemorySwapMax=0"},
      {"--buffers=4096"});
  ASSERT_GT(daemon->pid(), 0);
  const std::string cgroup = CgroupOf(daemon->pid());
  ASSERT_NE(cgroup.find(unit), std::string::npos) << cgroup;

  const v2::MemoryStatus start = Status(daemon->address()).memory();
  ASSERT_EQ(start.limit_bytes(), kCapMiB << 20u) << start.limit_source();
  EXPECT_NE(start.limit_source().find("memory.max"), std::string::npos);
  EXPECT_GT(start.baseline_bytes(), 0u);

  Types types(daemon->client());
  auto em = types.Make("bess.pb.ExactMatchArg",
                       R"({"fields": [{"offset": 26, "num_bytes": 4}, {"offset": 30, "num_bytes": 4}],
                           "default_gate": 0})");
  bess::sdk::PipelineBuilder b;
  b.Module("em", "ExactMatch", em.get());
  auto pipeline = b.Build();
  ASSERT_TRUE(pipeline) << pipeline.error().message;
  ASSERT_TRUE(daemon->client().ApplyPipeline(*pipeline));
  auto rules = daemon->client().Resource(bess::live::kSteeringResource);
  ASSERT_TRUE(rules) << rules.error().message;
  auto gate = types.Make(rules->value_type, R"({"gate": 1})");

  // Load until refused: every batch either commits whole or is refused whole.
  uint32_t loaded = 0;
  bess::sdk::Error refused;
  for (;;) {
    ASSERT_LT(loaded, 64u << 20u) << "never refused";
    auto tx = daemon->client().NewTransaction();
    for (uint32_t i = loaded; i < loaded + kBatch; i++) {
      ASSERT_TRUE(tx.Upsert(*rules, *types.Make(rules->key_type, bess::live::RuleKeyJson(i, ~i)),
                            *gate));
    }
    auto done = tx.Commit();
    if (!done) {
      refused = done.error();
      break;
    }
    loaded += kBatch;
  }
  EXPECT_EQ(refused.code, grpc::StatusCode::RESOURCE_EXHAUSTED) << refused.message;
  EXPECT_GT(loaded, 0u);

  // Alive, and still changing: erase a batch, then a small load fits again.
  ASSERT_TRUE(daemon->Alive());
  {
    auto tx = daemon->client().NewTransaction();
    for (uint32_t i = loaded - kBatch; i < loaded; i++) {
      ASSERT_TRUE(tx.Erase(*rules, *types.Make(rules->key_type, bess::live::RuleKeyJson(i, ~i))));
    }
    auto done = tx.Commit();
    ASSERT_TRUE(done) << done.error().message;
    auto again = daemon->client().NewTransaction();
    ASSERT_TRUE(again.Upsert(*rules, *types.Make(rules->key_type, bess::live::RuleKeyJson(loaded, 1)),
                             *gate));
    auto small = again.Commit();
    ASSERT_TRUE(small) << small.error().message;
  }

  const v2::MemoryStatus at = Status(daemon->address()).memory();
  EXPECT_LE(at.used_bytes(), at.limit_bytes());
  EXPECT_GT(at.refusals(), 0u);
  EXPECT_LT(at.unattributed_bytes(), kUnattributedBound)
      << "used " << at.used_bytes() << " admitted " << at.admitted_bytes() << " baseline "
      << at.baseline_bytes();
  const auto events = ReadKeyed(cgroup + "/memory.events");
  EXPECT_EQ(events.at("oom"), 0u);
  EXPECT_EQ(events.at("oom_kill"), 0u);
  ASSERT_TRUE(daemon->Alive());
  std::printf("[strict budget] %u rules, refused: %s; used %.1f MiB, admitted %.1f, baseline %.1f, "
              "unattributed %.1f, events max %llu\n",
              loaded, refused.message.c_str(), at.used_bytes() / 1048576.0,
              at.admitted_bytes() / 1048576.0, at.baseline_bytes() / 1048576.0,
              at.unattributed_bytes() / 1048576.0,
              static_cast<unsigned long long>(events.count("max") ? events.at("max") : 0));
  daemon->Shutdown();
}

}  // namespace
