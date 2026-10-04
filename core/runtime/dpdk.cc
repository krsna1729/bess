// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "dpdk.h"
#include "runtime/thread_placement.h"
#include "runtime/startup.h"

#include <syslog.h>
#include <unistd.h>

#include "utils/logging.h"
#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "runtime/memory.h"
#include "runtime/opts.h"

// AddressSanitizer: GCC defines __SANITIZE_ADDRESS__, clang reports it through
// __has_feature.
#if defined(__SANITIZE_ADDRESS__)
#define BESS_ADDRESS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define BESS_ADDRESS_SANITIZER 1
#endif
#endif
#ifndef BESS_ADDRESS_SANITIZER
#define BESS_ADDRESS_SANITIZER 0
#endif

namespace bess {
namespace {

void disable_syslog() {
  setlogmask(0x01);
}

void enable_syslog() {
  setlogmask(0xff);
}

// for log messages during rte_eal_init()
ssize_t dpdk_log_init_writer(void *, const char *data, size_t len) {
  enable_syslog();
  LOG(INFO) << std::string(data, len);
  disable_syslog();
  return len;
}

ssize_t dpdk_log_writer(void *, const char *data, size_t len) {
  LOG(INFO) << std::string(data, len);
  return len;
}

class CmdLineOpts {
 public:
  explicit CmdLineOpts(std::initializer_list<std::string> args)
      : args_(), argv_({nullptr}) {
    Append(args);
  }

  void Append(std::initializer_list<std::string> args) {
    for (const std::string &arg : args) {
      args_.emplace_back(arg.begin(), arg.end());
      args_.back().push_back('\0');
      argv_.insert(argv_.begin() + argv_.size() - 1, args_.back().data());
    }
  }

  char **Argv() { return argv_.data(); }

  int Argc() const { return args_.size(); }

  std::string Dump() {
    std::ostringstream os;
    os << "[";
    for (size_t i = 0; i < args_.size(); i++) {
      os << (i == 0 ? "" : ", ") << '"' << args_[i].data() << '"';
    }
    os << "]";
    return os.str();
  }

 private:
  // Contains a copy of each argument.
  std::vector<std::vector<char>> args_;
  // Pointer to each argument (in `args_`), plus an extra `nullptr`.
  std::vector<char *> argv_;
};

void init_eal(int dpdk_mb_per_socket, std::string nonworker_corelist) {
  CmdLineOpts rte_args{
      "bessd",
      // DPDK renamed --master-lcore to --main-lcore upstream.
      "--main-lcore",
      std::to_string(RTE_MAX_LCORE - 1),
      // DPDK's lcore-affinity ("id@cpuset") option is --lcores (plural);
      // --lcore was accepted in older DPDK but no longer exists.
      "--lcores",
      std::to_string(RTE_MAX_LCORE - 1) + "@" + nonworker_corelist,
  };

  // -m -1: host-free hugepages within the remaining hugetlb capacity of this
  // process's cgroup and its ancestors; otherwise normal pages (D-030).
  if (dpdk_mb_per_socket < 0) {
    const uint64_t usable = startup::UsableHugepageMB(
        "/sys/kernel/mm/hugepages", "/sys/fs/cgroup", "/proc/self/cgroup");
    if (usable == 0) {
      LOG(WARNING) << "No usable hugepages: DPDK memory is normal pages "
                      "(512 MB). Give bessd hugepages for production.";
      dpdk_mb_per_socket = 0;
    } else {
      LOG(INFO) << usable << " MB of hugepages usable; mapped as needed";
    }
  }

  // The NICs DPDK may probe: -pci_allow, else what a device plugin assigned
  // this container; with neither, every device DPDK can use.
  std::vector<std::string> allow;
  if (!FLAGS_pci_allow.empty()) {
    std::stringstream list(FLAGS_pci_allow);
    std::string address;
    while (std::getline(list, address, ',')) {
      if (!address.empty()) {
        allow.push_back(address);
      }
    }
  } else {
    allow = startup::DevicePluginPciAddresses(environ);
  }
  for (const std::string &address : allow) {
    rte_args.Append({"-a", address});
  }

  if (dpdk_mb_per_socket == 0) {
    // Do not bother with /var/run/.rte_config and .rte_hugepage_info,
    // since we don't want to interfere with other DPDK applications.
    rte_args.Append({"--no-shconf"});
    // DPDK renamed --iova to --iova-mode upstream.
    rte_args.Append({"--iova-mode", (FLAGS_iova != "") ? FLAGS_iova : "va"});
    rte_args.Append({"--no-huge"});

    // even if we opt out of using hugepages, many DPDK libraries still rely on
    // rte_malloc (e.g., rte_lpm), so we need to reserve some (normal page)
    // memory in advance. We allocate 512MB (this is shared among nodes).
    // BESS_DPDK_NOHUGE_MB=<MB> (tests and benchmarks only) sizes this heap;
    // unset, it is 512.
    const char *heap_mb = std::getenv("BESS_DPDK_NOHUGE_MB");
    rte_args.Append({"-m", (heap_mb != nullptr && std::atoi(heap_mb) > 0)
                               ? std::string(heap_mb)
                               : std::string("512")});
#if BESS_ADDRESS_SANITIZER
    // Under AddressSanitizer the default heap address lands inside ASan's
    // shadow region, so DPDK maps the heap high, above the IOMMU's DMA mask
    // (39 bits on VT-d) that IOVA-as-VA requires: "IOVA exceeding limits of
    // current DMA mask". Below the shadow (which starts at 0x7fff8000) there
    // are about 1.7 GB from 256 MB; ASan builds only (M22, D-072).
    rte_args.Append({"--base-virtaddr", "0x10000000"});
#endif
  } else {
    // IOVA mode: the EAL's own choice unless -iova says otherwise -- VA
    // when an IOMMU is present and every device supports it (vfio-pci: the
    // deployment norm, DMA confined by the IOMMU, no physical addresses
    // needed), PA where hardware requires it. BESS used to force PA here.
    if (!FLAGS_iova.empty()) {
      rte_args.Append({"--iova-mode", FLAGS_iova});
    }

    // Dynamic memory (D-029): hugepages are mapped as DPDK's heap needs
    // them -- packet pools at startup, tables when they are created -- within
    // whatever the host or a container's hugetlb limit allows, and at most
    // `dpdk_mb_per_socket` per socket when -m sets a cap. Nothing is mapped
    // on the packet path: every dataplane structure is allocated on the
    // control path.
    //
    // DPDK's limit is exclusive: an allocation fails once the heap would
    // reach it (eal_memalloc_mem_alloc_validate: `limit > new_len` passes),
    // so a cap of exactly one 1 GiB page rejected that page and bessd could
    // not start. -m N means the heap may reach N MB: pass N + 1.
    if (dpdk_mb_per_socket > 0) {
      const std::string cap = std::to_string(dpdk_mb_per_socket + 1);
      std::string limit = cap;
      for (int i = 1; i < NumNumaNodes(); i++) {
        limit += "," + cap;
      }
      rte_args.Append({"--socket-limit", limit});
    }

    // No hugetlbfs files and no runtime directory (memfd-backed; implies
    // what --no-shconf and --huge-unlink did): nothing to clean up, nothing
    // shared with other DPDK processes -- what a container wants. One file
    // descriptor per memory segment list instead of one per page.
    rte_args.Append({"--in-memory", "--single-file-segments"});
  }

  // reset getopt()
  optind = 0;

  // DPDK creates duplicated outputs (stdout and syslog).
  // We temporarily disable syslog, then set our log handler
  cookie_io_functions_t dpdk_log_init_funcs;
  cookie_io_functions_t dpdk_log_funcs;

  std::memset(&dpdk_log_init_funcs, 0, sizeof(dpdk_log_init_funcs));
  std::memset(&dpdk_log_funcs, 0, sizeof(dpdk_log_funcs));

  dpdk_log_init_funcs.write = &dpdk_log_init_writer;
  dpdk_log_funcs.write = &dpdk_log_writer;

  FILE *org_stdout = stdout;
  stdout = fopencookie(nullptr, "w", dpdk_log_init_funcs);

  disable_syslog();
  LOG(INFO) << "Initializing DPDK EAL with options: " << rte_args.Dump();
  int ret = rte_eal_init(rte_args.Argc(), rte_args.Argv());
  if (ret < 0) {
    LOG(FATAL) << "rte_eal_init() failed: ret = " << ret
               << " rte_errno = " << rte_errno << " ("
               << rte_strerror(rte_errno) << ")";
  }

  enable_syslog();
  fclose(stdout);
  stdout = org_stdout;

  rte_openlog_stream(fopencookie(nullptr, "w", dpdk_log_funcs));
}

// The CPU set bessd was started with, in "corelist" format (e.g.
// "0-12,16-28"): DPDK's main lcore may run anywhere in it (D-027).
std::string GetNonWorkerCoreList() {
  std::string corelist;
  const cpu_set_t set = bess::runtime::ProcessCpus();

  // Choose the last core available
  for (int i = 0; i < CPU_SETSIZE; i++) {
    if (CPU_ISSET(i, &set)) {
      int start = i;
      while (i < CPU_SETSIZE && CPU_ISSET(i, &set)) {
        i++;
      }
      int end = i - 1;

      std::string group = std::to_string(start);
      if (start < end) {
        group += "-" + std::to_string(end);
      }

      if (corelist == "") {
        corelist += group;
      } else {
        corelist += "," + group;
      }
    }
  }

  if (corelist == "") {
    // This should never happen, but just in case...
    PLOG(WARNING) << "No core is allowed for the process?";
    corelist = "0";
  }

  return corelist;
}

bool is_initialized = false;

}  // namespace

bool IsDpdkInitialized() {
  return is_initialized;
}

void InitDpdk(int dpdk_mb_per_socket) {
  if (!is_initialized) {
    is_initialized = true;
    // Test and benchmark binaries bring the EAL up lazily with no hugepages.
    // BESS_DPDK_HUGEPAGE_MB=<MB per socket> opts them into hugepage memory
    // (so tables beyond the TLB reach are measured the way a hugepage bessd
    // runs them). An unprivileged process cannot resolve physical addresses,
    // so this path uses IOVA-as-VA unless -iova says otherwise.
    if (dpdk_mb_per_socket == 0) {
      if (const char *env = std::getenv("BESS_DPDK_HUGEPAGE_MB")) {
        const int mb = std::atoi(env);
        if (mb > 0) {
          dpdk_mb_per_socket = mb;
          if (FLAGS_iova.empty()) {
            FLAGS_iova = "va";
          }
        }
      }
    }
    init_eal(dpdk_mb_per_socket, GetNonWorkerCoreList());
  }
}

}  // namespace bess
