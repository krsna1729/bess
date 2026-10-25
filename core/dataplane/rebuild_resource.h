// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_REBUILD_RESOURCE_H_
#define BESS_DATAPLANE_REBUILD_RESOURCE_H_

#include <algorithm>
#include <any>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "dataplane/resource.h"
#include "rcu/rcu_ptr.h"

namespace bess {
namespace dataplane {

// One rule as a build sees it: its id and the rule, owned by the resource for
// the duration of the call.
template <typename Id, typename Rule>
struct RebuildEntry {
  Id id;
  const Rule *rule;
};

// One operation of a transaction on a RebuildResource. An upsert's node holds
// the new rule until the publish moves it in (and then the rule it replaced);
// an erase's node receives the erased rule.
template <typename Id, typename Rule>
struct RebuildChange {
  Id id;
  typename std::map<Id, Rule>::node_type node;
  bool erase;
  bool existed;  // the committed rules held `id` before the transaction
};

// Every rule a build makes the table from, in id order, without a copy: the
// committed rules merged on the fly with a transaction's operations (sorted by
// id), an operation replacing or removing the committed rule with its id.
// Iterated once or more by the build, during the call only.
template <typename Id, typename Rule>
class RebuildRules {
 public:
  using Committed = std::map<Id, Rule>;
  using Change = RebuildChange<Id, Rule>;
  using Entry = RebuildEntry<Id, Rule>;

  class iterator {
   public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = Entry;
    using difference_type = std::ptrdiff_t;
    using pointer = const Entry *;
    using reference = const Entry &;

    iterator() = default;
    iterator(typename Committed::const_iterator committed,
             typename Committed::const_iterator committed_end, const Change *change,
             const Change *change_end)
        : committed_(committed),
          committed_end_(committed_end),
          change_(change),
          change_end_(change_end) {
      Settle();
    }

    reference operator*() const noexcept { return current_; }
    pointer operator->() const noexcept { return &current_; }

    iterator &operator++() {
      if (from_change_) {
        if (committed_ != committed_end_ && committed_->first == change_->id) {
          ++committed_;  // replaced
        }
        ++change_;
      } else {
        ++committed_;
      }
      Settle();
      return *this;
    }
    iterator operator++(int) {
      iterator before = *this;
      ++*this;
      return before;
    }

    friend bool operator==(const iterator &a, const iterator &b) noexcept {
      return a.committed_ == b.committed_ && a.change_ == b.change_;
    }

   private:
    // Moves to the next rule the table holds: skips erases and the committed
    // rules they remove.
    void Settle() {
      while (change_ != change_end_) {
        if (committed_ != committed_end_ && committed_->first < change_->id) {
          break;
        }
        if (!change_->erase) {
          current_ = Entry{change_->id, &change_->node.mapped()};
          from_change_ = true;
          return;
        }
        if (committed_ != committed_end_ && committed_->first == change_->id) {
          ++committed_;
        }
        ++change_;
      }
      if (committed_ != committed_end_) {
        current_ = Entry{committed_->first, &committed_->second};
      }
      from_change_ = false;
    }

    typename Committed::const_iterator committed_{};
    typename Committed::const_iterator committed_end_{};
    const Change *change_ = nullptr;
    const Change *change_end_ = nullptr;
    Entry current_{};
    bool from_change_ = false;
  };

  // `changes` sorted by id, at most one per id; `size` the rules they leave.
  RebuildRules(const Committed &committed, std::span<const Change> changes, size_t size)
      : committed_(committed), changes_(changes), size_(size) {}

  iterator begin() const {
    return iterator(committed_.cbegin(), committed_.cend(), changes_.data(),
                    changes_.data() + changes_.size());
  }
  iterator end() const {
    return iterator(committed_.cend(), committed_.cend(), changes_.data() + changes_.size(),
                    changes_.data() + changes_.size());
  }
  // The number of rules iterated.
  size_t size() const noexcept { return size_; }
  bool empty() const noexcept { return size_ == 0; }

 private:
  const Committed &committed_;
  std::span<const Change> changes_;
  size_t size_;
};

// What builds a table: every rule the table holds, in id order, to the
// immutable table they make, or the reason they make none.
template <typename Build, typename Id, typename Rule, typename Table>
concept RebuildFunction =
    std::is_invocable_r_v<std::expected<std::unique_ptr<const Table>, std::string>,
                          const Build &, const RebuildRules<Id, Rule> &>;

// An immutable table, rebuilt off the packet path once per transaction and
// published whole, exposed as a transactional resource (D-191; the shape
// D-179 gave Firewall): key = EncodeKey(the rule's Id), value = Rule.
//
// For a table that cannot change in place -- a sorted or compiled structure,
// a trie built from its rule set -- next to the in-place adapters
// (classifier/{exact,masked}_rule_resource.h), which change one entry per
// operation in a concurrent table. Here an operation costs one rebuild of the
// whole table per transaction however many operations it carries:
//
//   Reserve()  checks the key and the value and records the operation; an
//              upsert's std::map node is allocated here. The transaction's
//              first operation declares the one retirement (footprint).
//   Prepare()  calls `build` once with the committed rules and the operations
//              merged in id order (RebuildRules, no copy). A refusal refuses
//              the transaction; the published table is untouched.
//   Publish()  the transaction's first staged operation exchanges the
//              RcuPtr, retires the old table (freed a grace period later), and
//              moves the nodes into the committed rules: no allocation. The
//              other operations find nothing left to do.
//
// Workers keep running and see the old table or the new one, never a mix.
// The committed rules (`rules()`, id order) are the control side's: what the
// table was built from and what a readback reports. Their values hold no
// references, and an erase takes effect at once, so nothing may reference
// this resource.
//
// The owner keeps the RcuPtr its packet path reads (one acquire load per
// batch, nothing in between), publishes the first table with Initialize()
// before registering, and declares the table `.Rebuilds()` in its
// ResourceTable. Writers are the engine's: one transaction at a time.
template <typename Id, typename Rule, typename Table, typename Build>
  requires std::is_trivially_copyable_v<Id> && std::totally_ordered<Id> &&
           RebuildFunction<Build, Id, Rule, Table>
class RebuildResource final : public Resource {
 public:
  using Rules = std::map<Id, Rule>;
  using Entry = RebuildEntry<Id, Rule>;
  using View = RebuildRules<Id, Rule>;

  // `published`: the owner's, read by its packet path. `value_name` names the
  // rule type in the refusal of a value of another type ("wrong value type
  // (want <value_name>)").
  RebuildResource(std::string name, rcu::RcuPtr<Table> &published, Build build,
                  std::string value_name)
      : Resource(std::move(name)),
        published_(published),
        build_(std::move(build)),
        value_name_(std::move(value_name)) {}

  // Builds the table `rules` make and publishes it as the first one
  // (RcuPtr::Initialize): before registration, while no reader holds the
  // pointer. The rules become the committed rules.
  std::expected<void, std::string> Initialize(Rules rules) {
    auto built = build_(View(rules, {}, rules.size()));
    if (!built) {
      return std::unexpected(built.error());
    }
    published_.Initialize(std::move(*built));
    rules_ = std::move(rules);
    return {};
  }

  // The committed rules by id: the table's source, and a readback's.
  const Rules &rules() const noexcept { return rules_; }

  size_t LiveCount() const override { return rules_.size(); }

  bool Contains(const ResourceKey &key) const override {
    Id id;
    return DecodeKey(key, &id) && rules_.contains(id);
  }

  std::expected<Reservation, std::string> Reserve(const Op &op) override {
    Id id;
    if (!DecodeKey(op.key, &id)) {
      return std::unexpected("invalid rule id key");
    }
    // The engine refuses a second operation on one key, so the committed
    // rules answer whether this one existed.
    const bool existed = rules_.contains(id);
    Change change{id, {}, op.kind == OpKind::kErase, existed};
    if (change.erase) {
      if (!existed) {
        return std::unexpected("not found");
      }
    } else {
      const Rule *rule = std::any_cast<Rule>(&op.value);
      if (rule == nullptr) {
        return std::unexpected("wrong value type (want " + value_name_ + ")");
      }
      Rules source;
      source.emplace(id, *rule);
      change.node = source.extract(source.begin());
    }
    changes_.push_back(std::move(change));
    // One swap per transaction retires one table: the first operation
    // declares it.
    const uint32_t retires = changes_.size() == 1 ? 1 : 0;
    return Reservation{std::make_unique<SwapOp>(*this),
                       {},
                       {.retires = retires, .removals = 0, .callbacks = 0},
                       existed,
                       {}};
  }

  std::expected<void, std::string> Prepare() override {
    std::sort(changes_.begin(), changes_.end(),
              [](const Change &a, const Change &b) { return a.id < b.id; });
    size_t size = rules_.size();
    for (const Change &c : changes_) {
      if (c.erase) {
        size--;
      } else if (!c.existed) {
        size++;
      }
    }
    auto built = build_(View(rules_, changes_, size));
    if (!built) {
      return std::unexpected(built.error());
    }
    prepared_ = std::move(*built);
    return {};
  }

  void EndTransaction() noexcept override {
    // Frees what the transaction replaced or erased, or what it never
    // published.
    changes_.clear();
    prepared_.reset();
  }

 private:
  using Change = RebuildChange<Id, Rule>;

  // What every operation of a transaction stages: the first to publish swaps
  // the prepared table and the rules in, the rest find nothing left to do.
  class SwapOp final : public StagedOp {
   public:
    explicit SwapOp(RebuildResource &resource) : resource_(resource) {}
    void Publish(Retirer &retirer) noexcept override {
      RebuildResource &r = resource_;
      if (r.prepared_ == nullptr) {
        return;
      }
      retirer.Retire(r.published_.Exchange(std::move(r.prepared_)));
      for (Change &c : r.changes_) {
        if (c.erase) {
          c.node = r.rules_.extract(c.id);
          continue;
        }
        auto it = r.rules_.find(c.id);
        if (it == r.rules_.end()) {
          r.rules_.insert(std::move(c.node));
        } else {
          std::swap(it->second, c.node.mapped());
        }
      }
    }

   private:
    RebuildResource &resource_;
  };

  rcu::RcuPtr<Table> &published_;
  Build build_;
  std::string value_name_;
  Rules rules_;
  // During a transaction: its operations (in id order after Prepare()).
  std::vector<Change> changes_;
  // Built by Prepare(): the table the publish installs.
  std::unique_ptr<const Table> prepared_;
};

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_DATAPLANE_REBUILD_RESOURCE_H_
