#include "packet_pool.h"

#include <sys/mman.h>

#include <algorithm>
#include <limits>
#include <rte_errno.h>
#include <rte_mempool.h>

#include "dpdk.h"
#include "opts.h"
#include "utils/copy.h"

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

PacketPool *PacketPool::default_pools_[RTE_MAX_NUMA_NODES];

void PacketPool::CreateDefaultPools(size_t capacity, size_t data_room_size) {
  InitDpdk(FLAGS_dpdk ? FLAGS_m : 0);

  rte_dump_physmem_layout(stdout);

  for (int sid = 0; sid < NumNumaNodes(); sid++) {
    if (FLAGS_m == 0) {
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

  for (size_t i = 0; i < count; i++) {
    PacketHandle pkt = pkts[i];
    pkt->pkt_len = packet_len;
    pkt->tx_offload = 0;
    pkt->vlan_tci = 0;
    pkt->vlan_tci_outer = 0;
    pkt->port = RTE_MBUF_PORT_INVALID;
    pkt->ol_flags = initial_ol_flags;
    pkt->packet_type = 0;
    pkt->data_off = initial_data_off;
    pkt->data_len = data_len;
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
  if (Capacity() == 0) {
    LOG(FATAL) << name_ << " has no packets allocated\n"
               << "Troubleshooting:\n"
               << "  - Check 'ulimit -l'\n"
               << "  - Do you have enough memory on the machine?\n"
               << "  - Maybe memory is too fragmented. Try rebooting.\n";
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
