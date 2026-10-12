// SPDX-License-Identifier: BSD-3-Clause

// Lists a daemon's transactional resources with the installed C++ SDK:
//   list_resources [host:port]     (default localhost:10514)

#include <bess/sdk/client.h>

#include <iostream>
#include <string>

int main(int argc, char **argv) {
  const std::string target = argc > 1 ? argv[1] : "localhost:10514";
  auto client = bess::sdk::Client::Dial(target);
  auto resources = client->Resources();
  if (!resources) {
    std::cerr << target << ": " << resources.error().message << '\n';
    return 1;
  }
  for (const auto &[name, r] : *resources) {
    std::cout << name << "  " << r.key_type << " -> " << r.value_type << '\n';
  }
  return 0;
}
