// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include <memory>
#include <string>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_launch.h>

#include "utils/logging.h"
#include <gflags/gflags.h>

#include "dataplane/batch_tuning.h"
#include "api_server.h"
#include "bessctl/cli.h"
#include "bessd.h"
#include "cli_mode.h"
#include "framework/plugin_loader.h"
#include "control/http_server.h"
#include "debug.h"
#include "runtime/opts.h"
#include "runtime/numa_placement.h"
#include "runtime/malloc_policy.h"
#include "runtime/simd_width.h"
#include "packet_pool.h"
#include "runtime/startup.h"
#include "port.h"
#include "utils/dpdk_memory.h"
#include "utils/format.h"
#include "version.h"
#include "worker.h"

int main(int argc, char *argv[]) {
  // bessctl is this binary (D-120): the client runs before any daemon setup,
  // without privileges, on its own argument parser.
  if (const auto offset = bess::cli_mode::CliArgvOffset(argc, argv)) {
    if (!bess::cli_mode::DropPrivileges()) {
      return 1;
    }
    argv[*offset] = argv[0];
    return bess::ctl::Main(argc - *offset, argv + *offset);
  }

  bess::startup::BlockTerminationSignals();  // before any thread exists
  bess::runtime::CapMallocArenas();  // RT-1: also before any thread exists
  FLAGS_logbuflevel = -1;
  FLAGS_colorlogtostderr = true;
  google::InitGoogleLogging(argv[0]);
  // See debug.cc's InstallFailureFunction call sites for why this cast is
  // needed under Clang (GoPanic's [[noreturn]] doesn't implicitly convert
  // to glog's __attribute__((noreturn))-spelled function pointer type).
  google::InstallFailureFunction(
      reinterpret_cast<google::logging_fail_func_t>(bess::debug::GoPanic));
  bess::debug::SetTrapHandler();

  google::SetVersionString(VERSION);
  google::SetUsageMessage("BESS Command Line Options:");
  google::ParseCommandLineFlags(&argc, &argv, true);
  // BESSD_<FLAG> environment variables for flags not on the command line
  // (D-030).
  const std::vector<std::string> from_env =
      bess::startup::ApplyEnvironmentFlags(environ);
  bess::bessd::ProcessCommandLineArgs();
  for (const std::string &name : from_env) {
    LOG(INFO) << "Flag from the environment: " << name;
  }
  bess::bessd::CheckRunningAsRoot();

  std::string grpc_url = FLAGS_grpc_url;
  if (grpc_url.empty()) {
    grpc_url = bess::utils::Format("%s:%d", FLAGS_b.c_str(), FLAGS_p);
  }

  // Decision D-031 (docs/decisions.md).
  // Daemons keep a pidfile so -k can restart the same RPC endpoint. Distinct
  // endpoints get distinct default pidfiles; foreground PID 1 needs no file
  // unless -i is explicit (its filesystem may be read-only).
  gflags::CommandLineFlagInfo pidfile_flag;
  gflags::GetCommandLineFlagInfo("i", &pidfile_flag);
  const bool use_pidfile = !FLAGS_f || !pidfile_flag.is_default;
  if (use_pidfile && pidfile_flag.is_default) {
    FLAGS_i = bess::bessd::PidfilePathForRpcAddress(FLAGS_i, grpc_url);
  }
  int pidfile_fd = use_pidfile ? bess::bessd::CheckUniqueInstance(FLAGS_i) : -1;
  ignore_result(bess::bessd::SetResourceLimit());

  // HTTP (D-125), when asked for: bound before daemonizing, so a bad or
  // taken address fails this command with the reason on its terminal.
  std::unique_ptr<bess::control::HttpServer> http;
  if (!FLAGS_http_addr.empty()) {
    http = std::make_unique<bess::control::HttpServer>();
    std::string error;
    if (!http->Bind(FLAGS_http_addr, FLAGS_http_allowed_hosts, &error)) {
      LOG(ERROR) << error;
      return 1;
    }
  }

  int signal_fd = -1;
  if (FLAGS_f) {
    LOG(INFO) << "Launching BESS daemon in process mode...";
  } else {
    LOG(INFO) << "Launching BESS daemon in background...";

    if (FLAGS_logtostderr == true || FLAGS_alsologtostderr == true) {
      FLAGS_logtostderr = false;
      FLAGS_alsologtostderr = false;
      LOG(WARNING) << "Daemon doesn't get attached to stdio. "
                      "-logtostderr and -alsologtostderr options are ignored";
    }
    signal_fd = bess::bessd::Daemonize();
  }

  LOG(INFO) << "bessd " << google::VersionString();

  // Store our PID (child's, if daemonized) in the PID file.
  if (use_pidfile) {
    bess::bessd::WritePidfile(pidfile_fd, getpid());
  }

  // One memory node, control CPUs (D-123): resolved and logged once, and
  // the node preferred for normal memory before any thread exists.
  bess::runtime::ApplyProcessPlacement();

  // The SIMD width DPDK and BESS's kernels use (D-126): resolved, logged and
  // applied to the kernels once, before plugins load and the EAL starts.
  bess::runtime::ProcessSimdWidth();

  // Load plugins
  if (!bess::framework::LoadPlugins(FLAGS_modules)) {
    PLOG(WARNING) << "LoadPlugins() failed to load from directory: "
                  << FLAGS_modules;
  }

  // Dataplane memory policy (D-198): the --mem_* flags resolved once against
  // this host -- the backing, the hugepage cap, the fallback, the limit and
  // the reserve -- logged in one line and set before the EAL starts (which
  // maps it) and before any pool or table exists. --mem_backing=hugepages
  // without usable hugepages, or an inconsistent combination, stops here.
  {
    bess::startup::MemorySettings settings;
    // Validated with the flag (opts.cc); checked again rather than assumed.
    const auto backing = bess::startup::ParseMemoryBacking(FLAGS_mem_backing);
    if (!backing) {
      LOG(ERROR) << "Invalid --mem_backing: '" << FLAGS_mem_backing << "'";
      return 1;
    }
    settings.backing = *backing;
    settings.hugepage_mb = FLAGS_mem_hugepage_mb;
    settings.fallback = FLAGS_mem_fallback;
    settings.limit_mb = FLAGS_mem_limit_mb;
    settings.reserve_mb = FLAGS_mem_reserve_mb;
    std::string error;
    if (!bess::startup::ApplyMemorySettings(settings, &error)) {
      LOG(ERROR) << error;
      return 1;
    }
  }

  bess::PacketPool::CreateDefaultPools(FLAGS_buffers, FLAGS_packet_data_room);

  PortBuilder::InitDrivers();

  // Host facts that table-build-time tuning uses (docs/dataplane-tables.md,
  // "What is tuned, and when"). Computed here once so they are logged; every
  // table built later reuses the cached values.
  {
    const auto &cache = bess::dataplane::CacheGeometry::Smallest();
    LOG(INFO) << "Cache geometry (smallest over online CPUs): L1d "
              << cache.l1d_bytes / 1024 << " KiB, L2 " << cache.l2_bytes / 1024
              << " KiB, L3 " << cache.l3_bytes / 1024 << " KiB, line "
              << cache.line_bytes << " B; lookup body override: "
              << bess::dataplane::LookupBodyName(
                     bess::dataplane::LookupBodyOverride());
  }

  {
    ApiServer server;

    server.Listen(grpc_url);
    if (http) {
      server.ServeHttp(std::move(http));
    }

    // Signal the parent that all initialization has been finished.
    if (!FLAGS_f) {
      uint64_t one = 1;
      if (write(signal_fd, &one, sizeof(one)) < 0) {
        PLOG(FATAL) << "write(signal_fd)";
      }
      close(signal_fd);
    }

    server.Run();
  }

  rte_eal_mp_wait_lcore();

  // The normal shutdown path -- Admin.Shutdown's detached thread, or the
  // SIGTERM/SIGINT watcher -- only closes the event log and stops the server
  // (server->Shutdown with a 3 s deadline); nothing there stops or joins a
  // worker. So we get here with worker threads alive (paused by the client
  // before it asked, or still running) and still registered as RCU readers.
  // Stop them in order -- pause, quit, join, which also unregisters each
  // reader -- before static destruction tears down the runtime (and its
  // RcuDomain) under them. Detaching them instead left live readers behind and
  // aborted on the domain's registered-reader check.
  destroy_all_workers();

  LOG(INFO) << "BESS daemon has been gracefully shut down";

  return 0;
}
