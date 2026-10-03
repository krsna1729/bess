// SPDX-License-Identifier: BSD-3-Clause

#include "flow/owner.h"

#include "utils/logging.h"

namespace bess::flow {

OwnerToken ThreadOwner::Current() noexcept {
  // The address of a thread-local object is unique among live threads and
  // never zero.
  thread_local const char tag = 0;
  return reinterpret_cast<OwnerToken>(&tag);
}

namespace detail {

void OwnerViolation(const char *table_op, OwnerToken owner,
                    OwnerToken caller) {
  LOG(FATAL) << "flow table ownership violation in " << table_op
             << ": the table is owned by context " << owner
             << " and was called from context " << caller
             << " (a worker-owned flow table has no synchronisation)";
  __builtin_unreachable();
}

}  // namespace detail
}  // namespace bess::flow
