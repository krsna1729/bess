// Copyright (c) 2017, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// Copyright (c) 2017, Cloudigo.
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
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
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

#include "bessctl.h"

#include <thread>

#include <gflags/gflags.h>
#include <glog/logging.h>
#include <grpc++/server.h>
#include <grpc++/server_builder.h>
#include <grpc++/server_context.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "pb/service.grpc.pb.h"
#pragma GCC diagnostic pop

#include "control/control_plane.h"
#include "bessd.h"
#include "gate.h"
#include "gate_hooks/tcpdump.h"
#include "gate_hooks/track.h"
#include "message.h"
#include "metadata.h"
#include "module.h"
#include "module_graph.h"
#include "opts.h"
#include "packet_pool.h"
#include "port.h"
#include "resume_hook.h"
#include "scheduler.h"
#include "shared_obj.h"
#include "traffic_class.h"
#include "utils/ether.h"
#include "utils/time.h"
#include "worker.h"

#include <rte_mempool.h>
#include <rte_mempool.h>

using grpc::ServerContext;
using grpc::Status;

using bess::TrafficClassBuilder;
using namespace bess::pb;

template <typename T>
static inline Status return_with_error(T* response, int code, const char* fmt,
                                       ...) {
  va_list ap;
  va_start(ap, fmt);
  response->mutable_error()->set_code(code);
  response->mutable_error()->set_errmsg(bess::utils::FormatVarg(fmt, ap));
  va_end(ap);
  return Status::OK;
}

// Translates a control-plane error into the legacy protobuf error fields
// (grpc::Status is always OK; Error.code carries the failure).
template <typename T>
static inline Status return_with_control_error(
    T* response, const bess::control::ControlError& e) {
  response->mutable_error()->set_code(e.err);
  response->mutable_error()->set_errmsg(e.message);
  return Status::OK;
}

static inline bess::Gate* module_gate(const Module* m, bool is_igate,
                                      gate_idx_t gate_idx) {
  if (is_igate) {
    if (is_active_gate(m->igates(), gate_idx)) {
      return m->igates()[gate_idx];
    }
  } else {
    if (is_active_gate(m->ogates(), gate_idx)) {
      return m->ogates()[gate_idx];
    }
  }
  return nullptr;
}

static int collect_igates(Module* m, GetModuleInfoResponse* response) {
  for (const auto& g : m->igates()) {
    if (!g) {
      continue;
    }

    GetModuleInfoResponse_IGate* igate = response->add_igates();

    Track* t = reinterpret_cast<Track*>(g->FindHookByClass(Track::kName));

    if (t) {
      igate->set_cnt(t->cnt());
      igate->set_pkts(t->pkts());
      igate->set_bytes(t->bytes());
      igate->set_timestamp(get_epoch_time());
    }

    igate->set_igate(g->gate_idx());
    for (const auto& og : g->ogates_upstream()) {
      GetModuleInfoResponse_IGate_OGate* ogate = igate->add_ogates();
      ogate->set_ogate(og->gate_idx());
      ogate->set_name(og->module()->name());
    }

    for (const auto& hook : g->hooks()) {
      GetModuleInfoResponse_GateHook* hook_info = igate->add_gatehooks();
      hook_info->set_class_name(hook->class_name());
      hook_info->set_hook_name(hook->name());
    }
  }

  return 0;
}

static int collect_ogates(Module* m, GetModuleInfoResponse* response) {
  for (const auto& g : m->ogates()) {
    if (!g) {
      continue;
    }

    GetModuleInfoResponse_OGate* ogate = response->add_ogates();

    ogate->set_ogate(g->gate_idx());
    Track* t = reinterpret_cast<Track*>(g->FindHookByClass(Track::kName));
    if (t) {
      ogate->set_cnt(t->cnt());
      ogate->set_pkts(t->pkts());
      ogate->set_bytes(t->bytes());
      ogate->set_timestamp(get_epoch_time());
    }
    ogate->set_name(g->igate()->module()->name());
    ogate->set_igate(g->igate()->gate_idx());

    for (const auto& hook : g->hooks()) {
      GetModuleInfoResponse_GateHook* hook_info = ogate->add_gatehooks();
      hook_info->set_class_name(hook->class_name());
      hook_info->set_hook_name(hook->name());
    }
  }

  return 0;
}

static int collect_metadata(Module* m, GetModuleInfoResponse* response) {
  size_t i = 0;
  for (const auto& it : m->all_attrs()) {
    GetModuleInfoResponse_Attribute* attr = response->add_metadata();

    attr->set_name(it.name);
    attr->set_size(it.size);

    switch (it.mode) {
      case bess::metadata::Attribute::AccessMode::kRead:
        attr->set_mode("read");
        break;
      case bess::metadata::Attribute::AccessMode::kWrite:
        attr->set_mode("write");
        break;
      case bess::metadata::Attribute::AccessMode::kUpdate:
        attr->set_mode("update");
        break;
      default:
        DCHECK(0);
    }

    attr->set_offset(m->attr_offset(i));
    i++;
  }

  return 0;
}

static bess::control::TrafficClassSpec to_tc_spec(
    const bess::pb::TrafficClass& class_) {
  bess::control::TrafficClassSpec spec;
  spec.name = class_.name();
  spec.parent = class_.parent();
  spec.policy = class_.policy();
  spec.resource = class_.resource();
  spec.wid = class_.wid();
  spec.has_priority =
      class_.arg_case() == bess::pb::TrafficClass::kPriority;
  spec.priority = class_.priority();
  spec.has_share = class_.arg_case() == bess::pb::TrafficClass::kShare;
  spec.share = class_.share();
  spec.limit = {class_.limit().begin(), class_.limit().end()};
  spec.max_burst = {class_.max_burst().begin(), class_.max_burst().end()};
  spec.leaf_module_name = class_.leaf_module_name();
  spec.leaf_module_taskid = class_.leaf_module_taskid();
  return spec;
}

static void collect_tc(const bess::TrafficClass* c, int wid,
                       ListTcsResponse_TrafficClassStatus* status) {
  if (c->parent()) {
    status->set_parent(c->parent()->name());
  }

  status->mutable_class_()->set_name(c->name());
  status->mutable_class_()->set_blocked(c->blocked());

  if (c->policy() >= 0 && c->policy() < bess::NUM_POLICIES) {
    status->mutable_class_()->set_policy(bess::TrafficPolicyName[c->policy()]);
  } else {
    status->mutable_class_()->set_policy("invalid");
  }

  status->mutable_class_()->set_wid(wid);

  if (c->policy() == bess::POLICY_RATE_LIMIT) {
    const bess::RateLimitTrafficClass* rl =
        reinterpret_cast<const bess::RateLimitTrafficClass*>(c);
    std::string resource = bess::ResourceName.at(rl->resource());
    int64_t limit = rl->limit_arg();
    int64_t max_burst = rl->max_burst_arg();
    status->mutable_class_()->mutable_limit()->insert({resource, limit});
    status->mutable_class_()->mutable_max_burst()->insert(
        {resource, max_burst});
  } else if (c->policy() == bess::POLICY_LEAF) {
    const bess::LeafTrafficClass* leaf =
        static_cast<const bess::LeafTrafficClass*>(c);
    const Task* task = leaf->task();
    const Module* module = task->module();

    status->mutable_class_()->set_leaf_module_name(task->module()->name());

    auto it = std::find(module->tasks().begin(), module->tasks().end(), task);
    CHECK(it != module->tasks().end());
    uint64_t task_id = it - module->tasks().begin();
    status->mutable_class_()->set_leaf_module_taskid(task_id);
  }
}

class BESSControlImpl final : public BESSControl::Service {
 public:
  void set_shutdown_func(const std::function<void()>& func) {
    shutdown_func_ = func;
  }

  Status GetVersion(ServerContext*, const EmptyRequest*,
                    VersionResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    response->set_version(google::VersionString());
    return Status::OK;
  }

  Status ResetAll(ServerContext*, const EmptyRequest*,
                  EmptyResponse* response) override {
    if (auto ret = control_plane_.Reset(); !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status PauseAll(ServerContext*, const EmptyRequest*,
                  EmptyResponse*) override {
    (void)control_plane_.PauseAll();
    return Status::OK;
  }

  Status PauseWorker(ServerContext*, const PauseWorkerRequest* req,
                     EmptyResponse*) override {
    (void)control_plane_.PauseWorker(req->wid());
    return Status::OK;
  }

  Status ResumeAll(ServerContext*, const EmptyRequest*,
                   EmptyResponse*) override {
    (void)control_plane_.ResumeAll();
    return Status::OK;
  }

  Status ResumeWorker(ServerContext*, const ResumeWorkerRequest* req,
                      EmptyResponse*) override {
    (void)control_plane_.ResumeWorker(req->wid());
    return Status::OK;
  }

  Status ResetWorkers(ServerContext*, const EmptyRequest*,
                      EmptyResponse*) override {
    (void)control_plane_.ResetWorkers();
    return Status::OK;
  }

  Status ListWorkers(ServerContext*, const EmptyRequest*,
                     ListWorkersResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    for (int wid = 0; wid < Worker::kMaxWorkers; wid++) {
      if (!is_worker_active(wid))
        continue;
      ListWorkersResponse_WorkerStatus* status = response->add_workers_status();
      status->set_wid(wid);
      status->set_running(is_worker_running(wid));
      status->set_core(workers[wid]->core());
      status->set_num_tcs(workers[wid]->scheduler()->NumTcs());
      status->set_silent_drops(workers[wid]->silent_drops());
    }
    return Status::OK;
  }

  Status AddWorker(ServerContext*, const AddWorkerRequest* request,
                   EmptyResponse* response) override {
    if (auto ret = control_plane_.AddWorker(request->wid(), request->core(),
                                            request->scheduler());
        !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status DestroyWorker(ServerContext*, const DestroyWorkerRequest* request,
                       EmptyResponse* response) override {
    if (auto ret = control_plane_.DestroyWorker(request->wid()); !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status ResetTcs(ServerContext*, const EmptyRequest*,
                  EmptyResponse* response) override {
    if (auto ret = control_plane_.ResetTcs(); !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status ListTcs(ServerContext*, const ListTcsRequest* request,
                 ListTcsResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    int wid_filter = request->wid();
    if (wid_filter >= Worker::kMaxWorkers) {
      return return_with_error(response, EINVAL,
                               "'wid' must be between 0 and %d",
                               Worker::kMaxWorkers - 1);
    } else if (wid_filter < 0) {
      wid_filter = Worker::kAnyWorker;
    }

    for (const auto& tc_pair : TrafficClassBuilder::all_tcs()) {
      bess::TrafficClass* c = tc_pair.second.get();
      int wid = c->WorkerId();
      if (wid_filter == Worker::kAnyWorker || wid_filter == wid) {
        // WRR and Priority TCs associate share/priority to each child
        if (c->policy() == bess::POLICY_WEIGHTED_FAIR) {
          const auto* wrr_parent =
              static_cast<const bess::WeightedFairTrafficClass*>(c);
          for (const auto& child_data : wrr_parent->children()) {
            auto* status = response->add_classes_status();
            collect_tc(child_data.first, wid, status);
            status->mutable_class_()->set_share(child_data.second);
          }
        } else if (c->policy() == bess::POLICY_PRIORITY) {
          const auto* prio_parent =
              static_cast<const bess::PriorityTrafficClass*>(c);
          for (const auto& child_data : prio_parent->children()) {
            auto* status = response->add_classes_status();
            collect_tc(child_data.c_, wid, status);
            status->mutable_class_()->set_priority(child_data.priority_);
          }
        } else {
          for (const auto* child : c->Children()) {
            auto* status = response->add_classes_status();
            collect_tc(child, wid, status);
          }
        }

        if (!c->parent()) {
          auto* status = response->add_classes_status();
          collect_tc(c, wid, status);
        }
      }
    }

    return Status::OK;
  }

  Status CheckSchedulingConstraints(
      ServerContext*, const EmptyRequest*,
      CheckSchedulingConstraintsResponse* response) override {
    auto report = control_plane_.CheckSchedulingConstraints();
    if (!report) {
      return return_with_control_error(response, report.error());
    }

    for (const auto& v : report->violations) {
      auto violation = response->add_violations();
      violation->set_name(v.name);
      violation->set_constraint(v.constraint);
      violation->set_assigned_node(v.assigned_node);
      violation->set_assigned_core(v.assigned_core);
    }

    for (const auto& m : report->modules) {
      response->add_modules()->set_name(m.name);
    }

    response->set_fatal(report->fatal);
    return Status::OK;
  }

  Status AddTc(ServerContext*, const AddTcRequest* request,
               EmptyResponse* response) override {
    if (auto ret = control_plane_.AddTc(to_tc_spec(request->class_())); !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status UpdateTcParams(ServerContext*, const UpdateTcParamsRequest* request,
                        EmptyResponse* response) override {
    if (auto ret = control_plane_.UpdateTcParams(to_tc_spec(request->class_()));
        !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status UpdateTcParent(ServerContext*, const UpdateTcParentRequest* request,
                        EmptyResponse* response) override {
    if (auto ret = control_plane_.UpdateTcParent(to_tc_spec(request->class_()));
        !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status GetTcStats(ServerContext*, const GetTcStatsRequest* request,
                    GetTcStatsResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    const char* tc_name = request->name().c_str();

    bess::TrafficClass* c;

    if (request->name().length() == 0) {
      return return_with_error(response, EINVAL,
                               "Argument must be a name in str");
    }

    c = TrafficClassBuilder::Find(tc_name);
    if (!c) {
      return return_with_error(response, ENOENT, "No TC '%s' found", tc_name);
    }

    response->set_timestamp(get_epoch_time());
    response->set_count(c->stats().usage[bess::RESOURCE_COUNT]);
    response->set_cycles(c->stats().usage[bess::RESOURCE_CYCLE]);
    response->set_packets(c->stats().usage[bess::RESOURCE_PACKET]);
    response->set_bits(c->stats().usage[bess::RESOURCE_BIT]);

    return Status::OK;
  }

  Status ListDrivers(ServerContext*, const EmptyRequest*,
                     ListDriversResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    for (const auto& pair : PortBuilder::all_port_builders()) {
      const PortBuilder& builder = pair.second;
      response->add_driver_names(builder.class_name());
    }

    return Status::OK;
  }

  Status GetDriverInfo(ServerContext*, const GetDriverInfoRequest* request,
                       GetDriverInfoResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    if (request->driver_name().length() == 0) {
      return return_with_error(response, EINVAL,
                               "Argument must be a name in str");
    }

    const auto& it =
        PortBuilder::all_port_builders().find(request->driver_name());
    if (it == PortBuilder::all_port_builders().end()) {
      return return_with_error(response, ENOENT, "No driver '%s' found",
                               request->driver_name().c_str());
    }

#if 0
                        for (int i = 0; i < MAX_COMMANDS; i++) {
                          if (!drv->commands[i].cmd)
                            break;
                          response->add_commands(drv->commands[i].cmd);
                        }
#endif
    response->set_name(it->second.class_name());
    response->set_help(it->second.help_text());

    return Status::OK;
  }

  Status ResetPorts(ServerContext*, const EmptyRequest*,
                    EmptyResponse* response) override {
    if (auto ret = control_plane_.ResetPorts(); !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status ListPorts(ServerContext*, const EmptyRequest*,
                   ListPortsResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    for (const auto& pair : bess::control::runtime().ports().All()) {
      const ::Port* p = pair.second.get();
      bess::pb::ListPortsResponse::Port* port = response->add_ports();

      port->set_name(p->name());
      port->set_driver(p->port_builder()->class_name());
      port->set_mac_addr(p->conf().mac_addr.ToString());
      port->set_num_inc_q(p->num_rx_queues());
      port->set_num_out_q(p->num_tx_queues());
      port->set_size_inc_q(p->rx_queue_size());
      port->set_size_out_q(p->tx_queue_size());
      *port->mutable_driver_arg() = p->driver_arg();
    }

    return Status::OK;
  }

  Status CreatePort(ServerContext*, const CreatePortRequest* request,
                    CreatePortResponse* response) override {
    VLOG(1) << "CreatePortRequest from client:" << std::endl
            << request->DebugString();

    bess::control::PortSpec spec;
    spec.name = request->name();
    spec.driver = request->driver();
    spec.num_inc_q = request->num_inc_q();
    spec.num_out_q = request->num_out_q();
    spec.size_inc_q = request->size_inc_q();
    spec.size_out_q = request->size_out_q();
    spec.arg = request->arg();

    auto info = control_plane_.CreatePort(spec);
    if (!info) {
      return return_with_control_error(response, info.error());
    }

    response->set_name(info->name);
    response->set_mac_addr(info->mac_addr);

    return Status::OK;
  }

  Status SetPortConf(ServerContext*, const SetPortConfRequest* request,
                     CommandResponse* response) override {
    bess::control::PortConfSpec spec;
    spec.mac_addr = request->conf().mac_addr();
    spec.mtu = request->conf().mtu();
    spec.admin_up = request->conf().admin_up();

    auto ret = control_plane_.SetPortConf(request->name(), spec);
    if (!ret) {
      return return_with_control_error(response, ret.error());
    }

    *response = *ret;
    return Status::OK;
  }

  Status GetPortConf(ServerContext*, const GetPortConfRequest* request,
                     GetPortConfResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    if (!request->name().length()) {
      return return_with_error(response, EINVAL, "Port name is not given");
    }

    const char* port_name = request->name().c_str();
    const ::Port* port = bess::control::runtime().ports().Find(port_name);
    if (!port) {
      return return_with_error(response, ENOENT, "No port `%s' found",
                               port_name);
    }

    Port::Conf conf = port->conf();
    bess::pb::PortConf* pb_conf = response->mutable_conf();

    pb_conf->set_mac_addr(conf.mac_addr.ToString());
    pb_conf->set_mtu(conf.mtu);
    pb_conf->set_admin_up(conf.admin_up);

    return Status::OK;
  }

  Status DestroyPort(ServerContext*, const DestroyPortRequest* request,
                     EmptyResponse* response) override {
    if (auto ret = control_plane_.DestroyPort(request->name()); !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status GetPortStats(ServerContext*, const GetPortStatsRequest* request,
                      GetPortStatsResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    ::Port* port = bess::control::runtime().ports().Find(request->name());
    if (!port) {
      return return_with_error(response, ENOENT, "No port '%s' found",
                               request->name().c_str());
    }

    ::Port::PortStats stats = port->GetPortStats();

    response->mutable_inc()->set_packets(stats.inc.packets);
    response->mutable_inc()->set_dropped(stats.inc.dropped);
    response->mutable_inc()->set_bytes(stats.inc.bytes);
    *response->mutable_inc()->mutable_requested_hist() = {
        stats.inc.requested_hist.begin(), stats.inc.requested_hist.end()};
    *response->mutable_inc()->mutable_actual_hist() = {
        stats.inc.actual_hist.begin(), stats.inc.actual_hist.end()};
    *response->mutable_inc()->mutable_diff_hist() = {
        stats.inc.diff_hist.begin(), stats.inc.diff_hist.end()};

    response->mutable_out()->set_packets(stats.out.packets);
    response->mutable_out()->set_dropped(stats.out.dropped);
    response->mutable_out()->set_bytes(stats.out.bytes);
    *response->mutable_out()->mutable_requested_hist() = {
        stats.out.requested_hist.begin(), stats.out.requested_hist.end()};
    *response->mutable_out()->mutable_actual_hist() = {
        stats.out.actual_hist.begin(), stats.out.actual_hist.end()};
    *response->mutable_out()->mutable_diff_hist() = {
        stats.out.diff_hist.begin(), stats.out.diff_hist.end()};

    response->set_timestamp(get_epoch_time());

    return Status::OK;
  }

  Status GetLinkStatus(ServerContext*, const GetLinkStatusRequest* request,
                       GetLinkStatusResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    ::Port* port = bess::control::runtime().ports().Find(request->name());
    if (!port) {
      return return_with_error(response, ENOENT, "No port '%s' found",
                               request->name().c_str());
    }

    ::Port::LinkStatus status = port->GetLinkStatus();

    response->set_speed(status.speed);
    response->set_full_duplex(status.full_duplex);
    response->set_autoneg(status.autoneg);
    response->set_link_up(status.link_up);

    return Status::OK;
  }

  Status ResetModules(ServerContext*, const EmptyRequest*,
                      EmptyResponse* response) override {
    if (auto ret = control_plane_.ResetModules(); !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status ListModules(ServerContext*, const EmptyRequest*,
                     ListModulesResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    for (const auto& pair : ModuleGraph::GetAllModules()) {
      const Module* m = pair.second.get();
      ListModulesResponse_Module* module = response->add_modules();

      module->set_name(m->name());
      module->set_mclass(m->module_builder()->class_name());
      module->set_desc(m->GetDesc());
    }
    return Status::OK;
  }

  Status CreateModule(ServerContext*, const CreateModuleRequest* request,
                      CreateModuleResponse* response) override {
    VLOG(1) << "CreateModuleRequest from client:" << std::endl
            << request->DebugString();

    bess::control::ModuleSpec spec;
    spec.name = request->name();
    spec.mclass = request->mclass();
    spec.arg = request->arg();

    auto name = control_plane_.CreateModule(spec);
    if (!name) {
      return return_with_control_error(response, name.error());
    }

    response->set_name(*name);
    return Status::OK;
  }

  Status DestroyModule(ServerContext*, const DestroyModuleRequest* request,
                       EmptyResponse* response) override {
    if (auto ret = control_plane_.DestroyModule(request->name()); !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status GetModuleInfo(ServerContext*, const GetModuleInfoRequest* request,
                       GetModuleInfoResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    const char* m_name;
    Module* m;

    if (!request->name().length())
      return return_with_error(response, EINVAL,
                               "Argument must be a name in str");
    m_name = request->name().c_str();

    m = bess::control::runtime().modules().Find(request->name());
    if (!m) {
      return return_with_error(response, ENOENT, "No module '%s' found",
                               m_name);
    }

    response->set_name(m->name());
    response->set_mclass(m->module_builder()->class_name());
    response->set_desc(m->GetDesc());

    collect_igates(m, response);
    collect_ogates(m, response);
    collect_metadata(m, response);
    response->set_deadends(m->deadends());

    return Status::OK;
  }

  Status ConnectModules(ServerContext*, const ConnectModulesRequest* request,
                        EmptyResponse* response) override {
    VLOG(1) << "ConnectModulesRequest from client:" << std::endl
            << request->DebugString();

    bess::control::ConnectionSpec spec;
    spec.m1 = request->m1();
    spec.m2 = request->m2();
    spec.ogate = request->ogate();
    spec.igate = request->igate();
    spec.skip_default_hooks = request->skip_default_hooks();

    if (auto ret = control_plane_.ConnectModules(spec); !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status DisconnectModules(ServerContext*,
                           const DisconnectModulesRequest* request,
                           EmptyResponse* response) override {
    bess::control::DisconnectionSpec spec;
    spec.name = request->name();
    spec.ogate = request->ogate();

    if (auto ret = control_plane_.DisconnectModules(spec); !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status DumpMempool(ServerContext*, const DumpMempoolRequest* request,
                     DumpMempoolResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    int socket_filter = request->socket();
    socket_filter =
        (socket_filter == -1) ? (RTE_MAX_NUMA_NODES - 1) : socket_filter;
    int socket = (request->socket() == -1) ? 0 : socket_filter;
    for (; socket <= socket_filter; socket++) {
      bess::PacketPool* pool = bess::PacketPool::GetDefaultPool(socket);
      if (!pool) {
        continue;
      }

      rte_mempool* mempool = pool->pool();
      MempoolDump* dump = response->add_dumps();
      dump->set_socket(socket);
      dump->set_initialized(mempool != nullptr);
      if (mempool == nullptr) {
        continue;
      }
      // Backend-independent: only struct rte_mempool's own fields and the
      // public count APIs. The old code reached into `pool_data` as a
      // `struct rte_ring*`, which is only valid for the ring_mp_mc backend
      // PacketPool happens to select today -- the same "correct by
      // construction" coupling Phase A/B already removed twice. The
      // deprecated ring_* fields stay unset (zero).
      dump->set_mp_size(mempool->size);
      dump->set_mp_cache_size(mempool->cache_size);
      dump->set_mp_element_size(mempool->elt_size);
      dump->set_mp_populated_size(mempool->populated_size);
      dump->set_mp_available_count(rte_mempool_avail_count(mempool));
      dump->set_mp_in_use_count(rte_mempool_in_use_count(mempool));
    }
    return Status::OK;
  }

  Status ListGateHookClass(ServerContext*, const EmptyRequest*,
                           ListGateHookClassResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    for (const auto& pair : bess::GateHookBuilder::all_gate_hook_builders()) {
      const auto& builder = pair.second;
      response->add_names(builder.class_name());
    }
    return Status::OK;
  }

  Status GetGateHookClassInfo(ServerContext*,
                              const GetGateHookClassInfoRequest* request,
                              GetGateHookClassInfoResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    VLOG(1) << "GetGateHookClassInfo from client:" << std::endl
            << request->DebugString();
    if (!request->name().length()) {
      return return_with_error(response, EINVAL,
                               "Argument must be a name in str");
    }

    const std::string& cls_name = request->name();
    const auto& it =
        bess::GateHookBuilder::all_gate_hook_builders().find(cls_name);
    if (it == bess::GateHookBuilder::all_gate_hook_builders().end()) {
      return return_with_error(response, ENOENT, "No gatehook class '%s' found",
                               cls_name.c_str());
    }
    const bess::GateHookBuilder* cls = &it->second;

    response->set_name(cls->class_name());
    response->set_help(cls->help_text());
    for (const auto& cmd : cls->cmds()) {
      auto* out = response->add_cmds();
      out->set_name(cmd.cmd);
      out->set_arg_type(cmd.arg_type);
      out->set_thread_safe(cmd.mt_safe == GateHookCommand::THREAD_SAFE);
    }
    return Status::OK;
  }

  Status ListGateHooks(ServerContext*, const EmptyRequest*,
                       ListGateHooksResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    for (const auto& pair : ModuleGraph::GetAllModules()) {
      const Module* m = pair.second.get();
      for (auto& gate : m->igates()) {
        if (!gate) {
          continue;
        }
        for (auto& hook : gate->hooks()) {
          GateHookInfo* info = response->add_hooks();
          info->set_class_name(hook->class_name());
          info->set_hook_name(hook->name());
          info->set_module_name(m->name());
          info->set_igate(gate->gate_idx());
          *(info->mutable_arg()) = hook->arg();
        }
      }
      for (auto& gate : m->ogates()) {
        if (!gate) {
          continue;
        }
        for (auto& hook : gate->hooks()) {
          GateHookInfo* info = response->add_hooks();
          info->set_class_name(hook->class_name());
          info->set_hook_name(hook->name());
          info->set_module_name(m->name());
          info->set_ogate(gate->gate_idx());
          *(info->mutable_arg()) = hook->arg();
        }
      }
    }
    return Status::OK;
  }

  Status ConfigureGateHook(ServerContext*,
                           const ConfigureGateHookRequest* request,
                           ConfigureGateHookResponse* response) override {
    bess::control::GateHookSpec spec;
    spec.class_name = request->hook().class_name();
    spec.hook_name = request->hook().hook_name();
    spec.module_name = request->hook().module_name();
    spec.is_igate =
        request->hook().gate_case() == bess::pb::GateHookInfo::kIgate;
    spec.arg = request->hook().arg();

    if (spec.is_igate) {
      spec.gate_idx = request->hook().igate();
      spec.use_gate = request->hook().igate() >= 0;
    } else {
      spec.gate_idx = request->hook().ogate();
      spec.use_gate = request->hook().ogate() >= 0;
    }

    auto name = control_plane_.ConfigureGateHook(spec, request->enable());
    if (!name) {
      return return_with_control_error(response, name.error());
    }
    if (!name->empty()) {
      response->set_name(*name);
    }
    return Status::OK;
  }

  Status GateHookCommand(ServerContext*, const GateHookCommandRequest* request,
                         CommandResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    // No need to look up the hook builder: the gate either
    // has a hook instance with the right name, or doesn't.
    const bess::pb::GateHookInfo& rh = request->hook();
    Module* m = bess::control::runtime().modules().Find(rh.module_name());
    if (!m) {
      return return_with_error(response, ENOENT, "No module '%s' found",
                               rh.module_name().c_str());
    }
    bool is_igate = rh.gate_case() == bess::pb::GateHookInfo::kIgate;
    gate_idx_t gate_idx = is_igate ? rh.igate() : rh.ogate();
    bess::Gate* g = module_gate(m, is_igate, gate_idx);
    if (g == nullptr) {
      return return_with_error(
          response, EINVAL, "%s: %cgate '%hu' does not exist",
          m->name().c_str(), is_igate ? 'i' : 'o', gate_idx);
    }

    bess::GateHook* hook = g->FindHook(rh.hook_name());
    if (hook == nullptr) {
      return return_with_error(response, ENOENT,
                               "%s: %cgate '%hu' has no hook named '%s'",
                               m->name().c_str(), is_igate ? 'i' : 'o',
                               gate_idx, rh.hook_name().c_str());
    }

    // DPDK functions may be called, so be prepared
    current_worker.SetNonWorker();

    *response = hook->RunCommand(request->cmd(), rh.arg());
    return Status::OK;
  }

  Status ConfigureResumeHook(ServerContext*,
                             const ConfigureResumeHookRequest* request,
                             CommandResponse* response) override {
    auto ret = control_plane_.ConfigureResumeHook(
        request->hook_name(), request->enable(), request->arg());
    if (!ret) {
      return return_with_control_error(response, ret.error());
    }

    *response = *ret;
    return Status::OK;
  }

  Status KillBess(ServerContext*, const EmptyRequest*,
                  EmptyResponse*) override {
    auto lock = control_plane_.AcquireLock();

    WorkerPauser wp;
    LOG(WARNING) << "Halt requested by a client\n";

    CHECK(shutdown_func_ != nullptr);
    std::thread shutdown_helper([this]() {
      // Deadlock occurs when closing a gRPC server while processing a RPC.
      // Instead, we defer calling gRPC::Server::Shutdown() to a temporary
      // thread.
      shutdown_func_();
    });
    shutdown_helper.detach();

    return Status::OK;
  }

  Status ImportPlugin(ServerContext*, const ImportPluginRequest* request,
                      EmptyResponse* response) override {
    if (auto ret = control_plane_.ImportPlugin(request->path()); !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status UnloadPlugin(ServerContext*, const UnloadPluginRequest* request,
                      EmptyResponse* response) override {
    if (auto ret = control_plane_.UnloadPlugin(request->path()); !ret) {
      return return_with_control_error(response, ret.error());
    }
    return Status::OK;
  }

  Status ListPlugins(ServerContext*, const EmptyRequest*,
                     ListPluginsResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    auto list = bess::bessd::ListPlugins();
    for (auto& path : list) {
      response->add_paths(path);
    }
    return Status::OK;
  }

  Status ListMclass(ServerContext*, const EmptyRequest*,
                    ListMclassResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    for (const auto& pair : ModuleBuilder::all_module_builders()) {
      const ModuleBuilder& builder = pair.second;
      response->add_names(builder.class_name());
    }
    return Status::OK;
  }

  Status GetMclassInfo(ServerContext*, const GetMclassInfoRequest* request,
                       GetMclassInfoResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    VLOG(1) << "GetMclassInfo from client:" << std::endl
            << request->DebugString();
    if (!request->name().length()) {
      return return_with_error(response, EINVAL,
                               "Argument must be a name in str");
    }

    const std::string& cls_name = request->name();
    const auto& it = ModuleBuilder::all_module_builders().find(cls_name);
    if (it == ModuleBuilder::all_module_builders().end()) {
      return return_with_error(response, ENOENT, "No module class '%s' found",
                               cls_name.c_str());
    }
    const ModuleBuilder* cls = &it->second;

    response->set_name(cls->class_name());
    response->set_help(cls->help_text());
    for (const auto& cmd : cls->cmds()) {
      auto* out = response->add_cmds();
      out->set_name(cmd.cmd);
      out->set_arg_type(cmd.arg_type);
      out->set_thread_safe(cmd.mt_safe == Command::THREAD_SAFE);
    }
    return Status::OK;
  }

  Status ModuleCommand(ServerContext*, const CommandRequest* request,
                       CommandResponse* response) override {
    auto lock = control_plane_.AcquireLock();

    if (!request->name().length()) {
      return return_with_error(response, EINVAL,
                               "Missing module name field 'name'");
    }
    const auto& it = ModuleGraph::GetAllModules().find(request->name());
    if (it == ModuleGraph::GetAllModules().end()) {
      return return_with_error(response, ENOENT, "No module '%s' found",
                               request->name().c_str());
    }

    // DPDK functions may be called, so be prepared
    current_worker.SetNonWorker();

    Module* m = bess::control::runtime().modules().Find(request->name());
    *response = m->RunCommand(request->cmd(), request->arg());
    return Status::OK;
  }

 private:
  // BESS control-plane semantics live here; this class is a protocol
  // adapter. The control plane owns the (non-recursive) lock.
  bess::control::ControlPlane control_plane_;

  std::function<void()> shutdown_func_;

};

void ApiServer::Listen(const std::string& addr) {
  if (!builder_) {
    builder_ = new grpc::ServerBuilder();
  }

  LOG(INFO) << "Server listening on " << addr;

  builder_->AddListeningPort(addr, grpc::InsecureServerCredentials());
}

void ApiServer::Run() {
  if (!builder_) {
    // We are not listening on any sockets. There is nothing to do.
    return;
  }

  BESSControlImpl service;
  builder_->RegisterService(&service);
  builder_->SetSyncServerOption(grpc::ServerBuilder::MAX_POLLERS, 1);

  std::unique_ptr<grpc::Server> server = builder_->BuildAndStart();
  if (server == nullptr) {
    LOG(ERROR) << "ServerBuilder::BuildAndStart() failed";
    return;
  }

  service.set_shutdown_func([&server]() { server->Shutdown(); });
  server->Wait();
}
