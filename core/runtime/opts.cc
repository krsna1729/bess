// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "runtime/opts.h"
#include "runtime/thread_placement.h"
#include "runtime/numa_placement.h"
#include "runtime/simd_width.h"
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

// Note: currently BESS-managed hugepages do not support VFIO driver,
//       so DPDK is default for now.
DEFINE_bool(dpdk, true, "Let DPDK manage hugepages");

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
  const auto core = bess::runtime::CpuIdFrom(value);
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
    const auto core = bess::runtime::CpuIdFrom(cpu);
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

static bool ValidateMegabytesPerSocket(const char *, int32_t value) {
  if (value < -1) {
    LOG(ERROR) << "Invalid memory size: " << value;
    return false;
  }

  return true;
}
// How often the maintenance loop delivers worker-to-control module requests
// (TP4, D-077): the longest a posted request waits. 0 disables the loop.
DEFINE_int32(maintenance_interval_us, 1000,
             "Interval of the control-side maintenance loop in microseconds (0: off)");

DEFINE_int32(m, -1,
             "DPDK memory cap in MB on the memory node (other nodes get "
             "none, D-123). -1 (default): hugepages if "
             "any are usable, mapped as needed with no cap beyond the host or "
             "container limit, else normal pages; 0: no hugepages; N: "
             "hugepages, at most N MB");
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
static const bool _m_dummy[[maybe_unused]] =
    google::RegisterFlagValidator(&FLAGS_m, &ValidateMegabytesPerSocket);

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
