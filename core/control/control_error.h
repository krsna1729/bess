// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_CONTROL_ERROR_H_
#define BESS_CONTROL_CONTROL_ERROR_H_

#include <expected>
#include <string>

namespace bess {
namespace control {

// The control plane's single error model. The RPC adapter translates these to
// the legacy protobuf error fields; nothing inside the control plane inspects
// protobuf errors.
enum class ControlErrorCode {
  kInvalidArgument,
  kNotFound,
  kAlreadyExists,
  kConflict,
  kResourceBusy,
  kUnsupportedTransaction,
  kResourceFailure,
  kInternal,
};

struct ControlError {
  ControlErrorCode code = ControlErrorCode::kInternal;

  // errno-compatible value carried through to the legacy protobuf surface
  // (Error.code: 0 for success, errno > 0 for failure).
  int err = 0;

  // Human-readable message; identical to the text the legacy handlers
  // produced for the same failure.
  std::string message;

  // Optional structured context, filled in as operations migrate to the
  // transactional engine.
  std::string object;
  std::string field;
};

template <typename T>
using ControlResult = std::expected<T, ControlError>;

ControlErrorCode CodeFromErrno(int err);

// Builds an error from an errno value plus a printf-formatted message.
[[gnu::format(printf, 2, 3)]]
ControlError Err(int err, const char* fmt, ...);

// Builds an error whose message is strerror(err).
ControlError Errno(int err);

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_CONTROL_ERROR_H_
