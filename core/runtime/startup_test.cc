// SPDX-License-Identifier: BSD-3-Clause

// Container-friendly startup (Decision D-030): flags from the environment,
// usable hugepages from sysfs and the cgroup, and the NICs a device plugin
// assigned.

#include "runtime/startup.h"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "runtime/opts.h"

namespace {

namespace fs = std::filesystem;
using bess::startup::ApplyEnvironmentFlags;
using bess::startup::DevicePluginPciAddresses;
using bess::startup::UsableHugepageMB;
using bess::startup::DiscoverHugetlbLimit;
using bess::startup::DiscoverMemoryLimit;
using bess::startup::HugetlbChargedToMemory;
using bess::startup::MemoryLimit;
using bess::startup::MemorySettings;
using bess::startup::MemorySettingsFromEnvironment;
using bess::startup::ResolveMemoryPolicy;
using bess::utils::MemoryBacking;

std::vector<char *> Env(std::vector<std::string> &storage) {
  std::vector<char *> env;
  for (auto &s : storage) {
    env.push_back(s.data());
  }
  env.push_back(nullptr);
  return env;
}

TEST(StartupTest, FlagsComeFromTheEnvironmentUnlessGiven) {
  gflags::FlagSaver saver;
  std::vector<std::string> vars = {"PATH=/bin", "BESSD_BUFFERS=4096",
                                   "BESSD_PACKET_DATA_ROOM=4096",
                                   "BESSD_NOT_A_FLAG=1"};
  // packet_data_room was "given on the command line" (not default): kept.
  gflags::SetCommandLineOption("packet_data_room", "2048");
  auto env = Env(vars);
  const auto taken = ApplyEnvironmentFlags(env.data());
  EXPECT_EQ(taken, std::vector<std::string>{"BESSD_BUFFERS"});
  EXPECT_EQ(FLAGS_buffers, 4096);
  EXPECT_EQ(FLAGS_packet_data_room, 2048u);
}

TEST(StartupDeathTest, InvalidEnvironmentValuesAreFatal) {
  gflags::FlagSaver saver;
  std::vector<std::string> vars = {"BESSD_BUFFERS=1000"};  // not a power of 2
  auto env = Env(vars);
  EXPECT_DEATH(ApplyEnvironmentFlags(env.data()), "BESSD_BUFFERS=1000");
}

TEST(StartupTest, UsableHugepagesRespectAncestorRemainingCapacity) {
  const fs::path root = fs::temp_directory_path() / "bess_startup_test";
  fs::remove_all(root);
  auto write = [&](const fs::path &p, const std::string &v) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << v << "\n";
  };
  const fs::path pages = root / "hugepages", cgroup = root / "cgroup";
  const fs::path proc_cgroup = root / "proc" / "self" / "cgroup";
  write(pages / "hugepages-2048kB" / "free_hugepages", "512");  // 1 GiB
  write(pages / "hugepages-1048576kB" / "free_hugepages", "1"); // 1 GiB
  fs::create_directories(cgroup / "pod" / "container");
  write(proc_cgroup, "0::/pod/container");
  EXPECT_EQ(UsableHugepageMB(pages, cgroup, proc_cgroup), 2048u);

  // The pod ancestor's limit applies even though the process is in a child.
  write(cgroup / "pod" / "hugetlb.2MB.max", "536870912");  // 512 MiB
  EXPECT_EQ(UsableHugepageMB(pages, cgroup, proc_cgroup), 1024u);
  write(cgroup / "pod" / "hugetlb.2MB.current", "268435456");  // 256 MiB
  EXPECT_EQ(UsableHugepageMB(pages, cgroup, proc_cgroup), 1280u);
  write(cgroup / "pod" / "hugetlb.2MB.current", "536870912");
  EXPECT_EQ(UsableHugepageMB(pages, cgroup, proc_cgroup), 1024u);

  write(cgroup / "hugetlb.1GB.max", "0");
  write(cgroup / "hugetlb.1GB.current", "0");
  EXPECT_EQ(UsableHugepageMB(pages, cgroup, proc_cgroup), 0u);
  write(pages / "hugepages-2048kB" / "free_hugepages", "0");
  EXPECT_EQ(UsableHugepageMB(pages, cgroup, proc_cgroup), 0u);
  EXPECT_EQ(UsableHugepageMB(root / "absent", cgroup, proc_cgroup), 0u);
  fs::remove_all(root);
}

// Hugepages count against memory.max only where the cgroup2 mount says so
// (memory_hugetlb_accounting, D-198): the mount at the cgroup root, read
// from mountinfo's super options, among other mounts.
TEST(StartupTest, HugetlbChargingIsReadFromTheCgroup2Mount) {
  const fs::path root = fs::temp_directory_path() / "bess_startup_mountinfo";
  fs::remove_all(root);
  fs::create_directories(root);
  const fs::path mountinfo = root / "mountinfo";
  const std::string proc =
      "22 1 0:21 / /proc rw,nosuid,nodev,noexec,relatime shared:12 - proc proc rw\n";
  const std::string hugetlbfs =
      "40 24 0:41 / /dev/hugepages rw,relatime shared:22 - hugetlbfs hugetlbfs "
      "rw,pagesize=1024M\n";
  const auto cgroup2 = [](const std::string &mount_point,
                          const std::string &options) {
    return "35 24 0:30 / " + mount_point +
           " rw,nosuid,nodev,noexec,relatime shared:9 - cgroup2 cgroup2 " +
           options + "\n";
  };
  std::ofstream(mountinfo)
      << proc
      << cgroup2("/sys/fs/cgroup", "rw,nsdelegate,memory_recursiveprot,"
                                   "memory_hugetlb_accounting")
      << hugetlbfs;
  EXPECT_TRUE(HugetlbChargedToMemory(mountinfo, "/sys/fs/cgroup"));
  EXPECT_TRUE(HugetlbChargedToMemory(mountinfo, "/sys/fs/cgroup/"));

  std::ofstream(mountinfo) << proc
                           << cgroup2("/sys/fs/cgroup",
                                      "rw,nsdelegate,memory_recursiveprot")
                           << hugetlbfs;
  EXPECT_FALSE(HugetlbChargedToMemory(mountinfo, "/sys/fs/cgroup"));

  // Another cgroup2 mount's options do not speak for the root's.
  std::ofstream(mountinfo)
      << cgroup2("/run/other", "rw,memory_hugetlb_accounting")
      << cgroup2("/sys/fs/cgroup", "rw,nsdelegate") << proc;
  EXPECT_FALSE(HugetlbChargedToMemory(mountinfo, "/sys/fs/cgroup"));
  // No mount at the root: the first cgroup2 mount answers.
  EXPECT_TRUE(HugetlbChargedToMemory(mountinfo, "/elsewhere"));

  std::ofstream(mountinfo) << proc << hugetlbfs;
  EXPECT_FALSE(HugetlbChargedToMemory(mountinfo, "/sys/fs/cgroup"));
  EXPECT_FALSE(HugetlbChargedToMemory(root / "absent", "/sys/fs/cgroup"));
  fs::remove_all(root);
}

// The hugetlb controller's limit is a hugepage cap (D-198): the smallest
// finite hugetlb.<size>.max on the cgroup path, summed over the sizes bessd
// can use; none if any usable size is unlimited.
TEST(StartupTest, HugetlbLimitIsTheSmallestAncestorMaxPerPageSize) {
  const fs::path root = fs::temp_directory_path() / "bess_startup_hugetlb";
  fs::remove_all(root);
  auto write = [&](const fs::path &p, const std::string &v) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << v << "\n";
  };
  const fs::path pages = root / "hugepages", cgroup = root / "cgroup";
  const fs::path proc_cgroup = root / "proc" / "self" / "cgroup";
  write(pages / "hugepages-1048576kB" / "free_hugepages", "4");
  write(pages / "hugepages-2048kB" / "free_hugepages", "0");  // unusable
  fs::create_directories(cgroup / "pod" / "container");
  write(proc_cgroup, "0::/pod/container");
  EXPECT_EQ(DiscoverHugetlbLimit(pages, cgroup, proc_cgroup).bytes, 0u)
      << "no limit anywhere: no cap";

  write(cgroup / "pod" / "hugetlb.1GB.max", "3221225472");  // 3 GiB
  write(cgroup / "pod" / "container" / "hugetlb.1GB.max", "max");
  MemoryLimit cap = DiscoverHugetlbLimit(pages, cgroup, proc_cgroup);
  EXPECT_EQ(cap.bytes, 3ull << 30);
  EXPECT_EQ(cap.source, "cgroup hugetlb.1GB.max (/pod)");
  write(cgroup / "pod" / "container" / "hugetlb.1GB.max", "1073741824");
  cap = DiscoverHugetlbLimit(pages, cgroup, proc_cgroup);
  EXPECT_EQ(cap.bytes, 1ull << 30);
  EXPECT_EQ(cap.source, "cgroup hugetlb.1GB.max (/pod/container)");

  // A second usable size: limited, the caps add; unlimited, no cap.
  write(pages / "hugepages-2048kB" / "free_hugepages", "512");
  EXPECT_EQ(DiscoverHugetlbLimit(pages, cgroup, proc_cgroup).bytes, 0u);
  write(cgroup / "pod" / "hugetlb.2MB.max", "536870912");
  cap = DiscoverHugetlbLimit(pages, cgroup, proc_cgroup);
  EXPECT_EQ(cap.bytes, (1ull << 30) + (512ull << 20));
  fs::remove_all(root);
}

TEST(StartupTest, DevicePluginAddressesAreParsed) {
  std::vector<std::string> vars = {
      "PCIDEVICE_INTEL_COM_SRIOV_NET_A=0000:18:02.3,0000:18:02.2",
      "PCIDEVICE_INTEL_COM_SRIOV_NET_B=0000:18:02.2",
      "PCIDEVICE_INTEL_COM_SRIOV_NET_A_INFO={\"0000:18:02.2\":{}}",
      "PCIDEVICE_BROKEN=not-an-address",
      "OTHER=0000:00:01.0"};
  auto env = Env(vars);
  EXPECT_EQ(DevicePluginPciAddresses(env.data()),
            (std::vector<std::string>{"0000:18:02.2", "0000:18:02.3"}));
}

// D-198: the limit is the smallest finite memory.max on the way to the
// cgroup root, else MemTotal.
TEST(StartupTest, MemoryLimitIsTheSmallestAncestorMemoryMax) {
  const fs::path root = fs::temp_directory_path() / "bess_memory_limit_test";
  fs::remove_all(root);
  auto write = [&](const fs::path &p, const std::string &v) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << v << "\n";
  };
  const fs::path cgroup = root / "cgroup";
  const fs::path proc_cgroup = root / "proc" / "self" / "cgroup";
  const fs::path meminfo = root / "proc" / "meminfo";
  write(meminfo, "MemTotal:        4194304 kB\nMemFree:         1024 kB");
  write(proc_cgroup, "0::/pod/container");
  fs::create_directories(cgroup / "pod" / "container");

  // No memory.max anywhere: the host's memory.
  MemoryLimit limit = DiscoverMemoryLimit(cgroup, proc_cgroup, meminfo);
  EXPECT_EQ(limit.bytes, 4096ull << 20);
  EXPECT_EQ(limit.source, "MemTotal");
  EXPECT_EQ(limit.cgroup_dir, "");

  // "max" is unlimited; the pod ancestor's finite limit applies.
  write(cgroup / "pod" / "container" / "memory.max", "max");
  write(cgroup / "pod" / "memory.max", "1073741824");  // 1 GiB
  limit = DiscoverMemoryLimit(cgroup, proc_cgroup, meminfo);
  EXPECT_EQ(limit.bytes, 1ull << 30);
  EXPECT_EQ(limit.source, "cgroup memory.max (/pod)");
  EXPECT_EQ(fs::path(limit.cgroup_dir), cgroup / "pod");

  // The smaller of two finite limits wins, wherever it is.
  write(cgroup / "pod" / "container" / "memory.max", "536870912");  // 512 MiB
  limit = DiscoverMemoryLimit(cgroup, proc_cgroup, meminfo);
  EXPECT_EQ(limit.bytes, 512ull << 20);
  EXPECT_EQ(limit.source, "cgroup memory.max (/pod/container)");
  EXPECT_EQ(fs::path(limit.cgroup_dir), cgroup / "pod" / "container");

  // A cgroup limit above the host's memory does not bind.
  write(cgroup / "pod" / "container" / "memory.max", "max");
  write(cgroup / "pod" / "memory.max", "8589934592");  // 8 GiB
  limit = DiscoverMemoryLimit(cgroup, proc_cgroup, meminfo);
  EXPECT_EQ(limit.bytes, 4096ull << 20);
  EXPECT_EQ(limit.source, "MemTotal");

  // Nothing readable: no limit.
  limit = DiscoverMemoryLimit(root / "absent", root / "absent",
                              root / "absent");
  EXPECT_EQ(limit.bytes, 0u);
  fs::remove_all(root);
}

// The policy (D-198) from the bessd defaults as `mutate` adjusts them, on a
// host with `usable_hugepage_mb` of hugepages.
template <typename F>
bool Resolve(const MemoryLimit &discovered, uint64_t usable_hugepage_mb,
             F mutate, bess::utils::MemoryPolicy *policy, std::string *error) {
  MemorySettings settings;
  mutate(settings);
  return ResolveMemoryPolicy(settings, discovered, usable_hugepage_mb, policy,
                             error);
}

TEST(StartupTest, MemoryPolicyLimitAndReserve) {
  MemoryLimit discovered;
  discovered.bytes = 4096ull << 20;
  discovered.source = "cgroup memory.max (/pod)";
  discovered.cgroup_dir = "/sys/fs/cgroup/pod";
  bess::utils::MemoryPolicy policy;
  std::string error;

  // Defaults: the discovered limit, reserve max(128 MB, limit/16), normal
  // pages past the hugepages (D-206).
  ASSERT_TRUE(Resolve(discovered, 0, [](MemorySettings &) {}, &policy, &error));
  EXPECT_EQ(policy.limit_bytes, 4096ull << 20);
  EXPECT_EQ(policy.reserve_bytes, 256ull << 20);
  EXPECT_EQ(policy.limit_source, discovered.source);
  EXPECT_EQ(policy.cgroup_dir, discovered.cgroup_dir);
  EXPECT_TRUE(policy.fallback);

  // An explicit limit is bessd's RSS; the reserve's floor is 128 MB.
  // --mem_fallback=false forbids normal pages past the hugepages.
  ASSERT_TRUE(Resolve(
      discovered, 0,
      [](MemorySettings &s) {
        s.limit_mb = 1024;
        s.fallback = false;
      },
      &policy, &error));
  EXPECT_EQ(policy.limit_bytes, 1024ull << 20);
  EXPECT_EQ(policy.reserve_bytes, 128ull << 20);
  EXPECT_EQ(policy.limit_source, "--mem_limit_mb");
  EXPECT_EQ(policy.cgroup_dir, "");
  EXPECT_FALSE(policy.fallback);

  // A small limit keeps at least half of itself.
  ASSERT_TRUE(Resolve(
      discovered, 0, [](MemorySettings &s) { s.limit_mb = 200; }, &policy,
      &error));
  EXPECT_EQ(policy.reserve_bytes, 100ull << 20);

  // -1: no limit.
  ASSERT_TRUE(Resolve(
      discovered, 0,
      [](MemorySettings &s) {
        s.limit_mb = -1;
        s.reserve_mb = 64;
      },
      &policy, &error));
  EXPECT_EQ(policy.limit_bytes, 0u);
  EXPECT_EQ(policy.reserve_bytes, 64ull << 20);

  // A reserve that leaves nothing is refused.
  EXPECT_FALSE(Resolve(
      discovered, 0,
      [](MemorySettings &s) {
        s.limit_mb = 1024;
        s.reserve_mb = 1024;
      },
      &policy, &error));
  EXPECT_NE(error.find("--mem_reserve_mb=1024"), std::string::npos);
}

// The hugetlb controller's cap applies with hugepage backing, bounding the
// flag's; whether hugepages are charged is carried into the policy.
TEST(StartupTest, HugetlbLimitCapsTheHugepageHeap) {
  const MemoryLimit none{};
  MemoryLimit hugetlb;
  hugetlb.bytes = 2048ull << 20;
  hugetlb.source = "cgroup hugetlb.1GB.max (/pod)";
  bess::utils::MemoryPolicy policy;
  std::string error;
  MemorySettings settings;
  ASSERT_TRUE(ResolveMemoryPolicy(settings, none, 4096, &policy, &error, hugetlb,
                                  true));
  EXPECT_EQ(policy.hugepage_cap_bytes, 2048ull << 20);
  EXPECT_EQ(policy.hugepage_cap_source, hugetlb.source);
  EXPECT_TRUE(policy.hugetlb_charged);

  settings.hugepage_mb = 1024;  // below the cgroup's: the flag's
  ASSERT_TRUE(ResolveMemoryPolicy(settings, none, 4096, &policy, &error, hugetlb,
                                  false));
  EXPECT_EQ(policy.hugepage_cap_bytes, 1024ull << 20);
  EXPECT_EQ(policy.hugepage_cap_source, "--mem_hugepage_mb");
  EXPECT_FALSE(policy.hugetlb_charged);

  settings.hugepage_mb = 4096;  // above it: the cgroup's
  ASSERT_TRUE(ResolveMemoryPolicy(settings, none, 4096, &policy, &error, hugetlb));
  EXPECT_EQ(policy.hugepage_cap_bytes, 2048ull << 20);
  EXPECT_EQ(policy.hugepage_cap_source, hugetlb.source);

  // Normal pages: no cap, whatever the cgroup says.
  settings = MemorySettings{};
  ASSERT_TRUE(ResolveMemoryPolicy(settings, none, 0, &policy, &error, hugetlb));
  EXPECT_EQ(policy.backing, MemoryBacking::kNormal);
  EXPECT_EQ(policy.hugepage_cap_bytes, 0u);
}

TEST(StartupTest, MemoryBackingResolvesOnce) {
  const MemoryLimit none{};
  bess::utils::MemoryPolicy policy;
  std::string error;

  // auto: hugepages when any are usable, else normal pages; never kAuto.
  ASSERT_TRUE(Resolve(none, 1024, [](MemorySettings &) {}, &policy, &error));
  EXPECT_EQ(policy.backing, MemoryBacking::kHugepages);
  EXPECT_EQ(policy.hugepage_cap_bytes, 0u);
  ASSERT_TRUE(Resolve(none, 0, [](MemorySettings &) {}, &policy, &error));
  EXPECT_EQ(policy.backing, MemoryBacking::kNormal);

  // The cap is kept with hugepage backing only.
  const auto capped = [](MemorySettings &s) { s.hugepage_mb = 2048; };
  ASSERT_TRUE(Resolve(none, 4096, capped, &policy, &error));
  EXPECT_EQ(policy.hugepage_cap_bytes, 2048ull << 20);
  ASSERT_TRUE(Resolve(none, 0, capped, &policy, &error));
  EXPECT_EQ(policy.backing, MemoryBacking::kNormal);
  EXPECT_EQ(policy.hugepage_cap_bytes, 0u);

  // hugepages: none usable refuses to start, naming the flag.
  const auto hugepages = [](MemorySettings &s) {
    s.backing = MemoryBacking::kHugepages;
  };
  ASSERT_TRUE(Resolve(none, 1, hugepages, &policy, &error));
  EXPECT_EQ(policy.backing, MemoryBacking::kHugepages);
  EXPECT_FALSE(Resolve(none, 0, hugepages, &policy, &error));
  EXPECT_NE(error.find("--mem_backing=hugepages"), std::string::npos);

  // normal: whatever is usable; a hugepage cap with it is a contradiction.
  const auto normal = [](MemorySettings &s) {
    s.backing = MemoryBacking::kNormal;
  };
  ASSERT_TRUE(Resolve(none, 4096, normal, &policy, &error));
  EXPECT_EQ(policy.backing, MemoryBacking::kNormal);
  EXPECT_FALSE(Resolve(
      none, 4096,
      [](MemorySettings &s) {
        s.backing = MemoryBacking::kNormal;
        s.hugepage_mb = 512;
      },
      &policy, &error));
  EXPECT_NE(error.find("--mem_hugepage_mb=512"), std::string::npos);
}

// The library path (InitDpdk without bessd): BESSD_MEM_* over the library
// defaults, checked as the flags are.
TEST(StartupTest, MemorySettingsComeFromTheEnvironment) {
  MemorySettings settings = bess::startup::LibraryMemorySettings();
  EXPECT_EQ(settings.backing, MemoryBacking::kNormal);
  EXPECT_EQ(settings.limit_mb, -1);
  std::string error;

  std::vector<std::string> none = {"PATH=/bin"};
  auto env = Env(none);
  ASSERT_TRUE(MemorySettingsFromEnvironment(env.data(), &settings, &error));
  EXPECT_EQ(settings.backing, MemoryBacking::kNormal);
  EXPECT_TRUE(settings.fallback);  // D-206: normal pages past the hugepages

  std::vector<std::string> vars = {
      "BESSD_MEM_BACKING=hugepages", "BESSD_MEM_HUGEPAGE_MB=2048",
      "BESSD_MEM_FALLBACK=no", "BESSD_MEM_LIMIT_MB=3000",
      "BESSD_MEM_RESERVE_MB=0"};
  env = Env(vars);
  ASSERT_TRUE(MemorySettingsFromEnvironment(env.data(), &settings, &error));
  EXPECT_EQ(settings.backing, MemoryBacking::kHugepages);
  EXPECT_EQ(settings.hugepage_mb, 2048);
  EXPECT_FALSE(settings.fallback);
  EXPECT_EQ(settings.limit_mb, 3000);
  EXPECT_EQ(settings.reserve_mb, 0);

  // An invalid value names the variable and the flag, and changes nothing.
  for (const std::string bad :
       {"BESSD_MEM_BACKING=huge", "BESSD_MEM_HUGEPAGE_MB=-1",
        "BESSD_MEM_FALLBACK=maybe", "BESSD_MEM_LIMIT_MB=-2",
        "BESSD_MEM_RESERVE_MB=12MB"}) {
    std::vector<std::string> one = {bad};
    env = Env(one);
    const MemorySettings before = settings;
    EXPECT_FALSE(MemorySettingsFromEnvironment(env.data(), &settings, &error))
        << bad;
    EXPECT_NE(error.find(bad), std::string::npos) << error;
    EXPECT_NE(error.find("--mem_"), std::string::npos) << error;
    EXPECT_EQ(settings.backing, before.backing);
    EXPECT_EQ(settings.limit_mb, before.limit_mb);
  }
}

}  // namespace
