// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#ifdef BESS_TRANSACTION_TRACE
#include <sys/sdt.h>

#include <charconv>
#include <cstdint>
#include <string_view>

namespace bess::control {

// The SDK's default ID is 128 random bits encoded as 32 hex digits. Use all
// 128 bits and the stream's frame attempt number, without taking an address
// inside a protobuf object. Other IDs do not get these diagnostic markers.
struct TransactionTraceId {
  uint64_t hi = 0;
  uint64_t lo = 0;
  bool valid = false;

  explicit TransactionTraceId(std::string_view id) {
    if (id.size() != 32) {
      return;
    }
    const char *middle = id.data() + 16;
    const char *end = middle + 16;
    valid = std::from_chars(id.data(), middle, hi, 16).ptr == middle &&
            std::from_chars(middle, end, lo, 16).ptr == end;
  }
};

}  // namespace bess::control

#define BESS_TX_FRAME(frame)                                                   \
  const ::bess::control::TransactionTraceId tx_trace_id(                        \
      (frame).transactions().empty()                                           \
          ? std::string_view{}                                                 \
          : std::string_view((frame).transactions(0).request_id()));             \
  const uint64_t tx_trace_attempt = (frame).attempt_id()

#define BESS_TX_POINT(stage, value)                                            \
  do {                                                                         \
    if (tx_trace_id.valid) {                                                   \
      STAP_PROBE4(bess_tx, stage, tx_trace_id.hi, tx_trace_id.lo,                \
                  tx_trace_attempt, static_cast<uint64_t>(value));              \
    }                                                                          \
  } while (false)
#else
#define BESS_TX_FRAME(frame) static_cast<void>(0)
#define BESS_TX_POINT(stage, value) static_cast<void>(0)
#endif
