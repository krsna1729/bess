// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_QUEUE_H_
#define BESS_MODULES_QUEUE_H_

#include <rte_ring.h>

#include <atomic>
#include <expected>
#include <memory>
#include <string>

#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "stats/event_throttle.h"

class Queue : public Module {
 public:
  static const Commands cmds;
  static const ResourceTables resource_tables;

  Queue()
      : Module(),
        queue_(),
        prefetch_(),
        backpressure_(),
        burst_(),
        enqueue_fn_(&rte_ring_mp_enqueue_burst),
        size_(),
        high_water_(),
        low_water_(),
        stats_() {
    is_task_ = true;
    propagate_workers_ = false;
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::QueueArg &arg);
  CommandResponse GetInitialArg(const bess::pb::EmptyArg &);
  CommandResponse GetRuntimeConfig(const bess::pb::EmptyArg &arg);
  CommandResponse SetRuntimeConfig(const bess::pb::QueueArg &arg);

  void DeInit() override;

  int OnEvent(bess::Event event) override;

  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch,
                             void *arg) override;
  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  std::string GetDesc() const override;

  CommandResponse CommandSetBurst(const bess::pb::QueueCommandSetBurstArg &arg);
  CommandResponse CommandSetSize(const bess::pb::QueueCommandSetSizeArg &arg);
  CommandResponse CommandGetStatus(
      const bess::pb::QueueCommandGetStatusArg &arg);

  CheckConstraintResult CheckModuleConstraints() const override;

 private:
  const double kHighWaterRatio = 0.90;
  const double kLowWaterRatio = 0.15;

  // Frees a ring's queued packets, then the ring.
  struct RingFree {
    void operator()(struct rte_ring *ring) const;
  };
  using RingPtr = std::unique_ptr<struct rte_ring, RingFree>;

  // "<name>/settings" (D-129), staged: the new ring (null: same size)
  // allocated in stage, swapped in at apply.
  struct Staged {
    RingPtr ring;
    uint64_t size = 0;
    int burst = 0;
    bool prefetch = false;
    bool backpressure = false;
  };

  // An empty ring of `slots` slots, or the errno why not.
  static std::expected<RingPtr, int> AllocRing(uint64_t slots);
  // Moves `from`'s packets to `to`, freeing those that do not fit.
  static void MigrateRing(struct rte_ring *from, struct rte_ring *to);
  // set_size's validation of a size.
  static std::expected<void, std::string> ValidSize(uint64_t size);

  std::expected<Staged, std::string> StageSettings(
      const bess::pb::QueueSettings &value) const;
  void ApplySettings(Staged &staged);
  bess::pb::QueueSettings ReadSettings() const;

  int Resize(int slots);

  // Readjusts the water level according to `size_`.
  void AdjustWaterLevels();

  CommandResponse SetSize(uint64_t size);

  struct rte_ring *queue_;
  bool prefetch_;

  // Whether backpressure should be applied or not
  bool backpressure_;

  // Set by THREAD_SAFE commands while workers read it.

  std::atomic<int> burst_;
  using EnqueueFn = unsigned int (*)(struct rte_ring *, void * const *,
                                     unsigned int, unsigned int *);
  EnqueueFn enqueue_fn_;


  // Queue capacity
  uint64_t size_;

  // High water occupancy
  uint64_t high_water_;

  // Low water occupancy
  uint64_t low_water_;

  // Accumulated statistics counters. Several upstream workers enqueue at
  // once and the THREAD_SAFE get_status reads them while they run: relaxed
  // atomics, one add per batch (plain counters lost updates).
  struct {
    std::atomic<uint64_t> enqueued;
    std::atomic<uint64_t> dequeued;
    std::atomic<uint64_t> dropped;
  } stats_;

  bess::pb::QueueArg init_arg_;
  // Packets dropped because the ring was full (M25, D-089).
  bess::stats::EventThrottle full_;

  bess::framework::Singleton<bess::pb::QueueSettings, Staged> settings_;
};

#endif  // BESS_MODULES_QUEUE_H_
