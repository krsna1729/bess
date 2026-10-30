// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "runtime/opts.h"
#include "runtime/thread_placement.h"
#include "runtime/numa_placement.h"
#include "runtime/simd_width.h"
#include "runtime/startup.h"
#include "utils/logging.h"

#include <cstdint>

#include "runtime/path.h"
#include "packet.h"

// Port this BESS instance listens on.
// Panda came up with this default number
static const int kDefaultPort = 0x02912;  // 10514 in decimal
static const char *kDefaultBindAddr = "127.0.0.1";

// TODO(barath): Rename these flags to something more intuitive.
DEFINE_bool(t, false, "Dump the size of internal data structures");
DEFINE_string(i, "/var/run/bessd.pid", "Specifies where to write the pidfile");
DEFINE_bool(f, false, "Run BESS in foreground mode.");
DEFINE_bool(k, false, "Kill existing BESS instance, if any");
DEFINE_bool(d, false, "Run BESS in debug mode (with debug log messages)");
DEFINE_bool(skip_root_check, false,
            "Skip checking that the process is running as root.");
DEFINE_string(modules, bess::runtime::ExecutableDirectory() + "modules",
              "Load modules from the specified directory");
DEFINE_bool(core_dump, false, "Generate a core dump on fatal faults");
DEFINE_bool(no_crashlog, false, "Disable the generation of a crash log file");

static bool ValidateIovaMode(const char *, const std::string &value) {
  return (value == "") || (value == "pa") || (value == "va");
}
DEFINE_string(iova, "", "DPDK IOVA mode: pa or va. Set auto if not specified");
static bool _iova_dummy[[maybe_unused]] =
    google::RegisterFlagValidator(&FLAGS_iova, &ValidateIovaMode);

static bool ValidateCoreID(const char *, int32_t value) {
  if (value == -1) {
    return true;  // automatic: bess::runtime::DefaultWorkerCore()
  }
  const auto core = bess::dataplane::CpuIdFrom(value);
  if (!core || !bess::runtime::CpuPresent(*core)) {
    LOG(ERROR) << "Invalid core ID: " << value;
    return false;
  }
  // Inside the CPU set bessd was started with (a container's cpuset, a
  // taskset): a worker pinned elsewhere would fail to start (D-027).
  if (!bess::runtime::CpuAllowed(*core)) {
    LOG(ERROR) << "Core " << value << " is not in bessd's CPU set ("
               << bess::runtime::CpuList(bess::runtime::ProcessCpus()) << ")";
    return false;
  }
  return true;
}
DEFINE_int32(c, -1,
             "Core ID for the default worker thread (-1: a free CPU of the "
             "memory node, as for any worker given no core; D-123)");
static const bool _c_dummy[[maybe_unused]] =
    google::RegisterFlagValidator(&FLAGS_c, &ValidateCoreID);

static bool ValidateNumaNode(const char *, int32_t value) {
  if (value < -1) {
    LOG(ERROR) << "Invalid NUMA node: " << value;
    return false;
  }
  return true;
}
// Process placement (D-123): one memory node per bessd.
DEFINE_int32(numa_node, -1,
             "NUMA node bessd allocates all its memory on (-1: automatic, the "
             "node with both bessd's CPUs and memory it may use). A node "
             "outside those is used anyway, with a warning");
static const bool _numa_node_dummy[[maybe_unused]] =
    google::RegisterFlagValidator(&FLAGS_numa_node, &ValidateNumaNode);
DEFINE_string(control_cpus, "auto",
              "CPUs reserved for control threads (gRPC, DPDK's main lcore, "
              "maintenance): 'auto' (the first CPU of the memory node in "
              "bessd's CPU set; none with one CPU), 'none' (control threads "
              "avoid only the workers' CPUs), or a CPU list such as '0,1'");
// Refused at parse time, as -c is: a list that is not one, or names a CPU
// outside bessd's CPU set -- the one hard placement error (D-123).
static bool ValidateControlCpus(const char *, const std::string &value) {
  if (value == "auto" || value == "none") {
    return true;
  }
  const auto cpus = bess::runtime::ParseCpuList(value);
  if (!cpus || cpus->empty()) {
    LOG(ERROR) << "--control_cpus='" << value
               << "' is neither 'auto', 'none' nor a CPU list";
    return false;
  }
  for (int cpu : *cpus) {
    const auto core = bess::dataplane::CpuIdFrom(cpu);
    if (!core || !bess::runtime::CpuAllowed(*core)) {
      LOG(ERROR) << "--control_cpus=" << value << ": core " << cpu
                 << " is not in bessd's CPU set ("
                 << bess::runtime::CpuList(bess::runtime::ProcessCpus()) << ")";
      return false;
    }
  }
  return true;
}
static const bool _control_cpus_dummy[[maybe_unused]] =
    google::RegisterFlagValidator(&FLAGS_control_cpus, &ValidateControlCpus);

static bool ValidateTCPPort(const char *, int32_t value) {
  if (value <= 0) {
    LOG(ERROR) << "Invalid TCP port number: " << value;
    return false;
  }

  return true;
}
DEFINE_string(grpc_url, "",
              "Specifies the URL where the BESS gRPC server should listen. "
              "If non empty, overrides -b and -p options.");
DEFINE_string(b, kDefaultBindAddr,
              "Specifies the IP address of the interface the BESS gRPC server "
              "should bind to, if --grpc_url is empty. Deprecated, please use"
              "--grpc_url instead");
DEFINE_int32(
    p, kDefaultPort,
    "Specifies the TCP port on which BESS listens for controller connections, "
    "if --grpc_url is empty. Deprecated, please use --grpc_url instead");
static const bool _p_dummy[[maybe_unused]] =
    google::RegisterFlagValidator(&FLAGS_p, &ValidateTCPPort);

// D-125: HTTP is off unless an address is named; the host is required, so
// serving beyond loopback is always spelled out.
DEFINE_string(http_addr, "",
              "HOST:PORT for bessd's HTTP adapter (JSON API on /v2/, server-sent "
              "events, /metrics, a status page); empty (default): no HTTP. "
              "127.0.0.1:PORT keeps it local; any other host serves the network "
              "without authentication");

// Requests whose Host is none of these, localhost, an IP address or the
// --http_addr host are refused: a web page that rebinds its DNS name to
// bessd still sends its own name (D-125).
DEFINE_string(http_allowed_hosts, "",
              "Comma-separated DNS names (no port) HTTP requests may carry as "
              "Host besides localhost, IP addresses and the --http_addr host, "
              "e.g. the name a scraper uses for a non-loopback --http_addr");

// How often the maintenance loop delivers worker-to-control module requests
// (TP4, D-077): the longest a posted request waits. 0 disables the loop.
DEFINE_int32(maintenance_interval_us, 1000,
             "Interval of the control-side maintenance loop in microseconds (0: off)");

// Tracing is DPDK's (M25, D-089): its trace points (EAL, ethdev, mempool,
// ...) written as CTF for babeltrace or Trace Compass, not a BESS tracer.
DEFINE_string(dpdk_trace, "",
              "Enable DPDK trace points matching this regular expression "
              "(EAL --trace), e.g. 'lib.ethdev.*'. Empty: tracing off");
DEFINE_string(dpdk_trace_dir, "",
              "Where DPDK writes the trace (EAL --trace-dir; default: "
              "$HOME/dpdk-traces)");
DEFINE_string(pci_allow, "",
              "Comma-separated PCI addresses DPDK may probe (its -a list). "
              "Empty: the addresses a device plugin assigned "
              "(PCIDEVICE_* environment), else every device DPDK can use");

// Dataplane memory (D-198): one flag family, resolved once at startup
// (startup::ApplyMemorySettings) and logged in one line. Each is also
// BESSD_<FLAG> (D-030), and the same variables configure a process that
// brings DPDK up without bessd (InitDpdk: tests, benchmarks, library users).
static bool ValidateMemoryBacking(const char *, const std::string &value) {
  if (!bess::startup::ParseMemoryBacking(value)) {
    LOG(ERROR) << "Invalid --mem_backing: '" << value
               << "' (auto, hugepages or normal)";
    return false;
  }
  return true;
}
DEFINE_string(mem_backing, "auto",
              "What DPDK memory is: 'auto' (default): hugepages if any are "
              "usable at startup (host-free pages within the cgroup's hugetlb "
              "limit), mapped as needed, else normal pages with a warning; "
              "'hugepages': refuse to start without usable hugepages; "
              "'normal': normal (4 KiB) pages only (DPDK --no-huge, a heap of "
              "--mem_limit_mb less --mem_reserve_mb, 512 MB with no limit)");
static const bool _mem_backing_dummy[[maybe_unused]] =
    google::RegisterFlagValidator(&FLAGS_mem_backing, &ValidateMemoryBacking);
static bool ValidateHugepageMegabytes(const char *flag, int64_t value) {
  if (!bess::startup::ValidHugepageMegabytes(value)) {
    LOG(ERROR) << "Invalid --" << flag << ": " << value;
    return false;
  }
  return true;
}
DEFINE_int64(mem_hugepage_mb, 0,
             "Cap in MB on the hugepage heap on the memory node (other nodes "
             "get none, D-123); tables past it come from normal pages, or "
             "are refused with --mem_fallback=false. 0 (default): no cap "
             "beyond the host's or container's hugepages. Not with "
             "--mem_backing=normal");
static const bool _mem_hugepage_mb_dummy[[maybe_unused]] =
    google::RegisterFlagValidator(&FLAGS_mem_hugepage_mb,
                                  &ValidateHugepageMegabytes);
DEFINE_bool(mem_fallback, true,
            "When hugepages run out (or reach --mem_hugepage_mb), allocate "
            "tables and packet buffers from normal pages (slower lookups; "
            "admitted against the memory limit; reported per resource, "
            "D-206). Default on; --mem_fallback=false refuses instead");
static bool ValidateMemoryMegabytes(const char *flag, int64_t value) {
  if (!bess::startup::ValidMemoryMegabytes(value)) {
    LOG(ERROR) << "Invalid --" << flag << ": " << value;
    return false;
  }
  return true;
}
DEFINE_int64(mem_limit_mb, 0,
             "Memory limit in MB that table creations and transactions are "
             "refused past, rather than bessd being OOM-killed; with "
             "--mem_backing=normal it also sizes DPDK's heap (limit less "
             "reserve). 0 (default): the cgroup's memory.max, else the host's "
             "MemTotal; -1: no limit; N: N MB of bessd's resident memory");
static const bool _mem_limit_mb_dummy[[maybe_unused]] =
    google::RegisterFlagValidator(&FLAGS_mem_limit_mb, &ValidateMemoryMegabytes);
DEFINE_int64(mem_reserve_mb, -1,
             "MB of the memory limit kept free for memory bessd does not "
             "account (gRPC, glibc, thread stacks). -1 (default): "
             "max(128, limit/16), at most half the limit");
static const bool _mem_reserve_mb_dummy[[maybe_unused]] =
    google::RegisterFlagValidator(&FLAGS_mem_reserve_mb,
                                  &ValidateMemoryMegabytes);

// One SIMD width for DPDK and BESS's kernels, set before the EAL starts
// (D-126).
DEFINE_string(simd_width, "auto",
              "Widest SIMD paths DPDK and BESS kernels may take: 'auto' "
              "(DPDK's default: 256 bits on x86, so no AVX-512), '256' or "
              "'512'. Wider than the CPU: a warning, and the CPU's width");
static bool ValidateSimdWidth(const char *, const std::string &value) {
  if (!bess::runtime::ParseSimdWidthRequest(value)) {
    LOG(ERROR) << "--simd_width='" << value
               << "' is neither 'auto', '256' nor '512'";
    return false;
  }
  return true;
}
static const bool _simd_width_dummy[[maybe_unused]] =
    google::RegisterFlagValidator(&FLAGS_simd_width, &ValidateSimdWidth);

static bool ValidateBuffersPerSocket(const char *, int32_t value) {
  if (value <= 0) {
    LOG(ERROR) << "Invalid number of buffers: " << value;
    return false;
  }
  if (value & (value - 1)) {
    LOG(ERROR) << "Number of buffers must be a power of 2: " << value;
    return false;
  }
  return true;
}
DEFINE_int32(buffers, 262144,
             "Packet buffers in the default pool (one pool, on the memory "
             "node), must be a power of 2.");
static const bool _buffers_dummy[[maybe_unused]] =
    google::RegisterFlagValidator(&FLAGS_buffers, &ValidateBuffersPerSocket);

static bool ValidatePacketDataRoom(const char *, uint32_t value) {
  if (value == 0 || value > bess::kMaxPacketDataSize) {
    LOG(ERROR) << "Invalid packet data room: " << value
               << " (must be in [1," << bess::kMaxPacketDataSize << "])";
    return false;
  }
  return true;
}
DEFINE_uint32(packet_data_room, bess::kDefaultPacketDataSize,
              "Payload bytes in each packet mbuf data room.");
static const bool _packet_data_room_dummy[[maybe_unused]] =
    google::RegisterFlagValidator(&FLAGS_packet_data_room,
                                  &ValidatePacketDataRoom);
