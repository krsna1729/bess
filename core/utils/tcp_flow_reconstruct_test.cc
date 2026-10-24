// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "tcp_flow_reconstruct.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <pcap/pcap.h>

#include "ether.h"
#include "ip.h"
#include "packet_pool.h"
#include "testing/packet_builder.h"

namespace bess {
namespace utils {
namespace {

// A parameterized test fixture for testing TCP flows that specifies a PCAP file
// from which to read packets.
class TcpFlowReconstructTest : public ::testing::TestWithParam<const char *> {
 protected:
  virtual void SetUp() {
    // TODO(barath): Due to a problem with some versions of gtest when using the
    //               TEST_P macro, for the moment we just use a single tracefile
    //               for now.
    std::string tracefile_prefix = "testdata/test-pktcaptures/tcpflow-http-3";

    char errbuf[PCAP_ERRBUF_SIZE];
    handle_ = pcap_open_offline((tracefile_prefix + ".pcap").c_str(), errbuf);
    ASSERT_TRUE(handle_ != nullptr);

    const u_char *pcap_pkt;
    struct pcap_pkthdr pcap_hdr;
    while ((pcap_pkt = pcap_next(handle_, &pcap_hdr)) != nullptr) {
      ASSERT_EQ(pcap_hdr.caplen, pcap_hdr.len)
          << "Didn't capture the full packet.";
      PacketHandle p = pool_.Alloc(pcap_hdr.caplen);
      ASSERT_NE(p, nullptr);
      bess::PacketRef pkt_ref(p);
      bess::utils::Copy(pkt_ref.head_data(), pcap_pkt, pcap_hdr.caplen);
      pkts_.push_back(p);
    }

    // Read in file with good reconstruction.
    std::ifstream f(tracefile_prefix + ".bytes", std::ios::binary);
    ASSERT_FALSE(f.bad());

    f >> std::noskipws;
    std::istream_iterator<char> start(f), end;
    std::copy(start, end, std::back_inserter(bytestream_));
  }

  virtual void TearDown() {
    for (PacketHandle p : pkts_) {
      PacketFree(p);
    }
    if (handle_) {
      pcap_close(handle_);
    }
  }

  PlainPacketPool pool_;

  // The packets of the pcap trace file.
  std::vector<PacketHandle> pkts_;

  // The correctly reconstructed raw TCP byte stream.
  std::vector<char> bytestream_;

  pcap_t *handle_;
};

// Tests that the constructor initializes the underlying buffers to the
// specified initial size.
TEST(TcpFlowReconstruct, Constructor) {
  TcpFlowReconstruct t1, t2(7);
  EXPECT_EQ(1024, t1.buf_size());
  EXPECT_EQ(7, t2.buf_size());
}

// Tests that reconstructed flows contain the right data when done in order.
TEST_F(TcpFlowReconstructTest, StandardReconstruction) {
  TcpFlowReconstruct t(1);

  for (PacketHandle p : pkts_) {
    ASSERT_TRUE(t.InsertPacket(PacketRef(p)));
  }

  ASSERT_EQ(bytestream_.size(), t.contiguous_len());
  EXPECT_EQ(0, memcmp(t.buf(), bytestream_.data(), bytestream_.size()));
}

// Tests that reordering packets doesn't affect the reconstruction.
TEST_F(TcpFlowReconstructTest, ReorderedReconstruction) {
  PacketHandle syn = pkts_[0];

  std::vector<PacketHandle> pkt_rotation;
  for (size_t i = 1; i < pkts_.size(); ++i) {
    int ack_size = sizeof(Ethernet) + sizeof(Ipv4) + sizeof(Tcp);
    // Skip pure ACK packets for the permutations
    if (PacketRef(pkts_[i]).head_len() > ack_size) {
      pkt_rotation.push_back(pkts_[i]);
    }
  }
  std::sort(pkt_rotation.begin(), pkt_rotation.end());

  do {
    TcpFlowReconstruct t;
    ASSERT_TRUE(t.InsertPacket(PacketRef(syn)));

    for (PacketHandle p : pkt_rotation) {
      ASSERT_TRUE(t.InsertPacket(PacketRef(p)));
    }

    ASSERT_EQ(bytestream_.size(), t.contiguous_len());
    EXPECT_EQ(0, memcmp(t.buf(), bytestream_.data(), bytestream_.size()));
  } while (std::next_permutation(pkt_rotation.begin(), pkt_rotation.end()));
}

// Tests that we reject packet insertion without the SYN.
TEST_F(TcpFlowReconstructTest, MissingSyn) {
  PacketHandle syn = pkts_[0];
  PacketHandle nonsyn = pkts_[1];

  TcpFlowReconstruct t(1);
  ASSERT_FALSE(t.InsertPacket(PacketRef(nonsyn)));
  ASSERT_TRUE(t.InsertPacket(PacketRef(syn)));
  ASSERT_TRUE(t.InsertPacket(PacketRef(nonsyn)));
}

using bess::testing::Bytes;
using bess::testing::PacketBuilder;

constexpr uint8_t kSynFlag = 0x02, kAckFlag = 0x10;
constexpr uint32_t kSynSeq = 1000;
constexpr size_t kCap = TcpFlowReconstruct::kMaxBufLen;
// Frame offsets of the fields the tests make lie: the IPv4 total length and
// the TCP data offset (an untagged frame, IPv4 without options).
constexpr size_t kIpLength = 16, kTcpOffset = 46, kHeaders = 54;

Bytes Segment(uint8_t flags, uint32_t seq, const std::string &payload = "") {
  PacketBuilder b;
  b.Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
      .IPv4("10.0.0.1", "10.0.0.2")
      .Add(PacketBuilder::TcpHdr{.sport = 1000, .dport = 80, .flags = flags, .seq = seq});
  if (!payload.empty()) {
    b.Payload(payload);
  }
  return b.Build();
}

void SetIpLength(Bytes &f, uint16_t len) {
  f[kIpLength] = static_cast<uint8_t>(len >> 8u);
  f[kIpLength + 1] = static_cast<uint8_t>(len);
}

// Packets built byte by byte, so a test controls what lies past a first
// segment's data_len.
class TcpFlowReconstructBoundsTest : public ::testing::Test {
 protected:
  void TearDown() override {
    for (PacketHandle p : pkts_) {
      PacketFree(p);
    }
  }

  // `frame` as one segment or, with `split`, a chain whose first segment ends
  // after `split` bytes. The first segment's buffer past `split` holds 0xAA
  // bytes, or the frame's own bytes when `stale_frame` (a buffer reused
  // without clearing): what a reader past data_len would see.
  PacketRef Packet(const Bytes &frame, size_t split = 0, bool stale_frame = false) {
    PacketHandle head = pool_.Alloc(frame.size());
    EXPECT_NE(head, nullptr);
    PacketRef ref(head);
    auto *data = ref.head_data<uint8_t *>();
    std::memset(data, 0xAA, frame.size());
    std::memcpy(data, frame.data(), split == 0 || stale_frame ? frame.size() : split);
    pkts_.push_back(head);
    if (split != 0) {
      ref.set_data_len(static_cast<uint16_t>(split));
      ref.set_total_len(static_cast<uint32_t>(split));
      PacketHandle tail = pool_.Alloc(frame.size() - split);
      EXPECT_NE(tail, nullptr);
      std::memcpy(PacketRef(tail).head_data<uint8_t *>(), frame.data() + split,
                  frame.size() - split);
      EXPECT_EQ(0, rte_pktmbuf_chain(head, tail));
    }
    return ref;
  }

  PlainPacketPool pool_;
  std::vector<PacketHandle> pkts_;
};

// A sequence number far past the SYN's lies outside the buffer's window: the
// segment is ignored, and one that straddles the window's end keeps the part
// inside it. Before, (offset + length) * 2 wrapped in 32 bits, the buffer
// shrank to 32 B and the copy landed 2 GiB past it.
TEST_F(TcpFlowReconstructBoundsTest, DataPastTheWindowIsIgnored) {
  TcpFlowReconstruct t;
  ASSERT_TRUE(t.InsertPacket(Packet(Segment(kSynFlag, kSynSeq))));
  EXPECT_TRUE(t.InsertPacket(
      Packet(Segment(kAckFlag, kSynSeq + 1 + 0x7ffffff0u, std::string(32, 'x')))));
  EXPECT_LE(t.buf_size(), kCap);
  EXPECT_EQ(0, t.contiguous_len());

  EXPECT_TRUE(t.InsertPacket(Packet(Segment(kAckFlag, kSynSeq + 1 + kCap - 4, "abcdefgh"))));
  EXPECT_EQ(kCap, t.buf_size());
  EXPECT_EQ(0, std::memcmp(t.buf() + kCap - 4, "abcd", 4));

  EXPECT_TRUE(t.InsertPacket(Packet(Segment(kAckFlag, kSynSeq + 1, "GET"))));
  ASSERT_EQ(3, t.contiguous_len());
  EXPECT_EQ(0, std::memcmp(t.buf(), "GET", 3));
}

// Headers longer than the IPv4 total length says, or a total length shorter
// than the IP header or past the frame: refused, nothing read. Before, the
// payload length wrapped to about 4 GiB (a 4 GiB resize and a copy that
// long), or the copy read past the frame.
TEST_F(TcpFlowReconstructBoundsTest, LyingLengthsAreRefused) {
  TcpFlowReconstruct t;
  ASSERT_TRUE(t.InsertPacket(Packet(Segment(kSynFlag, kSynSeq))));

  Bytes long_tcp = Segment(kAckFlag, kSynSeq + 1, std::string(40, 'x'));
  long_tcp[kTcpOffset] = 0xf0;  // 60 B of TCP header ...
  SetIpLength(long_tcp, 60);    // ... in 40 B of L4
  EXPECT_FALSE(t.InsertPacket(Packet(long_tcp)));

  Bytes short_ip = Segment(kAckFlag, kSynSeq + 1, "GET");
  SetIpLength(short_ip, 10);
  EXPECT_FALSE(t.InsertPacket(Packet(short_ip)));

  Bytes past_frame = Segment(kAckFlag, kSynSeq + 1, "GET");
  SetIpLength(past_frame, 1000);
  EXPECT_FALSE(t.InsertPacket(Packet(past_frame)));

  Bytes runt = Segment(kAckFlag, kSynSeq + 1);
  runt.resize(20);
  EXPECT_FALSE(t.InsertPacket(Packet(runt)));

  EXPECT_EQ(1024, t.buf_size());
  EXPECT_EQ(0, t.contiguous_len());
}

// The headers must lie in the first segment (D-121): one that ends inside
// them is refused, whatever its buffer holds past data_len. Before, the TCP
// header and payload were read from there.
TEST_F(TcpFlowReconstructBoundsTest, HeadersPastTheFirstSegmentAreRefused) {
  TcpFlowReconstruct t;
  EXPECT_FALSE(t.InsertPacket(Packet(Segment(kSynFlag, kSynSeq), 30, true)));
  ASSERT_TRUE(t.InsertPacket(Packet(Segment(kSynFlag, kSynSeq))));
  EXPECT_FALSE(
      t.InsertPacket(Packet(Segment(kAckFlag, kSynSeq + 1, "GET / HTTP/1.1\r\n"), 30, true)));
  EXPECT_EQ(0, t.contiguous_len());
}

// A payload that continues in a later segment is read across the chain.
// Before, all of it was read from the first segment, past its data_len.
TEST_F(TcpFlowReconstructBoundsTest, ChainedPayloadIsReadAcrossSegments) {
  TcpFlowReconstruct t;
  ASSERT_TRUE(t.InsertPacket(Packet(Segment(kSynFlag, kSynSeq))));
  const std::string req = "GET / HTTP/1.1\r\n";
  EXPECT_TRUE(t.InsertPacket(Packet(Segment(kAckFlag, kSynSeq + 1, req), kHeaders + 5)));
  ASSERT_EQ(req.size(), t.contiguous_len());
  EXPECT_EQ(0, std::memcmp(t.buf(), req.data(), req.size()));
}

}  // namespace
}  // namespace utils
}  // namespace bess
