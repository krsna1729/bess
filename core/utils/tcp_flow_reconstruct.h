// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_TCP_FLOW_RECONSTRUCT_H_
#define BESS_UTILS_TCP_FLOW_RECONSTRUCT_H_

#include <algorithm>
#include <cstring>
#include <map>
#include <span>
#include <vector>

#include "common.h"
#include "endian.h"
#include "packet.h"
#include "packet_parse.h"
#include "tcp.h"

namespace bess {
namespace utils {

// A utility class that accumulates TCP packet data in the correct order.
class TcpFlowReconstruct {
 public:
  // The most flow bytes kept, counted from the SYN's sequence number: what an
  // HTTP request's line and headers need (the HTTP adapter's own header bound,
  // docs/limits.md). Data past it is outside the window and ignored; a
  // segment that straddles it keeps the part inside.
  static constexpr size_t kMaxBufLen = size_t{64} * 1024;

  // Constructs a TCP flow reconstruction object that can hold initial_buflen
  // bytes to start with.
  explicit TcpFlowReconstruct(size_t initial_buflen = 1024)
      : initialized_(false), init_seq_(0), buf_(initial_buflen) {}

  virtual ~TcpFlowReconstruct() {}

  // Returns the underlying buffer of reconstructed flow bytes.  Not guaranteed
  // to return the same pointer between calls to InsertPacket().
  const char *buf() const { return buf_.data(); }

  // Returns the size of the underlying buffer
  size_t buf_size() const { return buf_.size(); }

  // Returns the initial data sequence number extracted from the SYN.
  uint32_t init_seq() const { return init_seq_; }

  // Returns the length of contiguous data available in the buffer starting from
  // the beginning.  Updated every time InsertPacket() is called.
  size_t contiguous_len() const {
    const auto it = received_map_.begin();
    return (it == received_map_.end()) ? 0 : (it->second - it->first);
  }

  // Adds the data of the given packet based upon its TCP sequence number.
  // If the packet is a SYN then we use the SYN to set the initial sequence
  // number offset.
  //
  // Returns true upon success. Returns false if the packet is not an IPv4
  // TCP segment that the checked parse accepts (its headers in the first
  // segment, D-121; its lengths consistent), or if it is not a SYN but we
  // have not been given a SYN previously.
  bool InsertPacket(PacketRef p) {
    const std::span<const uint8_t> frame(p.head_data<const uint8_t *>(), p.head_len());
    bess::packet::ParsedFrame parsed;
    if (bess::packet::ParseFrame(frame, parsed, p.total_len()) != bess::packet::ParseStatus::kOk ||
        parsed.l3 != bess::packet::L3Kind::kIpv4 || parsed.l4 != bess::packet::L4Kind::kTcp) {
      return false;
    }
    return InsertParsed(p, parsed);
  }

  // InsertPacket for a packet the caller has parsed: `parsed` is ParseFrame's
  // kOk verdict on `p` (its first segment, its total length), an IPv4 TCP
  // segment.
  bool InsertParsed(PacketRef p, const bess::packet::ParsedFrame &parsed) {
    // The parse found the 20-byte TCP header in the first segment.
    const Tcp *tcp = p.head_data<const Tcp *>(parsed.l4_offset);

    uint32_t seq = tcp->seq_num.value();
    // Assumes we only get one SYN and the sequence number of it doesn't change
    // for any reason.  Also assumes we have no data in the SYN.
    if (parsed.tcp_flags & Tcp::Flag::kSyn) {
      init_seq_ = seq + 1;
      initialized_ = true;
      return true;
    }

    if (!initialized_) {
      VLOG(1) << "Non-SYN received but not yet initialized.";
      return false;
    }

    // Check if the sequence number is greater or equal than SYN + 1. Wraparound
    // is possible.
    if ((int32_t)(seq - init_seq_) < 0) {
      VLOG(1) << "Sequence number not match. Initial seq: " << init_seq_
              << ", Seq: " << seq;
      return false;
    }

    // Wraparound is possible.
    const size_t buf_offset = seq - init_seq_;

    // The parse checked the TCP header against the L4 length the IP header
    // states, except in a first fragment, whose header may run past it: that
    // one carries no payload here.
    const size_t header_len = size_t{tcp->offset} * 4;
    const size_t l4_len = parsed.l4_length;
    const size_t datalen = l4_len > header_len ? l4_len - header_len : 0;

    // pure-ACK packets, or data entirely outside the window?
    if (datalen == 0 || buf_offset >= kMaxBufLen) {
      return true;
    }

    // Both terms are below 2^32: no overflow in size_t.
    const size_t keep_end = std::min(buf_offset + datalen, kMaxBufLen);

    // If we will run out of space, make more room.
    if (keep_end > buf_.size()) {
      buf_.resize(std::min(keep_end * 2, kMaxBufLen));
    }

    // The payload's end is inside the packet (the parse checked the IP length
    // against it), but it may continue past the first segment.
    CopyOut(p, size_t{parsed.l4_offset} + header_len, buf_.data() + buf_offset,
            keep_end - buf_offset);

    uint32_t start = static_cast<uint32_t>(buf_offset);
    uint32_t end = static_cast<uint32_t>(keep_end);

    // Merge the new new data with existing segments
    // new segment                           |-------------|
    // existing segments with a hole   |---A---|   |--B--|-C-|
    //                                             ^
    //                                             lower_bound(start)
    auto it = received_map_.lower_bound(start);
    if (it != received_map_.begin()) {
      auto it_prev = it;
      it_prev--;
      if (it_prev->second >= start) {
        // The segment right before the lower_bound(start) may partially overlap
        // with the new segment (e.g., segment A in the above figure).
        // Include the segment for merging
        start = it_prev->first;
        it = it_prev;
      }
    }

    // Remove all ovlerapping segments
    while (it != received_map_.end() && it->first <= end) {
      end = std::max(end, it->second);
      it = received_map_.erase(it);
    }

    // Insert the merged segment
    received_map_.emplace(start, end);

    return true;
  }

 private:
  // Copies `len` bytes from offset `off` of `p` into `dst`, across its
  // segments. The range lies inside the packet's total length; a chain whose
  // segments end before it (a malformed one) leaves the rest of `dst` as it
  // was.
  static void CopyOut(PacketRef p, size_t off, char *dst, size_t len) {
    for (PacketRef seg = p; len > 0 && seg.handle() != nullptr; seg = seg.next()) {
      const size_t seg_len = seg.head_len();
      if (off >= seg_len) {
        off -= seg_len;
        continue;
      }
      const size_t n = std::min(len, seg_len - off);
      std::memcpy(dst, seg.head_data<const char *>(static_cast<uint16_t>(off)), n);
      dst += n;
      len -= n;
      off = 0;
    }
  }

  // Tracks whether the init_seq_ (and thus this object) has been initialized
  // with a SYN.
  bool initialized_;

  // The initial sequence number of data bytes in the TCP flow.
  uint32_t init_seq_;

  // A buffer (potentially with holes) of received data.
  std::vector<char> buf_;

  // Sorted list of received segments. Segments are merged as necessary.
  // Key: offset from init_seq_
  // T: end offset of the segment
  std::map<uint32_t, uint32_t> received_map_;

  DISALLOW_COPY_AND_ASSIGN(TcpFlowReconstruct);
};

}  // namespace utils
}  // namespace bess

#endif  // BESS_UTILS_TCP_FLOW_RECONSTRUCT_H_
