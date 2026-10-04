// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

// Internet checksum calculation/verification implementation
// for bytestream, IP, TCP, and incremental update of checksum

#ifndef BESS_UTILS_CHECKSUM_H_
#define BESS_UTILS_CHECKSUM_H_

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "arch/checksum_kernels.h"
#include "common.h"
#include "ip.h"
#include "tcp.h"
#include "udp.h"

namespace bess {
namespace utils {

// All input bytestreams for checksum should be network-order
// Todo: strongly-typed endian for input/output paramters

// The header functions below read 32-bit words of the header and mask out the
// checksum field by its position in a little-endian word.
static_assert(std::endian::native == std::endian::little,
              "checksum.h reads header words as little-endian");

namespace checksum_internal {

// Every read of packet bytes goes through these: memcpy is a byte access, so
// it may alias the header struct a caller has just written (GCC once hoisted a
// typed read of ip.length above `ip->length = ...` in url_filter.cc's
// Generate403Packet, see MODERNIZATION.md) and needs no alignment (IPv4 and
// L4 headers sit 2 bytes off a 4-byte boundary behind Ethernet).
static inline uint64_t Load32(const unsigned char *p) {
  uint32_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

static inline uint64_t Load16(const unsigned char *p) {
  uint16_t v;
  memcpy(&v, p, sizeof(v));
  return v;
}

// Folds an exact sum of 32-bit words into 32 bits with end-around carry: the
// result is congruent to `sum` modulo 2^32 - 1 and is 0 only if `sum` is.
static inline uint32_t Fold64(uint64_t sum) {
  sum = (sum >> 32) + (sum & 0xFFFFFFFF);
  sum += sum >> 32;
  return static_cast<uint32_t>(sum);
}

// Folds a sum below 2^47 into 32 bits for FoldChecksum(): congruent modulo
// 0xFFFF (2^16 is 1 there) and 0 only if `sum` is, which is all the 16-bit
// checksum depends on. Cheaper than Fold64() for the header sums.
static inline uint32_t FoldForChecksum(uint64_t sum) {
  return static_cast<uint32_t>((sum & 0xFFFF) + (sum >> 16));
}

}  // namespace checksum_internal

// Returns 32-bit one's complement sum of 'len' bytes from 'buf'.
//
// Whole 16-byte blocks are summed as 32-bit words (bess::arch::SumWords32:
// AVX2/add-with-carry on x86-64, a vectorizable loop elsewhere), the
// remainder as 16-bit words, then the odd byte, and the total is folded once.
// Only the sum modulo 0xFFFF and whether it is zero carry meaning; this split
// also gives the exact 32-bit value BESS has always returned.
static inline uint32_t CalculateSum(const void *buf, size_t len) {
  using checksum_internal::Load16;
  const auto *p = static_cast<const unsigned char *>(buf);
  const size_t words32_end = len & ~size_t{15};
  uint64_t sum64 = bess::arch::SumWords32(p, words32_end);

  // Fewer than 16 bytes left (the bound lets compilers unroll this fully):
  // 16-bit words, then the odd byte.
  const unsigned char *tail = p + words32_end;
  const size_t rem = len & 15;
  size_t i = 0;
  for (; i + 1 < rem; i += 2) {
    sum64 += Load16(tail + i);
  }
  if (rem & 1) {
    sum64 += tail[i];
  }

  return checksum_internal::Fold64(sum64);
}

// Fold a 32-bit non-inverted checksum into a inverted 16-bit one,
// which can be readily written to L3/L4 checksum field
static inline uint16_t FoldChecksum(uint32_t cksum) {
  cksum = (cksum >> 16) + (cksum & 0xFFFF);
  cksum += (cksum >> 16);
  return ~cksum;
}

// Returns internet checksum (the negative of 16-bit one's complement sum)
// of 'len' bytes from 'buf'
static inline uint16_t CalculateGenericChecksum(const void *buf, size_t len) {
  return FoldChecksum(CalculateSum(buf, len));
}

// Returns true if the 'cksum' is correct for the 'len' bytes from 'buf'
static inline bool VerifyGenericChecksum(const void *buf, size_t len,
                                         uint16_t cksum) {
  uint16_t ret = CalculateGenericChecksum(buf, len);
  return ret == cksum;
}

// Returns true if the 'len' bytes from 'buf' is correct
// Assumption: 'buf' already includes 16-bit checksum (e.g., IP/TCP header)
static inline bool VerifyGenericChecksum(const void *buf, size_t len) {
  return VerifyGenericChecksum(buf, len, 0);
}

namespace checksum_internal {

// One's complement sum (unfolded) of the 20-byte option-less IPv4 header at
// `p`, optionally without its checksum field (bytes 10-11, the high half of
// the third little-endian word).
static inline uint64_t SumIpv4Header(const unsigned char *p,
                                     bool skip_checksum) {
  return Load32(p) + Load32(p + 4) +
         (skip_checksum ? Load32(p + 8) & 0xFFFF : Load32(p + 8)) +
         Load32(p + 12) + Load32(p + 16);
}

// The IPv4 pseudo-header fields (RFC 768/793) as little-endian words: the
// addresses and length are in network order, the zero byte and protocol form
// the 16-bit word 0x00pp, read little-endian as 0xpp00.
static inline uint64_t SumPseudoHeader(be32_t src, be32_t dst, uint16_t l4_len,
                                       uint8_t proto) {
  return uint64_t{src.raw_value()} + dst.raw_value() + be16_t::swap(l4_len) +
         (uint32_t{proto} << 8);
}

}  // namespace checksum_internal

// Returns true if the IP checksum is correct
static inline bool VerifyIpv4NoOptChecksum(const Ipv4 &iph) {
  const auto *p = reinterpret_cast<const unsigned char *>(&iph);
  uint64_t sum = checksum_internal::SumIpv4Header(p, false);
  return FoldChecksum(checksum_internal::FoldForChecksum(sum)) == 0;
}

// Returns IP checksum of the ip header 'iph' without ip options
// It skips the checksum field into the calculation
// It does not set the checksum field in ip header
static inline uint16_t CalculateIpv4NoOptChecksum(const Ipv4 &iph) {
  const auto *p = reinterpret_cast<const unsigned char *>(&iph);
  uint64_t sum = checksum_internal::SumIpv4Header(p, true);
  return FoldChecksum(checksum_internal::FoldForChecksum(sum));
}

// Returns true if the IP checksum is correct
static inline bool VerifyIpv4Checksum(const Ipv4 &iph) {
  const auto *p = reinterpret_cast<const unsigned char *>(&iph);
  size_t ip_header_len = iph.header_length << 2;

  if (likely(ip_header_len == sizeof(iph))) {
    return VerifyIpv4NoOptChecksum(iph);
  }

  if (unlikely(ip_header_len < sizeof(iph))) {
    return false;  // Invalid IP header
  }

  uint64_t sum = CalculateSum(p + sizeof(iph), ip_header_len - sizeof(iph));
  sum += checksum_internal::SumIpv4Header(p, false);
  return FoldChecksum(checksum_internal::FoldForChecksum(sum)) == 0;
}

// Returns IP checksum of the ip header 'iph'
// It skips the checksum field into the calculation
// It does not set the checksum field in ip header
static inline uint16_t CalculateIpv4Checksum(const Ipv4 &iph) {
  const auto *p = reinterpret_cast<const unsigned char *>(&iph);
  size_t ip_header_len = iph.header_length << 2;

  if (likely(ip_header_len == sizeof(iph))) {
    return CalculateIpv4NoOptChecksum(iph);
  }

  if (unlikely(ip_header_len < sizeof(iph))) {
    return 0;  // Invalid IP header. Give up.
  }

  uint64_t sum = CalculateSum(p + sizeof(iph), ip_header_len - sizeof(iph));
  sum += checksum_internal::SumIpv4Header(p, true);
  return FoldChecksum(checksum_internal::FoldForChecksum(sum));
}

// Returns true if the UDP checksum is correct with the UDP header and
// pseudo header info - source ip, destiniation ip, and UDP byte stream length
// udp_len: UDP header + payload in bytes.
// NOTE: Undefined behavior if udp_len < 8
static inline bool VerifyIpv4UdpChecksum(const Udp &udph, be32_t src_ip,
                                         be32_t dst_ip, uint16_t udp_len) {
  using checksum_internal::Load32;
  const auto *p = reinterpret_cast<const unsigned char *>(&udph);

  // UDP checksum is optional, and all zeroes mean "not computed"
  if (udph.checksum == 0) {
    return true;
  }

  // UDP payload, header and pseudo header
  uint64_t sum = CalculateSum(p + sizeof(udph), udp_len - sizeof(udph));
  sum += Load32(p) + Load32(p + 4);
  sum += checksum_internal::SumPseudoHeader(src_ip, dst_ip, udp_len,
                                            Ipv4::Proto::kUdp);

  return FoldChecksum(checksum_internal::FoldForChecksum(sum)) == 0;
}

// Returns true if the UDP checksum is correct
static inline bool VerifyIpv4UdpChecksum(const Ipv4 &iph, const Udp &udph) {
  size_t udp_len = udph.length.value();

  if (unlikely(udp_len < sizeof(udph))) {
    return false;  // Invalid UDP header
  }

  return VerifyIpv4UdpChecksum(udph, iph.src, iph.dst, udp_len);
}

// Returns UDP (on IPv4) checksum of the UDP header 'udph' with pseudo header
// informations - source ip ('src'), destiniation ip ('dst'),
// and UDP byte stream length ('udp_len', udp_header + payload len)
// 'udp_len' is in host-order, and the others are in network-order
// It skips the checksum field into the calculation
// It does not set the checksum field in UDP header
// NOTE: Undefined behavior if udp_len < 8
static inline uint16_t CalculateIpv4UdpChecksum(const Udp &udph, be32_t src,
                                                be32_t dst, uint16_t udp_len) {
  using checksum_internal::Load32;
  const auto *p = reinterpret_cast<const unsigned char *>(&udph);

  // UDP payload, header without the checksum field (bytes 6-7) and pseudo
  // header
  uint64_t sum = CalculateSum(p + sizeof(udph), udp_len - sizeof(udph));
  sum += Load32(p) + (Load32(p + 4) & 0xFFFF);
  sum += checksum_internal::SumPseudoHeader(src, dst, udp_len,
                                            Ipv4::Proto::kUdp);

  // If the result of UDP checksum calculation is 0, return all ones (rfc 768)
  return FoldChecksum(checksum_internal::FoldForChecksum(sum)) ?: 0xFFFF;
}

// Returns UDP (on IPv4) checksum of the UDP header 'udph' with ip header 'iph'
// It skips the checksum field into the calculation
// It does not set the checksum field in UDP header
static inline uint16_t CalculateIpv4UdpChecksum(const Ipv4 &iph,
                                                const Udp &udph) {
  size_t udp_len = udph.length.value();

  if (unlikely(udp_len < sizeof(udph))) {
    return 0;
  }

  return CalculateIpv4UdpChecksum(udph, iph.src, iph.dst, udp_len);
}

// Returns true if the TCP checksum is correct with the TCP header and
// pseudo header info - source ip, destiniation ip, and tcp byte stream length
// tcp_len: TCP header + payload in bytes
// NOTE: Undefined behavior if tcp_len < 20
static inline bool VerifyIpv4TcpChecksum(const Tcp &tcph, be32_t src_ip,
                                         be32_t dst_ip, uint16_t tcp_len) {
  using checksum_internal::Load32;
  const auto *p = reinterpret_cast<const unsigned char *>(&tcph);

  // TCP options and payload, header and pseudo header
  uint64_t sum = CalculateSum(p + sizeof(tcph), tcp_len - sizeof(tcph));
  sum += Load32(p) + Load32(p + 4) + Load32(p + 8) + Load32(p + 12) +
         Load32(p + 16);
  sum += checksum_internal::SumPseudoHeader(src_ip, dst_ip, tcp_len,
                                            Ipv4::Proto::kTcp);

  return FoldChecksum(checksum_internal::FoldForChecksum(sum)) == 0;
}

// Returns true if the TCP checksum is correct
static inline bool VerifyIpv4TcpChecksum(const Ipv4 &iph, const Tcp &tcph) {
  // Unlike UDP, TCP doesn't have a length field. Derive from IP header.
  size_t ip_len = iph.length.value();
  size_t ip_header_len = iph.header_length << 2;

  if (unlikely(ip_len < ip_header_len + sizeof(tcph))) {
    return false;  // Invalid IP header
  }

  return VerifyIpv4TcpChecksum(tcph, iph.src, iph.dst, ip_len - ip_header_len);
}

// Returns TCP (on IPv4) checksum of the tcp header 'tcph' with pseudo header
// informations - source ip ('src'), destiniation ip ('dst'),
// and tcp byte stream length ('tcp_len', tcp_header + payload len)
// 'tcp_len' is in host-order, and the others are in network-order
// It skips the checksum field into the calculation
// It does not set the checksum field in TCP header
// NOTE: Undefined behavior if tcp_len < 20
static inline uint16_t CalculateIpv4TcpChecksum(const Tcp &tcph, be32_t src,
                                                be32_t dst, uint16_t tcp_len) {
  using checksum_internal::Load32;
  const auto *p = reinterpret_cast<const unsigned char *>(&tcph);

  // TCP options and payload, header without the checksum field (bytes 16-17,
  // the low half of the fifth little-endian word) and pseudo header
  uint64_t sum = CalculateSum(p + sizeof(tcph), tcp_len - sizeof(tcph));
  sum += Load32(p) + Load32(p + 4) + Load32(p + 8) + Load32(p + 12) +
         (Load32(p + 16) >> 16);
  sum += checksum_internal::SumPseudoHeader(src, dst, tcp_len,
                                            Ipv4::Proto::kTcp);

  return FoldChecksum(checksum_internal::FoldForChecksum(sum));
}

// Returns TCP (on IPv4) checksum of the tcp header 'tcph' with ip header 'iph'
// It skips the checksum field into the calculation
// It does not set the checksum field in TCP header
static inline uint16_t CalculateIpv4TcpChecksum(const Ipv4 &iph,
                                                const Tcp &tcph) {
  // Unlike UDP, TCP doesn't have a length field. Derive from IP header.
  size_t ip_len = iph.length.value();
  size_t ip_header_len = iph.header_length << 2;

  if (unlikely(ip_len < ip_header_len + sizeof(tcph))) {
    return 0;  // Invalid IP header
  }

  return CalculateIpv4TcpChecksum(tcph, iph.src, iph.dst,
                                  ip_len - ip_header_len);
}

// Incremental checksum update
//
// The functions below can be used to update multiple fields and update the
// checksum in a single shot:
//
// uint32_t increment = 0;
//
// increment += ChecksumIncrement32(iphdr->src, new_src);
// increment += ChecksumIncrement32(iphdr->dst, new_dst);
//
// iphdr->src = new_src
// iphdr->dst = new_dst
// iphdr->checksum = UpdateChecksumWithIncrement(iphdr->checksum, incremental);

static inline uint32_t ChecksumIncrement32(uint32_t old_value,
                                           uint32_t new_value) {
  uint32_t sum = (~old_value >> 16) + (~old_value & 0xFFFF);
  sum += (new_value >> 16) + (new_value & 0xFFFF);
  return sum;
}

// Note that the return type is uint32_t. You can add up increments from both
// ChecksumIncrement16() and ChecksumIncrement32()
static inline uint32_t ChecksumIncrement16(uint16_t old_value,
                                           uint16_t new_value) {
  return (~old_value & 0xFFFF) + new_value;
}

// Returns updated checksum value, which is ready to be written in the header
static inline uint16_t UpdateChecksumWithIncrement(uint16_t old_checksum,
                                                   uint32_t increment) {
  return FoldChecksum((~old_checksum & 0xFFFF) + increment);
}

// Returns incrementally updated checksum from old_checksum
// when 32-bit 'old_value' changes to 'new_value' e.g., changed IPv4 address
static inline uint16_t UpdateChecksum32(uint16_t old_checksum,
                                        uint32_t old_value,
                                        uint32_t new_value) {
  // new checksum = ~(~old_checksum + ~old_value + new_value) by RFC 1624
  uint32_t inc = ChecksumIncrement32(old_value, new_value);

  return UpdateChecksumWithIncrement(old_checksum, inc);
}

// Returns incrementally updated checksum from old_checksum
// when 16-bit 'old_value' changes to 'new_value' e.g., changed port number
static inline uint16_t UpdateChecksum16(uint16_t old_checksum,
                                        uint16_t old_value,
                                        uint16_t new_value) {
  // new checksum = ~(~old_checksum + ~old_value + new_value) by RFC 1624
  uint32_t inc = ChecksumIncrement16(old_value, new_value);

  return UpdateChecksumWithIncrement(old_checksum, inc);
}

}  // namespace utils
}  // namespace bess

#endif
