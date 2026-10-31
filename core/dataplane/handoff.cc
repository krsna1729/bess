// SPDX-License-Identifier: BSD-3-Clause

#include "dataplane/handoff.h"

#include <rte_mbuf.h>
#include <rte_memory.h>

#include <new>

#include "utils/dpdk_memory.h"

// DPDK 25.11's rte_ring_elem.h has no `extern "C"` of its own (rte_ring.h
// includes it before opening its block), so the declaration seen from C++
// names a C++-mangled function that the library does not define. The same C
// function under another name:
extern "C" ssize_t BessHandoffRingMemsizeElem(unsigned esize, unsigned count)
    __asm__("rte_ring_get_memsize_elem");

namespace bess::dataplane {
namespace detail {

ssize_t RingMemsize(unsigned esize, unsigned count) noexcept {
  return BessHandoffRingMemsizeElem(esize, count);
}

void FreePacket(PacketHandle packet) noexcept { rte_pktmbuf_free(packet); }

}  // namespace detail

namespace {

void *DpdkAllocateBlock(size_t bytes, size_t align, int socket,
                        int *placed) noexcept {
  void *block = nullptr;
  try {
    block = bess::utils::DpdkAllocate(bytes, align, socket);
  } catch (const std::bad_alloc &) {
    return nullptr;
  }
  // "bess-normal" (D-206; every block under --no-huge, BP-3) is labelled
  // with a heap id, not a node: its pages follow bessd's memory node, as
  // they are first touched under its MPOL_PREFERRED policy (D-123).
  const rte_memseg_list *msl = rte_mem_virt2memseg_list(block);
  if (msl != nullptr && msl->external) {
    *placed = bess::utils::MemoryNode();
    return block;
  }
  const rte_memseg *segment = rte_mem_virt2memseg(block, msl);
  *placed = segment != nullptr ? segment->socket_id : -1;
  return block;
}

void DpdkDeallocateBlock(void *block) noexcept { bess::utils::DpdkFree(block); }

}  // namespace

const HandoffAllocator &DpdkHandoffAllocator() noexcept {
  static const HandoffAllocator allocator{DpdkAllocateBlock,
                                          DpdkDeallocateBlock};
  return allocator;
}

}  // namespace bess::dataplane
