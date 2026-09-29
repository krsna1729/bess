// SPDX-License-Identifier: BSD-3-Clause

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
