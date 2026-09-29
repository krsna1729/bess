// SPDX-License-Identifier: BSD-3-Clause

#include "utils/dpdk_memory.h"

#include <rte_malloc.h>

#include "dpdk.h"

namespace bess::utils {

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
    // The socket asked for is out of memory; any socket beats failing.
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
