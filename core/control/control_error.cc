// SPDX-License-Identifier: BSD-3-Clause

#include "control/control_error.h"

#include <cerrno>
#include <cstdarg>
#include <cstring>

#include "utils/dpdk_memory.h"
#include "utils/format.h"

namespace bess {
namespace control {

ControlErrorCode CodeFromErrno(int err) {
  switch (err) {
    case EINVAL:
    case EPERM:
    case EACCES:
    case ENOTSUP:  // the request names what its target does not support (a command)
      return ControlErrorCode::kInvalidArgument;
    case ENOENT:
      return ControlErrorCode::kNotFound;
    case EEXIST:
      return ControlErrorCode::kAlreadyExists;
    case EBUSY:
      return ControlErrorCode::kResourceBusy;
    case ENOMEM:
      // Memory is a bounded resource (D-198): the same request is refused the
      // same way until memory is freed, so it is not a failure to retry.
      return ControlErrorCode::kResourceExhausted;
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

ControlError OutOfMemory(const std::bad_alloc &e) {
  ControlError error;
  error.code = ControlErrorCode::kResourceExhausted;
  error.err = ENOMEM;
  if (const auto *refused = dynamic_cast<const utils::MemoryRefused *>(&e)) {
    error.message = refused->what();
  } else {
    error.message = "out of memory";
  }
  return error;
}

}  // namespace control
}  // namespace bess
