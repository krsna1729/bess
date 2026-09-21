// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
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
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
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

#include "worker.h"

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
#include "opts.h"
#include "packet_pool.h"
#include "resume_hook.h"
#include "control/runtime_state.h"
#include "rcu/rcu_domain.h"
#include "control/worker_manager.h"
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

#define SYS_CPU_DIR "/sys/devices/system/cpu/cpu%u"
#define CORE_ID_FILE "topology/core_id"

/* Check if a cpu is present by the presence of the cpu information for it */
int is_cpu_present(unsigned int core_id) {
  char path[PATH_MAX];
  int len = snprintf(path, sizeof(path), SYS_CPU_DIR "/" CORE_ID_FILE, core_id);
  if (len <= 0 || (unsigned)len >= sizeof(path)) {
    return 0;
  }
  if (access(path, F_OK) != 0) {
    return 0;
  }

  return 1;
}

int is_worker_active(int wid) {
  return bess::control::runtime().workers().IsActive(wid);
}

int is_worker_core(int cpu) {
  return bess::control::runtime().workers().IsCoreUsed(cpu);
}

void pause_worker(int wid) {
  bess::control::runtime().workers().Pause(wid);
}

void pause_all_workers() {
  bess::control::runtime().workers().PauseAll();
}

void resume_worker(int wid) {
  bess::control::runtime().workers().Resume(wid);
}

void resume_all_workers() {
  bess::control::runtime().workers().ResumeAll();
}

void attach_orphans() {
  bess::control::runtime().workers().AttachOrphans();
}

void destroy_worker(int wid) {
  bess::control::runtime().workers().Destroy(wid);
}

void destroy_all_workers() {
  bess::control::runtime().workers().DestroyAll();
}

void detach_all_worker_threads() {
  bess::control::runtime().workers().DetachAllThreads();
}

bool is_any_worker_running() {
  return bess::control::runtime().workers().AnyRunning();
}

bool is_worker_running(int wid) {
  return bess::control::runtime().workers().IsRunning(wid);
}

void launch_worker(int wid, int core, const std::string &scheduler) {
  bess::control::runtime().workers().Launch(wid, core, scheduler);
}

Worker *get_next_active_worker() {
  return bess::control::runtime().workers().NextActive();
}

void add_tc_to_orphan(bess::TrafficClass *c, int wid) {
  bess::control::runtime().workers().AddOrphan(c, wid);
}

bool remove_tc_from_orphan(bess::TrafficClass *c) {
  return bess::control::runtime().workers().RemoveOrphan(c);
}

const std::list<std::pair<int, bess::TrafficClass *>> &list_orphan_tcs() {
  return bess::control::runtime().workers().orphan_tcs();
}

bool detach_tc(bess::TrafficClass *c) {
  return bess::control::runtime().workers().DetachTc(c);
}

void Worker::SetNonWorker() {
  // These TLS variables should not be accessed by non-worker threads.
  // Assign INT_MIN to the variables so that the program can crash
  // when accessed as an index of an array.
  wid_ = INT_MIN;
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

int Worker::BlockWorker() {
  bess::control::worker_signal t;
  int ret;

  // Leave the RCU reader domain *before* blocking (K1): a grace period must
  // never depend on a thread that will not report quiescence until it is woken
  // again. This is the offline-before-blocking rule.
  bess::control::runtime().rcu().Offline(wid_);

  status_ = WORKER_PAUSED;

  ret = read(fd_event_, &t, sizeof(t));
  CHECK_EQ(ret, sizeof(t));

  if (t == bess::control::worker_signal::unblock) {
    // Back online before any dataplane work resumes, and report quiescence so
    // that a grace period started while this worker was paused can complete.
    bess::control::runtime().rcu().Online(wid_);
    bess::control::runtime().rcu().Quiescent(wid_);
    status_ = WORKER_RUNNING;
    return 0;
  }

  if (t == bess::control::worker_signal::quit) {
    status_ = WORKER_FINISHED;
    return 1;
  }

  CHECK(0);
  return 0;
}

/* The entry point of worker threads */
void *Worker::Run(void *_arg) {
  bess::control::WorkerThreadArg *arg = (bess::control::WorkerThreadArg *)_arg;
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
  core_ = arg->core;
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

  bess::control::runtime().workers().Publish(wid_, this);

  // Register as a reader, but stay offline: a worker that merely exists as a
  // thread is not an active RCU participant. It goes online when dataplane
  // execution is about to resume (BlockWorker's unblock path).
  {
    auto registered = bess::control::runtime().rcu().Register(wid_);
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
  bess::control::runtime().rcu().Offline(wid_);
  bess::control::runtime().rcu().Unregister(wid_);

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
