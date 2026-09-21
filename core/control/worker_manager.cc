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

#include "control/worker_manager.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <set>

#include <glog/logging.h>

#include "control/runtime_state.h"
#include "module.h"
#include "opts.h"
#include "resume_hook.h"
#include "resume_hooks/metadata.h"
#include "scheduler.h"
#include "utils/random.h"
#include "utils/time.h"

using bess::DefaultScheduler;
using bess::ExperimentalScheduler;
using bess::Scheduler;

namespace bess {
namespace control {

namespace {

// Entry point of a worker OS thread. `current_worker` is that thread's own TLS
// Worker object (declared in worker.h); the thread publishes it into this
// manager from Worker::Run().
void *run_worker(void *_arg) {
  // current_worker (a __thread/TLS object) should always start zeroed for a
  // brand new OS thread -- glibc zeroes .tbss-backed TLS synchronously inside
  // pthread_create(), before the new thread runs a single instruction, so this
  // is NOT a TLS-reuse timing question. If this fires, either a real bug wrote
  // to this thread's current_worker before it got here (which shouldn't be
  // reachable: run_worker() is the thread's entry point), or heap corruption
  // from elsewhere landed on this block. Worker::Run() also never
  // (re)initializes silent_drops_/current_ns_, so a non-pristine block would
  // otherwise leak stale values into reported stats forever. Reset explicitly
  // (so the daemon doesn't go down over it), but log loudly with the actual
  // stale values so this is diagnosable if it ever fires again.
  const Worker kZeroWorker{};
  if (memcmp(&current_worker, &kZeroWorker, sizeof(Worker)) != 0) {
    const auto *arg = static_cast<const WorkerThreadArg *>(_arg);
    LOG(ERROR) << "current_worker was not pristine at the start of worker "
               << arg->wid << " (core " << arg->core
               << ") -- resetting. Stale values: wid=" << current_worker.wid()
               << " core=" << current_worker.core()
               << " socket=" << current_worker.socket()
               << " fd_event=" << current_worker.fd_event();
    memset(&current_worker, 0, sizeof(Worker));
  }
  return current_worker.Run(_arg);
}

}  // namespace

bool WorkerManager::IsRunning(int wid) const {
  Worker *worker = IsActive(wid) ? workers_[wid].load() : nullptr;
  return worker != nullptr && worker->status() == WORKER_RUNNING;
}

bool WorkerManager::AnyRunning() const {
  for (int wid = 0; wid < Worker::kMaxWorkers; wid++) {
    if (IsRunning(wid)) {
      return true;
    }
  }

  return false;
}

bool WorkerManager::IsCoreUsed(int core) const {
  for (int wid = 0; wid < Worker::kMaxWorkers; wid++) {
    Worker *worker = workers_[wid].load();
    if (worker && worker->core() == core) {
      return true;
    }
  }

  return false;
}

void WorkerManager::Pause(int wid) {
  Worker *worker = Get(wid);
  if (worker && worker->status() == WORKER_RUNNING) {
    worker->set_status(WORKER_PAUSING);

    FULL_BARRIER();

    while (worker->status() == WORKER_PAUSING) {
    } /* spin */
  }
}

void WorkerManager::PauseAll() {
  for (int wid = 0; wid < Worker::kMaxWorkers; wid++) {
    Pause(wid);
  }
}

void WorkerManager::Resume(int wid) {
  Worker *worker = Get(wid);
  if (worker && worker->status() == WORKER_PAUSED) {
    worker_signal sig = worker_signal::unblock;

    int ret = write(worker->fd_event(), &sig, sizeof(sig));
    CHECK_EQ(ret, sizeof(uint64_t));

    while (worker->status() == WORKER_PAUSED) {
    } /* spin */
  }
}

void WorkerManager::ResumeAll() {
  for (int wid = 0; wid < Worker::kMaxWorkers; wid++) {
    Worker *worker = workers_[wid].load();
    if (worker) {
      worker->scheduler()->AdjustDefault();
    }
  }

  for (int wid = 0; wid < Worker::kMaxWorkers; wid++) {
    Resume(wid);
  }
}

void WorkerManager::Destroy(int wid) {
  Pause(wid);

  Worker *worker = Get(wid);
  if (worker && worker->status() == WORKER_PAUSED) {
    worker_signal sig = worker_signal::quit;

    int ret = write(worker->fd_event(), &sig, sizeof(sig));
    CHECK_EQ(ret, sizeof(uint64_t));

    while (worker->status() == WORKER_PAUSED) {
    } /* spin */

    // Wait for the OS thread to fully exit -- not just for status_ to have
    // left WORKER_PAUSED, which happens earlier, inside BlockWorker() --
    // before returning. Worker::Run()'s teardown (`delete scheduler_`)
    // recursively destroys the TC tree, and the registry bookkeeping that
    // goes with it must not run concurrently with control-plane mutation.
    threads_[wid].join();

    workers_[wid].store(nullptr);

    num_workers_--;
  }

  if (num_workers_ > 0) {
    return;
  }

  auto &hooks = bess::global_resume_hooks;
  for (auto it = hooks.begin(); it != hooks.end();) {
    if ((*it)->is_default()) {
      it++;
    } else {
      it = hooks.erase(it);
    }
  }
}

void WorkerManager::DestroyAll() {
  for (int wid = 0; wid < Worker::kMaxWorkers; wid++) {
    Destroy(wid);
  }
}

void WorkerManager::DetachAllThreads() {
  for (int wid = 0; wid < Worker::kMaxWorkers; wid++) {
    if (threads_[wid].joinable()) {
      threads_[wid].detach();
    }
  }
}

void WorkerManager::Launch(int wid, int core, const std::string &scheduler) {
  WorkerThreadArg arg = {.wid = wid, .core = core, .scheduler = nullptr};
  if (scheduler == "") {
    arg.scheduler = new DefaultScheduler();
  } else if (scheduler == "experimental") {
    arg.scheduler = new ExperimentalScheduler();
  } else {
    CHECK(false) << "Scheduler " << scheduler << " is invalid.";
  }

  // std::thread::operator= calls std::terminate() if the target is still
  // joinable. That should be impossible here -- Destroy() always joins this
  // wid's thread before clearing the slot, and DetachAllThreads() (called
  // once, at daemon shutdown) detaches any thread still joinable at that
  // point -- but if that invariant is ever violated by a future change,
  // better to CHECK loudly here than to let the assignment below abort the
  // daemon with a bare "terminate called" and no context.
  CHECK(!threads_[wid].joinable())
      << "worker thread " << wid << " is still joinable; "
      << "Destroy() must join it before this wid can be reused.";
  threads_[wid] = std::thread(run_worker, &arg);
  INST_BARRIER();

  /* spin until it becomes ready and fully paused */
  Worker *worker = workers_[wid].load();
  while (worker == nullptr || worker->status() != WORKER_PAUSED) {
    worker = workers_[wid].load();
  }

  num_workers_++;
}

Worker *WorkerManager::NextActive() {
  static int prev_wid = 0;
  if (num_workers_ == 0) {
    Launch(0, FLAGS_c, "");
    return Get(0);
  }

  while (!IsActive(prev_wid)) {
    prev_wid = (prev_wid + 1) % Worker::kMaxWorkers;
  }

  Worker *ret = workers_[prev_wid].load();
  prev_wid = (prev_wid + 1) % Worker::kMaxWorkers;
  return ret;
}

void WorkerManager::AttachOrphans() {
  CHECK(!AnyRunning());
  // Distribute all orphan TCs to workers.
  for (const auto &tc : orphan_tcs_) {
    TrafficClass *c = tc.second;
    if (c->parent()) {
      continue;
    }

    Worker *w;

    int wid = tc.first;
    Worker *named = (wid == Worker::kAnyWorker) ? nullptr : workers_[wid].load();
    if (named == nullptr) {
      w = NextActive();
    } else {
      w = named;
    }

    w->scheduler()->AttachOrphan(c, w->wid());
  }

  orphan_tcs_.clear();
}

void WorkerManager::AddOrphan(TrafficClass *c, int wid) {
  orphan_tcs_.emplace_back(wid, c);
}

bool WorkerManager::RemoveOrphan(TrafficClass *c) {
  for (auto it = orphan_tcs_.begin(); it != orphan_tcs_.end();) {
    if (it->second == c) {
      orphan_tcs_.erase(it);
      return true;
    } else {
      it++;
    }
  }

  return false;
}

bool WorkerManager::DetachTc(TrafficClass *c) {
  TrafficClass *parent = c->parent();
  if (parent) {
    return parent->RemoveChild(c);
  }

  // Try to remove from root of one of the schedulers
  for (int wid = 0; wid < Worker::kMaxWorkers; wid++) {
    Worker *worker = workers_[wid].load();
    if (worker) {
      bool found = worker->scheduler()->RemoveRoot(c);
      if (found) {
        return true;
      }
    }
  }

  // Try to remove from orphan_tcs
  return RemoveOrphan(c);
}

}  // namespace control
}  // namespace bess
