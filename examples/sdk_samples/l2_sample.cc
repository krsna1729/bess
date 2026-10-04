// SPDX-License-Identifier: BSD-3-Clause

// Forwarding database (l2/fdb.h): learn, look up a batch, age, and read the
// learn result. Storage is a type the module picks (D-073): `l2::Fdb` is
// MacTable-backed, and `BasicFdb<PackedMacTable<...>>` is the packed table a
// bridge uses. Ownership: the FDB is a unique_ptr; one writer learns and ages,
// readers look up.

#include <array>
#include <cstdint>
#include <span>

#include "l2/fdb.h"

namespace sample {

using bess::dataplane::InterfaceId;
using bess::l2::BridgeDomainId;
using bess::l2::Fdb;
using bess::l2::LearnResult;
using bess::l2::MacAddress;

bool L2Sample(uint64_t now) {
  auto fdb = Fdb::Create(Fdb::Config{.capacity = 1024});
  if (!fdb) {
    return false;
  }
  const BridgeDomainId domain{uint16_t{0}};
  const MacAddress host{{0x02, 0, 0, 0, 0, 1}};
  const MacAddress multicast{{0x01, 0, 0x5e, 0, 0, 1}};

  if ((*fdb)->Learn(domain, host, InterfaceId{uint16_t{2}}, now) != LearnResult::kLearned) {
    return false;
  }
  // A multicast source is never learned: the result says why.
  if ((*fdb)->Learn(domain, multicast, InterfaceId{uint16_t{2}}, now) != LearnResult::kIgnored) {
    return false;
  }
  // The MAC moves: the same call reports it.
  if ((*fdb)->Learn(domain, host, InterfaceId{uint16_t{3}}, now) != LearnResult::kMoved) {
    return false;
  }
  const std::array<bess::l2::FdbKey, 1> keys = {bess::l2::MakeKey(domain, host)};
  std::array<InterfaceId, 1> out{};
  if ((*fdb)->LookupBatch(keys, out) != 1 || out[0] != InterfaceId{uint16_t{3}}) {
    return false;
  }
  // Ageing is budgeted work the owner schedules.
  (void)(*fdb)->Age(now + Fdb::Config{}.aging + 1, /*budget=*/64);
  return true;
}

}  // namespace sample
