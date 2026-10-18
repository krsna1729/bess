// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_WORKER_MANAGER_H_
#define BESS_RUNTIME_WORKER_MANAGER_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dataplane/worker_id.h"
#include "runtime/numa_placement.h"
#include "runtime/thread_placement.h"
#include "traffic_class.h"
#include "worker.h"

namespace bess {
class Scheduler;

namespace runtime {

using dataplane::WorkerId;

// Arguments handed to a worker OS thread: the BESS worker id, the CPU it is
// pinned to, and the scheduler it owns (the thread takes ownership).
struct WorkerThreadArg {
  WorkerId wid;
  CpuId core;
  Scheduler *scheduler;
};

// Signals a paused worker thread can receive over its eventfd.
enum class worker_signal : uint64_t {
  unblock = 1,
  quit,
};

// Owns worker management state: the worker slots, the OS threads and the
// orphan-traffic-class list. The `Worker` objects themselves live in each
// worker thread's TLS storage (`current_worker`), so the manager publishes
// pointers to them rather than owning them; it does own the threads.
//
// This is management ownership only: the packet path keeps using
// `current_worker` and the plain `Worker *` it already has.
class WorkerManager {
 public:
  WorkerManager() = default;

  int num_workers() const { return num_workers_; }

  Worker *Get(WorkerId wid) {
    return wid.value() < Worker::kMaxWorkers ? workers_[wid.value()].load()
                                             : nullptr;
  }

  const Worker *Get(WorkerId wid) const {
    return wid.value() < Worker::kMaxWorkers ? workers_[wid.value()].load()
                                             : nullptr;
  }

  // The scheduler name this worker was launched with ("" for the default one).
  const std::string &scheduler_name(WorkerId wid) const {
    static const std::string kEmpty;
    if (wid.value() >= Worker::kMaxWorkers) {
      return kEmpty;
    }
    return scheduler_names_[wid.value()];
  }

  bool IsActive(WorkerId wid) const { return Get(wid) != nullptr; }
  bool IsRunning(WorkerId wid) const;
  bool AnyRunning() const;
  Worker *NextActive();

  // Starts worker `wid` on `core`, and keeps this process's other threads
  // off every worker's CPU (D-027). Logs, once, each warning
  // DescribeWorkerCpu has for that CPU (another node, a control CPU, a core
  // shared with another worker or a control CPU): diagnosed, never refused
  // (D-123).
  void Launch(WorkerId wid, CpuId core, const std::string &scheduler);
  void Destroy(WorkerId wid);
  void DestroyAll();
  void DetachAllThreads();

  void Pause(WorkerId wid);
  void PauseAll();
  void Resume(WorkerId wid);
  void ResumeAll();

  // True if a live worker is pinned to this CPU.
  bool IsCoreUsed(CpuId core) const;
  // The CPUs the live workers run on, except worker `except`'s: what an
  // automatic worker must not take (D-123). Plain CPU numbers, as the
  // placement model (Placement, read from sysfs) takes them.
  std::vector<int> Cores(std::optional<WorkerId> except = std::nullopt) const;
  // Where live worker `wid` runs, against the other workers' CPUs as they are
  // now (D-123); a default report for a wid with no worker.
  WorkerCpuReport CpuReport(WorkerId wid) const;

  // Orphan traffic classes: created by TC/module setup and attached to
  // workers when they resume, to worker `wid` or (none) any.
  void AddOrphan(TrafficClass *c, std::optional<WorkerId> wid);
  bool RemoveOrphan(TrafficClass *c);
  const std::list<std::pair<std::optional<WorkerId>, TrafficClass *>>
      &orphan_tcs() const {
    return orphan_tcs_;
  }
  void AttachOrphans();

  // Detaches `c` from a scheduler root or from the orphan list, handing
  // ownership of the detached class to the caller.
  bool DetachTc(TrafficClass *c);

  // Collapses the default round-robin wrapper on `wid`'s scheduler, if it has
  // one: orphan attachment creates it when a scheduler briefly holds more than
  // one root. Does nothing unless that worker is parked -- the rewrite deletes
  // the class its thread would otherwise be reading.
  void AdjustSchedulerDefault(WorkerId wid);

  // The same, over every worker: the workers a pauser does not resume (it
  // records only the ones it found running) still need their wrapper gone.
  void AdjustSchedulerDefaults();

  // Called by the worker thread itself once it is up.
  void Publish(WorkerId wid, Worker *worker) {
    workers_[wid.value()].store(worker);
  }

 private:
  // Moves non-worker threads off the active workers' CPUs.
  void PlaceThreads();
  // The worker whose scheduler has `c` as its root, or nullptr. A root is
  // attached but parentless, so this is the question `c->parent()` cannot
  // answer; AttachOrphans needs it to keep a class from being attached to two
  // workers at once.
  const Worker *RootedOnAnyWorker(const TrafficClass *c) const;
  // The slots are written by the worker thread and read by the control thread
  // (the launch spin below, pause/resume, teardown), so they are atomic --
  // the legacy `Worker *volatile workers[]` array is what this replaces.
  std::array<std::atomic<Worker *>, Worker::kMaxWorkers> workers_{};
  std::array<std::thread, Worker::kMaxWorkers> threads_;
  std::array<std::string, Worker::kMaxWorkers> scheduler_names_;
  int num_workers_ = 0;
  std::list<std::pair<std::optional<WorkerId>, TrafficClass *>> orphan_tcs_;
};

}  // namespace runtime
}  // namespace bess

#endif  // BESS_RUNTIME_WORKER_MANAGER_H_
