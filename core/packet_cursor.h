// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
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

#ifndef BESS_PACKET_CURSOR_H_
#define BESS_PACKET_CURSOR_H_

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <type_traits>

#include "packet.h"

namespace bess::packet {

// Read-only cursor over the logical bytes of a native packet. The cursor never
// makes a packet linear: contiguous reads borrow the current segment, while
// ReadBytes()/Read<T>() copy across segment boundaries.
//
// A failed Skip() or ReadBytes() leaves the cursor unchanged. PacketRef is a
// value view, so copying PacketCursor is the cheap mark/restore operation for
// optional parsers.
class PacketCursor {
 public:
  explicit PacketCursor(::bess::PacketRef packet) noexcept
      : segment_(packet),
        segment_offset_(0),
        packet_offset_(0),
        remaining_(packet.handle() == nullptr ? 0 : packet.total_len()) {}

  [[nodiscard]] size_t offset() const noexcept { return packet_offset_; }
  [[nodiscard]] size_t remaining() const noexcept { return remaining_; }

  [[nodiscard]] bool Skip(size_t bytes) noexcept {
    if (bytes > remaining_) {
      return false;
    }
    PacketCursor next = *this;
    if (!next.SkipUnchecked(bytes)) {
      return false;
    }
    *this = next;
    return true;
  }

  [[nodiscard]] std::span<const std::byte> PeekContiguous(
      size_t bytes) const noexcept {
    if (bytes == 0 || bytes > remaining_ || segment_.handle() == nullptr) {
      return {};
    }

    const size_t data_len = segment_.data_len();
    if (segment_offset_ > data_len ||
        bytes > data_len - segment_offset_) {
      return {};
    }

    const auto *data = segment_.head_data<const std::byte *>(
        static_cast<uint16_t>(segment_offset_));
    return std::span<const std::byte>(data, bytes);
  }

  [[nodiscard]] bool ReadBytes(std::span<std::byte> out) noexcept {
    if (out.size() > remaining_) {
      return false;
    }
    PacketCursor next = *this;
    if (!next.ReadBytesUnchecked(out)) {
      return false;
    }
    *this = next;
    return true;
  }

  template <typename T>
    requires(std::is_trivially_copyable_v<T> && !std::is_const_v<T> &&
             !std::is_volatile_v<T>)
  [[nodiscard]] std::optional<T> Read() noexcept {
    std::array<std::byte, sizeof(T)> raw;
    if (auto contiguous = PeekContiguous(sizeof(T));
        contiguous.size() == sizeof(T)) {
      std::memcpy(raw.data(), contiguous.data(), sizeof(T));
      if (!Skip(sizeof(T))) {
        return std::nullopt;
      }
    } else if (!ReadBytes(raw)) {
      return std::nullopt;
    }
    return std::bit_cast<T>(raw);
  }

 private:
  void Normalize() noexcept {
    while (segment_.handle() != nullptr &&
           segment_offset_ >= static_cast<size_t>(segment_.data_len())) {
      segment_ = segment_.next();
      segment_offset_ = 0;
    }
  }

  [[nodiscard]] bool SkipUnchecked(size_t bytes) noexcept {
    while (bytes != 0) {
      Normalize();
      if (segment_.handle() == nullptr) {
        return false;
      }

      const size_t available =
          static_cast<size_t>(segment_.data_len()) - segment_offset_;
      const size_t step = std::min(bytes, available);
      segment_offset_ += step;
      packet_offset_ += step;
      remaining_ -= step;
      bytes -= step;
    }
    Normalize();
    return true;
  }

  [[nodiscard]] bool ReadBytesUnchecked(std::span<std::byte> out) noexcept {
    while (!out.empty()) {
      Normalize();
      if (segment_.handle() == nullptr) {
        return false;
      }

      const size_t available =
          static_cast<size_t>(segment_.data_len()) - segment_offset_;
      const size_t step = std::min(out.size(), available);
      const auto *source = segment_.head_data<const std::byte *>(
          static_cast<uint16_t>(segment_offset_));
      std::memcpy(out.data(), source, step);
      out = out.subspan(step);
      segment_offset_ += step;
      packet_offset_ += step;
      remaining_ -= step;
    }
    Normalize();
    return true;
  }

  ::bess::PacketRef segment_;
  size_t segment_offset_ = 0;
  size_t packet_offset_ = 0;
  size_t remaining_ = 0;
};

static_assert(std::is_trivially_copyable_v<PacketCursor>);
static_assert(std::is_trivially_destructible_v<PacketCursor>);

}  // namespace bess::packet

#endif  // BESS_PACKET_CURSOR_H_
