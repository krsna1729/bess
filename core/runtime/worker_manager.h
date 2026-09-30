// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_WORKER_MANAGER_H_
#define BESS_RUNTIME_WORKER_MANAGER_H_

#include <array>
#include <atomic>
#include <cstdint>
#include <list>
#include <string>
#include <thread>
#include <utility>

#include "traffic_class.h"
#include "worker.h"

namespace bess {
class Scheduler;

namespace runtime {

// Arguments handed to a worker OS thread: the BESS worker id, the CPU it is
// pinned to, and the scheduler it owns (the thread takes ownership).
struct WorkerThreadArg {
  int wid;
  int core;
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

  Worker *Get(int wid) {
    if (wid < 0 || wid >= Worker::kMaxWorkers) {
      return nullptr;
    }
    return workers_[wid].load();
  }

  const Worker *Get(int wid) const {
    if (wid < 0 || wid >= Worker::kMaxWorkers) {
      return nullptr;
    }
    return workers_[wid].load();
  }

  // The scheduler name this worker was launched with ("" for the default one).
  const std::string &scheduler_name(int wid) const {
    static const std::string kEmpty;
    if (wid < 0 || wid >= Worker::kMaxWorkers) {
      return kEmpty;
    }
    return scheduler_names_[wid];
  }

  bool IsActive(int wid) const {
    return wid >= 0 && wid < Worker::kMaxWorkers &&
           workers_[wid].load() != nullptr;
  }
  bool IsRunning(int wid) const;
  bool AnyRunning() const;
  Worker *NextActive();

  // Starts worker `wid` on `core`, and keeps this process's other threads
  // off every worker's CPU (D-027).
  void Launch(int wid, int core, const std::string &scheduler);
  void Destroy(int wid);
  void DestroyAll();
  void DetachAllThreads();

  void Pause(int wid);
  void PauseAll();
  void Resume(int wid);
  void ResumeAll();

  // True if a live worker is pinned to this CPU.
  bool IsCoreUsed(int core) const;

  // Orphan traffic classes: created by TC/module setup and attached to
  // workers when they resume.
  void AddOrphan(TrafficClass *c, int wid);
  bool RemoveOrphan(TrafficClass *c);
  const std::list<std::pair<int, TrafficClass *>> &orphan_tcs() const {
    return orphan_tcs_;
  }
  void AttachOrphans();

  // Detaches `c` from a scheduler root or from the orphan list, handing
  // ownership of the detached class to the caller.
  bool DetachTc(TrafficClass *c);

  // Collapses the per-worker default round-robin wrappers that orphan
  // attachment creates when a scheduler briefly holds more than one root.
  void AdjustSchedulerDefaults();

  // Called by the worker thread itself once it is up.
  void Publish(int wid, Worker *worker) { workers_[wid].store(worker); }

 private:
  // Moves non-worker threads off the active workers' CPUs.
  void PlaceThreads();
  // The slots are written by the worker thread and read by the control thread
  // (the launch spin below, pause/resume, teardown), so they are atomic --
  // the legacy `Worker *volatile workers[]` array is what this replaces.
  std::array<std::atomic<Worker *>, Worker::kMaxWorkers> workers_{};
  std::array<std::thread, Worker::kMaxWorkers> threads_;
  std::array<std::string, Worker::kMaxWorkers> scheduler_names_;
  int num_workers_ = 0;
  std::list<std::pair<int, TrafficClass *>> orphan_tcs_;
};

}  // namespace runtime
}  // namespace bess

#endif  // BESS_RUNTIME_WORKER_MANAGER_H_
