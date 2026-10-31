// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_METER_H_
#define BESS_MODULES_METER_H_

#include <memory>
#include <optional>
#include <string>

#include "framework/memory_scope.h"
#include "framework/resource_bindings.h"
#include "meter/meter_resource.h"
#include "meter/meter_set.h"
#include "module.h"
#include "pb/module_msg.pb.h"
#include "rcu/rcu_ptr.h"

// Per-session metering: K5's MeterSet as a module, and its meters as a
// transactional resource (G1.2b).
//
// Packet path: up to `levels` meter ids arrive in packet metadata (contiguous
// be32 slots of one `meter_id` attribute a classifier result writes). At one
// TSC reading per batch, the module runs one colour-aware pass per level, in
// order, over the packets the earlier levels let live: green and yellow
// packets are charged to the later levels and a red packet stops, so a later
// (session) bucket is not billed for what an earlier (flow) bucket dropped.
// The packet leaves on the gate that is its worst colour: 0 green, 1 yellow,
// 2 red. So the policy is the graph -- connect 0 and 1 to a forwarding module
// and leave 2 unconnected, and red packets are dropped as deadends. An id that
// names no meter is dropped rather than forwarded unmetered; id 0 means "no
// meter at this level" and passes it.
//
// Control path: `<module>/meters` is a bess::meter::MeterResource (D-199), so
// one transaction can create a meter together with the rule whose result
// names it. A meter is an immutable policy over mutable token state that
// survives unrelated updates (K5): reconfiguring one meter publishes a
// generation in which every other meter keeps its buckets. An erase keeps the
// meter readable for one removal-cascade stage -- a reader may still hold its
// id from a rule's old result -- and then publishes a generation without it,
// so the id cannot be reused until then (SlotTable's two-step removal, D-021).
//
// Meters are shared: any worker may check one, and each check takes the
// meter's own spinlock. A worker-exclusive meter is faster but needs a
// placement guarantee this module cannot verify; a pipeline that pins a
// session to one worker can use MeterSet directly (K5).
//
// Operational state: `get_status` (a THREAD_SAFE action) reads meters without
// stopping traffic. For each meter it returns the profile, the sharing mode,
// the TSC of the last update, and both token buckets with their current
// tokens and limits. A shared meter is read under the same spinlock its
// checks take, so the reading is coherent and the packet path gains no
// atomic, lock or write. The reading is per meter state, never a module-wide
// total: this module creates shared meters, so the reply reports each as
// shared; a worker-exclusive meter's state belongs to its owner and is only
// coherent when that worker is not running it (MeterState::Inspect documents
// the core API's contract).
class Meter final : public Module {
 public:
  static const gate_idx_t kNumOGates = 3;
  static constexpr gate_idx_t kGreenGate = 0;
  static constexpr gate_idx_t kYellowGate = 1;
  static constexpr gate_idx_t kRedGate = 2;

  static const Commands cmds;
  // "<module>/meters": MeterIdKey -> MeterPolicyValue (D-129).
  static const ResourceTables resource_tables;
  // Any worker charges a shared meter's token buckets under the meter's lock.
  static constexpr ModuleCapabilities kCapabilities{ModuleCapabilities::State::kShared,
                                                    ModuleCapabilities::FlowAffinity::kNone};

  Meter() : Module(), published_(init_context().rcu()) {
    max_allowed_workers_ = Worker::kMaxWorkers;
  }

  CommandResponse Init(const bess::pb::MeterArg &arg);
  void DeInit() override;

  // Reports an unreadable 'meter_id' attribute once per resume (the control
  // side's place for it, as in ExactMatch): the packet path only fails closed.
  int OnEvent(bess::Event event) override;

  void ProcessBatch(Context *ctx, bess::PacketBatch *batch) override;

  // The `get_status` action: read-only inspection of the meters' profiles and
  // token state. It takes no locks beyond each shared meter's own check lock,
  // so it runs while workers keep metering (THREAD_SAFE). Without ids it
  // returns one page, ascending from `start_id`, of at most `limit` (at most
  // kStatusPage) meters, and the next page's `start_id` (0 after the last).
  CommandResponse CommandGetStatus(
      const bess::pb::MeterCommandGetStatusArg &arg);
  // The largest get_status page: about 80 bytes a meter, far below the reply
  // limit however many meters are configured.
  static constexpr size_t kStatusPage = 1024;

  std::string GetDesc() const override;

  // What ProcessBatch decides, without emitting: gates[i] is packet i's output
  // gate (green 0, yellow 1, red 2; DROP_GATE for an id that names no meter).
  // The same code as the packet path, for tests and benchmarks.
  void MeterBatch(bess::PacketBatch *batch, gate_idx_t *gates) const;

  // The policy for one meter: its profile. Sharing and placement are the
  // module's (see the class comment).
  using Policy = bess::meter::MeterProfileSpec;

 private:
  // "<module>/meters" (D-199), with its D-198 admission: Init admits
  // MeterResource::Bytes(capacity) and the resource admits all it grows by
  // through the lease's hook. Declared before published_, which it
  // initializes, so the published generation is freed first.
  bess::framework::Sized<bess::meter::MeterResource> meters_;
  bess::rcu::RcuPtr<bess::meter::MeterSet> published_;
  bess::framework::ResourceBinding binding_;  // after meters_ (D-044)

  // How many meter ids one packet carries (the `meter_id` attribute is
  // 4 * levels_ bytes wide). 1 is the module's historical behaviour.
  size_t levels_ = 1;
  // What one packet is charged: its bytes (the historical unit) or one unit,
  // for a packet-rate meter.
  bool packets_ = false;

  // What the meters charge: the whole frame (historical), the IP datagram
  // (L3), the transport segment inside it (L4), or every frame byte from
  // `byte_offset_` on (CUSTOM).
  enum class ByteBasis : uint8_t { kL2, kL3, kL4, kCustom };
  ByteBasis byte_basis_ = ByteBasis::kL2;
  uint32_t byte_offset_ = 0;

  // The bytes this packet is charged at `byte_basis_`, or nullopt when the
  // packet cannot be measured (then it is dropped, never forwarded unmetered).
  std::optional<uint32_t> AccountedBytes(bess::PacketRef pkt) const;

  int meter_id_attr_ = -1;
  // The attribute's registered name, for the one log line that names it.
  std::string id_attr_name_;
};

#endif  // BESS_MODULES_METER_H_
