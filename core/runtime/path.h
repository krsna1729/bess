// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RUNTIME_PATH_H_
#define BESS_RUNTIME_PATH_H_

#include <string>

namespace bess::runtime {

// Returns the directory containing the current executable, with a trailing '/'.
std::string ExecutableDirectory();

}  // namespace bess::runtime

#endif  // BESS_RUNTIME_PATH_H_
