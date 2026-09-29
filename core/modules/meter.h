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

#ifndef BESS_MODULES_METER_H_
#define BESS_MODULES_METER_H_

#include <memory>
#include <set>
#include <string>

#include "../dataplane/resource.h"
#include "../meter/meter_set.h"
#include "../module.h"
#include "../pb/module_msg.pb.h"
#include "../rcu/rcu_ptr.h"

// Per-session metering: K5's MeterSet as a module, and its meters as a
// transactional resource (G1.2b).
//
// Packet path: the meter id arrives in packet metadata (a `meter_id` be32
// attribute that an ActionTable writes), the meter runs colour-blind on the
// packet's byte count at one TSC reading per batch, and the packet leaves on
// the gate that is its colour: 0 green, 1 yellow, 2 red. So the policy is the
// graph -- connect 0 and 1 to a forwarding module and leave 2 unconnected,
// and red packets are dropped as deadends. An id that names no meter is
// dropped rather than forwarded unmetered; id 0 (no meter) is green.
//
// Control path: `<module>/meters` is a resource, so one transaction can create
// a meter together with the action that names it and the rule that selects
// that action. A meter is an immutable policy over mutable token state that
// survives unrelated updates (K5): reconfiguring one meter publishes a
// generation in which every other meter keeps its buckets. An erase keeps the
// meter readable for one removal-cascade stage -- a reader may still hold its
// id from an action's old value -- and then publishes a generation without it,
// so the id cannot be reused until then (SlotTable's two-step removal, D-021).
//
// Meters are shared: any worker may check one, and each check takes the
// meter's own spinlock. A worker-exclusive meter is faster but needs a
// placement guarantee this module cannot verify; a pipeline that pins a
// session to one worker can use MeterSet directly (K5).
class Meter final : public Module {
 public:
  static const gate_idx_t kNumOGates = 3;
  static constexpr gate_idx_t kGreenGate = 0;
  static constexpr gate_idx_t kYellowGate = 1;
  static constexpr gate_idx_t kRedGate = 2;

  static const Commands cmds;

  Meter() : Module(), published_(bess::control::runtime().rcu()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::MeterArg &arg);
  void DeInit() override;

  // Reports an unreadable 'meter_id' attribute once per resume (the control
  // side's place for it, as in ExactMatch): the packet path only fails closed.
  int OnEvent(bess::Event event) override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  std::string GetDesc() const override;

  // What ProcessBatch decides, without emitting: gates[i] is packet i's output
  // gate (green 0, yellow 1, red 2; DROP_GATE for an id that names no meter).
  // The same code as the packet path, for tests and benchmarks.
  void MeterBatch(bess::PacketBatch *batch, gate_idx_t *gates) const;

  // The policy for one meter: its profile. Sharing and placement are the
  // module's (see the class comment).
  using Policy = bess::meter::MeterProfileSpec;

  // The module's `meters` resource. Public so tests can drive it directly.
  class MetersResource;

 private:
  friend class MetersResource;

  // What a transaction does to the desired state: edits a private copy of the
  // builder, so a rejected transaction leaves the live one untouched.
  void BeginTransaction();
  // A transaction ended (published or aborted): the private copy goes.
  void EndTransaction() noexcept;
  // Swaps the edited copy in and publishes `generation`, retiring the
  // generation it replaced through `retirer`.
  void PublishStaged(std::unique_ptr<const bess::meter::MeterSet> generation,
                     bess::dataplane::Retirer &retirer);
  // Runs one removal-cascade stage after an erase: the id is unpublishable
  // from the erase until this runs, and readable until it does.
  void Unpublish(bess::meter::MeterId id, bess::dataplane::Retirer &later);

  std::unique_ptr<bess::meter::MeterSetBuilder> live_;
  // The transaction's edit copy while one is in flight (see BeginTransaction).
  std::unique_ptr<bess::meter::MeterSetBuilder> staged_;
  // The edit copy has become the live one (its first publish ran).
  bool published_staged_ = false;
  // Erased meters whose removal stage has not run: absent to the control side,
  // still readable, and not publishable again (SlotTable's kRetiring).
  std::set<bess::meter::MeterId> retiring_;

  bess::rcu::RcuPtr<bess::meter::MeterSet> published_;

  std::unique_ptr<bess::dataplane::Resource> resource_;

  int meter_id_attr_ = -1;
};

#endif  // BESS_MODULES_METER_H_
