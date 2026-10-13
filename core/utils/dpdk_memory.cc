// SPDX-License-Identifier: BSD-3-Clause

#include "utils/dpdk_memory.h"

#include <mutex>

#include <rte_eal.h>
#include <rte_malloc.h>

#include "dpdk.h"
#include "runtime/numa_placement.h"
#include "utils/logging.h"

namespace bess::utils {

int MemoryNode() {
  if (!IsDpdkInitialized()) {
    InitDpdk();
  }
  // --no-huge: DPDK's one normal-page heap is labelled socket 0 whatever node
  // its pages are on (they follow the process mempolicy, the memory node).
  return rte_eal_has_hugepages() ? bess::runtime::MemoryNode() : 0;
}

void *DpdkAllocate(size_t bytes, size_t align, int socket) {
  if (!IsDpdkInitialized()) {
    InitDpdk();
  }
  if (bytes == 0) {
    bytes = 1;
  }
  void *p = rte_malloc_socket("bess", bytes, static_cast<unsigned>(align),
                              socket);
  if (p == nullptr && socket != SOCKET_ID_ANY) {
    // The socket asked for is out of memory; any socket beats failing, but
    // the memory then crosses the interconnect on every access.
    static std::once_flag warned;
    std::call_once(warned, [socket] {
      LOG(WARNING) << "DPDK socket " << socket
                   << " is out of memory; allocating on any socket "
                      "(cross-NUMA memory; give that node more hugepages)";
    });
    p = rte_malloc_socket("bess", bytes, static_cast<unsigned>(align),
                          SOCKET_ID_ANY);
  }
  if (p == nullptr) {
    throw std::bad_alloc();
  }
  return p;
}

void DpdkFree(void *p) noexcept { rte_free(p); }

}  // namespace bess::utils
