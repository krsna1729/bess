// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_PKTBATCH_H_
#define BESS_PKTBATCH_H_

#include <cstddef>

#include "packet_handle.h"
#include "utils/copy.h"

namespace bess {

class PacketRef;
class PacketRange;

class PacketBatch {
 public:
  int cnt() const { return cnt_; }
  void set_cnt(int cnt) { cnt_ = cnt; }
  void incr_cnt(int n = 1) { cnt_ += n; }

  // The stored representation: for ownership/transport machinery (ports,
  // rings, allocators, batch copies). Packet-processing code wants packets()
  // or packet(i).
  PacketHandle *handles() { return pkts_; }
  const PacketHandle *handles() const { return pkts_; }

  // Non-owning view of one packet. Definition lives in packet.h, where
  // PacketRef is complete.
  PacketRef packet(size_t i);
  PacketRef packet(size_t i) const;

  // The live packets, in order, for loops that need no index:
  // `for (PacketRef pkt : batch->packets())`. Defined in packet.h.
  PacketRange packets() const;

  void clear() { cnt_ = 0; }

  // WARNING: this function has no bounds checks and so it's possible to
  // overrun the buffer by calling this. We are not adding bounds check because
  // we want maximum GOFAST.
  void add(PacketHandle pkt) { pkts_[cnt_++] = pkt; }

  // Same thing through the seam; defined in packet.h.
  void add(PacketRef pkt);
  void add(PacketBatch *batch) {
    bess::utils::CopyInlined(handles() + cnt_, batch->handles(),
                             batch->cnt() * sizeof(PacketHandle));
    cnt_ += batch->cnt();
  }

  bool empty() { return (cnt_ == 0); }

  bool full() { return (cnt_ == kMaxBurst); }

  void Copy(const PacketBatch *src) {
    cnt_ = src->cnt_;
    bess::utils::CopyInlined(handles(), src->handles(),
                             cnt_ * sizeof(PacketHandle));
  }

  // Calls `keep(handle, i)` on each packet in order (`i` its index on entry)
  // and keeps, in order, the packets it returns true for. `keep` may replace
  // the handle (with a copy, say), and disposes of a packet it returns false
  // for (drops it or emits it to a gate) before it returns. A single call
  // site lets large packet operations remain inline. An output pointer
  // avoids a separate compaction index and its per-packet conversion.
  template <typename Keep>
  void KeepIf(Keep &&keep) {
    PacketHandle *const first = pkts_;
    PacketHandle *const last = pkts_ + cnt_;
    PacketHandle *out = first;
    for (PacketHandle *in = first; in != last; ++in) {
      PacketHandle pkt = *in;
      if (keep(pkt, static_cast<int>(in - first))) [[likely]] {
        *out++ = pkt;
      }
    }
    cnt_ = static_cast<int>(out - first);
  }

  inline static const size_t kMaxBurst = 32;

 private:
  int cnt_;
  PacketHandle pkts_[kMaxBurst];
};

static_assert(std::is_standard_layout<PacketBatch>::value &&
                  std::is_trivial<PacketBatch>::value,
              "PacketBatch must be standard-layout and trivial");

}  // namespace bess

#endif  // BESS_PKTBATCH_H_
