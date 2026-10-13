// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_MODULES_FLOWGEN_H_
#define BESS_MODULES_FLOWGEN_H_

#include <atomic>
#include <optional>

#include "framework/singleton_resource.h"
#include "module.h"
#include "pb/module_msg.pb.h"

#include <queue>
#include <stack>

#include "utils/endian.h"
#include "utils/random.h"

#define MAX_TEMPLATE_SIZE 1536

typedef std::pair<uint64_t, struct flow *> Event;
typedef std::priority_queue<Event, std::vector<Event>, std::greater<Event>>
    EventQueue;

struct flow {
  int packets_left;
  bool first_pkt;

  uint32_t next_seq_no;
  bess::utils::be32_t src_ip, dst_ip;
  bess::utils::be16_t src_port, dst_port;
};

class FlowGen final : public Module {
 public:
  enum class Arrival {
    kUniform = 0,
    kExponential,
  };

  enum class Duration {
    kUniform = 0,
    kPareto,
  };

  static const gate_idx_t kNumIGates = 0;

  FlowGen()
      : Module(),
        active_flows_(),
        generated_flows_(),
        flows_free_(),
        events_(),
        params_(),
        rng_(),
        quick_rampup_(),
        burst_() {
    is_task_ = true;
  }

  static const Commands cmds;
  // "settings" (FlowGenSettings): what `update` and `set_burst` set; the
  // task reads the generator's parameters unsynchronized, so it requires
  // paused workers (D-129).
  static const ResourceTables resource_tables;

  CommandResponse Init(const bess::pb::FlowGenArg &arg);
  CommandResponse CommandUpdate(const bess::pb::FlowGenArg &arg);
  CommandResponse CommandSetBurst(
      const bess::pb::FlowGenCommandSetBurstArg &arg);

  void DeInit() override;

  struct task_result RunTask(Context *ctx, bess::PacketBatch *batch,
                             void *arg) override;

  std::string GetDesc() const override;

 private:
  // What `update` changes: the template, the load and the distributions,
  // and what is derived from them. Only the control thread writes it (Init,
  // update, the settings resource with workers paused); the task reads it.
  struct Params {
    unsigned char tmpl[MAX_TEMPLATE_SIZE] = {};
    int template_size = 0;
    uint8_t l4_proto = 0;

    /* base ip and ports IN HOST ORDER */
    uint32_t ip_src_base = 0;
    uint32_t ip_dst_base = 0;
    uint16_t port_src_base = 0;
    uint16_t port_dst_base = 0;

    Arrival arrival = Arrival::kUniform;
    Duration duration = Duration::kUniform;

    /* load parameters */
    double total_pps = 0;
    double flow_rate = 0;     /* in flows/s */
    double flow_duration = 0; /* in seconds */

    /* derived variables */
    double concurrent_flows = 0; /* expected # of flows */
    double flow_pps = 0;         /* packets/s/flow */
    double flow_pkts = 0;        /* flow_pps * flow_duration */
    double flow_gap_ns = 0;      /* == 10^9 / flow_rate */

    struct {
      double alpha;
      double inversed_alpha; /* 1.0 / alpha */
      double mean;           /* determined by alpha */
    } pareto = {};
  };

  // A staged settings value: the parameters to install, and the burst
  // (unset: unchanged).
  struct Settings {
    Params params;
    std::optional<int> burst;
  };

  static void UpdateDerivedParameters(Params *p);
  double NewFlowPkts();
  double MaxFlowPkts() const;
  uint64_t NextFlowArrival();
  struct flow *ScheduleFlow(uint64_t time_ns);
  static void MeasureParetoMean(Params *p);
  void PopulateInitialFlows();

  static void UpdateBaseAddresses(Params *p);
  bess::PacketRef FillUdpPacket(struct flow *f);
  bess::PacketRef FillTcpPacket(struct flow *f);
  void GeneratePackets(Context *ctx, bess::PacketBatch *batch);

  CommandResponse ProcessArguments(const bess::pb::FlowGenArg &arg);
  // Validates `update`'s fields and applies them to `p` (zero or empty:
  // unchanged). `p` is half-updated on a refusal.
  static CommandResponse ProcessUpdatableArguments(
      const bess::pb::FlowGenArg &arg, Params *p);

  // the number of concurrent flows
  int active_flows_;
  // the total number of flows generated so far (statistics only)
  uint64_t generated_flows_;
  // pool of free flow structs. LIFO for temporal locality.
  std::stack<struct flow *> flows_free_;

  // Priority queue of future events
  EventQueue events_;

  Params params_;

  Random rng_;

  /* behavior parameters */
  int quick_rampup_;

  /* ranges over which to vary ips and ports */
  uint32_t ip_src_range_;
  uint32_t ip_dst_range_;
  uint16_t port_src_range_;
  uint16_t port_dst_range_;

  // Set by THREAD_SAFE commands while workers read it.
  std::atomic<int> burst_;

  // "<name>/settings"; after the state it sets.
  bess::framework::Singleton<bess::pb::FlowGenSettings, Settings> settings_;
};

#endif  // BESS_MODULES_FLOWGEN_H_
