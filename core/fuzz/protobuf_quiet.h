// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FUZZ_PROTOBUF_QUIET_H_
#define BESS_FUZZ_PROTOBUF_QUIET_H_

// Malformed input is what a fuzzer feeds: protobuf's per-parse error logs are
// noise. Protobuf 22 and later log through Abseil; earlier releases (Ubuntu
// 24.04 ships 3.21) through their own handler.
#if __has_include(<absl/log/globals.h>)
#include <absl/log/globals.h>
#else
#include <google/protobuf/stubs/logging.h>
#endif

namespace bess::fuzz {

inline void QuietProtobufLogs() {
#if __has_include(<absl/log/globals.h>)
  absl::SetMinLogLevel(absl::LogSeverityAtLeast::kInfinity);
#else
  google::protobuf::SetLogHandler(nullptr);
#endif
}

}  // namespace bess::fuzz

#endif  // BESS_FUZZ_PROTOBUF_QUIET_H_
