// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_OPTS_H_
#define BESS_RUNTIME_OPTS_H_

#include <gflags/gflags.h>

// TODO(barath): Rename these flags to something more intuitive.
DECLARE_bool(t);
DECLARE_string(i);
DECLARE_bool(f);
DECLARE_bool(k);
DECLARE_bool(d);
DECLARE_bool(a);
DECLARE_int32(c);
DECLARE_string(b);
DECLARE_int32(p);
DECLARE_string(grpc_url);
DECLARE_int32(m);
DECLARE_string(pci_allow);
DECLARE_bool(skip_root_check);
DECLARE_string(modules);
DECLARE_bool(core_dump);
DECLARE_bool(no_crashlog);
DECLARE_int32(buffers);
DECLARE_uint32(packet_data_room);
DECLARE_bool(dpdk);
DECLARE_string(iova);

#endif  // BESS_RUNTIME_OPTS_H_
