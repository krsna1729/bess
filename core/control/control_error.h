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
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
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
