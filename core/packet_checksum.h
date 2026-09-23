// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
//   list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
//   this list of conditions and the following disclaimer in the documentation
//   and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
//   contributors may be used to endorse or promote products derived from this
//   software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_PACKET_CHECKSUM_H_
#define BESS_PACKET_CHECKSUM_H_

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

#include "packet.h"
#include "utils/endian.h"

namespace bess::packet {

enum class IpVersion : uint8_t {
  kIpv4,
  kIpv6,
};

enum class NetworkChecksum : uint8_t {
  kNone,
  kIpv4Header,
};
enum class TransportChecksum : uint8_t {
  kNone,
  kUdp,
  kTcp,
};

struct ChecksumPlan {
  size_t network_offset = 0;
  size_t transport_offset = 0;
  IpVersion ip_version = IpVersion::kIpv4;
  NetworkChecksum network = NetworkChecksum::kNone;
  TransportChecksum transport = TransportChecksum::kNone;
};

struct ChecksumValues {
  std::optional<utils::be16_t> network;
  std::optional<utils::be16_t> transport;
};

enum class ChecksumError : uint8_t {
  kNullPacket,
  kMalformedChain,
  kInvalidPlan,
  kLengthOutOfRange,
  kInvalidIpv4Header,
  kInvalidIpv6Header,
  kInvalidTransportHeader,
  kProtocolMismatch,
  kFragmentedDatagram,
  kUnsupportedExtensionHeader,
  kUnsupportedJumbogram,
  kAllocationFailed,
  kInsufficientWritableCapacity,
};

// Computes requested checksums from the packet's logical bytes. This is
// read-only, traverses chains without linearizing, and uses IP-declared lengths
// rather than consuming unrelated trailing packet bytes. UDP results of zero
// are encoded as 0xffff. The caller may share payload backing.
std::expected<ChecksumValues, ChecksumError> ComputeChecksums(
    PacketRef packet, const ChecksumPlan &plan) noexcept;

// Applies all requested checksums transactionally. The caller must exclusively
// own the descriptor chain; shared backing is copied only when a target
// checksum field lies in it. Split fields are written across segments without
// flattening the chain. An error leaves packet bytes, topology, and metadata
// unchanged.
std::expected<void, ChecksumError> ApplySoftwareChecksums(
    PacketHandle &packet, const ChecksumPlan &plan) noexcept;

}  // namespace bess::packet

#endif  // BESS_PACKET_CHECKSUM_H_
