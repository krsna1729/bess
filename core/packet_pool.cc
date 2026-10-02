#include "packet_pool.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <limits>
#include <sys/mman.h>

#include <rte_errno.h>
#include <rte_eal.h>
#include <rte_mempool.h>

#if defined(__SSE2__)
#include <emmintrin.h>
#endif

#include "dpdk.h"
#include "runtime/memory.h"
#include "runtime/opts.h"
#include "utils/copy.h"
#include "worker.h"

namespace bess {
namespace {

struct PoolPrivate {
  rte_pktmbuf_pool_private dpdk_priv;
};

void DoMunmap(rte_mempool_memhdr *memhdr, void *) {
  if (munmap(memhdr->addr, memhdr->len) < 0) {
    PLOG(WARNING) << "munmap()";
  }
}

}  // namespace
class BessPacketPool final : public PacketPool {
 public:
  BessPacketPool(size_t capacity = kDefaultCapacity, int socket_id = -1,
                 size_t data_room_size = kDefaultPacketDataSize);

  bool IsVirtuallyContiguous() override { return true; }
  bool IsPhysicallyContiguous() override { return true; }
  bool IsPinned() override { return true; }

 private:
  DmaMemoryPool mem_;
};

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

PacketPool *PacketPool::default_pools_[RTE_MAX_NUMA_NODES];

void PacketPool::CreateDefaultPools(size_t capacity, size_t data_room_size) {
  current_worker.SetNonWorker();
  InitDpdk(FLAGS_dpdk ? FLAGS_m : 0);

  // A debug aid: with normal pages (no hugepages) it is one line per 4 KB
  // segment, tens of MB of log per daemon start.
  if (VLOG_IS_ON(1)) {
    rte_dump_physmem_layout(stdout);
  }

  for (int sid = 0; sid < NumNumaNodes(); sid++) {
    // What the EAL actually did: -m -1 resolves to normal pages when no
    // hugepages are usable.
    if (!rte_eal_has_hugepages()) {
      LOG(WARNING) << "Hugepage is disabled! Creating PlainPacketPool for "
                   << capacity << " packets on node " << sid;
      default_pools_[sid] =
          new PlainPacketPool(capacity, sid, data_room_size);
    } else if (FLAGS_dpdk) {
      LOG(INFO) << "Creating DpdkPacketPool for " << capacity
                << " packets on node " << sid;
      default_pools_[sid] =
          new DpdkPacketPool(capacity, sid, data_room_size);
    } else {
      LOG(INFO) << "Creating BessPacketPool for " << capacity
                << " packets on node " << sid;
      default_pools_[sid] =
          new BessPacketPool(capacity, sid, data_room_size);
    }
    CHECK(default_pools_[sid])
        << "Packet pool allocation on node " << sid << " failed!";
  }
}

PacketPool::PacketPool(size_t capacity, int socket_id, size_t data_room_size)
    : data_room_size_(data_room_size) {
  if (!IsDpdkInitialized()) {
    current_worker.SetNonWorker();
    InitDpdk(0);
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

  // Decision D-034 (docs/decisions.md): two 128-bit metadata stores per packet,
  // plus scalar clears for tx_offload and vlan_tci_outer. The layout
  // assertions above protect these stores against DPDK mbuf ABI changes.
#if defined(__SSE2__)
  // 1st store (16 B at &rearm_data): [data_off|refcnt|nb_segs|port] [ol_flags]
  const uint64_t low_rearm =
      static_cast<uint64_t>(initial_data_off) |
      (UINT64_C(1) << 16) |  // refcnt = 1
      (UINT64_C(1) << 32) |  // nb_segs = 1
      (static_cast<uint64_t>(RTE_MBUF_PORT_INVALID) << 48);
  const __m128i rearm = _mm_set_epi64x(
      static_cast<long long>(initial_ol_flags),
      static_cast<long long>(std::bit_cast<int64_t>(low_rearm)));

  // 2nd store (16 B at rx_descriptor_fields1): [packet_type|pkt_len] [data_len|vlan_tci|rss]
  const __m128i rxdesc = _mm_setr_epi32(
      0,                                        // packet_type
      static_cast<int32_t>(packet_len),         // pkt_len
      static_cast<int32_t>(data_len),           // data_len | vlan_tci=0
      0);                                       // rss (don't care)

  size_t i = 0;
  for (; i + 1 < count; i += 2) {
    _mm_storeu_si128(reinterpret_cast<__m128i *>(&pkts[i]->rearm_data), rearm);
    _mm_storeu_si128(reinterpret_cast<__m128i *>(pkts[i]->rx_descriptor_fields1),
                     rxdesc);
    pkts[i]->tx_offload = 0;
    pkts[i]->vlan_tci_outer = 0;
    _mm_storeu_si128(reinterpret_cast<__m128i *>(&pkts[i + 1]->rearm_data),
                     rearm);
    _mm_storeu_si128(
        reinterpret_cast<__m128i *>(pkts[i + 1]->rx_descriptor_fields1), rxdesc);
    pkts[i + 1]->tx_offload = 0;
    pkts[i + 1]->vlan_tci_outer = 0;
  }
  if (i < count) {
    _mm_storeu_si128(reinterpret_cast<__m128i *>(&pkts[i]->rearm_data), rearm);
    _mm_storeu_si128(reinterpret_cast<__m128i *>(pkts[i]->rx_descriptor_fields1),
                     rxdesc);
    pkts[i]->tx_offload = 0;
    pkts[i]->vlan_tci_outer = 0;
  }
#else  // scalar fallback for non-x86 architectures
  for (size_t i = 0; i < count; i++) {
    PacketHandle pkt = pkts[i];
    pkt->data_off       = initial_data_off;
    rte_mbuf_refcnt_set(pkt, 1);
    pkt->nb_segs        = 1;
    pkt->port           = RTE_MBUF_PORT_INVALID;
    pkt->ol_flags       = initial_ol_flags;
    pkt->packet_type    = 0;
    pkt->pkt_len        = packet_len;
    pkt->data_len       = data_len;
    pkt->tx_offload     = 0;
    pkt->vlan_tci       = 0;
    pkt->vlan_tci_outer = 0;
  }
#endif
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
  // can change (D-030): the packet count (-buffers, BESSD_BUFFERS) and the
  // memory behind it (hugepages, -m / BESSD_M).
  const size_t element = pool_->elt_size + pool_->header_size +
                         pool_->trailer_size;
  const size_t wanted = pool_->size;
  if (Capacity() < wanted) {
    const auto mb = [&](size_t n) { return std::to_string(n * element >> 20); };
    const std::string message =
        name_ + " holds " + std::to_string(Capacity()) + " of the " +
        std::to_string(wanted) + " packets asked for (" + mb(Capacity()) +
        " of " + mb(wanted) +
        " MB): not enough DPDK memory on its socket. Lower -buffers "
        "(BESSD_BUFFERS), raise the memory cap (-m, BESSD_M) or give bessd "
        "more hugepages; also check 'ulimit -l'.";
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

BessPacketPool::BessPacketPool(size_t capacity, int socket_id,
                               size_t data_room_size)
    : PacketPool(capacity, socket_id, data_room_size),
      mem_(static_cast<size_t>(FLAGS_m) * 1024 * 1024, socket_id) {
  size_t page_shift = __builtin_ctzl(getpagesize());

  while (pool_->populated_size < pool_->size) {
    size_t deficit = pool_->size - pool_->populated_size;
    size_t min_chunk_size, align;
    size_t bytes = rte_mempool_op_calc_mem_size_default(
        pool_, deficit, page_shift, &min_chunk_size, &align);

    auto [addr, alloced_bytes] = mem_.AllocUpto(bytes);
    if (addr == nullptr) {
      LOG(WARNING) << "Node " << socket_id << ": " << capacity
                   << " packets requested, but only " << pool_->populated_size
                   << " allocated in total";
      break;
    }

    int ret = rte_mempool_populate_iova(pool_, static_cast<char *>(addr),
                                        Virt2Phy(addr), alloced_bytes, nullptr,
                                        nullptr);
    if (ret < 0) {
      LOG(WARNING) << "Node " << socket_id
                   << ": rte_mempool_populate_iova() returned " << ret;
    } else {
      LOG(INFO) << "Node " << socket_id << ": " << ret
                << " packets added from " << alloced_bytes << " bytes";
    }
  }

  PostPopulate();
}

DpdkPacketPool::DpdkPacketPool(size_t capacity, int socket_id,
                               size_t data_room_size)
    : PacketPool(capacity, socket_id, data_room_size) {
  int ret = rte_mempool_populate_default(pool_);
  if (ret < static_cast<ssize_t>(pool_->size)) {
    LOG(WARNING) << "rte_mempool_populate_default() returned " << ret
                 << " (rte_errno=" << rte_errno << ", "
                 << rte_strerror(rte_errno) << ")";
  }

  PostPopulate();
}

}  // namespace bess
