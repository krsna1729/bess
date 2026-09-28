// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// Container-friendly startup (Decision D-030): flags from the environment,
// usable hugepages from sysfs and the cgroup, and the NICs a device plugin
// assigned.

#include "startup.h"

#include <gflags/gflags.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "opts.h"

namespace {

namespace fs = std::filesystem;
using bess::startup::ApplyEnvironmentFlags;
using bess::startup::DevicePluginPciAddresses;
using bess::startup::UsableHugepageMB;

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

}  // namespace
