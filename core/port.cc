// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "port.h"

#include <glog/logging.h>

#include <cassert>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <initializer_list>
#include <memory>
#include <sstream>
#include <string>

#include "message.h"

Port *PortBuilder::CreatePort(const std::string &name) const {
  Port *p = port_generator_();
  p->set_name(name);
  p->set_port_builder(this);
  return p;
}

bool PortBuilder::InitPortClass() {
  if (initialized_) {
    return false;
  }

  std::unique_ptr<Port> p(port_generator_());
  p->InitDriver();
  initialized_ = true;
  return true;
}

void PortBuilder::InitDrivers() {
  for (auto &pair : all_port_builders()) {
    if (!const_cast<PortBuilder &>(pair.second).InitPortClass()) {
      LOG(WARNING) << "Initializing driver (port class) "
                   << pair.second.class_name() << " failed.";
    }
  }
}

bool PortBuilder::RegisterPortClass(
    std::function<Port *()> port_generator, const std::string &class_name,
    const std::string &name_template, const std::string &help_text,
    std::function<CommandResponse(Port *, const google::protobuf::Any &)>
        init_func) {
  all_port_builders_holder().emplace(
      std::piecewise_construct, std::forward_as_tuple(class_name),
      std::forward_as_tuple(port_generator, class_name, name_template,
                            help_text, init_func));
  return true;
}

const std::map<std::string, PortBuilder> &PortBuilder::all_port_builders() {
  return all_port_builders_holder();
}

std::map<std::string, PortBuilder> &PortBuilder::all_port_builders_holder(
    bool reset) {
  // Maps from class names to port builders.  Tracks all port classes (via their
  // PortBuilders).
  static std::map<std::string, PortBuilder> all_port_builders;

  if (reset) {
    all_port_builders.clear();
  }

  return all_port_builders;
}

void Port::CollectStats(bool) {}

CommandResponse Port::InitWithGenericArg(const google::protobuf::Any &arg) {
  CommandResponse ret = port_builder_->RunInit(this, arg);
  if (!ret.has_error()) {
    driver_arg_ = arg;
  }
  return ret;
}

Port::PortStats Port::GetPortStats() {
  CollectStats(false);

  PortStats ret = port_stats_;

  for (queue_t qid = 0; qid < num_queues[PACKET_DIR_INC]; qid++) {
    const QueueStats &inc = queue_stats[PACKET_DIR_INC][qid];

    ret.inc.packets += inc.packets;
    ret.inc.dropped += inc.dropped;
    ret.inc.tx_prepare_errors += inc.tx_prepare_errors;
    ret.inc.bytes += inc.bytes;
    ret.inc.requested_hist += inc.requested_hist;
    ret.inc.actual_hist += inc.actual_hist;
    ret.inc.diff_hist += inc.diff_hist;
  }

  for (queue_t qid = 0; qid < num_queues[PACKET_DIR_OUT]; qid++) {
    const QueueStats &out = queue_stats[PACKET_DIR_OUT][qid];
    ret.out.packets += out.packets;
    ret.out.dropped += out.dropped;
    ret.out.tx_prepare_errors += out.tx_prepare_errors;
    ret.out.bytes += out.bytes;
    ret.out.requested_hist += out.requested_hist;
    ret.out.actual_hist += out.actual_hist;
    ret.out.diff_hist += out.diff_hist;
  }

  return ret;
}

int Port::AcquireQueues(const struct module *m, packet_dir_t dir,
                        const queue_t *queues, int num) {
  queue_t qid;
  int i;

  if (dir != PACKET_DIR_INC && dir != PACKET_DIR_OUT) {
    LOG(ERROR) << "Incorrect packet dir " << dir;
    return -EINVAL;
  }

  if (queues == nullptr) {
    for (qid = 0; qid < num_queues[dir]; qid++) {
      const struct module *user;

      user = users[dir][qid];

      /* the queue is already being used by someone else? */
      if (user && user != m) {
        return -EBUSY;
      }
    }

    for (qid = 0; qid < num_queues[dir]; qid++) {
      users[dir][qid] = m;
    }

    return 0;
  }

  for (i = 0; i < num; i++) {
    const struct module *user;

    qid = queues[i];

    if (qid >= num_queues[dir]) {
      return -EINVAL;
    }

    user = users[dir][qid];

    /* the queue is already being used by someone else? */
    if (user && user != m) {
      return -EBUSY;
    }
  }

  for (i = 0; i < num; i++) {
    qid = queues[i];
    users[dir][qid] = m;
  }

  return 0;
}

void Port::ReleaseQueues(const struct module *m, packet_dir_t dir,
                         const queue_t *queues, int num) {
  queue_t qid;
  int i;

  if (dir != PACKET_DIR_INC && dir != PACKET_DIR_OUT) {
    LOG(ERROR) << "Incorrect packet dir " << dir;
    return;
  }

  if (queues == nullptr) {
    for (qid = 0; qid < num_queues[dir]; qid++) {
      if (users[dir][qid] == m)
        users[dir][qid] = nullptr;
    }

    return;
  }

  for (i = 0; i < num; i++) {
    qid = queues[i];
    if (qid >= num_queues[dir])
      continue;

    if (users[dir][qid] == m)
      users[dir][qid] = nullptr;
  }
}
