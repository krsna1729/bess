#include "packet_pool.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sys/mman.h>
#include <unistd.h>

#include <rte_errno.h>
#include <rte_eal.h>
#include <rte_mempool.h>
#include <rte_memzone.h>
#include <rte_vfio.h>

#include "dpdk.h"
#include "framework/memory_scope.h"
#include "utils/copy.h"
#include "utils/dpdk_memory.h"
#include "worker.h"

namespace bess {
namespace {

constexpr size_t kMB = size_t{1} << 20;

struct PoolPrivate {
  rte_pktmbuf_pool_private dpdk_priv;
};

void DoMunmap(rte_mempool_memhdr *memhdr, void *) {
  if (munmap(memhdr->addr, memhdr->len) < 0) {
    PLOG(WARNING) << "munmap()";
  }
}

void FreeMemzone(rte_mempool_memhdr *, void *mz) {
  rte_memzone_free(static_cast<const rte_memzone *>(mz));
}

// A normal-page part mapped into the VFIO container (D-206).
void UnmapDma(rte_mempool_memhdr *memhdr, void *opaque) {
  const auto va = reinterpret_cast<uint64_t>(memhdr->addr);
  rte_vfio_container_dma_unmap(RTE_VFIO_DEFAULT_CONTAINER_FD, va, va,
                               memhdr->len);
  DoMunmap(memhdr, opaque);
}

}  // namespace

static_assert(offsetof(rte_mbuf, data_off) == offsetof(rte_mbuf, rearm_data));
static_assert(offsetof(rte_mbuf, refcnt) == offsetof(rte_mbuf, rearm_data) + 2);
static_assert(offsetof(rte_mbuf, nb_segs) == offsetof(rte_mbuf, rearm_data) + 4);
static_assert(offsetof(rte_mbuf, port) == offsetof(rte_mbuf, rearm_data) + 6);
static_assert(offsetof(rte_mbuf, ol_flags) == offsetof(rte_mbuf, rearm_data) + 8);
static_assert(offsetof(rte_mbuf, rx_descriptor_fields1) ==
              offsetof(rte_mbuf, rearm_data) + 16);
static_assert(offsetof(rte_mbuf, packet_type) ==
              offsetof(rte_mbuf, rx_descriptor_fields1));
static_assert(offsetof(rte_mbuf, pkt_len) ==
              offsetof(rte_mbuf, rx_descriptor_fields1) + 4);
static_assert(offsetof(rte_mbuf, data_len) ==
              offsetof(rte_mbuf, rx_descriptor_fields1) + 8);
static_assert(offsetof(rte_mbuf, vlan_tci) ==
              offsetof(rte_mbuf, rx_descriptor_fields1) + 10);
static_assert(offsetof(rte_mbuf, tx_offload) >=
              offsetof(rte_mbuf, rx_descriptor_fields1) + 16);
static_assert(offsetof(rte_mbuf, vlan_tci_outer) >=
              offsetof(rte_mbuf, rx_descriptor_fields1) + 16);

PacketPool *PacketPool::default_pool_;

void PacketPool::CreateDefaultPools(size_t capacity, size_t data_room_size) {
  current_worker.SetNonWorker();
  InitDpdk();

  // A debug aid: with normal pages (no hugepages) it is one line per 4 KB
  // segment, tens of MB of log per daemon start.
  if (VLOG_IS_ON(1)) {
    rte_dump_physmem_layout(stdout);
  }

  // One pool, on the memory node: everything bessd allocates lives there
  // (D-123), so a pool per node would only be memory nobody uses.
  const int sid = bess::utils::MemoryNode();
  // The pool follows the memory backing (--mem_backing, D-198), which the
  // EAL was brought up with: DPDK's heap with hugepages, normal pages of its
  // own (mmap) without.
  try {
    if (!rte_eal_has_hugepages()) {
      LOG(WARNING) << "Hugepage is disabled! Creating PlainPacketPool for "
                   << capacity << " packets on node " << sid;
      default_pool_ = new PlainPacketPool(capacity, sid, data_room_size);
    } else {
      LOG(INFO) << "Creating DpdkPacketPool for " << capacity
                << " packets on node " << sid;
      default_pool_ = new DpdkPacketPool(capacity, sid, data_room_size);
    }
  } catch (const utils::MemoryRefused &e) {
    // Admitted like a table (D-198): a pool past the memory limit or the
    // hugepage cap is refused before its pages are touched, and bessd stops
    // with the reason rather than being OOM-killed later.
    LOG(FATAL) << "Packet pool of " << capacity << " packets refused: "
               << e.what() << ". Lower --buffers (BESSD_BUFFERS) or "
               << "--packet_data_room, or raise the memory limit or cap.";
  }
  CHECK(default_pool_) << "Packet pool allocation on node " << sid
                       << " failed!";
}

PacketPool::PacketPool(size_t capacity, int socket_id, size_t data_room_size)
    : data_room_size_(data_room_size) {
  if (!IsDpdkInitialized()) {
    current_worker.SetNonWorker();
    InitDpdk();
  }

  CHECK_GT(data_room_size_, 0u);
  CHECK_LE(data_room_size_, kMaxPacketDataSize);

  static int next_id_;
  name_ = "PacketPool" + std::to_string(next_id_++);

  LOG(INFO) << name_ << " requests for " << capacity << " packets and "
            << data_room_size_ << " payload bytes";

  pool_ = rte_mempool_create_empty(
      name_.c_str(), capacity, PacketMempoolElementSize(data_room_size_),
      capacity > 1024 ? kMaxCacheSize : 0, sizeof(PoolPrivate), socket_id, 0);
  if (!pool_) {
    LOG(FATAL) << "rte_mempool_create() failed: " << rte_strerror(rte_errno)
               << " (rte_errno=" << rte_errno << ")";
  }

  int ret = rte_mempool_set_ops_byname(pool_, "ring_mp_mc", nullptr);
  if (ret < 0) {
    LOG(FATAL) << "rte_mempool_set_ops_byname() returned " << ret;
  }
}

PacketPool::~PacketPool() {
  // munmap is triggered by the registered callback DoMunmap().
  rte_mempool_free(pool_);
  if (admitted_bytes_ != 0) {
    framework::MemoryAccounts::Process().Release("packet_pool",
                                                 admitted_bytes_);
  }
}

void PacketPool::AdmitPoolMemory(size_t bytes) {
  // The packet memory, admitted like a table's before it is committed
  // (D-198): against the memory limit with normal backing, against the
  // hugepage cap with hugepages; reported as bessd/packet_pool. Throws
  // utils::MemoryRefused, admitting nothing, when it does not fit.
  framework::MemoryAccounts::Process().Admit("packet_pool", bytes);
  admitted_bytes_ += bytes;
}

bool PacketPool::AllocBulk(PacketHandle *pkts, size_t count, size_t len) {
  if (count == 0) {
    return true;
  }
  if (count > std::numeric_limits<unsigned>::max()) {
    return false;
  }

  const uint16_t data_room = rte_pktmbuf_data_room_size(pool_);
  if (data_room < RTE_PKTMBUF_HEADROOM ||
      len > static_cast<size_t>(data_room) - RTE_PKTMBUF_HEADROOM) {
    return false;
  }

  const uint64_t initial_ol_flags =
      (rte_pktmbuf_priv_flags(pool_) &
       RTE_PKTMBUF_POOL_F_PINNED_EXT_BUF)
          ? RTE_MBUF_F_EXTERNAL
          : 0;
  const uint16_t initial_data_off = static_cast<uint16_t>(
      std::min<unsigned>(static_cast<unsigned>(RTE_PKTMBUF_HEADROOM),
                         static_cast<unsigned>(data_room)));
  const uint32_t packet_len = static_cast<uint32_t>(len);
  const uint16_t data_len = static_cast<uint16_t>(len);

  // rte_mbuf_raw_alloc_bulk() establishes the pool/buffer fields and the
  // simple ownership invariants. Initialize the remaining fresh-packet state
  // in one traversal, including BESS's requested initial length.
  if (rte_mbuf_raw_alloc_bulk(pool_, pkts, static_cast<unsigned>(count)) < 0) {
    return false;
  }

  // Decision D-034 (docs/decisions.md): two 16-byte metadata stores per
  // packet. tx_offload and vlan_tci_outer are not cleared here (D-097): they
  // matter only with transmit-offload flags set, and the one place that sets
  // those (CommitMetadataAndSeeds, packet_tx_checksum.cc) writes tx_offload
  // whole; nothing sets RTE_MBUF_F_TX_QINQ. A received mbuf never came
  // through here anyway. The layout
  // assertions above protect these stores against DPDK mbuf ABI changes; the
  // patterns are little-endian words. Each 16-byte memcpy compiles to one
  // vector store where the target has them (x86 SSE2, arm64).
  static_assert(std::endian::native == std::endian::little);
  // 1st store (16 B at rearm_data): [data_off|refcnt|nb_segs|port] [ol_flags]
  const uint64_t rearm[2] = {
      static_cast<uint64_t>(initial_data_off) |
          (UINT64_C(1) << 16) |  // refcnt = 1
          (UINT64_C(1) << 32) |  // nb_segs = 1
          (static_cast<uint64_t>(RTE_MBUF_PORT_INVALID) << 48),
      initial_ol_flags};
  // 2nd store (16 B at rx_descriptor_fields1):
  // [packet_type|pkt_len] [data_len|vlan_tci=0|rss=0]
  const uint32_t rxdesc[4] = {0, packet_len, data_len, 0};
  static_assert(sizeof(rearm) == 16 && sizeof(rxdesc) == 16);

  for (size_t i = 0; i < count; i++) {
    // Addressed from the mbuf, not through the members: each store spans
    // several fields.
    char *mbuf = reinterpret_cast<char *>(pkts[i]);
    std::memcpy(mbuf + offsetof(rte_mbuf, rearm_data), rearm, sizeof(rearm));
    std::memcpy(mbuf + offsetof(rte_mbuf, rx_descriptor_fields1), rxdesc,
                sizeof(rxdesc));
  }
  return true;
}

PacketHandle PacketPool::AllocCopy(const void *data, size_t len) {
  if (data == nullptr && len != 0) {
    return nullptr;
  }
  if (len > std::numeric_limits<uint32_t>::max()) {
    return nullptr;
  }

  PacketHandle head = Alloc();
  if (head == nullptr) {
    return nullptr;
  }

  const auto *src = static_cast<const uint8_t *>(data);
  PacketHandle tail = head;
  uint16_t nb_segs = 1;
  size_t remaining = len;
  while (remaining > 0) {
    PacketRef tail_ref(tail);
    const size_t copy_len =
        std::min(remaining, static_cast<size_t>(tail_ref.tailroom()));
    if (copy_len == 0) {
      if (nb_segs == std::numeric_limits<uint16_t>::max()) {
        PacketFree(head);
        return nullptr;
      }

      PacketHandle next = Alloc();
      if (next == nullptr) {
        PacketFree(head);
        return nullptr;
      }
      tail_ref.set_next(PacketRef(next));
      tail = next;
      nb_segs++;
      continue;
    }

    utils::Copy(tail_ref.append(static_cast<uint16_t>(copy_len)), src,
                copy_len);
    src += copy_len;
    remaining -= copy_len;
  }

  head->nb_segs = nb_segs;
  head->pkt_len = static_cast<uint32_t>(len);
  return head;
}

PacketHandle PacketPool::AllocExternal(
    void *buf_addr, rte_iova_t buf_iova, uint16_t buf_len,
    rte_mbuf_ext_shared_info *shinfo, size_t data_len, uint16_t data_off) {
  if (buf_addr == nullptr || shinfo == nullptr || shinfo->free_cb == nullptr ||
      rte_mbuf_ext_refcnt_read(shinfo) == 0 || data_off > buf_len ||
      data_len > static_cast<size_t>(buf_len) - data_off) {
    return nullptr;
  }

  PacketHandle pkt = rte_pktmbuf_alloc(pool_);
  if (pkt == nullptr) {
    return nullptr;
  }

  rte_pktmbuf_attach_extbuf(pkt, buf_addr, buf_iova, buf_len, shinfo);
  pkt->data_off = data_off;
  pkt->data_len = static_cast<uint16_t>(data_len);
  pkt->pkt_len = static_cast<uint32_t>(data_len);
  return pkt;
}

void PacketPool::PostPopulate() {
  PoolPrivate priv = {
      .dpdk_priv = {
          .mbuf_data_room_size = static_cast<uint16_t>(mbuf_data_room_size()),
          .mbuf_priv_size = kPacketPrivateSize,
          .flags = 0}};

  rte_pktmbuf_pool_init(pool_, &priv.dpdk_priv);
  rte_mempool_obj_iter(pool_, rte_pktmbuf_init, nullptr);

  LOG(INFO) << name_ << " has been created with " << Capacity() << " packets";
  // Say how far short of the request a pool fell, in the units an operator
  // can change (D-030): the packet count (--buffers, BESSD_BUFFERS) and the
  // memory behind it (hugepages, --mem_hugepage_mb / BESSD_MEM_HUGEPAGE_MB,
  // normal pages past them unless --mem_fallback=false, D-206).
  const size_t element = pool_->elt_size + pool_->header_size +
                         pool_->trailer_size;
  const size_t wanted = pool_->size;
  if (Capacity() < wanted) {
    const auto mb = [&](size_t n) { return std::to_string(n * element >> 20); };
    const std::string message =
        name_ + " holds " + std::to_string(Capacity()) + " of the " +
        std::to_string(wanted) + " packets asked for (" + mb(Capacity()) +
        " of " + mb(wanted) +
        " MB): not enough DPDK memory on its socket, and normal pages past "
        "the hugepages are forbidden (--mem_fallback=false) or cannot be "
        "given to devices (physical-address IOVA mode). Lower --buffers "
        "(BESSD_BUFFERS), raise the hugepage cap (--mem_hugepage_mb, "
        "BESSD_MEM_HUGEPAGE_MB) or give bessd more hugepages; also check "
        "'ulimit -l'.";
    if (Capacity() == 0) {
      LOG(FATAL) << message;
    }
    LOG(WARNING) << message;
  }
}

PlainPacketPool::PlainPacketPool(size_t capacity, int socket_id,
                                 size_t data_room_size)
    : PacketPool(capacity, socket_id, data_room_size) {
  pool_->flags |= MEMPOOL_F_NO_IOVA_CONTIG;

  size_t page_shift = __builtin_ctzl(getpagesize());
  size_t min_chunk_size, align;
  size_t size = rte_mempool_op_calc_mem_size_default(
      pool_, pool_->size, page_shift, &min_chunk_size, &align);

  AdmitPoolMemory(size);
  void *addr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (addr == MAP_FAILED) {
    PLOG(FATAL) << "mmap()";
  }

  // No error check, as we do not provide a guarantee that memory is pinned.
  int ret = mlock(addr, size);
  pinned_ = (ret == 0);  // may fail as non-root users have mlock limit

  ret = rte_mempool_populate_iova(pool_, static_cast<char *>(addr),
                                  RTE_BAD_IOVA, size, DoMunmap, nullptr);
  if (ret < static_cast<ssize_t>(pool_->size)) {
    LOG(WARNING) << "rte_mempool_populate_iova() returned " << ret
                 << " (rte_errno=" << rte_errno << ", "
                 << rte_strerror(rte_errno) << ")";
  }

  PostPopulate();
}

DpdkPacketPool::DpdkPacketPool(size_t capacity, int socket_id,
                               size_t data_room_size)
    : PacketPool(capacity, socket_id, data_room_size) {
  // What rte_mempool_populate_default() would take from DPDK's heap, as one
  // contiguous piece (page_shift 0): the bytes admitted, under the cap or
  // past it on normal pages (D-198, D-206).
  size_t min_chunk_size, align;
  const ssize_t admitted = rte_mempool_op_calc_mem_size_default(
      pool_, pool_->size, 0, &min_chunk_size, &align);
  AdmitPoolMemory(static_cast<size_t>(admitted));
  PopulateHugepages();
  PopulateNormalPages();
  PostPopulate();
}

void DpdkPacketPool::PopulateHugepages() {
  // Hugepages first (D-206): the memory node's heap gives what it can, in
  // memzones that halve on ENOMEM as rte_mempool_populate_default's do --
  // but running out keeps what was populated (that function frees it all)
  // and leaves the rest to normal pages. Skipped when the admission already
  // placed the pool past the hugepage cap.
  if (utils::ArmedForNormalPages()) {
    return;
  }
  size_t page_size = 0;
  if (rte_mempool_get_page_size(pool_, &page_size) < 0) {
    return;
  }
  const size_t page_shift = page_size == 0 ? 0 : std::countr_zero(page_size);
  size_t max_piece = std::numeric_limits<size_t>::max();
  for (unsigned id = 0; pool_->populated_size < pool_->size; id++) {
    size_t min_chunk_size, align;
    const ssize_t bytes = rte_mempool_op_calc_mem_size_default(
        pool_, pool_->size - pool_->populated_size, page_shift,
        &min_chunk_size, &align);
    if (bytes < 0) {
      return;
    }
    unsigned flags = RTE_MEMZONE_1GB | RTE_MEMZONE_SIZE_HINT_ONLY;
    if (min_chunk_size == static_cast<size_t>(bytes)) {
      flags |= RTE_MEMZONE_IOVA_CONTIG;
    }
    const std::string zone = name_ + "_" + std::to_string(id);
    // Pieces stop halving at 2 MiB (or the whole rest, if smaller): the heap
    // keeps a little room for what DPDK allocates beside the pool (the
    // chunk headers populate adds), rather than being drained to the byte.
    const size_t smallest = std::max(
        min_chunk_size, std::min(static_cast<size_t>(bytes), 2 * kMB));
    const rte_memzone *mz = nullptr;
    do {
      mz = rte_memzone_reserve_aligned(
          zone.c_str(), std::min(static_cast<size_t>(bytes), max_piece),
          pool_->socket_id, flags, static_cast<unsigned>(align));
      if (mz != nullptr || rte_errno != ENOMEM) {
        break;
      }
      max_piece = std::min(max_piece, static_cast<size_t>(bytes)) / 2;
    } while (max_piece >= smallest);
    if (mz == nullptr) {
      return;
    }
    const bool contiguous =
        page_size == 0 || (flags & RTE_MEMZONE_IOVA_CONTIG) != 0;
    const int ret =
        contiguous
            ? rte_mempool_populate_iova(pool_, static_cast<char *>(mz->addr),
                                        mz->iova, mz->len, FreeMemzone,
                                        const_cast<rte_memzone *>(mz))
            : rte_mempool_populate_virt(pool_, static_cast<char *>(mz->addr),
                                        mz->len, page_size, FreeMemzone,
                                        const_cast<rte_memzone *>(mz));
    if (ret < 0 && !contiguous) {
      // populate_virt freed every chunk it registered, this one included
      // if any of it was.
      LOG(ERROR) << name_ << ": rte_mempool_populate_virt() returned " << ret;
      return;
    }
    if (ret <= 0) {
      rte_memzone_free(mz);
      return;
    }
    utils::NotePlacement(mz->len, pool_->socket_id);
  }
}

void DpdkPacketPool::PopulateNormalPages() {
  // What hugepages could not hold goes on normal pages (D-206), in one
  // anonymous mapping admitted against the memory limit before it is
  // touched, like PlainPacketPool's. A device reaches it by virtual address
  // only -- IOVA = VA, mapped into the VFIO container when there is one --
  // so with physical-address IOVA there is no address to give a NIC, and
  // the pool stays smaller, as when normal pages are forbidden.
  const size_t missing = pool_->size - pool_->populated_size;
  if (missing == 0 || !utils::memory_policy().fallback ||
      rte_eal_iova_mode() != RTE_IOVA_VA) {
    return;
  }
  // populate places no object across a boundary of the pool's page size
  // (rte_mempool_get_page_size: the hugepage size under hugepages), so the
  // part is sized for that page and the mapping starts on one.
  size_t page_size = 0;
  if (rte_mempool_get_page_size(pool_, &page_size) < 0) {
    return;
  }
  const size_t page_shift = page_size == 0 ? 0 : std::countr_zero(page_size);
  size_t min_chunk_size, align;
  const ssize_t needed = rte_mempool_op_calc_mem_size_default(
      pool_, missing, page_shift, &min_chunk_size, &align);
  if (needed < 0) {
    return;
  }
  const size_t page = static_cast<size_t>(getpagesize());
  const size_t bytes = (static_cast<size_t>(needed) + page - 1) / page * page;
  utils::AdmitNormalMemory(bytes);
  const size_t start = std::max(page_size, page);
  // Map a start page more than the part and trim it to a start-aligned
  // `bytes` (untouched, so the extra never becomes resident).
  void *raw = mmap(nullptr, bytes + start, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
  if (raw == MAP_FAILED) {
    PLOG(WARNING) << name_ << ": mmap() of the normal-page part";
    return;
  }
  auto *addr = reinterpret_cast<char *>(
      (reinterpret_cast<uintptr_t>(raw) + start - 1) / start * start);
  auto *raw_end = static_cast<char *>(raw) + bytes + start;
  if (addr != raw) {
    munmap(raw, static_cast<size_t>(addr - static_cast<char *>(raw)));
  }
  if (addr + bytes != raw_end) {
    munmap(addr + bytes, static_cast<size_t>(raw_end - (addr + bytes)));
  }
  const auto va = reinterpret_cast<uint64_t>(addr);
  const bool dma_mapped = rte_vfio_container_dma_map(
                              RTE_VFIO_DEFAULT_CONTAINER_FD, va, va, bytes) == 0;
  if (rte_mempool_populate_iova(pool_, addr, va, bytes,
                                dma_mapped ? UnmapDma : DoMunmap,
                                nullptr) <= 0) {
    if (dma_mapped) {
      rte_vfio_container_dma_unmap(RTE_VFIO_DEFAULT_CONTAINER_FD, va, va,
                                   bytes);
    }
    munmap(addr, bytes);
    return;
  }
  utils::NotePlacement(bytes, utils::TableBacking::kNormal);
  LOG(WARNING) << name_ << ": " << missing << " of " << pool_->size
               << " packets are on normal pages (hugepages exhausted, D-206)";
}

}  // namespace bess
