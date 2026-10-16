// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_WORKER_H_
#define BESS_WORKER_H_

#include "utils/logging.h"

#include <atomic>
#include <cstdint>
#include <optional>
#include <list>
#include <set>
#include <string>
#include <utility>

#include <sys/types.h>
#include <thread>
#include <type_traits>

#include "dataplane/cpu_id.h"
#include "dataplane/worker_id.h"
#include "gate.h"
#include "utils/common.h"
#include "utils/random.h"

#define MAX_GATES 8192

/*  TODO: worker threads doesn't necessarily be pinned to 1 core
 *
 *  Three separate identities -- do NOT assume they are the same value:
 *
 *  BESS WorkerId   stable BESS logical identity (launch_worker()'s `wid`,
 *                  [0, kMaxWorkers-1]; the index into `workers[]`)
 *  CPU ID          the physical CPU the worker asked to be pinned to
 *  DPDK lcore ID   dynamically allocated by rte_thread_register() at
 *                  Worker::Run() time (see MODERNIZATION.md entry 33) --
 *                  a re-created worker gets whatever id DPDK hands out
 *                  next, so there is no wid == lcore_id mapping
 *  master          RTE_MAX_LCORE-1 (EAL's main lcore; all other allowed
 *                  cores, no registration)
 */

typedef enum {
  WORKER_PAUSING = 0, /* transient state for blocking or quitting */
  WORKER_PAUSED,
  WORKER_RUNNING,
  WORKER_FINISHED,
} worker_status_t;

namespace bess {
class Scheduler;
class PacketPool;
class TrafficClass;
}  // namespace bess


class Task;

class Worker {
 public:
  // `constexpr` (not `const`): these are used as array bounds and in
  // comparisons, and a sanitizer build odr-uses them -- which needs the
  // implicitly-inline definition that `constexpr` static members have.
  static constexpr int kMaxWorkers = 64;
  static_assert(kMaxWorkers == bess::dataplane::kMaxWorkers);
  // "No worker given" on the wire and in a pipeline spec (a TC's or task's
  // wid); typed code says std::optional<WorkerId> instead.
  static constexpr int kAnyWorker = -1;

  /* ----------------------------------------------------------------------
   * functions below are invoked by non-worker threads (the master)
   * ---------------------------------------------------------------------- */
  void SetNonWorker();

  /* ----------------------------------------------------------------------
   * functions below are invoked by worker threads
   * ---------------------------------------------------------------------- */
  inline int is_pause_requested() { return status() == WORKER_PAUSING; }

  /* Block myself. Return nonzero if the worker needs to die */
  int BlockWorker();

  /* Report an RCU quiescent state (K1). The scheduler calls this at its
   * periodic boundary -- after a task invocation returned and before the next
   * one starts -- which is the only place a worker can legitimately claim that
   * no RCU pointer from the previous call stack is still in use. Never call it
   * from inside packet processing. */
  void ReportQuiescent();

  /* The entry point of worker threads */
  void *Run(void *_arg);

  // The master and the worker hand status_ back and forth: each side's
  // release store publishes what it wrote before it, and the other's
  // acquire load (a plain mov on x86) sees it. std::atomic_ref keeps Worker
  // trivially constructible, as __thread requires.
  worker_status_t status() {
    return std::atomic_ref(status_).load(std::memory_order_acquire);
  }
  void set_status(worker_status_t status) {
    std::atomic_ref(status_).store(status, std::memory_order_release);
  }

  // Meaningful on a worker thread only: elsewhere wid() is out of range
  // (CurrentWorkerId() checks), and core() and socket() are unset.
  bess::dataplane::WorkerId wid() const {
    return bess::dataplane::WorkerId(static_cast<uint16_t>(wid_));
  }
  bess::dataplane::CpuId core() const { return bess::dataplane::CpuId(core_); }
  // The worker's OS thread id (gettid), set before it reports ready.
  pid_t tid() const { return tid_; }
  bess::dataplane::SocketId socket() const {
    return bess::dataplane::SocketId(socket_);
  }
  int fd_event() { return fd_event_; }

  bess::PacketPool *packet_pool() { return packet_pool_; }

  bess::Scheduler *scheduler() const { return scheduler_; }

  uint64_t silent_drops() { return silent_drops_; }
  void set_silent_drops(uint64_t drops) { silent_drops_ = drops; }
  void incr_silent_drops(uint64_t drops) { silent_drops_ += drops; }

  uint64_t current_tsc() const { return current_tsc_; }
  void set_current_tsc(uint64_t tsc) { current_tsc_ = tsc; }

  uint64_t current_ns() const { return current_ns_; }
  void set_current_ns(uint64_t ns) { current_ns_ = ns; }

  Random *rand() const { return rand_; }

 private:
  alignas(std::atomic_ref<worker_status_t>::required_alignment)
      worker_status_t status_;

  // StrongId's default member initializer would make Worker non-trivial
  // (see __thread below), so the ids are stored raw. 32 bits, as the int it
  // was: the layout (every field's offset) is unchanged (D-133).
  uint32_t wid_;  // [0, kMaxWorkers - 1] on a worker; UINT16_MAX elsewhere
  // Whether this worker is an online RCU reader; touched only by its own
  // thread (BlockWorker, the thread's exit), read by ReportQuiescent().
  bool rcu_online_;
  uint32_t core_;
  pid_t tid_;  // set in Run() (Worker stays trivially constructible)
  uint32_t socket_;
  int fd_event_;

  bess::PacketPool *packet_pool_;

  bess::Scheduler *scheduler_;

  uint64_t silent_drops_; /* packets that have been sent to a deadend */

  uint64_t current_tsc_;
  uint64_t current_ns_;

  Random *rand_;
};

// NOTE: Do not use "thread_local" here. It requires a function call every time
// it is accessed. Use __thread instead, which incurs minimal runtime overhead.
// For this, g++ requires Worker to have a trivial constructor and destructor.
// Initial-exec: objects are built -fPIC, which makes each access a
// __tls_get_addr call (relaxed at link time, but every caller still saves the
// registers the call clobbers). The variable is defined in the executable, so
// it is always in static TLS, also when a dlopen'd plugin reads it.
extern __thread Worker current_worker
    __attribute__((tls_model("initial-exec")));

// the following traits are not supported in g++ 4.x
#if __GNUC__ >= 5
static_assert(std::is_trivially_constructible<Worker>::value,
              "not trivially constructible");
static_assert(std::is_trivially_destructible<Worker>::value,
              "not trivially destructible");
#endif

// A worker as the wire and pipeline specs carry it: its id, or kAnyWorker.
inline int WidOrAny(std::optional<bess::dataplane::WorkerId> wid) {
  return wid ? int{wid->value()} : Worker::kAnyWorker;
}

// The worker a wire or spec wid names: none for kAnyWorker. The value must
// already be validated (kAnyWorker or [0, kMaxWorkers)).
inline std::optional<bess::dataplane::WorkerId> WorkerOrAny(int wid) {
  if (wid == Worker::kAnyWorker) {
    return std::nullopt;
  }
  const auto id = bess::dataplane::WorkerIdFrom(wid);
  CHECK(id.has_value()) << "unvalidated worker id " << wid;
  return id;
}

// TODO: C++-ify

/* ------------------------------------------------------------------------
 * functions below are invoked by non-worker threads (the master)
 * ------------------------------------------------------------------------ */
int is_worker_core(bess::dataplane::CpuId cpu);

void pause_worker(bess::dataplane::WorkerId wid);
void pause_all_workers();

/*!
 * Attach orphan TCs to workers. Note this does not ensure optimal placement.
 */
void attach_orphans();
void resume_worker(bess::dataplane::WorkerId wid);
// Sends PreResume to every module worker `wid` runs that has not had it yet
// (`sent` records them), so they rebind to the current metadata offsets and
// graph before the worker resumes. Modules that answer -ENOTSUP are not asked
// again.
void send_pre_resume(bess::dataplane::WorkerId wid, std::set<Module *> *sent);
void resume_all_workers();
void destroy_worker(bess::dataplane::WorkerId wid);
void destroy_all_workers();

// Detach any worker_threads[] entry that's still joinable. Nothing on the
// normal shutdown path (Admin.Shutdown, or the SIGTERM/SIGINT watcher) ever
// joins or detaches worker threads -- the shutdown lambda only closes the
// event log and stops the server -- so main() exits with
// worker threads still alive (paused, or still running) and their handles
// still joinable. Call this once,
// right before process exit, so their std::thread destructors (run at
// static-destruction time, since worker_threads is a namespace-scope
// global) don't call std::terminate() over a still-joinable thread. Not a
// substitute for destroy_worker()'s join() on the actual teardown path
// (ResetWorkers/DestroyWorker RPCs); this only runs once, at shutdown.
void detach_all_worker_threads();

bool is_any_worker_running();

int is_cpu_present(unsigned int core_id);

int is_worker_active(bess::dataplane::WorkerId wid);

bool is_worker_running(bess::dataplane::WorkerId wid);

// Starts worker `wid` on CPU `core`, optionally with a named scheduler.
void launch_worker(bess::dataplane::WorkerId wid, bess::dataplane::CpuId core,
                   const std::string &scheduler = "");

Worker *get_next_active_worker();

// Add 'c' to the list of orphan traffic classes, for worker `wid` or (none)
// whichever worker attach_orphans() picks.
void add_tc_to_orphan(bess::TrafficClass *c,
                      std::optional<bess::dataplane::WorkerId> wid);

// Return true if 'c' was removed from the list of orphan traffic classes.
// 'c' is now owned by the caller, and it must be attached to a tree or
// destroyed.
//
// Otherwise, return false
bool remove_tc_from_orphan(bess::TrafficClass *c);

// Returns a list of all the orphan traffic classes.
const std::list<
    std::pair<std::optional<bess::dataplane::WorkerId>, bess::TrafficClass *>>
    &list_orphan_tcs();

// Try to detach 'c' from a scheduler, or from the list of orhpan traffic
// classes.
//
// Return true if successful. 'c' is now owned by the caller, and it must be
// attached to a tree or destroyed.
//
// Otherwise, return false
bool detach_tc(bess::TrafficClass *c);

// This class is used as a resource manager to automatically pause workers if
// running and then restarts workers if they were previously paused.
class WorkerPauser {
 public:
  explicit WorkerPauser();
  ~WorkerPauser();

 private:
  std::list<bess::dataplane::WorkerId> workers_paused_;
};

#endif  // BESS_WORKER_H_
