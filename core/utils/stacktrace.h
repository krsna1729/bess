// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_UTILS_STACKTRACE_H_
#define BESS_UTILS_STACKTRACE_H_

#include <string>

namespace bess::utils {

// Returns a symbolized stack trace with source context when addr2line is
// available.
std::string StackTrace(void *trap_ip);

}  // namespace bess::utils

#endif  // BESS_UTILS_STACKTRACE_H_
