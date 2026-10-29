// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DPDK_H_
#define BESS_DPDK_H_

namespace bess {

bool IsDpdkInitialized();

// Initializes DPDK under the memory policy (D-198): the one bessd resolved
// from its --mem_* flags, else -- tests, benchmarks, library users -- the
// same settings from BESSD_MEM_* (default: normal pages, a 512 MB heap).
// Safe to call multiple times.
void InitDpdk();

// Writes DPDK's trace (--dpdk_trace) to its directory; nothing when tracing
// is off. At shutdown, with workers paused and still registered with EAL (a
// thread's trace buffer is freed when it unregisters); bessd does not call
// rte_eal_cleanup(), which would save it.
void SaveDpdkTrace();

}  // namespace bess

#endif  // BESS_DPDK_H_
