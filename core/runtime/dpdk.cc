// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "dpdk.h"
#include "runtime/numa_placement.h"
#include "runtime/thread_placement.h"
#include "runtime/simd_width.h"
#include "runtime/startup.h"

#include <syslog.h>
#include <unistd.h>

#include "utils/logging.h"
#include "utils/sanitizers.h"
#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_trace.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>

#include "arch/dpdk_simd.h"
#include "runtime/opts.h"
#include "utils/dpdk_memory.h"

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

// The EAL arguments follow the memory policy resolved before it
// (startup::ApplyMemorySettings, D-198): hugepages -> dynamic memory with
// the --mem_hugepage_mb cap; normal -> --no-huge with a heap sized from the
// limit. Never auto: resolving auto (hugepages if usable) happened there.
void init_eal(const utils::MemoryPolicy &policy,
              std::string nonworker_corelist) {
  CHECK(policy.backing != utils::MemoryBacking::kAuto)
      << "the memory policy is resolved before the EAL starts";
  CmdLineOpts rte_args{
      "bessd",
      // DPDK renamed --master-lcore to --main-lcore upstream.
      "--main-lcore",
      std::to_string(RTE_MAX_LCORE - 1),
      // DPDK's lcore-affinity ("id@cpuset") option is --lcores (plural);
      // --lcore was accepted in older DPDK but no longer exists.
      // Commas inside the CPU set must not start separate lcore mappings.
      "--lcores",
      std::to_string(RTE_MAX_LCORE - 1) + "@(" + nonworker_corelist + ")",
  };

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

  if (policy.backing == utils::MemoryBacking::kNormal) {
    // Do not bother with /var/run/.rte_config and .rte_hugepage_info,
    // since we don't want to interfere with other DPDK applications.
    rte_args.Append({"--no-shconf"});
    // DPDK renamed --iova to --iova-mode upstream.
    rte_args.Append({"--iova-mode", (FLAGS_iova != "") ? FLAGS_iova : "va"});
    rte_args.Append({"--no-huge"});

    // even if we opt out of using hugepages, many DPDK libraries still rely on
    // rte_malloc (e.g., rte_lpm), so we need to reserve some (normal page)
    // memory in advance: DPDK's --no-huge heap is fixed at its -m MB. bessd
    // sizes it to what its memory policy allows (D-198): the limit less the
    // reserve, so the heap is not what refuses a table -- the limit is, as
    // DpdkAllocate admits each allocation against it. Without a known limit
    // (--mem_limit_mb=-1; the library default for tests, benchmarks and
    // library users bringing DPDK up lazily) it is 512 MB. A benchmark that
    // needs a bigger heap sets BESSD_MEM_LIMIT_MB (and BESSD_MEM_RESERVE_MB).
    //
    // [INFERENCE, to be measured] A heap page untouched is not resident: the
    // region is mapped (memfd, MAP_SHARED) and only written as allocations
    // use it, so a large heap costs address space, not memory -- except
    // DPDK's per-page segment bookkeeping (an rte_memseg per 4 KiB, written
    // at init, 48 bytes each), about 1/85 of the heap size.
    size_t heap_mb = 512;
    if (policy.limit_bytes > policy.reserve_bytes) {
      heap_mb = (policy.limit_bytes - policy.reserve_bytes) >> 20;
#if BESS_ADDRESS_SANITIZER
      // The heap must fit below ASan's shadow (see --base-virtaddr below).
      heap_mb = std::min<size_t>(heap_mb, 1536);
#endif
    }
    rte_args.Append({"-m", std::to_string(heap_mb)});
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
    // --mem_hugepage_mb on the memory node when it sets a cap. Nothing is
    // mapped on the packet path: every dataplane structure is allocated on
    // the control path.
    //
    // DPDK's limit is exclusive: an allocation fails once the heap would
    // reach it (eal_memalloc_mem_alloc_validate: `limit > new_len` passes),
    // so a cap of exactly one 1 GiB page rejected that page and bessd could
    // not start. A cap of N MB means the heap may reach N MB: pass N + 1.
    //
    // One memory node per bessd (D-123): every other node gets a 1 MB limit,
    // which no hugepage fits under, so nothing is ever mapped there. (DPDK
    // reads a limit of 0 as "no limit".) The memory node keeps the cap, or
    // no limit. DPDK takes at most RTE_MAX_NUMA_NODES entries (node ids
    // past that are not DPDK sockets at all).
    const size_t cap_mb = policy.hugepage_cap_bytes >> 20;
    const runtime::Placement &placement = runtime::ProcessPlacement();
    const int memory_node = placement.node;
    // A node DPDK cannot name is not confined (the warning says so): every
    // node gets the cap, or no limit, as before D-123.
    const bool confined = memory_node < RTE_MAX_NUMA_NODES;
    if (!confined) {
      LOG(WARNING) << "memory node " << memory_node << " is beyond DPDK's "
                   << RTE_MAX_NUMA_NODES << " NUMA nodes: hugepage memory is "
                   << "not confined to it";
    }
    int nodes = memory_node + 1;  // an entry for every node with memory
    for (int node : placement.topology.memory_nodes) {
      nodes = std::max(nodes, node + 1);
    }
    nodes = std::min(nodes, RTE_MAX_NUMA_NODES);
    if (cap_mb > 0 || (confined && nodes > 1)) {
      const std::string cap = cap_mb > 0 ? std::to_string(cap_mb + 1) : "0";
      std::string limit;
      for (int i = 0; i < nodes; i++) {
        const bool own = !confined || i == memory_node;
        limit += (i == 0 ? "" : ",") + (own ? cap : std::string("1"));
      }
      rte_args.Append({"--socket-limit", limit});
    }

    // No hugetlbfs files and no runtime directory (memfd-backed; implies
    // what --no-shconf and --huge-unlink did): nothing to clean up, nothing
    // shared with other DPDK processes -- what a container wants. One file
    // descriptor per memory segment list instead of one per page.
    rte_args.Append({"--in-memory", "--single-file-segments"});
  }

  // The SIMD width (D-126), decided once before the EAL: under auto nothing,
  // else --force-max-simd-bitwidth, which DPDK applies while parsing these
  // arguments and then holds (rte_vect_set_max_simd_bitwidth() returns
  // -EPERM). Setting it through that function before rte_eal_init() would
  // not survive: rte_eal_init() resets its internal configuration first.
  for (const std::string &arg : runtime::ProcessSimdWidth().eal_args) {
    rte_args.Append({arg});
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

  if (!FLAGS_dpdk_trace.empty()) {
    rte_args.Append({"--trace", FLAGS_dpdk_trace});
    if (!FLAGS_dpdk_trace_dir.empty()) {
      rte_args.Append({"--trace-dir", FLAGS_dpdk_trace_dir});
    }
  }

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
  // What DPDK applies (D-126): bessd's width on x86, DPDK's default on arm64.
  const uint16_t dpdk_width = arch::DpdkMaxSimdWidth();
  LOG(INFO) << "DPDK SIMD limit: "
            << (dpdk_width >= arch::kDpdkUnlimitedSimdWidth ? std::string("unlimited")
                                                            : std::to_string(dpdk_width));

  rte_openlog_stream(fopencookie(nullptr, "w", dpdk_log_funcs));
}

// Where DPDK's main lcore may run, in "corelist" format (e.g. "0-12,16-28"):
// in bessd the reserved control CPUs (D-123), else -- none reserved, or a
// test or benchmark bringing DPDK up lazily -- the CPU set the process was
// started with (D-027), which the main thread and every thread created
// after it keep.
std::string GetNonWorkerCoreList() {
  cpu_set_t set = bess::runtime::ReservedControlCpus();
  if (CPU_COUNT(&set) == 0) {
    set = bess::runtime::ProcessCpus();
  }
  std::string corelist = bess::runtime::CpuList(set);
  if (corelist.empty()) {
    // This should never happen, but just in case...
    PLOG(WARNING) << "No core is allowed for the process?";
    corelist = "0";
  }
  return corelist;
}

bool is_initialized = false;

}  // namespace

void SaveDpdkTrace() {
  if (!IsDpdkInitialized() || !rte_trace_is_enabled()) {
    return;
  }
  if (int ret = rte_trace_save(); ret != 0) {
    LOG(ERROR) << "rte_trace_save() failed: " << ret;
  } else {
    LOG(INFO) << "DPDK trace saved";
  }
}

bool IsDpdkInitialized() {
  return is_initialized;
}

void InitDpdk() {
  if (!is_initialized) {
    is_initialized = true;
    // bessd's main() resolved its --mem_* flags before its first pool. A
    // process without bessd -- a test, a benchmark, a library user bringing
    // the EAL up lazily -- resolves the same settings from BESSD_MEM_* here,
    // starting from normal pages and no limit (a 512 MB --no-huge heap);
    // BESSD_MEM_BACKING=hugepages opts it into hugepage memory (so tables
    // beyond the TLB reach are measured the way a hugepage bessd runs
    // them). An unprivileged process cannot resolve physical addresses, so
    // that path uses IOVA-as-VA unless -iova says otherwise.
    if (utils::memory_policy().backing == utils::MemoryBacking::kAuto) {
      startup::MemorySettings settings = startup::LibraryMemorySettings();
      std::string error;
      if (!startup::MemorySettingsFromEnvironment(environ, &settings, &error) ||
          !startup::ApplyMemorySettings(settings, &error)) {
        LOG(FATAL) << error;
      }
      if (utils::memory_policy().backing == utils::MemoryBacking::kHugepages &&
          FLAGS_iova.empty()) {
        FLAGS_iova = "va";
      }
    }
    init_eal(utils::memory_policy(), GetNonWorkerCoreList());
    // The hugepage heap grows only past the validator (D-198): the cap, and
    // the memory limit where hugepages are charged to memory.max.
    utils::InstallHeapValidator();
  }
}

}  // namespace bess
