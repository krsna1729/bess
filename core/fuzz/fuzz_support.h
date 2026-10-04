// SPDX-License-Identifier: BSD-3-Clause

// Shared pieces of the fuzz harnesses (core/fuzz/*_fuzz.cc): a byte reader
// that decodes structured input from the fuzzer's bytes, an oracle check that
// aborts (so libFuzzer and the replay main both report it), and a hand-built
// mbuf chain that needs no mempool and therefore no EAL.

#ifndef BESS_FUZZ_FUZZ_SUPPORT_H_
#define BESS_FUZZ_FUZZ_SUPPORT_H_

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <span>
#include <vector>

#include <rte_mbuf.h>

#include "packet.h"

// Oracle violation: print where and abort. Unlike assert() this is active in
// every build type.
#define FUZZ_CHECK(cond)                                              \
  do {                                                                \
    if (!(cond)) [[unlikely]] {                                       \
      std::fprintf(stderr, "%s:%d: fuzz oracle violated: %s\n",       \
                   __FILE__, __LINE__, #cond);                        \
      std::abort();                                                   \
    }                                                                 \
  } while (0)

namespace bess::fuzz {

// Consumes the input front to back. Reads past the end return zeros (and
// empty spans), so a harness never has to special-case short inputs.
class FuzzInput {
 public:
  FuzzInput(const uint8_t *data, size_t size) : data_(data), size_(size) {}

  size_t remaining() const { return size_ - pos_; }
  bool empty() const { return pos_ >= size_; }

  uint8_t U8() { return pos_ < size_ ? data_[pos_++] : 0; }
  bool Bool() { return (U8() & 1) != 0; }
  uint16_t U16() {
    uint16_t v = U8();
    return static_cast<uint16_t>(v | (U8() << 8));
  }
  uint32_t U32() {
    uint32_t v = U16();
    return v | (static_cast<uint32_t>(U16()) << 16);
  }
  uint64_t U64() {
    uint64_t v = U32();
    return v | (static_cast<uint64_t>(U32()) << 32);
  }
  // A value in [0, bound); bound must be > 0.
  uint32_t Below(uint32_t bound) { return U32() % bound; }

  // Up to n bytes (fewer at the end of the input).
  std::span<const uint8_t> Bytes(size_t n) {
    const size_t take = n < remaining() ? n : remaining();
    std::span<const uint8_t> out(data_ + pos_, take);
    pos_ += take;
    return out;
  }
  // A length-prefixed (u16) byte string.
  std::span<const uint8_t> Blob() { return Bytes(U16()); }
  std::span<const uint8_t> Rest() { return Bytes(remaining()); }

 private:
  const uint8_t *data_;
  size_t size_;
  size_t pos_ = 0;
};

// One segment as the harness wants it: data bytes plus room on both sides.
struct SegmentSpec {
  std::vector<uint8_t> data;
  uint16_t headroom = 0;
  uint16_t tailroom = 0;
};

// A packet built from heap blocks laid out like a mempool element (struct
// rte_mbuf, the BESS private area, then the data room), chained by hand.
// Nothing here touches a mempool, so the EAL is never needed; the chain must
// not be passed to anything that frees or allocates mbufs. Each block is its
// own allocation, so ASan flags any access outside a segment's buffer.
class MbufChain {
 public:
  // Segments whose headroom + data + tailroom exceed a uint16_t buf_len are
  // shrunk (tailroom first, then data). An empty spec list yields one empty
  // segment, as a real packet always has a head.
  explicit MbufChain(std::vector<SegmentSpec> specs) {
    if (specs.empty()) {
      specs.emplace_back();
    }
    for (SegmentSpec &spec : specs) {
      Clamp(spec);
      const size_t room = size_t{spec.headroom} + spec.data.size() + spec.tailroom;
      const size_t bytes = kHeader + room;
      Block block{static_cast<uint8_t *>(::operator new(
                      bytes, std::align_val_t{alignof(rte_mbuf)})),
                  bytes};
      std::memset(block.base, 0, kHeader);
      rte_mbuf *m = reinterpret_cast<rte_mbuf *>(block.base);
      m->buf_addr = block.base + kHeader;
      m->buf_len = static_cast<uint16_t>(room);
      m->data_off = spec.headroom;
      m->data_len = static_cast<uint16_t>(spec.data.size());
      m->priv_size = static_cast<uint16_t>(::bess::kPacketPrivateSize);
      m->nb_segs = 1;
      rte_mbuf_refcnt_set(m, 1);
      // Poison the rooms so reads of uninitialised space show up as
      // nondeterminism in the oracles rather than as zeros.
      std::memset(block.base + kHeader, 0xA5, room);
      if (!spec.data.empty()) {
        std::memcpy(block.base + kHeader + spec.headroom, spec.data.data(),
                    spec.data.size());
      }
      if (!blocks_.empty()) {
        Mbuf(blocks_.size() - 1)->next = m;
      }
      blocks_.push_back(block);
      total_ += spec.data.size();
    }
    rte_mbuf *head = head_mbuf();
    head->nb_segs = static_cast<uint16_t>(blocks_.size());
    head->pkt_len = static_cast<uint32_t>(total_);
  }

  ~MbufChain() {
    for (Block &b : blocks_) {
      ::operator delete(b.base, std::align_val_t{alignof(rte_mbuf)});
    }
  }
  MbufChain(const MbufChain &) = delete;
  MbufChain &operator=(const MbufChain &) = delete;

  rte_mbuf *head_mbuf() const { return Mbuf(0); }
  ::bess::PacketRef ref() const { return ::bess::PacketRef(head_mbuf()); }
  size_t segments() const { return blocks_.size(); }
  rte_mbuf *Mbuf(size_t i) const {
    return reinterpret_cast<rte_mbuf *>(blocks_[i].base);
  }

  // The logical packet bytes, walking the chain as it is now.
  std::vector<uint8_t> Flatten() const {
    std::vector<uint8_t> out;
    for (const rte_mbuf *m = head_mbuf(); m != nullptr; m = m->next) {
      const uint8_t *p = rte_pktmbuf_mtod(m, const uint8_t *);
      out.insert(out.end(), p, p + m->data_len);
    }
    return out;
  }

  // Splits `bytes` into segments at the given lengths (the last segment takes
  // the rest), each with the given headroom/tailroom.
  static std::vector<SegmentSpec> Split(std::span<const uint8_t> bytes,
                                        std::span<const uint16_t> lengths,
                                        uint16_t headroom = 0,
                                        uint16_t tailroom = 0) {
    std::vector<SegmentSpec> specs;
    size_t at = 0;
    for (uint16_t len : lengths) {
      const size_t take = len < bytes.size() - at ? len : bytes.size() - at;
      specs.push_back({{bytes.begin() + at, bytes.begin() + at + take},
                       headroom,
                       tailroom});
      at += take;
    }
    specs.push_back({{bytes.begin() + at, bytes.end()}, headroom, tailroom});
    return specs;
  }

 private:
  struct Block {
    uint8_t *base;
    size_t bytes;
  };

  static constexpr size_t kHeader = sizeof(rte_mbuf) + ::bess::kPacketPrivateSize;

  static void Clamp(SegmentSpec &spec) {
    constexpr size_t kMax = 0xFFFF;
    if (size_t{spec.headroom} + spec.data.size() + spec.tailroom <= kMax) {
      return;
    }
    const size_t fixed = size_t{spec.headroom} + spec.data.size();
    if (fixed <= kMax) {
      spec.tailroom = static_cast<uint16_t>(kMax - fixed);
      return;
    }
    spec.tailroom = 0;
    spec.data.resize(kMax - spec.headroom);
  }

  std::vector<Block> blocks_;
  size_t total_ = 0;
};

}  // namespace bess::fuzz

#endif  // BESS_FUZZ_FUZZ_SUPPORT_H_
