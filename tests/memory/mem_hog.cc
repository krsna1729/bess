// SPDX-License-Identifier: BSD-3-Clause

// For tools/run_capped.py --self-test: allocates argv[1] bytes, writes every
// page so each is resident, and exits 0. The wrapper must fail it when that
// is above the declaration and pass it when it is below.

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char **argv) {
  if (argc != 2) {
    (void)std::fprintf(stderr, "usage: %s BYTES\n", argv[0]);
    return 2;
  }
  const size_t bytes = std::stoull(argv[1]);
  const auto page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
  auto *p = static_cast<volatile char *>(std::malloc(bytes));
  if (p == nullptr) {
    std::perror("malloc");
    return 2;
  }
  for (size_t i = 0; i < bytes; i += page) {
    p[i] = 1;
  }
  std::printf("touched %zu bytes\n", bytes);
  std::free(const_cast<char *>(p));
  return 0;
}
