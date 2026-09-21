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

#include "control/control_error.h"

#include <cerrno>
#include <cstdarg>
#include <cstring>

#include "utils/format.h"

namespace bess {
namespace control {

ControlErrorCode CodeFromErrno(int err) {
  switch (err) {
    case EINVAL:
    case EPERM:
    case EACCES:
      return ControlErrorCode::kInvalidArgument;
    case ENOENT:
      return ControlErrorCode::kNotFound;
    case EEXIST:
      return ControlErrorCode::kAlreadyExists;
    case EBUSY:
      return ControlErrorCode::kResourceBusy;
    case ENOMEM:
    case EIO:
    case EAGAIN:
      return ControlErrorCode::kResourceFailure;
    case 0:
      // Legacy internal APIs report some failures with code 0 and no message;
      // keep the code 0 on the wire but do not pretend it is a valid argument.
      return ControlErrorCode::kInternal;
    default:
      return ControlErrorCode::kInternal;
  }
}

ControlError Err(int err, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  std::string message = bess::utils::FormatVarg(fmt, ap);
  va_end(ap);

  ControlError error;
  error.code = CodeFromErrno(err);
  error.err = err;
  error.message = std::move(message);
  return error;
}

ControlError Errno(int err) {
  ControlError error;
  error.code = CodeFromErrno(err);
  error.err = err;
  error.message = strerror(err);
  return error;
}

}  // namespace control
}  // namespace bess
