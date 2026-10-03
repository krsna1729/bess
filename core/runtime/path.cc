// SPDX-License-Identifier: BSD-3-Clause

#include "runtime/path.h"

#include <limits.h>
#include <unistd.h>

#include <cstring>
#include <string>

#include "utils/logging.h"

namespace bess::runtime {

std::string ExecutableDirectory() {
  char dest[PATH_MAX + 1];
  const ssize_t res = readlink("/proc/self/exe", dest, PATH_MAX);
  if (res == -1) {
    PLOG(FATAL) << "readlink()";
  }
  dest[res] = '\0';
  const char *slash = std::strrchr(dest, '/');
  if (slash == nullptr) {
    PLOG(FATAL) << "strrchr()";
  }
  return std::string(dest, slash - dest + 1);
}

}  // namespace bess::runtime
