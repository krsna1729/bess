// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "worker.h"

#include <pthread.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <glog/logging.h>
#include <rte_config.h>
#include <rte_errno.h>
#include <rte_lcore.h>

#include <cassert>
#include <climits>
#include <cstring>
#include <list>
#include <string>
#include <utility>

#include "metadata.h"
#include "module.h"
#include "packet_pool.h"
#include "resume_hook.h"
#include "runtime/runtime_state.h"
#include "rcu/rcu_domain.h"
#include "runtime/worker_manager.h"
#include "resume_hooks/metadata.h"
#include "scheduler.h"
#include "utils/random.h"
#include "utils/time.h"

using bess::DefaultScheduler;
using bess::ExperimentalScheduler;
using bess::Scheduler;


using bess::TrafficClassBuilder;
using namespace bess::traffic_class_initializer_types;
using bess::ResumeHookBuilder;


// See worker.h
__thread Worker current_worker;

int is_worker_active(int wid) {
  return bess::runtime::runtime().workers().IsActive(wid);
}

int is_worker_core(int cpu) {
  return bess::runtime::runtime().workers().IsCoreUsed(cpu);
}

void pause_worker(int wid) {
  bess::runtime::runtime().workers().Pause(wid);
}

void pause_all_workers() {
  bess::runtime::runtime().workers().PauseAll();
}

void resume_worker(int wid) {
  bess::runtime::runtime().workers().Resume(wid);
}

void resume_all_workers() {
  bess::runtime::runtime().workers().ResumeAll();
}

void attach_orphans() {
  bess::runtime::runtime().workers().AttachOrphans();
}

void destroy_worker(int wid) {
  bess::runtime::runtime().workers().Destroy(wid);
}

void destroy_all_workers() {
  bess::runtime::runtime().workers().DestroyAll();
}

void detach_all_worker_threads() {
  bess::runtime::runtime().workers().DetachAllThreads();
}

bool is_any_worker_running() {
  return bess::runtime::runtime().workers().AnyRunning();
}

bool is_worker_running(int wid) {
  return bess::runtime::runtime().workers().IsRunning(wid);
}

void launch_worker(int wid, int core, const std::string &scheduler) {
  bess::runtime::runtime().workers().Launch(wid, core, scheduler);
}

Worker *get_next_active_worker() {
  return bess::runtime::runtime().workers().NextActive();
}

void add_tc_to_orphan(bess::TrafficClass *c, int wid) {
  bess::runtime::runtime().workers().AddOrphan(c, wid);
}

bool remove_tc_from_orphan(bess::TrafficClass *c) {
  return bess::runtime::runtime().workers().RemoveOrphan(c);
}

const std::list<std::pair<int, bess::TrafficClass *>> &list_orphan_tcs() {
  return bess::runtime::runtime().workers().orphan_tcs();
}

bool detach_tc(bess::TrafficClass *c) {
  return bess::runtime::runtime().workers().DetachTc(c);
}

void Worker::SetNonWorker() {
  // These TLS variables should not be accessed by non-worker threads.
  // Assign INT_MIN to the variables so that the program can crash
  // when accessed as an index of an array.
  wid_ = INT_MIN;
  rcu_online_ = false;
  core_ = INT_MIN;
  socket_ = INT_MIN;
  fd_event_ = INT_MIN;

  if (!packet_pool_) {
    // Packet pools should be available to non-worker threads.
    // (doesn't need to be NUMA-aware, so pick any)
    for (int socket = 0; socket < RTE_MAX_NUMA_NODES; socket++) {
      if (bess::PacketPool *pool = bess::PacketPool::GetDefaultPool(socket)) {
        packet_pool_ = pool;
        break;
      }
    }
  }
}

void Worker::ReportQuiescent() {
  // Only an online reader reports: a worker whose first scheduler round runs
  // before it is ever resumed must not appear online to DPDK's QSBR (a
  // report on an offline thread does exactly that). rcu_online_ is this
  // thread's own flag, so the check costs nothing shared.
  if (!rcu_online_ || wid_ < 0 || wid_ >= Worker::kMaxWorkers) {
    return;
  }
  bess::runtime::runtime().rcu().Quiescent(wid_);
}

int Worker::BlockWorker() {
  bess::runtime::worker_signal t;
  int ret;

  // Leave the RCU reader domain *before* blocking (K1): a grace period must
  // never depend on a thread that will not report quiescence until it is woken
  // again. This is the offline-before-blocking rule.
  rcu_online_ = false;
  bess::runtime::runtime().rcu().Offline(wid_);

  status_ = WORKER_PAUSED;

  ret = read(fd_event_, &t, sizeof(t));
  CHECK_EQ(ret, sizeof(t));

  if (t == bess::runtime::worker_signal::unblock) {
    // Back online before any dataplane work resumes, and report quiescence so
    // that a grace period started while this worker was paused can complete.
    bess::runtime::runtime().rcu().Online(wid_);
    rcu_online_ = true;
    bess::runtime::runtime().rcu().Quiescent(wid_);
    status_ = WORKER_RUNNING;
    return 0;
  }

  if (t == bess::runtime::worker_signal::quit) {
    status_ = WORKER_FINISHED;
    return 1;
  }

  CHECK(0);
  return 0;
}

/* The entry point of worker threads */
void *Worker::Run(void *_arg) {
  bess::runtime::WorkerThreadArg *arg = (bess::runtime::WorkerThreadArg *)_arg;
  rand_ = new Random();

  cpu_set_t set;

  CPU_ZERO(&set);
  CPU_SET(arg->core, &set);
  // Checked, not best-effort: registration below captures this thread's
  // cpuset and derives its NUMA/socket id from it. Silently continuing on
  // failure (a restrictive cgroup/cpuset, a CPU that is not in the process
  // mask) would leave the worker reporting core_ = arg->core while running
  // somewhere else AND registering a socket that does not match where it
  // runs -- i.e. it would pick the wrong PacketPool and key its mempool
  // cache under a wrong lcore. Dying here is the lesser evil; rejecting the
  // worker-creation RPC instead would be nicer, later.
  CHECK_EQ(rte_thread_set_affinity(&set), 0)
      << "failed to pin worker " << arg->wid << " to CPU " << arg->core;

  // Register this pthread as a non-EAL DPDK lcore. DPDK's per-lcore
  // machinery -- above all the default mempool cache that
  // rte_mempool_default_cache() keys on rte_lcore_id() -- needs a valid
  // lcore id; without one every Packet allocation would silently bypass the
  // per-core cache. This is DPDK's public API for exactly that; BESS used
  // to poke DPDK's private TLS (RTE_PER_LCORE(_lcore_id) = arg->wid)
  // instead -- see MODERNIZATION.md entry 33 (Phase C item).
  //
  // What this deliberately does *not* do is keep WorkerId and the lcore id
  // equal: they are separate concepts now, and a re-created worker gets
  // whatever id DPDK hands out next. Nothing may assume
  // wid == rte_lcore_id() any more; arg->wid stays BESS's identity and
  // arg->core stays the physical CPU.
  //
  // Order matters: registration captures the thread's cpuset and derives
  // the NUMA/socket id from it, so the affinity call has to happen first.
  //
  // BESS's EAL configuration (core/dpdk.cc: --lcores 127@<all cpus>, main
  // lcore 127) leaves lcores 0..126 free, so Worker::kMaxWorkers (64)
  // workers always have one available. The CHECK is here so that a future
  // EAL-config change fails loudly instead of silently degrading every
  // worker's allocator to the cache-bypassing path.
  CHECK_EQ(rte_thread_register(), 0)
      << "rte_thread_register() failed for worker " << arg->wid << ": "
      << rte_strerror(rte_errno);
  const unsigned lcore_id = rte_lcore_id();

  wid_ = arg->wid;
  rcu_online_ = false;
  core_ = arg->core;
  tid_ = gettid();  // read by the control plane once this worker is ready
  // Named for ps/top (15 characters at most); otherwise it inherits the name
  // of whichever thread launched it (a gRPC handler, say).
  const std::string name = "bess-worker-" + std::to_string(wid_);
  pthread_setname_np(pthread_self(), name.substr(0, 15).c_str());
  socket_ = rte_socket_id();

  // For some reason, rte_socket_id() does not return a correct NUMA ID.
  // Nevertheless, BESS should not crash.
  if (socket_ == SOCKET_ID_ANY) {
    LOG(WARNING) << "rte_socket_id() returned -1 for " << arg->core;
    socket_ = 0;
  }

  fd_event_ = eventfd(0, 0);
  CHECK_GE(fd_event_, 0);

  scheduler_ = arg->scheduler;

  current_tsc_ = rdtsc();

  packet_pool_ = bess::PacketPool::GetDefaultPool(socket_);
  CHECK_NOTNULL(packet_pool_);

  status_ = WORKER_PAUSING;

  STORE_BARRIER();

  bess::runtime::runtime().workers().Publish(wid_, this);

  // Register as a reader, but stay offline: a worker that merely exists as a
  // thread is not an active RCU participant. It goes online when dataplane
  // execution is about to resume (BlockWorker's unblock path).
  {
    auto registered = bess::runtime::runtime().rcu().Register(wid_);
    CHECK(registered.has_value())
        << "RCU reader registration failed for worker " << wid_ << ": "
        << registered.error().message;
  }

  LOG(INFO) << "Worker " << wid_ << "(" << this << ") "
            << "is running on core " << core_ << " (socket " << socket_
            << ", DPDK lcore " << lcore_id << ")";

  CPU_ZERO(&set);
  scheduler_->ScheduleLoop();

  LOG(INFO) << "Worker " << wid_ << "(" << this << ") "
            << "is quitting... (core " << core_ << ", socket " << socket_
            << ")";

  // The thread is finished with the dataplane: give the reader slot back
  // before teardown, so a recreated worker with this id can register again and
  // a grace period stops waiting for it.
  rcu_online_ = false;
  bess::runtime::runtime().rcu().Offline(wid_);
  bess::runtime::runtime().rcu().Unregister(wid_);

  delete scheduler_;
  delete rand_;

  // Release the lcore id last, after scheduler/TrafficClass teardown: that
  // teardown can still free packets, and a free wants this worker's mempool
  // cache context to put them back into.
  rte_thread_unregister();

  return nullptr;
}

WorkerPauser::WorkerPauser() {
  if (is_any_worker_running()) {
    for (int wid = 0; wid < Worker::kMaxWorkers; wid++) {
      if (is_worker_running(wid)) {
        workers_paused_.push_back(wid);
        VLOG(1) << "*** Pausing Worker " << wid << " ***";
        pause_worker(wid);
      }
    }
  }
}

WorkerPauser::~WorkerPauser() {
  attach_orphans();  // All workers should be paused at this point.

  if (!workers_paused_.empty()) {
    bess::run_global_resume_hooks(false);
  }

  std::set<Module *> modules_run;
  for (int wid : workers_paused_) {
    auto &resume_modules = bess::event_modules[bess::Event::PreResume];
    for (auto it = resume_modules.begin(); it != resume_modules.end();) {
      Module *m = *it;
      if (!modules_run.count(m) && m->active_workers()[wid]) {
        int ret = m->OnEvent(bess::Event::PreResume);
        modules_run.insert(m);
        if (ret == -ENOTSUP) {
          it = resume_modules.erase(it);
        } else {
          it++;
        }
      } else {
        it++;
      }
    }
    resume_worker(wid);
    VLOG(1) << "*** Worker " << wid << " Resumed ***";
  }
}
