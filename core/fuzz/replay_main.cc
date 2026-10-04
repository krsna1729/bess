// SPDX-License-Identifier: BSD-3-Clause

// main() for fuzz harnesses built without libFuzzer (GCC): runs every file
// named on the command line, and every regular file inside each directory
// named (recursively, in sorted order), through LLVMFuzzerTestOneInput once.
// Arguments starting with '-' are libFuzzer flags and are ignored. A harness
// oracle violation aborts, which fails the test that runs the corpus.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

namespace {

bool RunFile(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "replay: cannot read %s\n", path.c_str());
    return false;
  }
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
  // Pass a non-null pointer for empty inputs, as libFuzzer does.
  static const uint8_t kEmpty = 0;
  LLVMFuzzerTestOneInput(bytes.empty() ? &kEmpty : bytes.data(),
                         bytes.size());
  return true;
}

}  // namespace

int main(int argc, char **argv) {
  std::vector<std::filesystem::path> inputs;
  for (int i = 1; i < argc; i++) {
    const std::string arg = argv[i];
    if (arg.starts_with("-")) {
      continue;
    }
    std::error_code ec;
    if (std::filesystem::is_directory(arg, ec)) {
      for (const auto &entry :
           std::filesystem::recursive_directory_iterator(arg)) {
        if (entry.is_regular_file()) {
          inputs.push_back(entry.path());
        }
      }
    } else {
      inputs.emplace_back(arg);
    }
  }
  std::sort(inputs.begin(), inputs.end());
  if (inputs.empty()) {
    std::fprintf(stderr, "replay: no inputs\n");
    return 1;
  }
  size_t ran = 0;
  for (const auto &path : inputs) {
    if (!RunFile(path)) {
      return 1;
    }
    ran++;
  }
  std::printf("replay: %zu inputs ok\n", ran);
  return 0;
}
