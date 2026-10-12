// SPDX-License-Identifier: BSD-3-Clause

#include "control/admin_v2.h"

#include <cerrno>
#include <format>
#include <string>
#include <thread>

#include <rte_mempool.h>

#include "control/api_v2.h"
#include "gate.h"
#include "packet_pool.h"
#include "pb/bess_msg.pb.h"
#include "port.h"
#include "runtime/runtime_state.h"
#include "utils/logging.h"
#include "worker.h"

namespace bess {
namespace control {

namespace v2 = pb::v2;

namespace {

// The capture hooks' registered class names (gate_hooks/tcpdump.cc,
// pcapng.cc). By name, as the registry finds them: the control layer does
// not link the hooks (tools/check_link_graph.py). The tests compare them
// with the hooks' kName.
constexpr char kPcapHookClass[] = "TcpDump";
constexpr char kPcapngHookClass[] = "PcapNg";

grpc::Status Invalid(grpc::ServerContext *context, const char *field, const std::string &message) {
  ControlError error = Err(EINVAL, "%s", message.c_str());
  error.field = field;
  return ToStatus(error, context);
}

void Fill(const QueueStats &q, v2::PortStatus::Counters *out) {
  out->set_packets(q.packets);
  out->set_bytes(q.bytes);
  out->set_dropped(q.dropped);
  out->set_prepare_errors(q.tx_prepare_errors);
}

void Fill(::Port *port, v2::PortStatus *out) {
  out->set_name(port->name());
  out->set_driver(port->port_builder()->class_name());
  const ::Port::LinkStatus link = port->GetLinkStatus();
  out->mutable_link()->set_up(link.link_up);
  out->mutable_link()->set_speed_mbps(link.speed);
  out->mutable_link()->set_full_duplex(link.full_duplex);
  out->mutable_link()->set_autoneg(link.autoneg);
  const ::Port::PortStats stats = port->GetPortStats();
  Fill(stats.inc, out->mutable_rx());
  Fill(stats.out, out->mutable_tx());
}

}  // namespace

grpc::Status AdminV2Service::ImportPlugin(grpc::ServerContext *context,
                                          const v2::ImportPluginRequest *request,
                                          v2::ImportPluginResponse *response) {
  if (auto refused = RefuseOtherEpoch(request->daemon_epoch(), epoch_, context)) {
    return *refused;
  }
  if (auto done = control_plane_.ImportPlugin(request->path()); !done) {
    return ToStatus(done.error(), context);
  }
  response->set_daemon_epoch(epoch_);
  return grpc::Status::OK;
}

grpc::Status AdminV2Service::UnloadPlugin(grpc::ServerContext *context,
                                          const v2::UnloadPluginRequest *request,
                                          v2::UnloadPluginResponse *response) {
  if (auto refused = RefuseOtherEpoch(request->daemon_epoch(), epoch_, context)) {
    return *refused;
  }
  if (auto done = control_plane_.UnloadPlugin(request->path()); !done) {
    return ToStatus(done.error(), context);
  }
  response->set_daemon_epoch(epoch_);
  return grpc::Status::OK;
}

grpc::Status AdminV2Service::GetPortStatus(grpc::ServerContext *context,
                                           const v2::GetPortStatusRequest *request,
                                           v2::GetPortStatusResponse *response) {
  auto lock = control_plane_.AcquireLock();
  const runtime::PortRegistry &ports = runtime::runtime().ports();
  if (request->names().empty()) {
    for (const auto &[name, port] : ports.All()) {
      Fill(port.get(), response->add_ports());
    }
  } else {
    for (const std::string &name : request->names()) {
      ::Port *port = ports.Find(name);
      if (port == nullptr) {
        ControlError error = Err(ENOENT, "No port '%s' found", name.c_str());
        error.object = name;
        return ToStatus(error, context);
      }
      Fill(port, response->add_ports());
    }
  }
  response->set_daemon_epoch(epoch_);
  return grpc::Status::OK;
}

grpc::Status AdminV2Service::GetPacketPools(grpc::ServerContext *,
                                            const v2::GetPacketPoolsRequest *,
                                            v2::GetPacketPoolsResponse *response) {
  auto lock = control_plane_.AcquireLock();
  for (int socket = 0; socket < RTE_MAX_NUMA_NODES; socket++) {
    PacketPool *pool = PacketPool::GetDefaultPool(socket);
    if (pool == nullptr || pool->pool() == nullptr) {
      continue;
    }
    // Only struct rte_mempool's own fields and the public counts: valid for
    // every mempool backend.
    const rte_mempool *mp = pool->pool();
    v2::PacketPool *out = response->add_pools();
    out->set_socket(static_cast<uint32_t>(socket));
    out->set_size(mp->size);
    out->set_cache_size(mp->cache_size);
    out->set_element_size(mp->elt_size);
    out->set_populated(mp->populated_size);
    out->set_available(rte_mempool_avail_count(mp));
    out->set_in_use(rte_mempool_in_use_count(mp));
  }
  response->set_daemon_epoch(epoch_);
  return grpc::Status::OK;
}

grpc::Status AdminV2Service::SetCapture(grpc::ServerContext *context,
                                        const v2::SetCaptureRequest *request,
                                        v2::SetCaptureResponse *response) {
  if (auto refused = RefuseOtherEpoch(request->daemon_epoch(), epoch_, context)) {
    return *refused;
  }
  if (request->module().empty()) {
    return Invalid(context, "module", "module is required");
  }
  if (request->direction() != v2::SetCaptureRequest::DIRECTION_INPUT &&
      request->direction() != v2::SetCaptureRequest::DIRECTION_OUTPUT) {
    return Invalid(context, "direction", "direction must be DIRECTION_INPUT or DIRECTION_OUTPUT");
  }
  // Checked before narrowing to gate_idx_t: 65536 would capture gate 0.
  if (request->has_gate() && request->gate() >= MAX_GATES) {
    return Invalid(context, "gate", std::format("gate {} is out of range (< {})",
                                                request->gate(), MAX_GATES));
  }
  GateHookSpec spec;
  google::protobuf::Message *arg = nullptr;
  pb::TcpdumpArg pcap;
  pb::PcapngArg pcapng;
  switch (request->format()) {
    case v2::SetCaptureRequest::FORMAT_PCAP:
      spec.class_name = kPcapHookClass;
      pcap.set_fifo(request->fifo());
      arg = &pcap;
      break;
    case v2::SetCaptureRequest::FORMAT_PCAPNG:
      spec.class_name = kPcapngHookClass;
      pcapng.set_fifo(request->fifo());
      arg = &pcapng;
      break;
    default:
      return Invalid(context, "format", "format must be FORMAT_PCAP or FORMAT_PCAPNG");
  }
  if (request->enable() && request->fifo().empty()) {
    return Invalid(context, "fifo", "fifo is required to start a capture");
  }
  spec.module_name = request->module();
  spec.is_igate = request->direction() == v2::SetCaptureRequest::DIRECTION_INPUT;
  spec.use_gate = request->has_gate();
  spec.gate_idx = request->has_gate() ? request->gate() : 0;
  // A libpcap stream has one file header, and each hook writes its own: pcap
  // on "every gate" is the module's only gate in that direction, pinned here
  // so a gate added before the hook is installed changes nothing. pcapng
  // allows a section per hook.
  if (request->enable() && !spec.use_gate &&
      request->format() == v2::SetCaptureRequest::FORMAT_PCAP) {
    auto lock = control_plane_.AcquireLock();
    const Module *m = runtime::runtime().modules().Find(spec.module_name);
    if (m == nullptr) {
      ControlError error = Err(ENOENT, "No module '%s' found", spec.module_name.c_str());
      error.object = spec.module_name;
      return ToStatus(error, context);
    }
    std::vector<gate_idx_t> gates;
    const auto collect = [&](const auto &all) {
      for (size_t i = 0; i < all.size(); i++) {
        if (all[i] != nullptr) {
          gates.push_back(static_cast<gate_idx_t>(i));
        }
      }
    };
    spec.is_igate ? collect(m->igates()) : collect(m->ogates());
    if (gates.size() != 1) {
      ControlError error =
          gates.empty()
              ? Err(ENOENT, "'%s' has no %s gates to capture", spec.module_name.c_str(),
                    spec.is_igate ? "input" : "output")
              : Err(EINVAL,
                    "'%s' has %zu %s gates and a pcap stream holds one: name a gate, or use "
                    "FORMAT_PCAPNG",
                    spec.module_name.c_str(), gates.size(), spec.is_igate ? "input" : "output");
      error.field = "gate";
      return ToStatus(error, context);
    }
    spec.use_gate = true;
    spec.gate_idx = gates[0];
  }
  // Started under one name per format, so a gate has at most one capture of
  // each (a second is ALREADY_EXISTS, not a second writer on the FIFO);
  // stopped by class, which also stops one the legacy API started.
  if (request->enable()) {
    spec.hook_name = request->format() == v2::SetCaptureRequest::FORMAT_PCAP ? "pcap_capture"
                                                                              : "pcapng_capture";
  }
  CHECK(spec.arg.PackFrom(*arg));
  if (auto done = control_plane_.ConfigureGateHook(spec, request->enable()); !done) {
    return ToStatus(done.error(), context);
  }
  response->set_daemon_epoch(epoch_);
  return grpc::Status::OK;
}

grpc::Status AdminV2Service::PauseWorkers(grpc::ServerContext *context,
                                          const v2::PauseWorkersRequest *request,
                                          v2::PauseWorkersResponse *response) {
  if (auto refused = RefuseOtherEpoch(request->daemon_epoch(), epoch_, context)) {
    return *refused;
  }
  if (auto done = control_plane_.PauseAll(); !done) {
    return ToStatus(done.error(), context);
  }
  response->set_daemon_epoch(epoch_);
  return grpc::Status::OK;
}

grpc::Status AdminV2Service::ResumeWorkers(grpc::ServerContext *context,
                                           const v2::ResumeWorkersRequest *request,
                                           v2::ResumeWorkersResponse *response) {
  if (auto refused = RefuseOtherEpoch(request->daemon_epoch(), epoch_, context)) {
    return *refused;
  }
  // The legacy ResumeAll: orphan tasks attached, resume hooks run first.
  if (auto done = control_plane_.ResumeAll(); !done) {
    return ToStatus(done.error(), context);
  }
  response->set_daemon_epoch(epoch_);
  return grpc::Status::OK;
}

grpc::Status AdminV2Service::Shutdown(grpc::ServerContext *context,
                                      const v2::ShutdownRequest *request,
                                      v2::ShutdownResponse *response) {
  if (auto refused = RefuseOtherEpoch(request->daemon_epoch(), epoch_, context)) {
    return *refused;
  }
  if (!shutdown_) {
    return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "this server cannot shut down");
  }
  auto lock = control_plane_.AcquireLock();
  WorkerPauser pause;
  LOG(WARNING) << "Shutdown requested by a client";
  // Stopping the server waits for the calls in flight, this one included:
  // from a thread of its own, after this answer is on its way.
  std::thread(shutdown_).detach();
  response->set_daemon_epoch(epoch_);
  return grpc::Status::OK;
}

}  // namespace control
}  // namespace bess
