// SPDX-License-Identifier: BSD-3-Clause

#include "runtime/worker_manager.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

#include "utils/logging.h"

#include "arch/cpu.h"
#include "runtime/runtime_state.h"
#include "runtime/thread_placement.h"
#include "runtime/numa_placement.h"
#include "module.h"
#include "runtime/opts.h"
#include "resume_hook.h"
#include "resume_hooks/metadata.h"
#include "scheduler.h"
#include "utils/random.h"
#include "utils/time.h"

using bess::DefaultScheduler;
using bess::ExperimentalScheduler;
using bess::Scheduler;

namespace bess {
namespace runtime {

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
               << arg->wid.value() << " (core " << arg->core.value()
               << ") -- resetting. Stale values: wid="
               << current_worker.wid().value()
               << " core=" << current_worker.core().value()
               << " socket=" << current_worker.socket().value()
               << " fd_event=" << current_worker.fd_event();
    memset(&current_worker, 0, sizeof(Worker));
  }
  return current_worker.Run(_arg);
}

}  // namespace

bool WorkerManager::IsRunning(WorkerId wid) const {
  // status() is not const: the slot's pointer, not Get()'s const one.
  Worker *worker = wid.value() < Worker::kMaxWorkers
                       ? workers_[wid.value()].load()
                       : nullptr;
  return worker != nullptr && worker->status() == WORKER_RUNNING;
}

bool WorkerManager::AnyRunning() const {
  for (WorkerId wid : dataplane::AllWorkerIds()) {
    if (IsRunning(wid)) {
      return true;
    }
  }

  return false;
}

bool WorkerManager::IsCoreUsed(CpuId core) const {
  for (const auto &slot : workers_) {
    const Worker *worker = slot.load();
    if (worker && worker->core() == core) {
      return true;
    }
  }

  return false;
}

std::vector<int> WorkerManager::Cores(std::optional<WorkerId> except) const {
  std::vector<int> cores;
  for (WorkerId wid : dataplane::AllWorkerIds()) {
    const Worker *worker = Get(wid);
    if (worker != nullptr && wid != except) {
      cores.push_back(static_cast<int>(worker->core().value()));
    }
  }
  return cores;
}

WorkerCpuReport WorkerManager::CpuReport(WorkerId wid) const {
  const Worker *worker = Get(wid);
  if (worker == nullptr) {
    return {};
  }
  return ProcessPlacement().DescribeWorkerCpu(
      static_cast<int>(worker->core().value()), Cores(wid));
}

void WorkerManager::Pause(WorkerId wid) {
  Worker *worker = Get(wid);
  if (worker && worker->status() == WORKER_RUNNING) {
    // Release store, acquire loads of the same atomic: the worker's
    // BlockWorker() store of WORKER_PAUSED publishes its last writes here.
    worker->set_status(WORKER_PAUSING);

    while (worker->status() == WORKER_PAUSING) {
      arch::CpuRelax();
    }
  }
}

void WorkerManager::PauseAll() {
  for (WorkerId wid : dataplane::AllWorkerIds()) {
    Pause(wid);
  }
}

void WorkerManager::Resume(WorkerId wid) {
  Worker *worker = Get(wid);
  if (worker && worker->status() == WORKER_PAUSED) {
    // The last moment this worker's tree may be rewritten: the thread is
    // parked and reads root_ and the round-robin's children again the instant
    // it is unblocked. Doing it here is also what lets a transaction leave no
    // trace: the pauser attaches orphans and resumes in one step, so the
    // wrapper that attach created is collapsed before the worker runs again.
    AdjustSchedulerDefault(wid);

    worker_signal sig = worker_signal::unblock;

    int ret = write(worker->fd_event(), &sig, sizeof(sig));
    CHECK_EQ(ret, sizeof(uint64_t));

    while (worker->status() == WORKER_PAUSED) {
      arch::CpuRelax();
    }
  }
}

void WorkerManager::ResumeAll() {
  for (WorkerId wid : dataplane::AllWorkerIds()) {
    Resume(wid);
  }
}

void WorkerManager::Destroy(WorkerId wid) {
  Pause(wid);

  Worker *worker = Get(wid);
  if (worker && worker->status() == WORKER_PAUSED) {
    worker_signal sig = worker_signal::quit;

    int ret = write(worker->fd_event(), &sig, sizeof(sig));
    CHECK_EQ(ret, sizeof(uint64_t));

    while (worker->status() == WORKER_PAUSED) {
      arch::CpuRelax();
    }

    // Wait for the OS thread to fully exit -- not just for status_ to have
    // left WORKER_PAUSED, which happens earlier, inside BlockWorker() --
    // before returning. Worker::Run()'s teardown (`delete scheduler_`)
    // recursively destroys the TC tree, and the registry bookkeeping that
    // goes with it must not run concurrently with control-plane mutation.
    threads_[wid.value()].join();

    workers_[wid.value()].store(nullptr);
    scheduler_names_[wid.value()].clear();

    num_workers_--;
    PlaceThreads();  // its CPU is free for control threads again
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
  for (WorkerId wid : dataplane::AllWorkerIds()) {
    Destroy(wid);
  }
}

void WorkerManager::DetachAllThreads() {
  for (std::thread &thread : threads_) {
    if (thread.joinable()) {
      thread.detach();
    }
  }
}

void WorkerManager::Launch(WorkerId wid, CpuId core,
                           const std::string &scheduler) {
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
  std::thread &thread = threads_[wid.value()];
  CHECK(!thread.joinable())
      << "worker thread " << wid.value() << " is still joinable; "
      << "Destroy() must join it before this wid can be reused.";
  thread = std::thread(run_worker, &arg);
  scheduler_names_[wid.value()] = scheduler;

  /* spin until it becomes ready and fully paused */
  Worker *worker = Get(wid);
  while (worker == nullptr || worker->status() != WORKER_PAUSED) {
    arch::CpuRelax();
    worker = Get(wid);
  }

  num_workers_++;
  PlaceThreads();

  for (const std::string &warning : CpuReport(wid).warnings) {
    LOG(WARNING) << "worker " << wid.value() << ": " << warning;
  }
}

void WorkerManager::PlaceThreads() {
  std::vector<WorkerPlacement> placed;
  for (const auto &slot : workers_) {
    if (const Worker *worker = slot.load()) {
      placed.push_back({worker->tid(), worker->core()});
    }
  }
  PlaceControlThreads(placed);
}

Worker *WorkerManager::NextActive() {
  static uint16_t prev_wid = 0;
  if (num_workers_ == 0) {
    Launch(WorkerId(0), DefaultWorkerCore(), "");
    return Get(WorkerId(0));
  }

  while (!IsActive(WorkerId(prev_wid))) {
    prev_wid = (prev_wid + 1) % Worker::kMaxWorkers;
  }

  Worker *ret = Get(WorkerId(prev_wid));
  prev_wid = (prev_wid + 1) % Worker::kMaxWorkers;
  return ret;
}

void WorkerManager::AdjustSchedulerDefault(WorkerId wid) {
  Worker *worker = Get(wid);
  if (worker == nullptr || worker->status() != WORKER_PAUSED) {
    // A running worker's thread is inside Scheduler::Next() ->
    // PickNextChild() reading exactly what AdjustDefault() deletes, and
    // neither Scheduler nor TrafficClass has a lock of its own, so its tree is
    // rewritten only after a pause parks it. A worker still parking
    // (WORKER_PAUSING) is left alone for the same reason.
    return;
  }

  worker->scheduler()->AdjustDefault();
}

void WorkerManager::AdjustSchedulerDefaults() {
  for (WorkerId wid : dataplane::AllWorkerIds()) {
    AdjustSchedulerDefault(wid);
  }
}

const Worker *WorkerManager::RootedOnAnyWorker(const TrafficClass *c) const {
  for (const auto &slot : workers_) {
    const Worker *worker = slot.load();
    if (worker != nullptr && worker->scheduler()->root() == c) {
      return worker;
    }
  }
  return nullptr;
}

void WorkerManager::AttachOrphans() {
  CHECK(!AnyRunning());
  // Distribute all orphan TCs to workers.
  for (const auto &tc : orphan_tcs_) {
    TrafficClass *c = tc.second;
    if (c->parent()) {
      continue;
    }

    // A root has no parent, so the check above cannot see a class that is
    // already attached as a worker's root. Attaching it to a second worker
    // would run its task on two workers at once: for a module whose task owns
    // a single-consumer ring that is two consumers, and that is the state that
    // hands out a slot nobody wrote (D-019; drr.h's ring comment). A class is
    // orphaned or attached, never both.
    const Worker *owner = RootedOnAnyWorker(c);
    DCHECK(owner == nullptr)
        << "traffic class " << c->name() << " is the root of worker "
        << owner->wid().value() << " and is also in the orphan list";
    if (owner != nullptr) {
      LOG(ERROR) << "traffic class " << c->name() << " is already the root of "
                 << "worker " << owner->wid().value()
                 << "; not attaching it to a second worker";
      continue;
    }

    Worker *w;

    Worker *named = tc.first ? Get(*tc.first) : nullptr;
    if (named == nullptr) {
      w = NextActive();
    } else {
      w = named;
    }

    if (!w->scheduler()->AttachOrphan(c, w->wid())) {
      LOG(ERROR) << "attaching traffic class " << c->name() << " to worker "
                 << w->wid().value() << " failed";
    }
  }

  orphan_tcs_.clear();
}

void WorkerManager::AddOrphan(TrafficClass *c, std::optional<WorkerId> wid) {
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
  for (const auto &slot : workers_) {
    Worker *worker = slot.load();
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

}  // namespace runtime
}  // namespace bess
