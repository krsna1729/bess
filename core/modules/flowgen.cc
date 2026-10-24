// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "flowgen.h"

#include <cmath>
#include <expected>
#include <functional>
#include <string>

#include "utils/checksum.h"
#include "utils/ether.h"
#include "utils/format.h"
#include "utils/ip.h"
#include "utils/tcp.h"
#include "utils/udp.h"
#include "utils/time.h"

using bess::utils::Ethernet;
using bess::utils::Ipv4;
using bess::utils::Tcp;
using bess::utils::Udp;
using bess::utils::be16_t;
using bess::utils::be32_t;

/* we ignore the last 1% tail to make the variance finite */
const double PARETO_TAIL_LIMIT = 0.99;
constexpr size_t kMinTcpPacketSize = 60;

// "settings" (FlowGenSettings) holds the parameters; the class has no
// actions.
const Commands FlowGen::cmds = {};

const ResourceTables FlowGen::resource_tables = {
    ResourceTable::Singleton<bess::pb::FlowGenSettings>("settings",
                                                       /*requires_paused_workers=*/true)};

namespace {

// set_burst's refusal ("" when `burst` is valid).
std::string BurstError(uint64_t burst) {
  if (burst > bess::PacketBatch::kMaxBurst) {
    return bess::utils::Format("'burst' must be no greater than %zu",
                               bess::PacketBatch::kMaxBurst);
  }
  return "";
}

}  // namespace

/* find x from CDF of pareto distribution from given y in [0.0, 1.0) */
static inline double pareto_variate(double inversed_alpha, double y) {
  return pow(1.0 / (1.0 - y * PARETO_TAIL_LIMIT), inversed_alpha);
}

static inline double scaled_pareto_variate(double inversed_alpha, double mean,
                                           double desired_mean, double y) {
  double x = pareto_variate(inversed_alpha, y);

  return 1.0 + (x - 1.0) / (mean - 1.0) * (desired_mean - 1.0);
}

inline double FlowGen::NewFlowPkts() {
  switch (params_.duration) {
    case Duration::kUniform:
      return params_.flow_pkts;
    case Duration::kPareto:
      return scaled_pareto_variate(params_.pareto.inversed_alpha,
                                   params_.pareto.mean, params_.flow_pkts,
                                   rng_.GetReal());
    default:
      CHECK(0);
  }
  return 0;
}

inline double FlowGen::MaxFlowPkts() const {
  switch (params_.duration) {
    case Duration::kUniform:
      return params_.flow_pkts;
    case Duration::kPareto:
      return scaled_pareto_variate(params_.pareto.inversed_alpha,
                                   params_.pareto.mean, params_.flow_pkts, 1.0);
    default:
      CHECK(0);
  }
  return 0;
}

inline uint64_t FlowGen::NextFlowArrival() {
  switch (params_.arrival) {
    case Arrival::kUniform:
      return params_.flow_gap_ns;
    case Arrival::kExponential:
      return -log(rng_.GetRealNonzero()) * params_.flow_gap_ns;
    default:
      CHECK(0);
  }
  return 0;
}

// should never fail, always returning a non-null pointer
inline flow *FlowGen::ScheduleFlow(uint64_t time_ns) {
  struct flow *f;

  if (flows_free_.empty()) {
    f = new flow();
  } else {
    f = flows_free_.top();
    flows_free_.pop();
  }

  f->first_pkt = true;
  f->next_seq_no = 12345;
  f->src_ip = be32_t(params_.ip_src_base + rng_.GetRange(ip_src_range_));
  f->dst_ip = be32_t(params_.ip_dst_base + rng_.GetRange(ip_dst_range_));
  f->src_port = be16_t(params_.port_src_base + rng_.GetRange(port_src_range_));
  f->dst_port = be16_t(params_.port_dst_base + rng_.GetRange(port_dst_range_));

  /* compensate the fraction part by adding [0.0, 1.0) */
  f->packets_left = NewFlowPkts() + rng_.GetReal();

  active_flows_++;
  generated_flows_++;

  events_.emplace(time_ns, f);

  return f;
}

void FlowGen::MeasureParetoMean(Params *p) {
  const int iteration = 1000000;
  double total = 0.0;

  for (int i = 0; i <= iteration; i++) {
    double y = i / (double)iteration;
    double x = pareto_variate(p->pareto.inversed_alpha, y);
    total += x;
  }

  p->pareto.mean = total / (iteration + 1);
}

void FlowGen::PopulateInitialFlows() {
  /* cannot use ctx.current_ns in the master thread... */
  uint64_t now_ns = rdtsc() / tsc_hz * 1e9;

  ScheduleFlow(now_ns);

  if (!quick_rampup_ || params_.flow_pps < 1.0 || params_.flow_rate < 1.0) {
    return;
  }

  /* emulate pre-existing flows at the beginning */
  double past_origin = MaxFlowPkts() / params_.flow_pps; /* in secs */
  double step = 1.0 / params_.flow_rate;

  for (double past = step; past < past_origin; past += step) {
    double pre_consumed_pkts = params_.flow_pps * past;
    double flow_pkts = NewFlowPkts();

    if (flow_pkts > pre_consumed_pkts) {
      uint64_t jitter = 1e9 * rng_.GetReal() / params_.flow_pps;

      struct flow *f = ScheduleFlow(now_ns + jitter);

      /* overwrite with a emulated pre-existing flow */
      f->first_pkt = false;
      f->next_seq_no = 56789;
      f->packets_left = flow_pkts - pre_consumed_pkts;
    }
  }
}

CommandResponse FlowGen::ProcessUpdatableArguments(
    const bess::pb::FlowGenArg &arg, Params *p) {

  if (arg.template_().length() == 0) {
    if (strnlen(reinterpret_cast<const char *>(p->tmpl), MAX_TEMPLATE_SIZE) ==
        0) {
      return CommandFailure(EINVAL, "must specify 'template'");
    }
  } else {
    // update template
    if (arg.template_().length() > MAX_TEMPLATE_SIZE) {
      return CommandFailure(EINVAL, "'template' is too big");
    }

    const char *tmpl = arg.template_().c_str();
    const Ethernet *eth = reinterpret_cast<const Ethernet *>(tmpl);
    if (eth->ether_type != be16_t(Ethernet::Type::kIpv4)) {
      return CommandFailure(EINVAL, "'template' is not IPv4");
    }

    const Ipv4 *ip = reinterpret_cast<const Ipv4 *>(eth + 1);
    if (ip->protocol != Ipv4::Proto::kUdp &&
        ip->protocol != Ipv4::Proto::kTcp) {
      return CommandFailure(EINVAL, "'template' is not UDP or TCP");
    }
    if (ip->protocol == Ipv4::Proto::kTcp &&
        arg.template_().length() < kMinTcpPacketSize) {
      return CommandFailure(EINVAL, "TCP 'template' must be at least 60 bytes");
    }

    if (p->l4_proto == 0) {
      p->l4_proto = ip->protocol;
    } else if (p->l4_proto != ip->protocol) {
      return CommandFailure(EINVAL, "'template' can not be updated");
    }

    p->template_size = arg.template_().length();

    memset(p->tmpl, 0, MAX_TEMPLATE_SIZE);
    bess::utils::Copy(p->tmpl, tmpl, p->template_size);
    UpdateBaseAddresses(p);
  }

  if (arg.pps() != 0) {
    if (std::isnan(arg.pps()) || arg.pps() < 0.0) {
      return CommandFailure(EINVAL, "invalid 'pps'");
    }
    p->total_pps = arg.pps();
  }

  if (arg.flow_rate() != 0) {
    if (std::isnan(arg.flow_rate()) || arg.flow_rate() < 0.0) {
      return CommandFailure(EINVAL, "invalid 'flow_rate'");
    }
    p->flow_rate = arg.flow_rate();
  }

  if (p->flow_rate > p->total_pps) {
    return CommandFailure(EINVAL, "flow rate cannot be more than pps");
  }

  if (arg.flow_duration() != 0) {
    if (std::isnan(arg.flow_duration()) || arg.flow_duration() < 0.0) {
      return CommandFailure(EINVAL, "invalid 'flow_duration'");
    }
    p->flow_duration = arg.flow_duration();
  }

  if (arg.arrival() == "uniform") {
    p->arrival = Arrival::kUniform;
  } else if (arg.arrival() == "exponential") {
    p->arrival = Arrival::kExponential;
  } else {
    if (arg.arrival() != "") {
      return CommandFailure(
          EINVAL, "'arrival' must be either 'uniform' or 'exponential'");
    }
  }

  if (arg.duration() == "uniform") {
    p->duration = Duration::kUniform;
  } else if (arg.duration() == "pareto") {
    p->duration = Duration::kPareto;
  } else {
    if (arg.duration() != "") {
      return CommandFailure(EINVAL,
                            "'duration' must be either 'uniform' or 'pareto'");
    }
  }

  return CommandSuccess();
}

CommandResponse FlowGen::ProcessArguments(const bess::pb::FlowGenArg &arg) {
  if (arg.quick_rampup()) {
    quick_rampup_ = 1;
  }

  ip_src_range_ = arg.ip_src_range();
  ip_dst_range_ = arg.ip_dst_range();

  if (arg.port_src_range() > 65535 || arg.port_dst_range() > 65535) {
    return CommandFailure(EINVAL, "portrang must be e <= 65535");
  }

  port_src_range_ = (uint16_t)arg.port_src_range();
  port_dst_range_ = (uint16_t)arg.port_dst_range();

  if (ip_src_range_ == 0 && ip_dst_range_ == 0 && port_src_range_ == 0 &&
      port_dst_range_ == 0) {
    /*randomize ports anyway*/
    port_dst_range_ = 20000;
    port_src_range_ = 20000;
  }

  return CommandSuccess();
}

void FlowGen::UpdateDerivedParameters(Params *p) {
  /* calculate derived variables */
  p->pareto.inversed_alpha = 1.0 / p->pareto.alpha;

  if (p->duration == Duration::kPareto) {
    MeasureParetoMean(p);
  }

  p->concurrent_flows = p->flow_rate * p->flow_duration;
  if (p->concurrent_flows > 0.0) {
    p->flow_pps = p->total_pps / p->concurrent_flows;
  }

  p->flow_pkts = p->flow_pps * p->flow_duration;
  if (p->flow_rate > 0.0) {
    p->flow_gap_ns = 1e9 / p->flow_rate;
  }
}

CommandResponse FlowGen::Init(const bess::pb::FlowGenArg &arg) {
  task_id_t tid;
  CommandResponse err;

  rng_.SetSeed(0xBAADF00DDEADBEEFul);

  /* set default parameters */
  params_.total_pps = 1000.0;
  params_.flow_rate = 10.0;
  params_.flow_duration = 10.0;
  params_.arrival = Arrival::kUniform;
  params_.duration = Duration::kUniform;
  params_.pareto.alpha = 1.3;
  burst_.store(bess::PacketBatch::kMaxBurst, std::memory_order_relaxed);
  params_.l4_proto = 0;

  /* register task */
  tid = RegisterTask(nullptr);
  if (tid == INVALID_TASK_ID) {
    return CommandFailure(ENOMEM, "task creation failed");
  }

  err = ProcessArguments(arg);
  if (err.error().code() != 0) {
    return err;
  }

  err = ProcessUpdatableArguments(arg, &params_);
  if (err.error().code() != 0) {
    return err;
  }

  UpdateDerivedParameters(&params_);

  /* add a seed flow (and background flows if necessary) */
  PopulateInitialFlows();

  // The task reads the parameters unsynchronized: a staged value is built
  // whole (derived parameters included) from the current one, as update
  // builds it, and copied in with the workers paused. Flows already
  // scheduled keep going, as after update.
  auto registered = settings_.Register(
      init_context().resources(), init_context().codecs(), name() + "/settings",
      [this](const bess::pb::FlowGenSettings &v)
          -> std::expected<Settings, std::string> {
        bess::pb::FlowGenArg update;
        update.set_template_(v.template_());
        update.set_pps(v.pps());
        update.set_flow_rate(v.flow_rate());
        update.set_flow_duration(v.flow_duration());
        update.set_arrival(v.arrival());
        update.set_duration(v.duration());
        Settings s{params_, std::nullopt};
        CommandResponse err = ProcessUpdatableArguments(update, &s.params);
        if (err.error().code() != 0) {
          return std::unexpected(err.error().errmsg());
        }
        UpdateDerivedParameters(&s.params);
        if (v.has_burst()) {
          if (std::string burst_err = BurstError(v.burst()); !burst_err.empty()) {
            return std::unexpected(std::move(burst_err));
          }
          s.burst = static_cast<int>(v.burst());
        }
        return s;
      },
      [this](Settings &s) {
        params_ = s.params;
        if (s.burst) {
          burst_.store(*s.burst, std::memory_order_relaxed);
        }
      },
      [this] {
        bess::pb::FlowGenSettings v;
        v.set_template_(reinterpret_cast<const char *>(params_.tmpl),
                        params_.template_size);
        v.set_pps(params_.total_pps);
        v.set_flow_rate(params_.flow_rate);
        v.set_flow_duration(params_.flow_duration);
        v.set_arrival(params_.arrival == Arrival::kExponential ? "exponential"
                                                               : "uniform");
        v.set_duration(params_.duration == Duration::kPareto ? "pareto"
                                                             : "uniform");
        v.set_burst(burst_.load(std::memory_order_relaxed));
        return v;
      },
      [this] { return HasRunningWorker(); });
  if (!registered) {
    return CommandFailure(EEXIST, "%s", registered.error().c_str());
  }

  return CommandSuccess();
}

void FlowGen::DeInit() {
  settings_.Release();
  while (!events_.empty()) {  // scheduled flows: owned by the queue until popped
    delete events_.top().second;
    events_.pop();
  }
  while (!flows_free_.empty()) {
    delete flows_free_.top();
    flows_free_.pop();
  }
}

void FlowGen::UpdateBaseAddresses(Params *p) {
  Ipv4 *ip = reinterpret_cast<Ipv4 *>(p->tmpl + sizeof(Ethernet));
  p->ip_src_base = ip->src.value();
  p->ip_dst_base = ip->dst.value();
  size_t ip_bytes = (ip->header_length) << 2;

  /* UDP and TCP can share the same header only for  port' contexts */
  Udp *l4 = reinterpret_cast<Udp *>(p->tmpl + sizeof(Ethernet) + ip_bytes);
  p->port_src_base = l4->src_port.value();
  p->port_dst_base = l4->dst_port.value();
}

bess::PacketRef FlowGen::FillUdpPacket(struct flow *f) {
  bess::PacketRef pkt(current_worker.packet_pool()->Alloc());
  const int size = params_.template_size;
  if (!pkt.handle()) {
    return {};
  }

  pkt.reset();
  void *data = pkt.append(size);
  if (!data) {
    bess::PacketFree(pkt.handle());
    return {};
  }
  bess::utils::Copy(data, params_.tmpl, size, true);

  Ethernet *eth = pkt.head_data<Ethernet *>();
  Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1);
  ip->src = f->src_ip;
  ip->dst = f->dst_ip;

  size_t ip_bytes = (ip->header_length) << 2;
  Udp *udp = reinterpret_cast<Udp *>(reinterpret_cast<char *>(ip) + ip_bytes);
  udp->src_port = f->src_port;
  udp->dst_port = f->dst_port;

  udp->checksum = bess::utils::CalculateIpv4UdpChecksum(*ip, *udp);
  ip->checksum = bess::utils::CalculateIpv4Checksum(*ip);

  return pkt;
}

bess::PacketRef FlowGen::FillTcpPacket(struct flow *f) {
  bess::PacketRef pkt(current_worker.packet_pool()->Alloc());
  const int size = params_.template_size;
  if (!pkt.handle()) {
    return {};
  }

  pkt.reset();
  void *data = pkt.append(size);
  if (!data) {
    bess::PacketFree(pkt.handle());
    return {};
  }
  bess::utils::Copy(data, params_.tmpl, size, true);

  Ethernet *eth = pkt.head_data<Ethernet *>();
  Ipv4 *ip = reinterpret_cast<Ipv4 *>(eth + 1);

  // SYN or FIN?
  if (f->first_pkt || f->packets_left <= 1) {
    pkt.trim(size - kMinTcpPacketSize);
    ip->length = be16_t(40);
  }

  uint8_t tcp_flags = f->first_pkt ? /* SYN */ 0x02 : /* ACK */ 0x10;
  if (f->packets_left <= 1) {
    tcp_flags |= 0x01; /* FIN */
  }

  ip->src = f->src_ip;
  ip->dst = f->dst_ip;

  size_t ip_bytes = (ip->header_length) << 2;
  Tcp *tcp = reinterpret_cast<Tcp *>(reinterpret_cast<char *>(ip) + ip_bytes);
  tcp->src_port = f->src_port;
  tcp->dst_port = f->dst_port;
  tcp->flags = tcp_flags;
  tcp->seq_num = be32_t(f->next_seq_no);
  tcp->checksum = bess::utils::CalculateIpv4TcpChecksum(*ip, *tcp);
  ip->checksum = bess::utils::CalculateIpv4Checksum(*ip);

  f->next_seq_no +=
      f->first_pkt ? 1 : size - (sizeof(*eth) + sizeof(*ip) + sizeof(*tcp));

  return pkt;
}

void FlowGen::GeneratePackets(Context *ctx, bess::PacketBatch *batch) {
  uint64_t now = ctx->current_ns;

  batch->clear();
  const int burst = burst_.load(std::memory_order_relaxed);

  while (batch->cnt() < burst && !events_.empty()) {
    uint64_t t = events_.top().first;
    struct flow *f = events_.top().second;
    if (!f || now < t)
      return;

    events_.pop();

    if (f->packets_left <= 0) {
      flows_free_.push(f);
      active_flows_--;
      continue;
    }

    bess::PacketRef pkt;
    if (params_.l4_proto == Ipv4::Proto::kUdp) {
      pkt = FillUdpPacket(f);
    } else if (params_.l4_proto == Ipv4::Proto::kTcp) {
      pkt = FillTcpPacket(f);
    }
    if (pkt.handle()) {
      batch->add(pkt);
    }

    if (f->first_pkt) {
      ScheduleFlow(t + NextFlowArrival());
      f->first_pkt = false;
    }

    f->packets_left--;

    events_.emplace(t + static_cast<uint64_t>(1e9 / params_.flow_pps), f);
  }
}

struct task_result FlowGen::RunTask(Context *ctx, bess::PacketBatch *batch,
                                    void *) {
  if (children_overload_ > 0) {
    return {
        .block = true, .packets = 0, .bits = 0,
    };
  }

  const int pkt_overhead = 24;

  GeneratePackets(ctx, batch);
  RunNextModule(ctx, batch);

  uint32_t cnt = batch->cnt();
  return {.block = (cnt == 0),
          .packets = cnt,
          .bits = uint64_t{static_cast<uint32_t>(params_.template_size +
                                                 pkt_overhead)} *
                  cnt * 8};
}

std::string FlowGen::GetDesc() const {
  return bess::utils::Format("%d flows", active_flows_);
}

ADD_MODULE(FlowGen, "flowgen", "generates packets on a flow basis")
