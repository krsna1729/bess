// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DPDK_H_
#define BESS_DPDK_H_

namespace bess {

bool IsDpdkInitialized();

// Initialize DPDK, with the specified amount of hugepage memory.
// Safe to call multiple times.
void InitDpdk(int dpdk_mb_per_socket = 0);

// Writes DPDK's trace (--dpdk_trace) to its directory; nothing when tracing
// is off. At shutdown: bessd does not call rte_eal_cleanup(), which would.
void SaveDpdkTrace();

}  // namespace bess

#endif  // BESS_DPDK_H_
