// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_CLASSIFIER_CONCURRENT_MASKED_H_
#define BESS_CLASSIFIER_CONCURRENT_MASKED_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "classifier/backend.h"
#include "classifier/concurrent_exact.h"
#include "classifier/masked_exact.h"
#include "classifier/record_arena.h"
#include "rcu/rcu_domain.h"
#include "rcu/rcu_ptr.h"

namespace bess::classifier {

// A masked (ternary) table updated in place while workers read it (G1.2
// mode C): tuple-space search where every tuple -- one per distinct mask --
// is a ConcurrentExactTable keyed by the masked value.
//
//   tuple list (RcuPtr, republished only when a mask appears or goes away)
//     ├── mask A -> ConcurrentExactTable: masked value -> rule id
//     └── mask B -> ...
//   rule records (write-once, id-indexed): {priority, sequence, result}
//
// A table value packs the rule id (low 32 bits) with the rule's result (bits
// 32-47) and its priority (bits 48-63, biased, when it fits in [-32767,
// 32766]; 0 otherwise), so a packet that exactly one tuple matches takes its
// result from the table without touching the record, and a second match ranks
// by the packed priorities; a record is loaded only for equal priorities (the
// later command wins) or one that does not fit. Bits 48-63 all set mark a
// chain instead (below).
//
// A rule may also carry ranges (D-186): the field at a byte offset of the key,
// read as a big-endian unsigned integer, must lie in an inclusive [low, high].
// A ranged field is not in the rule's mask, so a range adds no tuple. Rule
// identity is (mask, value, ranges): rules that share a (mask, value) and
// differ in their ranges coexist under one table key. A key whose only rule
// has no range keeps the packed value above; any other key's value points
// at an immutable chain: (rule, ranges) entries best first (priority, then
// sequence). A lookup that hits a chain takes its first entry whose ranges
// all hold the packet's unmasked field values, or no match from that tuple.
// Changing any rule of such a key writes a new chain and swaps the value to
// it; the old chain is freed only after a grace period, as rule ids are.
//
// Each tuple keeps the highest priority any of its rules has had (raised
// before a rule becomes visible, never lowered while the tuple lives), and the
// list is ordered by the highest priority any of its rules has had or is being
// prepared with: a transaction's rules raise the ceiling only at commit, which
// cannot republish, so the order is settled when they are prepared. A lookup
// skips a tuple once every packet of the batch already holds a match of
// strictly higher priority: a rule there cannot win (D-098). The order only
// decides how often that happens; the skip itself reads the ceiling.
//
// A lookup masks the batch once per tuple, looks it up, and keeps the best
// rule per packet: higher priority, then the later command (sequence). Adding
// the same (mask, value) again replaces the rule, and the replacement counts
// as the later command -- the semantics WildcardMatch promises.
//
// Rule records are write-once. Changing a rule writes a new record under a
// new id and swaps the table value to it atomically; the old id is recycled
// only after a grace period on the runtime RcuDomain, because a reader may
// have loaded it just before the swap (D-013: a shared table needs a grace
// period before memory is reused). Deleting a rule retires its id the same
// way. Records live in fixed chunks that are never moved or freed while the
// table lives, so a reader's record load is a plain indexed load.
//
// One writer (the caller serializes); lock-free readers that are RcuDomain
// readers (workers are). Values and masks must be canonical: value & ~mask
// == 0.
class ConcurrentMaskedTable {
 public:
  using RuleId = uint32_t;  // 1-based; 0 is never a rule

  struct Rule {
    int64_t priority;
    uint64_t sequence;  // command order: ties go to the later command
    uint16_t gate;      // the output gate a matching packet leaves on
    // The rule's result record id, 0 for none (D-172). It lives
    // in the record, not in the packed value: bits 32-47 of the packed value
    // keep carrying the gate, so a lookup that needs no result reads no
    // record. Only a module with result fields reads one.
    uint32_t result_id;
  };

  // An inclusive range on one field of the key (D-186): the `width` bytes at
  // `offset`, read as a big-endian unsigned integer, lie in [low, high]. A
  // rule's ranges are sorted by offset and do not overlap; the mask is zero
  // under each, and a range that covers its field's whole domain is no range
  // (refused: the rule without it is the same rule).
  struct KeyRange {
    uint32_t offset;
    uint32_t width;  // 1..8 bytes
    uint64_t low;
    uint64_t high;
    bool operator==(const KeyRange &) const = default;
  };
  using Ranges = std::span<const KeyRange>;
  // MaskedRuleResource keys carry ranges as these bytes: no padding.
  static_assert(sizeof(KeyRange) == 24);

 private:
  // One rule of a chain: its packed value (as a table value would hold it)
  // and its ranges, a slice of the chain's range array.
  struct ChainEntry {
    uint64_t packed;
    uint32_t first_range;
    uint32_t range_count;
  };
  // An immutable chain (write-once once published): one allocation holding
  // the header, `entry_capacity` entries and `range_capacity` ranges, so a
  // short chain is a cache line or two. Allocated to a capacity by the
  // writer before it is needed (a commit fills a spare and cannot allocate).
  struct alignas(8) Chain {
    uint32_t count;  // entries, best first
    uint32_t range_count;
    uint32_t entry_capacity;
    uint32_t range_capacity;

    // Frees what New allocated: one raw ::operator new block holding the
    // header and its trailing arrays (the header is trivially destructible).
    struct Free {
      void operator()(Chain *chain) const noexcept { ::operator delete(chain); }
    };
    static std::unique_ptr<Chain, Free> New(uint32_t entries, uint32_t ranges);

    const ChainEntry *entries() const noexcept {
      return reinterpret_cast<const ChainEntry *>(this + 1);
    }
    ChainEntry *entries() noexcept {
      return reinterpret_cast<ChainEntry *>(this + 1);
    }
    const KeyRange *ranges() const noexcept {
      return reinterpret_cast<const KeyRange *>(entries() + entry_capacity);
    }
    KeyRange *ranges() noexcept {
      return reinterpret_cast<KeyRange *>(entries() + entry_capacity);
    }
    Ranges RangesOf(const ChainEntry &e) const noexcept {
      return Ranges(ranges() + e.first_range, e.range_count);
    }
  };
  using ChainPtr = std::unique_ptr<Chain, Chain::Free>;
  // The writer's view of one table key with prepared, uncommitted changes:
  // how many, what the upserts among them add, and one spare chain per
  // change once the key can hold a chain, each large enough for any order
  // in which the changes commit.
  struct KeyState {
    size_t changes = 0;
    size_t upserts = 0;
    size_t upsert_ranges = 0;
    size_t ranged_upserts = 0;
    std::vector<ChainPtr> spares;
  };
  using KeyStates = std::map<std::vector<std::byte>, KeyState>;

 public:
  // One lookup result: the winning rule's gate (out of the packed value) and
  // the rule's id, so a caller that needs the record can load it.
  struct Match {
    uint16_t gate;
    uint32_t rule_id;
  };

  enum class UpsertResult : uint8_t {
    kInserted,
    kUpdated,
    kTooManyTuples,  // a new mask beyond max_tuples
    kFull,           // out of rule ids, or a tuple table could not grow
    kNotCanonical,
    kReservedResult,  // kPendingResult is not a rule result
    kBadRange,        // a range that is not canonical (see KeyRange)
  };

  // The result a rule being prepared by a transaction carries (D-024): it
  // never wins against another match, and alone reads as a miss.
  static constexpr uint16_t kPendingResult = 0xFFFF;

  static constexpr size_t kMaxBatch = 64;

  static std::expected<std::unique_ptr<ConcurrentMaskedTable>, std::string>
  Create(uint32_t key_len, size_t max_tuples, rcu::RcuDomain &domain);

  ~ConcurrentMaskedTable();

  ConcurrentMaskedTable(const ConcurrentMaskedTable &) = delete;
  ConcurrentMaskedTable &operator=(const ConcurrentMaskedTable &) = delete;

  // -- writer ------------------------------------------------------------------

  UpsertResult Upsert(ConstBytes mask, ConstBytes value, int64_t priority,
                      uint16_t gate, uint32_t result_id = 0,
                      Ranges ranges = {});
  // False if no rule has exactly this (mask, value, ranges).
  bool Erase(ConstBytes mask, ConstBytes value, Ranges ranges = {});
  // Removes every rule and every tuple.
  void Clear();

  // -- transactional writer (D-024) ----------------------------------------
  //
  // One rule's change split for the transaction engine. Prepare*() does
  // everything that can fail -- a new tuple, a tuple table's growth, a rule
  // id and its record -- and leaves every lookup's answer unchanged: a new
  // rule is inserted as a pending entry (pointing at a record that loses to
  // every rule, with kPendingResult), a replacement's record is written but
  // not yet named. Commit() makes the change visible with one in-place table
  // store or delete and cannot fail; it returns the id no longer named (0 if
  // none), which the caller hands to Recycle() once a grace period has
  // passed. Cancel() undoes a Prepare*() that will not be committed. Rules
  // prepared together must have distinct (mask, value, ranges); several may
  // share a (mask, value), and each commit builds that key's chain from what
  // is live at the time, so any commit order (and cancelling any of them)
  // ends in the same table.
  struct Prepared {
    std::vector<std::byte> mask;
    std::vector<std::byte> value;
    std::vector<KeyRange> ranges;
    RuleId id = 0;        // the new record (upsert); 0 for an erase
    RuleId old = 0;       // the rule replaced or erased; 0 for a new rule
    uint16_t gate = 0;
    uint32_t result_id = 0;
    KeyStates::iterator key;  // the writer's pending state for the key
  };
  std::expected<Prepared, UpsertResult> PrepareUpsert(ConstBytes mask,
                                                      ConstBytes value,
                                                      int64_t priority,
                                                      uint16_t gate,
                                                      uint32_t result_id = 0,
                                                      Ranges ranges = {});
  // nullopt if no rule has exactly this (mask, value, ranges).
  std::optional<Prepared> PrepareErase(ConstBytes mask, ConstBytes value,
                                       Ranges ranges = {});
  RuleId Commit(const Prepared &prepared) noexcept;
  void Cancel(const Prepared &prepared) noexcept;
  // Returns a committed-away id for reuse, and drops tuples a commit or a
  // cancel left empty (neither can republish the tuple list: it allocates).
  void Recycle(RuleId id);

  // The committed rule with exactly this (mask, value, ranges), if any.
  std::optional<Rule> FindRule(ConstBytes mask, ConstBytes value,
                               Ranges ranges = {}) const;

  // The record a Match names (D-141). Records live in fixed chunks that are
  // never moved or freed while the table lives, so this is a plain indexed
  // load -- the one a module with result fields makes per winner.
  const Rule &RuleAt(RuleId id) const noexcept { return Record(id); }

  // Visits every rule: fn(ConstBytes mask, ConstBytes value, const Rule &,
  // Ranges). A key with prepared new rules only (D-024) is visited once,
  // with the pending record.
  template <typename Fn>
  void ForEach(Fn &&fn) const {
    for (const Tuple &tuple : tuples_.Read()->tuples) {
      const ConstBytes mask(tuple.mask.data(), tuple.mask.size());
      tuple.table->ForEach([&](ConstBytes value, uint64_t packed) {
        if (!IsChain(packed)) {
          fn(mask, value, Record(IdOf(packed)), Ranges{});
          return;
        }
        const Chain &chain = ChainOf(packed);
        for (uint32_t e = 0; e < chain.count; e++) {
          const ChainEntry &entry = chain.entries()[e];
          fn(mask, value, Record(IdOf(entry.packed)), chain.RangesOf(entry));
        }
      });
    }
  }

  size_t size() const noexcept { return size_; }
  size_t tuple_count() const noexcept { return tuples_.Read()->tuples.size(); }
  size_t max_tuples() const noexcept { return max_tuples_; }
  uint32_t key_len() const noexcept { return key_len_; }
  // Rule ids retired but not yet past their grace period.
  size_t retiring_ids() const noexcept { return records_.retiring(); }
  // Chains replaced but not yet past their grace period.
  size_t retiring_chains() const noexcept { return retired_chains_.size(); }
  // Rule ids not free: live rules, retiring ids, prepared records and the
  // pending record.
  size_t ids_in_use() const noexcept { return records_.in_use(); }

  // -- reader -------------------------------------------------------------------

  // Classifies `n` (<= kMaxBatch) keys packed at `stride` bytes. Writes
  // matches[i].gate and sets bit i for each key some rule matches; others are
  // left untouched. With `with_rule_id` (D-141), also writes the winning
  // rule's id -- what a module with result fields reads the record with;
  // a gate-mode lookup does not store it, so it pays nothing for the field.
  uint64_t LookupBatch(ConstBytes keys, size_t stride, Match *matches, size_t n,
                       bool with_rule_id = true) const noexcept {
    promise(n <= kMaxBatch);
    const TupleList *list = tuples_.Read();
    if (n == 0 || list->tuples.empty()) {
      return 0;
    }
    std::array<std::byte, kMaxBatch * detail::kMaskedMaxKeyBytes> scratch;
    std::array<uint64_t, kMaxBatch> ids;
    std::array<uint64_t, kMaxBatch> best;  // packed table value
    std::array<int64_t, kMaxBatch> best_priority;
    const size_t scratch_bytes = n * key_len_;
    const uint64_t all = n == 64 ? ~uint64_t{0} : (uint64_t{1} << n) - 1;
    uint64_t matched = 0;
    for (const Tuple &tuple : list->tuples) {
      if (matched == all) {
        // Every packet holds a match; a rule here wins only with a priority
        // at least as high as the lowest of them (equal: the later command).
        const int64_t ceiling = tuple.ceiling->value.load(std::memory_order_acquire);
        int64_t lowest = best_priority[0];
        for (size_t i = 1; i < n; i++) {
          lowest = std::min(lowest, best_priority[i]);
        }
        if (lowest > ceiling) {
          continue;
        }
      }
      mask_batch_(keys, stride, ConstBytes(tuple.mask.data(), key_len_), n,
                  MutableBytes(scratch).first(scratch_bytes));
      const uint64_t hits = tuple.table->LookupBatch(
          ConstBytes(scratch).first(scratch_bytes), key_len_, ids.data(), n);
      for (uint64_t m = hits; m != 0; m &= m - 1) {
        const size_t i = static_cast<size_t>(__builtin_ctzll(m));
        const uint64_t bit = uint64_t{1} << i;
        uint64_t hit = ids[i];
        if (IsChain(hit)) {
          // The key's first rule whose ranges hold this packet, read from
          // its unmasked key.
          hit = Resolve(ChainOf(hit), keys.data() + i * stride);
          if (hit == 0) {
            continue;
          }
        }
        if ((matched & bit) == 0) {
          best[i] = hit;
          best_priority[i] = PriorityOf(hit);
          matched |= bit;
        } else if (Better(hit, best[i])) {
          best[i] = hit;
          best_priority[i] = PriorityOf(hit);
        }
      }
    }
    for (uint64_t m = matched; m != 0; m &= m - 1) {
      const size_t i = static_cast<size_t>(__builtin_ctzll(m));
      const auto gate = static_cast<uint16_t>(best[i] >> 32);
      // The best match is a rule a transaction is preparing only when
      // nothing else matched (its record loses to every rule): a miss.
      if (gate == kPendingResult) {
        matched &= ~(uint64_t{1} << i);
      } else {
        matches[i].gate = gate;
        if (with_rule_id) {
          matches[i].rule_id = IdOf(best[i]);
        }
      }
    }
    return matched;
  }

 private:
  // Shared by every list that holds the tuple.
  struct Ceiling {
    explicit Ceiling(int64_t priority) : value(priority), order(priority) {}
    // The highest priority any rule of this mask has had (the lookup's skip).
    std::atomic<int64_t> value;
    // At least `value`: also counts rules being prepared. The list's sort
    // key; the writer's alone (readers never load it).
    int64_t order;
  };
  struct Tuple {
    std::vector<std::byte> mask;
    std::shared_ptr<ConcurrentExactTable> table;
    std::shared_ptr<Ceiling> ceiling;
  };
  struct TupleList {
    std::vector<Tuple> tuples;
  };
  ConcurrentMaskedTable(uint32_t key_len, size_t max_tuples,
                        rcu::RcuDomain &domain);

  static RuleId IdOf(uint64_t value) noexcept {
    return static_cast<RuleId>(value);
  }
  // Bits 48-63: the priority biased by 2^15 when it is in [-32767, 32766],
  // else 0 (rank from the record). 0xFFFF marks a chain.
  static constexpr int64_t kPackedPriorityLow = -32767;
  static constexpr int64_t kPackedPriorityHigh = 32766;
  static uint64_t PackedPriority(int64_t priority) noexcept {
    return priority >= kPackedPriorityLow && priority <= kPackedPriorityHigh
               ? static_cast<uint64_t>(priority + 32768)
               : 0;
  }
  // The rule's record must be written (it is, before a value names it).
  uint64_t Pack(RuleId id, uint16_t result) const noexcept {
    return uint64_t{id} | (uint64_t{result} << 32) |
           (PackedPriority(Record(id).priority) << 48);
  }
  int64_t PriorityOf(uint64_t packed) const noexcept {
    const uint64_t biased = packed >> 48;
    return biased != 0 ? static_cast<int64_t>(biased) - 32768 : Record(IdOf(packed)).priority;
  }

  // A chain's table value: its address (below 2^48, checked when the chain
  // is allocated) under the 0xFFFF tag no packed priority reaches.
  static constexpr uint64_t kChainTag = uint64_t{0xFFFF} << 48;
  static bool IsChain(uint64_t value) noexcept {
    return (value & kChainTag) == kChainTag;
  }
  static const Chain &ChainOf(uint64_t value) noexcept {
    // A tagged pointer: the table's 64-bit value is the only slot the
    // lookup reads, and an id-indexed chain table would add a dependent
    // load to every ranged hit. The address came from ChainValue.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    return *reinterpret_cast<const Chain *>(
        static_cast<uintptr_t>(value & ~kChainTag));
  }
  static uint64_t ChainValue(const Chain *chain) noexcept {
    return reinterpret_cast<uintptr_t>(chain) | kChainTag;
  }
  // The field a range names, big-endian, out of an unmasked key.
  static uint64_t FieldOf(const std::byte *key, const KeyRange &range) noexcept {
    uint64_t v = 0;
    for (uint32_t b = 0; b < range.width; b++) {
      v = (v << 8) | static_cast<uint8_t>(key[range.offset + b]);
    }
    return v;
  }
  // The packed value of `chain`'s first entry whose ranges all hold `key`,
  // or 0.
  static uint64_t Resolve(const Chain &chain, const std::byte *key) noexcept {
    // The count first, with acquire (paired with BuildChain's release).
    const uint32_t count = __atomic_load_n(&chain.count, __ATOMIC_ACQUIRE);
    const ChainEntry *entries = chain.entries();
    const KeyRange *ranges = chain.ranges();
    for (uint32_t e = 0; e < count; e++) {
      const KeyRange *r = ranges + entries[e].first_range;
      const KeyRange *end = r + entries[e].range_count;
      for (; r != end; r++) {
        const uint64_t v = FieldOf(key, *r);
        if (v < r->low || v > r->high) {
          break;
        }
      }
      if (r == end) {
        return entries[e].packed;
      }
    }
    return 0;
  }

  static bool Better(const Rule &candidate, const Rule &incumbent) noexcept {
    return candidate.priority != incumbent.priority
               ? candidate.priority > incumbent.priority
               : candidate.sequence > incumbent.sequence;
  }
  // Packed values: by their packed priorities when both are and differ, else
  // by the records.
  bool Better(uint64_t candidate, uint64_t incumbent) const noexcept {
    const uint64_t c = candidate >> 48, i = incumbent >> 48;
    if (c != 0 && i != 0 && c != i) {
      return c > i;
    }
    return Better(Record(IdOf(candidate)), Record(IdOf(incumbent)));
  }
  // Raises `tuple`'s ceiling to `priority` (writer, before the rule it is for
  // becomes visible).
  static void RaiseCeiling(std::atomic<int64_t> &ceiling, int64_t priority) noexcept {
    if (priority > ceiling.load(std::memory_order_relaxed)) {
      ceiling.store(priority, std::memory_order_release);
    }
  }

  const Rule &Record(RuleId id) const noexcept {
    return *reinterpret_cast<const Rule *>(records_.Lookup(id).data());
  }
  Rule &MutableRecord(RuleId id) {
    return *reinterpret_cast<Rule *>(records_.Record(id).data());
  }

  // Writer helpers.
  std::shared_ptr<ConcurrentExactTable> NewTupleTable(size_t rules) const;
  // A copy of `from` at least twice its capacity; nullptr on failure.
  std::shared_ptr<ConcurrentExactTable> Grown(
      const ConcurrentExactTable &from) const;
  // Publishes `list`, highest order first, and reclaims what readers are done
  // with.
  void PublishTuples(std::unique_ptr<TupleList> list);
  // Raises the order of `list`'s tuple `index` to `priority`, republishing
  // the list if that moves the tuple.
  void RaiseOrder(const TupleList &list, size_t index, int64_t priority);
  // The raw table value stored for `value`, if any.
  static std::optional<uint64_t> FindValue(const ConcurrentExactTable &table,
                                           ConstBytes value);
  // The id of the committed rule with exactly `ranges` in table value
  // `packed` (one rule, a chain, or a pending entry), or 0.
  RuleId IdentityIn(uint64_t packed, Ranges ranges) const noexcept;
  // Rules and ranges a table value holds (a pending entry holds none).
  uint32_t RulesIn(uint64_t packed) const noexcept;
  static uint32_t RangesIn(uint64_t packed) noexcept;
  // Whether `ranges` are canonical for `mask` (see KeyRange).
  bool CanonicalRanges(ConstBytes mask, Ranges ranges) const noexcept;
  // The index of the tuple with this mask in `list`, or list size.
  size_t TupleIndex(const TupleList &list, ConstBytes mask) const;
  // Republishes the tuple list without tuples that have no entries (left by
  // transactional erases, which cannot republish).
  void DropEmptyTuples();

  // Pending-key bookkeeping (prepare side): the key's state, with spare
  // chains for every change once the key can hold a chain (`live` is its
  // current table value, 0 when absent), sized for the change being
  // prepared too. Nothing is counted until Track() succeeds.
  KeyStates::iterator PrepareKey(ConstBytes mask, ConstBytes value,
                                 uint64_t live, size_t new_ranges, bool upsert);
  // Counts a prepared change against its key (cannot fail).
  void Track(KeyStates::iterator key, size_t new_ranges, bool upsert) noexcept;
  // Drops a key no change counts any more (a failed prepare's, or the last
  // change's after its commit or cancel).
  void Untrack(KeyStates::iterator key) noexcept;
  // Un-counts a committed or cancelled change, and frees spares beyond the
  // changes left.
  void Settle(const Prepared &prepared) noexcept;
  // Writes into `out` the rules of table value `live` without `drop` and
  // with `add` (its packed value and ranges; 0 for none) in rank order.
  void BuildChain(uint64_t live, RuleId drop, uint64_t add, Ranges add_ranges,
                  Chain &out) const noexcept;
  // Hands a replaced chain to the grace period (capacity reserved by its
  // prepare: cannot allocate).
  void RetireChain(const Chain *chain) noexcept;
  // Frees retired chains whose grace period completed.
  void ReclaimChains() noexcept;
  // Upsert/Erase through Prepare + Commit, for keys that hold or will hold
  // a chain.
  UpsertResult UpsertViaCommit(ConstBytes mask, ConstBytes value,
                               int64_t priority, uint16_t gate,
                               uint32_t result_id, Ranges ranges);

  const uint32_t key_len_;
  const size_t max_tuples_;
  rcu::RcuDomain &domain_;
  detail::MaskBatchFn mask_batch_;
  rcu::RcuPtr<TupleList> tuples_;
  // Rule records live here, addressed by RuleId (write-once; grace-period
  // recycling). A reader's load is an acquire load of the chunk base.
  RecordArena records_;
  uint64_t next_sequence_ = 1;
  // The record every pending entry names (allocated on first use).
  RuleId pending_id_ = 0;
  size_t size_ = 0;
  // Keys with prepared, uncommitted changes (writer only).
  KeyStates pending_keys_;
  // Prepared changes outstanding over every key. Each commit retires at most
  // one chain, so retired_chains_ keeps this much spare capacity: a commit
  // then queues without allocating.
  size_t prepared_changes_ = 0;
  struct RetiredChain {
    ChainPtr chain;
    rcu::GracePeriod token;
  };
  std::vector<RetiredChain> retired_chains_;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_CONCURRENT_MASKED_H_
