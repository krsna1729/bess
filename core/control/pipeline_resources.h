// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_PIPELINE_RESOURCES_H_
#define BESS_CONTROL_PIPELINE_RESOURCES_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "control/control_error.h"
#include "control/pipeline_diff.h"
#include "control/pipeline_spec.h"
#include "dataplane/resource.h"
#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"

namespace bess::control {

// Resource contents as desired state (D-129): read back through each
// resource's codec, and converged to by one dataplane transaction -- the
// same decoding and the same engine as ApplyTransaction.
struct ResourceConvergence {
  ResourceDelta delta;
  std::vector<dataplane::Op> ops;  // with build_ops only
  bool requires_paused_workers = false;
};

// The contents of the named resources (all reachable ones if `names` is
// empty), by name; entries in the codec's order. An unknown name is
// NOT_FOUND. Keys and values are in canonical wire form (deterministic
// serialization), so a read-back compares equal to itself. Contents above
// `max_bytes` serialized (each entry framed as a repeated ResourceEntry) are
// RESOURCE_EXHAUSTED: before any entry is read when the live counts at the
// smallest entry the types allow already exceed it, else as soon as the
// entries read do, so a refused read holds at most `max_bytes` and one entry.
ControlResult<std::vector<ResourceSpec>> ReadResources(
    const dataplane::TransactionEngine &engine, const framework::ResourceBindings &bindings,
    std::span<const std::string> names, size_t max_bytes);

// One page of a resource from a cursor (empty: the first page): about
// `max_entries` (>= 1) entries and `max_bytes` framed, and the cursor of the
// next page (unset: the read is done). A codec with a position
// (ResourceCodec::VisitFrom) is read from it, so a page costs its own
// entries, with SCAN semantics: an entry present for the whole read is
// returned at least once, one changed meanwhile may be missed or repeat. A
// page may run past the bounds by the entries sharing a position (a
// wildcard key's rules) and by the one entry that crossed the byte bound.
// Any other codec is read by canonical key bytes: each page walks the whole
// resource for the smallest keys after the cursor, holding at most
// `max_entries` + 1 entries. Either way a read holds about a page, never the
// resource, and pages read at different generations are not one snapshot.
struct ResourceEntryPage {
  std::vector<ResourceEntrySpec> entries;
  std::optional<std::string> next;  // the next page's cursor; unset: done
};
ControlResult<ResourceEntryPage> ReadResourcePage(
    const dataplane::TransactionEngine &engine, const framework::ResourceBindings &bindings,
    const std::string &name, const std::string &cursor, size_t max_entries, size_t max_bytes);

// Compares `desired` with the resource's contents: every desired key decodes
// (INVALID_ARGUMENT naming the entry if not, or if a key repeats), a key
// absent now is created, a value whose canonical bytes differ is updated,
// and a key desired state does not name is removed. With `build_ops`, also
// decodes the values that change into the transaction's operations (for a
// singleton this stages the value, so a diff does not).
ControlResult<ResourceConvergence> Converge(const dataplane::TransactionEngine &engine,
                                            const framework::ResourceBindings &bindings,
                                            const ResourceSpec &desired, bool build_ops);

}  // namespace bess::control

#endif  // BESS_CONTROL_PIPELINE_RESOURCES_H_
