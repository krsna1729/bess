#ifndef BESS_PACKET_POOL_H_
#define BESS_PACKET_POOL_H_

#include "memory.h"
#include "packet.h"

// "Contiguous" here means that all packets reside in a single memory region
// in the virtual/physical address space.
//                                       Contiguous?
//                   Backed memory    Virtual  Physical  mlock()ed  fail-free
// --------------------------------------------------------------------------
// PlainPacketPool   Plain 4k pages   O        X         X          O
//   : For standalone benchmarks and unittests. Cannot be used for DMA.
//
// BessPacketPool    BESS hugepages   O        O         O          X
//   : BESS default. It allocates and manages huge pages internally.
//     No hugetlbfs is required.
//
// DpdkPacketPool    DPDK hugepages   O/X      O/X       O          O
//   : It is used as a fallback option if allocation of BessPacketPool has
//     failed. The memory region will be contiguous in most cases,
//     but not so if 2MB hugepages are used and scattered. MLX4/5 drivers
//     will fail in this case.

namespace bess {

// PacketPool is a C++ wrapper for a native DPDK pktmbuf mempool. Alloc() and
// Free() are thread-safe; packet processing goes through PacketRef while the
// pool transports PacketHandle values.
class PacketPool {
 public:
  static PacketPool *GetDefaultPool(int node) { return default_pools_[node]; }

  static void CreateDefaultPools(
      size_t capacity = kDefaultCapacity,
      size_t data_room_size = kDefaultPacketDataSize);

  // data_room_size is payload capacity; DPDK headroom is added internally.
  PacketPool(size_t capacity = kDefaultCapacity, int socket_id = -1,
             size_t data_room_size = kDefaultPacketDataSize);
  virtual ~PacketPool();

  PacketPool(const PacketPool &) = delete;
  PacketPool &operator=(const PacketPool &) = delete;

  // Allocate a packet with the specified initial length. The length is checked
  // against the actual pool data room before the mbuf is returned.
  PacketHandle Alloc(size_t len = 0) {
    PacketHandle pkt = rte_pktmbuf_alloc(pool_);
    if (pkt == nullptr) {
      return nullptr;
    }

    if (len > rte_pktmbuf_tailroom(pkt)) {
      rte_pktmbuf_free(pkt);
      return nullptr;
    }

    pkt->pkt_len = static_cast<uint32_t>(len);
    pkt->data_len = static_cast<uint16_t>(len);
    return pkt;
  }

  // Allocate multiple packets. There is no partial success: all count mbufs
  // are allocated and initialized, or the function returns false.
  bool AllocBulk(PacketHandle *pkts, size_t count, size_t len = 0);

  // Copy a byte buffer into one or more packet segments. The returned packet
  // owns the complete chain; allocation failure frees every segment already
  // allocated and returns nullptr.
  PacketHandle AllocCopy(const void *data, size_t len);

  // Allocate an mbuf and attach one caller-managed external buffer. On
  // success, ownership of one external-buffer reference transfers to the
  // returned packet and its shinfo callback releases it after the last free.
  // On failure, the caller retains ownership.
  PacketHandle AllocExternal(
      void *buf_addr, rte_iova_t buf_iova, uint16_t buf_len,
      rte_mbuf_ext_shared_info *shinfo, size_t data_len = 0,
      uint16_t data_off = RTE_PKTMBUF_HEADROOM);

  size_t Capacity() const { return pool_->populated_size; }
  size_t Size() const { return rte_mempool_avail_count(pool_); }

  size_t data_room_size() const { return data_room_size_; }
  size_t mbuf_data_room_size() const {
    return RTE_PKTMBUF_HEADROOM + data_room_size_;
  }

  // Note: it would be ideal not to expose this.
  rte_mempool *pool() { return pool_; }
  const rte_mempool *pool() const { return pool_; }

  virtual bool IsVirtuallyContiguous() = 0;
  virtual bool IsPhysicallyContiguous() = 0;
  virtual bool IsPinned() = 0;

 protected:
  static const size_t kDefaultCapacity = (1 << 16) - 1;  // 64k - 1
  static const size_t kMaxCacheSize = 512;                // per-core cache size

  // Child classes are expected to call this function in their constructor.
  void PostPopulate();

  std::string name_;
  rte_mempool *pool_;
  const size_t data_room_size_;

 private:
  static PacketPool *default_pools_[RTE_MAX_NUMA_NODES];
};

class PlainPacketPool : public PacketPool {
 public:
  PlainPacketPool(size_t capacity = kDefaultCapacity, int socket_id = -1,
                  size_t data_room_size = kDefaultPacketDataSize);

  bool IsVirtuallyContiguous() override { return true; }
  bool IsPhysicallyContiguous() override { return false; }
  bool IsPinned() override { return pinned_; }

 private:
  bool pinned_;
};

class BessPacketPool : public PacketPool {
 public:
  BessPacketPool(size_t capacity = kDefaultCapacity, int socket_id = -1,
                 size_t data_room_size = kDefaultPacketDataSize);

  bool IsVirtuallyContiguous() override { return true; }
  bool IsPhysicallyContiguous() override { return true; }
  bool IsPinned() override { return true; }

 private:
  DmaMemoryPool mem_;
};

class DpdkPacketPool : public PacketPool {
 public:
  DpdkPacketPool(size_t capacity = kDefaultCapacity, int socket_id = -1,
                 size_t data_room_size = kDefaultPacketDataSize);

  // TODO(sangjin): it may or may not be contiguous. Check it.
  bool IsVirtuallyContiguous() override { return false; }
  bool IsPhysicallyContiguous() override { return false; }
  bool IsPinned() override { return true; }
};

}  // namespace bess

#endif  // BESS_PACKET_POOL_H_
