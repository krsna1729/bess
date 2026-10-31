// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CONTROL_DATAPLANE_TRANSACTIONS_H_
#define BESS_CONTROL_DATAPLANE_TRANSACTIONS_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "control/control_error.h"
#include "dataplane/transaction_engine.h"
#include "framework/memory_scope.h"
#include "framework/resource_bindings.h"
#include "utils/dpdk_memory.h"
#include "pb/control_v2.pb.h"

namespace bess::control {

// The dataplane transaction RPC's server side (G1.2c, Decision D-025), apart
// from gRPC: typed operations decoded through each resource's bound codec, applied
// through the TransactionEngine, and outcomes recorded by request_id.
//
//   - Idempotency: an outcome is recorded with a digest of the request's
//     contents (everything but request_id). The same id and contents replay
//     the record; the same id with other contents is refused (kConflict).
//     BUSY and CONFLICT outcomes attempted nothing and are not recorded:
//     the client retries under the same id.
//   - Records live in a bounded window, oldest out first: at most
//     kMaxRecords records, and at most kMaxRecordBytes charged (D-207), on
//     glibc, every byte charged through utils::AdmittedGlibcAllocator to
//     the daemon's scope, resource "transaction_records" (D-206, class C):
//     the id, the record and the window's bookkeeping. A record is kept
//     serialized and compacted (D-207): only the operation results that
//     differ from the transaction's common one (an applied transaction keeps
//     none), so a 1600-operation load costs its id, not 1600 results; Get
//     and a replay rebuild the full record, byte-identical. Charged before
//     the transaction is attempted: a refused charge refuses it with nothing
//     applied (RESOURCE_EXHAUSTED), so an applied outcome is always recorded.
//   - The daemon epoch is drawn at construction: a record of another epoch
//     cannot exist, so a client that sees the epoch change knows an unknown
//     id means "unknown", not "never ran".
//
// Not thread-safe: callers hold the control-plane lock, as for module
// commands, which write the same tables.
class DataplaneTransactions {
 public:
  static constexpr size_t kMaxRecords = 4096;
  static constexpr size_t kMaxRecordBytes = size_t{4} << 20u;

  DataplaneTransactions(dataplane::TransactionEngine &engine,
                        const framework::ResourceBindings &bindings);
  DataplaneTransactions(dataplane::TransactionEngine &engine,
                        const framework::ResourceBindings &bindings,
                        uint64_t epoch)
      : engine_(engine),
        bindings_(bindings),
        epoch_(epoch),
        records_(Records::allocator_type(lease_.growth())),
        order_(Order::allocator_type(lease_.growth())) {}
  // The containers' allocators point at lease_'s growth hook.
  DataplaneTransactions(const DataplaneTransactions &) = delete;
  DataplaneTransactions &operator=(const DataplaneTransactions &) = delete;

  // `on_busy`: whether the caller passes a BUSY outcome on or retries it
  // itself (TransactionEngine::OnBusy; metrics count only answers).
  ControlResult<pb::v2::ApplyTransactionResponse> Apply(
      const pb::v2::ApplyTransactionRequest &request,
      dataplane::TransactionEngine::OnBusy on_busy = dataplane::TransactionEngine::OnBusy::kAnswer);
  pb::v2::GetTransactionResponse Get(const std::string &request_id) const;
  pb::v2::ListTransactionResourcesResponse List() const;
  // What each reachable resource holds now and, where it can state one, its
  // capacity (GetResourceStatus). Same reachability rule as List(): a
  // resource without a codec is not reported.
  pb::v2::GetResourceStatusResponse ResourceStatus() const;

  // Observation uses the same codec-bound reachability as the control API.
  // The visitor runs under the caller's control-plane lock.
  template <typename Visit>
  void VisitReachableResources(Visit &&visit) const {
    for (const std::string &name : engine_.ResourceNames()) {
      const dataplane::Resource *resource = engine_.FindResource(name);
      if (resource != nullptr && bindings_.Find(*resource) != nullptr) {
        visit(*resource);
      }
    }
  }

  uint64_t epoch() const { return epoch_; }
  size_t records() const { return records_.size(); }
  // What the window is charged now (the lease kMaxRecordBytes bounds).
  size_t bytes() const { return lease_.bytes(); }

 private:
  using Text = std::basic_string<char, std::char_traits<char>,
                                 utils::AdmittedGlibcAllocator<char>>;
  using Positions =
      std::vector<uint32_t, utils::AdmittedGlibcAllocator<uint32_t>>;
  // A record as kept: `record` serializes it without its id (the map key)
  // and without the results equal to `common` (status, no error); it holds
  // the others, at positions `at`, of `ops` in all.
  struct Recorded {
    uint64_t digest;
    Text record;
    uint32_t ops = 0;
    pb::v2::TransactionOpResult::Status common =
        pb::v2::TransactionOpResult::STATUS_APPLIED;
    Positions at;
  };
  // Looked up by std::string without a copy: compared as string_views
  // (strings of different allocators have no operator==).
  struct TextHash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const noexcept {
      return std::hash<std::string_view>{}(s);
    }
  };
  struct TextEq {
    using is_transparent = void;
    bool operator()(std::string_view a, std::string_view b) const noexcept {
      return a == b;
    }
  };
  using Records = std::unordered_map<
      Text, Recorded, TextHash, TextEq,
      utils::AdmittedGlibcAllocator<std::pair<const Text, Recorded>>>;
  // Keys of records_ (its nodes do not move), oldest first.
  using Order =
      std::deque<const Text *, utils::AdmittedGlibcAllocator<const Text *>>;

  // The record of a transaction about to be attempted, charged now (D-206):
  // its id, the bytes its applied outcome serializes to (compacted, it keeps
  // no operation result), its map node and bucket room, and a window slot at
  // the back of order_ (nullptr until CommitRecord). Throws MemoryRefused,
  // leaving order_ unchanged; the caller pops the slot if it does not commit.
  Records::node_type ReserveRecord(const std::string &request_id,
                                   uint64_t digest);
  // Records `record`, compacted, in a node and slot ReserveRecord gave,
  // ageing the oldest out past kMaxRecords or kMaxRecordBytes. Allocates
  // nothing for an applied record; a rejection's kept results may throw
  // MemoryRefused, recording nothing.
  void CommitRecord(Records::node_type node,
                    const pb::v2::TransactionRecord &record);
  // The full record `kept` was compacted from, its id `request_id`.
  static pb::v2::TransactionRecord Expand(std::string_view request_id,
                                          const Recorded &kept);

  dataplane::TransactionEngine &engine_;
  const framework::ResourceBindings &bindings_;
  const uint64_t epoch_;
  // Charges the daemon's scope (bessd/transaction_records) for what the
  // containers hold; declared before them.
  framework::MemoryLease lease_ =
      framework::MemoryAccounts::Process().Lease("transaction_records", 0);
  Records records_;
  Order order_;
};

}  // namespace bess::control

#endif  // BESS_CONTROL_DATAPLANE_TRANSACTIONS_H_
