// SPDX-License-Identifier: BSD-3-Clause

// The battery's own test, built and run from the installed bess-dev alone.

#include "lease_table.h"

#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

void Expect(bool ok, const char *what) {
  if (!ok) {
    std::fprintf(stderr, "FAILED: %s\n", what);
    failures++;
  }
}

struct Subscriber {
  uint32_t id;
};

}  // namespace

int main() {
  auto table = lease::LeaseTable<Subscriber>::Create(2);
  Expect(table != nullptr, "create");

  const lease::LeaseHandle a = table->Grant({7}, /*now=*/0, /*ttl=*/10);
  const lease::LeaseHandle b = table->Grant({8}, 0, 20);
  Expect(table->Holding(a) != nullptr && table->Holding(a)->id == 7, "a held");
  Expect(table->Grant({9}, 0, 10) == lease::LeaseHandle{}, "full table refuses");

  // a renewed to 25; at 21 only b has expired.
  Expect(table->Renew(a, 5, 20), "renew a");
  uint32_t ended_id = 0;
  Expect(table->Expire(21, [&](const Subscriber &s) noexcept { ended_id = s.id; }) == 1, "one expired");
  Expect(ended_id == 8, "b expired, not a");
  Expect(table->Holding(b) == nullptr, "b's handle is stale");
  Expect(!table->Renew(b, 21, 10), "a stale handle cannot renew");

  // b's slot is reused; the old handle still cannot reach the new lease.
  const lease::LeaseHandle c = table->Grant({10}, 21, 10);
  Expect(c.id == b.id && c.generation != b.generation, "slot reused, new generation");
  Expect(table->Holding(b) == nullptr && table->Holding(c)->id == 10, "old handle stays stale");

  Expect(table->Expire(40, [](const Subscriber &) noexcept {}) == 2, "both expire");
  if (failures == 0) {
    std::printf("lease_table_test: OK\n");
  }
  return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
