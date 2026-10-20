// SPDX-License-Identifier: BSD-3-Clause

// Fuzzes the dataplane transaction RPC's server side apart from gRPC:
// control::DataplaneTransactions::Apply on serialized ApplyTransactionRequest
// messages, decoded through framework::TypedCodec bindings and applied by a
// standalone dataplane::TransactionEngine (its own RcuDomain, no reader
// registered, so every grace period is complete at once; no EAL).
//
// The engine holds std::map-backed fake resources, bound as modules bind
// theirs (codecs mirror modules/meter.cc, router.cc, exact_match.cc):
//   "meters"    MeterIdKey -> MeterPolicyValue;
//   "next_hops" RouterNextHopIdKey -> RouterNextHopValue; referenced by
//               routes, defers erases (Retirer::RemoveLater);
//   "routes"    RouterRouteKey -> RouterRouteValue; declares references to
//               "next_hops" and to "groups", which never registers (a value
//               naming a group is an undeclared reference);
//   "rules"     ExactMatchRuleKey (fields of 4 and 2 bytes) ->
//               ExactMatchRuleValue; provides scope-snapshot consistency;
//   "hidden"    registered but not bound: unreachable over the RPC.
//
// Input: u64 daemon epoch, then up to 32 records of [u8 ctl] and, unless
// ctl % 4 == 1, a u16-length-prefixed serialized ApplyTransactionRequest
// (unparsable bytes are skipped). ctl % 4 == 1 applies the previous request
// again (idempotent replay). All requests go to one DataplaneTransactions.
//
// Oracle: a reference model of the request semantics documented in
// control/dataplane_transactions.h and dataplane/transaction_engine.h --
// committed state (std::map per resource), engine generation, and the
// request_id record window (keyed by the deterministic serialization of the
// request without its id). For every request the model predicts the typed
// error (unknown consistency level; request_id reused for other contents;
// consistency a resource cannot provide) or the record: outcome, generation,
// per-operation statuses (which operation failed: undecodable, duplicate
// key, erase of a missing key, undeclared reference, dangling reference),
// visibility, request_id, replayed flag. After every request each resource's
// committed state equals the model's -- the request's effect iff APPLIED,
// unchanged otherwise (atomicity) -- and so do the generation, the engine's
// reference ledger, the record count and Get(request_id), and each bound
// resource's read-back (ResourceCodec::Entries) decodes through its codec to
// exactly the committed state. List() is checked at the end.

#include <any>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <google/protobuf/any.pb.h>
#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <google/protobuf/util/message_differencer.h>

#include "control/control_error.h"
#include "control/dataplane_transactions.h"
#include "dataplane/resource.h"
#include "dataplane/transaction_engine.h"
#include "framework/resource_bindings.h"
#include "framework/resource_codec.h"
#include "fuzz/fuzz_support.h"
#include "fuzz/protobuf_quiet.h"
#include "gate.h"
#include "pb/control_v2.pb.h"
#include "pb/module_msg.pb.h"
#include "rcu/rcu_domain.h"
#include "utils/ip.h"

namespace {

namespace v2 = bess::pb::v2;
using bess::control::ControlErrorCode;
using bess::control::DataplaneTransactions;
using bess::dataplane::Consistency;
using bess::dataplane::EncodeKey;
using bess::dataplane::Op;
using bess::dataplane::OpKind;
using bess::dataplane::Reference;
using bess::dataplane::Resource;
using bess::dataplane::ResourceKey;
using bess::dataplane::Retirer;
using bess::dataplane::StagedOp;
using bess::dataplane::TransactionEngine;
using bess::fuzz::FuzzInput;
using google::protobuf::util::MessageDifferencer;

// What every fake resource stores: the decoded value.
struct Val {
  uint32_t next_hop = 0;  // routes: a reference to "next_hops" if non-zero
  uint32_t group = 0;     // routes: a reference to "groups" if non-zero
  std::string blob;       // meters and next hops: the value; rules: the gate
  friend bool operator==(const Val &, const Val &) = default;
};

template <typename T>
std::string Bytes(const T &v) {
  return std::string(reinterpret_cast<const char *>(&v), sizeof(v));
}

using State = std::map<ResourceKey, Val>;

// -- conversion functions (the modules' codec lambdas) ------------------------

using KeyResult = std::expected<ResourceKey, std::string>;
using ValueResult = std::expected<Val, std::string>;

KeyResult MeterKey(const bess::pb::MeterIdKey &key) {
  if (key.id() == 0) {
    return std::unexpected("meter id 0 is invalid");
  }
  return EncodeKey(uint32_t{key.id()});
}
ValueResult MeterValue(const bess::pb::MeterPolicyValue &value) {
  if (value.profile_case() == bess::pb::MeterPolicyValue::PROFILE_NOT_SET) {
    return std::unexpected("meter policy has no profile set");
  }
  return Val{0, 0, value.SerializeAsString()};
}
KeyResult NextHopKey(const bess::pb::RouterNextHopIdKey &key) {
  if (key.id() == 0) {
    return std::unexpected("next hop id 0 is invalid");
  }
  return EncodeKey(uint32_t{key.id()});
}
ValueResult NextHopValue(const bess::pb::RouterNextHopValue &value) {
  if (!bess::IsValidGateValue(value.egress_gate())) {
    return std::unexpected("invalid egress gate " +
                           std::to_string(value.egress_gate()));
  }
  return Val{0, 0, value.SerializeAsString()};
}
KeyResult RouteKey(const bess::pb::RouterRouteKey &key) {
  bess::utils::be32_t addr;
  if (!bess::utils::ParseIpv4Address(key.ipv4(), &addr)) {
    return std::unexpected("invalid IPv4 prefix address '" + key.ipv4() + "'");
  }
  if (key.prefix_length() > 32) {
    return std::unexpected("invalid prefix length");
  }
  if (key.domain() >= 16) {
    return std::unexpected("invalid route domain");
  }
  return Bytes(addr.value()) + Bytes(key.prefix_length()) + Bytes(key.domain());
}
ValueResult RouteValue(const bess::pb::RouterRouteValue &value) {
  // Ids are one-based: a route names exactly one of the two, never both.
  if (value.next_hop_id() != 0 && value.group_id() != 0) {
    return std::unexpected("a route names a next hop or a group, not both");
  }
  if (value.next_hop_id() != 0) {
    return Val{value.next_hop_id(), 0, {}};
  }
  if (value.group_id() != 0) {
    return Val{0, value.group_id(), {}};
  }
  return std::unexpected("a route names a next hop or a group");
}
constexpr size_t kFieldSizes[] = {4, 2};
KeyResult RuleKey(const bess::pb::ExactMatchRuleKey &key) {
  if (key.fields_size() != static_cast<int>(std::size(kFieldSizes))) {
    return std::unexpected("rule should have 2 fields");
  }
  ResourceKey packed;
  for (int i = 0; i < key.fields_size(); i++) {
    const bess::pb::FieldData &f = key.fields(i);
    if (f.encoding_case() == bess::pb::FieldData::kValueBin) {
      if (f.value_bin().size() != kFieldSizes[i]) {
        return std::unexpected("field has the wrong size");
      }
      packed += f.value_bin();
    } else {
      uint64_t v = f.value_int();
      for (size_t j = 0; j < kFieldSizes[i]; j++) {
        packed.push_back(static_cast<char>(v & 0xFF));
        v >>= 8;
      }
    }
  }
  return packed;
}
ValueResult RuleValue(const bess::pb::ExactMatchRuleValue &value) {
  if (!bess::IsValidGateValue(value.gate())) {
    return std::unexpected("invalid gate");
  }
  if (!value.result().empty()) {
    return std::unexpected("result is " +
                           std::to_string(value.result().size()) +
                           " bytes, the schema's record is 0");
  }
  const uint64_t gate = value.gate();
  return Val{0, 0, std::string(reinterpret_cast<const char *>(&gate),
                               sizeof(gate))};
}

// The read-back (the modules' entries functions): a committed entry as the
// messages a client sends for it.
uint32_t IdOf(const ResourceKey &key) {
  uint32_t id = 0;
  FUZZ_CHECK(bess::dataplane::DecodeKey(key, &id));
  return id;
}
void MeterRead(const ResourceKey &key, const Val &v, bess::pb::MeterIdKey *k,
               bess::pb::MeterPolicyValue *m) {
  k->set_id(IdOf(key));
  FUZZ_CHECK(m->ParseFromString(v.blob));
}
void NextHopRead(const ResourceKey &key, const Val &v,
                 bess::pb::RouterNextHopIdKey *k,
                 bess::pb::RouterNextHopValue *m) {
  k->set_id(IdOf(key));
  FUZZ_CHECK(m->ParseFromString(v.blob));
}
void RouteRead(const ResourceKey &key, const Val &v,
               bess::pb::RouterRouteKey *k, bess::pb::RouterRouteValue *m) {
  uint32_t addr = 0;
  uint32_t prefix = 0;
  uint32_t domain = 0;
  FUZZ_CHECK(key.size() == sizeof(addr) + sizeof(prefix) + sizeof(domain));
  std::memcpy(&addr, key.data(), sizeof(addr));
  std::memcpy(&prefix, key.data() + sizeof(addr), sizeof(prefix));
  std::memcpy(&domain, key.data() + sizeof(addr) + sizeof(prefix),
              sizeof(domain));
  k->set_ipv4(bess::utils::ToIpv4Address(bess::utils::be32_t(addr)));
  k->set_prefix_length(prefix);
  k->set_domain(domain);
  if (v.next_hop != 0) {
    m->set_next_hop_id(v.next_hop);
  } else {
    m->set_group_id(v.group);
  }
}
void RuleRead(const ResourceKey &key, const Val &v,
              bess::pb::ExactMatchRuleKey *k, bess::pb::ExactMatchRuleValue *m) {
  size_t at = 0;
  for (const size_t size : kFieldSizes) {
    k->add_fields()->set_value_bin(std::string(key.substr(at, size)));
    at += size;
  }
  uint64_t gate = 0;
  FUZZ_CHECK(v.blob.size() == sizeof(gate));
  std::memcpy(&gate, v.blob.data(), sizeof(gate));
  m->set_gate(gate);
}

// -- the fake resource ----------------------------------------------------------

// A std::map of committed values. Upserts publish a node prepared in
// Reserve() (publication does not allocate) and retire the value they
// replace; erases take the key out at once and, if `defers_erase`, declare a
// (no-op) removal step so the engine runs its removal cascade. No packet
// reader exists here, so "readable until the cascade" is moot.
class MapResource final : public Resource {
 public:
  using Map = std::map<ResourceKey, std::unique_ptr<const Val>>;

  MapResource(std::string name, std::vector<std::string> references,
              bool defers_erase, Consistency consistency)
      : Resource(std::move(name), std::move(references)),
        defers_erase_(defers_erase),
        consistency_(consistency) {}

  size_t LiveCount() const override { return map_.size(); }
  bool Contains(const ResourceKey &key) const override {
    return map_.contains(key);
  }
  bool DefersErase() const override { return defers_erase_; }
  Consistency ProvidedConsistency() const override { return consistency_; }

  std::vector<Reference> ReferencesOf(const ResourceKey &key) const override {
    auto it = map_.find(key);
    return it == map_.end() ? std::vector<Reference>{} : RefsOf(*it->second);
  }
  void VisitReferences(
      const std::function<void(const Reference &)> &visit) const override {
    for (const auto &[key, value] : map_) {
      for (const Reference &ref : RefsOf(*value)) {
        visit(ref);
      }
    }
  }

  std::expected<Reservation, std::string> Reserve(const Op &op) override {
    auto it = map_.find(op.key);
    const bool existed = it != map_.end();
    if (op.kind == OpKind::kErase) {
      if (!existed) {
        return std::unexpected("not found");
      }
      Reservation r{std::make_unique<EraseOp>(*this, op.key), {},
                    defers_erase_ ? Footprint{.removals = 1}
                                  : Footprint{.retires = 1}};
      r.existed = true;
      r.previous_references = RefsOf(*it->second);
      return r;
    }
    const Val *value = std::any_cast<Val>(&op.value);
    if (value == nullptr) {
      return std::unexpected("wrong value type");
    }
    Map staging;
    staging.emplace(op.key, std::make_unique<const Val>(*value));
    Reservation r{std::make_unique<UpsertOp>(*this, staging.extract(op.key)),
                  RefsOf(*value), Footprint{.retires = existed ? 1u : 0u}};
    r.existed = existed;
    if (existed) {
      r.previous_references = RefsOf(*it->second);
    }
    return r;
  }

  // The committed state, for comparison with the model.
  State Snapshot() const {
    State out;
    for (const auto &[key, value] : map_) {
      out.emplace(key, *value);
    }
    return out;
  }

 private:
  std::vector<Reference> RefsOf(const Val &v) const {
    std::vector<Reference> refs;
    if (declared_references().empty()) {
      return refs;
    }
    if (v.next_hop != 0) {
      refs.push_back({"next_hops", EncodeKey(v.next_hop)});
    }
    if (v.group != 0) {
      refs.push_back({"groups", EncodeKey(v.group)});
    }
    return refs;
  }

  class UpsertOp final : public StagedOp {
   public:
    UpsertOp(MapResource &owner, Map::node_type node)
        : owner_(owner), node_(std::move(node)) {}
    void Publish(Retirer &retirer) noexcept override {
      auto it = owner_.map_.find(node_.key());
      if (it == owner_.map_.end()) {
        owner_.map_.insert(std::move(node_));
        return;
      }
      it->second.swap(node_.mapped());
      retirer.Retire(std::move(node_.mapped()));
    }

   private:
    MapResource &owner_;
    Map::node_type node_;
  };

  class EraseOp final : public StagedOp {
   public:
    EraseOp(MapResource &owner, ResourceKey key)
        : owner_(owner), key_(std::move(key)) {}
    void Publish(Retirer &retirer) noexcept override {
      node_ = owner_.map_.extract(key_);
      if (owner_.defers_erase_) {
        retirer.RemoveLater([](Retirer &) {});
      } else {
        retirer.Retire(std::move(node_.mapped()));
      }
    }

   private:
    MapResource &owner_;
    ResourceKey key_;
    Map::node_type node_;
  };

  const bool defers_erase_;
  const Consistency consistency_;
  Map map_;
};

template <typename KeyMsg, typename ValueMsg>
std::shared_ptr<const bess::framework::ResourceCodec> Codec(
    KeyResult (*key_fn)(const KeyMsg &), ValueResult (*value_fn)(const ValueMsg &),
    void (*read_fn)(const ResourceKey &, const Val &, KeyMsg *, ValueMsg *),
    const MapResource &table) {
  return std::make_shared<bess::framework::TypedCodec<KeyMsg, ValueMsg>>(
      key_fn,
      [value_fn](const ValueMsg &m) -> std::expected<std::any, std::string> {
        auto v = value_fn(m);
        if (!v) {
          return std::unexpected(v.error());
        }
        return std::any(std::move(*v));
      },
      [read_fn, &table](const auto &visit) {
        for (const auto &[key, value] : table.Snapshot()) {
          KeyMsg k;
          ValueMsg v;
          read_fn(key, value, &k, &v);
          visit(k, v);
        }
      });
}

// -- the reference model ----------------------------------------------------------

constexpr const char *kMeters = "meters";
constexpr const char *kNextHops = "next_hops";
constexpr const char *kRoutes = "routes";
constexpr const char *kRules = "rules";
constexpr const char *kHidden = "hidden";

// The type_url rule, written independently of TypedCodec: the message is
// named by the text after the url's last '/'; a url without '/' names none.
bool UrlNames(std::string_view url, std::string_view full_name) {
  size_t start = url.size();
  while (start > 0 && url[start - 1] != '/') {
    start--;
  }
  return start != 0 && url.substr(start) == full_name;
}

template <typename Msg>
bool ModelUnpack(const google::protobuf::Any &any, Msg *msg) {
  return UrlNames(any.type_url(), Msg::descriptor()->full_name()) &&
         msg->ParseFromString(any.value());
}

struct ModelOp {
  std::string resource;
  bool erase = false;
  ResourceKey key;
  Val value;
};

template <typename KeyMsg, typename ValueMsg>
bool ModelDecodeTyped(const v2::TransactionOp &op,
                      KeyResult (*key_fn)(const KeyMsg &),
                      ValueResult (*value_fn)(const ValueMsg &), ModelOp *out) {
  KeyMsg key_msg;
  if (!ModelUnpack(op.key(), &key_msg)) {
    return false;
  }
  auto key = key_fn(key_msg);
  if (!key) {
    return false;
  }
  out->resource = op.resource();
  out->key = *key;
  out->erase = op.erase();
  if (op.erase()) {
    return !op.has_value();
  }
  ValueMsg value_msg;
  if (!ModelUnpack(op.value(), &value_msg)) {
    return false;
  }
  auto value = value_fn(value_msg);
  if (!value) {
    return false;
  }
  out->value = *value;
  return true;
}

bool ModelDecode(const v2::TransactionOp &op, ModelOp *out) {
  if (op.resource() == kMeters) {
    return ModelDecodeTyped(op, MeterKey, MeterValue, out);
  }
  if (op.resource() == kNextHops) {
    return ModelDecodeTyped(op, NextHopKey, NextHopValue, out);
  }
  if (op.resource() == kRoutes) {
    return ModelDecodeTyped(op, RouteKey, RouteValue, out);
  }
  if (op.resource() == kRules) {
    return ModelDecodeTyped(op, RuleKey, RuleValue, out);
  }
  return false;  // unknown, or "hidden" (not bound)
}

std::string Contents(const v2::ApplyTransactionRequest &request) {
  v2::ApplyTransactionRequest contents = request;
  contents.clear_request_id();
  std::string bytes;
  {
    google::protobuf::io::StringOutputStream stream(&bytes);
    google::protobuf::io::CodedOutputStream out(&stream);
    out.SetSerializationDeterministic(true);
    FUZZ_CHECK(contents.SerializeToCodedStream(&out));
  }
  return bytes;
}

struct Model {
  std::map<std::string, State> state{{kMeters, {}}, {kNextHops, {}},
                                     {kRoutes, {}}, {kRules, {}},
                                     {kHidden, {}}};
  uint64_t generation = 0;
  struct Recorded {
    std::string contents;
    v2::TransactionRecord record;
  };
  std::map<std::string, Recorded> records;
  std::deque<std::string> order;
  // Retirements (objects and removal steps) the last applied transaction
  // left with the RCU domain and the engine's cascades.
  size_t last_retired = 0;

  // References to next hop `key` held by the routes in `s`.
  static size_t Refs(const std::map<std::string, State> &s,
                     const ResourceKey &key) {
    size_t n = 0;
    for (const auto &[k, v] : s.at(kRoutes)) {
      n += v.next_hop != 0 && EncodeKey(v.next_hop) == key;
    }
    return n;
  }
};

// What the model expects of one request.
struct Expected {
  std::optional<ControlErrorCode> error;
  std::string error_object;
  bool replayed = false;
  v2::TransactionRecord::Outcome outcome = v2::TransactionRecord::OUTCOME_UNSPECIFIED;
  int failed = -1;  // the op that failed, for REJECTED
  bool busy_possible = false;
};

struct Fixture {
  bess::rcu::RcuDomain domain{1};
  MapResource meters{kMeters, {}, true, Consistency::kReferential};
  MapResource next_hops{kNextHops, {}, true, Consistency::kReferential};
  MapResource routes{kRoutes, {kNextHops, "groups"}, true,
                     Consistency::kReferential};
  MapResource rules{kRules, {}, false, Consistency::kScopeSnapshot};
  MapResource hidden{kHidden, {}, false, Consistency::kReferential};
  TransactionEngine engine{domain};
  bess::framework::ResourceBindings bindings;
  bess::framework::ResourceBinding meters_binding;
  bess::framework::ResourceBinding next_hops_binding;
  bess::framework::ResourceBinding routes_binding;
  bess::framework::ResourceBinding rules_binding;
  std::optional<DataplaneTransactions> tx;

  explicit Fixture(uint64_t epoch) {
    FUZZ_CHECK(engine.Register(&meters).has_value());
    FUZZ_CHECK(engine.Register(&next_hops).has_value());
    FUZZ_CHECK(engine.Register(&routes).has_value());
    FUZZ_CHECK(engine.Register(&rules).has_value());
    FUZZ_CHECK(engine.Register(&hidden).has_value());
    meters_binding = bindings.Bind(meters, Codec(MeterKey, MeterValue, MeterRead, meters));
    next_hops_binding =
        bindings.Bind(next_hops,
                      Codec(NextHopKey, NextHopValue, NextHopRead, next_hops));
    routes_binding =
        bindings.Bind(routes, Codec(RouteKey, RouteValue, RouteRead, routes));
    rules_binding = bindings.Bind(rules, Codec(RuleKey, RuleValue, RuleRead, rules));
    tx.emplace(engine, bindings, epoch);
  }
  ~Fixture() {
    tx.reset();
    rules_binding.Reset();
    routes_binding.Reset();
    next_hops_binding.Reset();
    meters_binding.Reset();
  }

  const MapResource &Get(const std::string &name) const {
    return name == kMeters    ? meters
           : name == kNextHops ? next_hops
           : name == kRoutes   ? routes
           : name == kRules    ? rules
                               : hidden;
  }
};

// The model's prediction for `request`; on APPLIED, `after` is the new state.
Expected Predict(const Model &model, const v2::ApplyTransactionRequest &request,
                 const std::string &contents,
                 std::map<std::string, State> *after, size_t *retired) {
  Expected e;
  const int consistency = request.consistency();
  if (consistency != v2::ApplyTransactionRequest::CONSISTENCY_UNSPECIFIED &&
      consistency != v2::ApplyTransactionRequest::CONSISTENCY_REFERENTIAL &&
      consistency != v2::ApplyTransactionRequest::CONSISTENCY_SCOPE_SNAPSHOT) {
    e.error = ControlErrorCode::kInvalidArgument;
    return e;
  }
  const std::string &id = request.request_id();
  if (!id.empty()) {
    if (auto it = model.records.find(id); it != model.records.end()) {
      if (it->second.contents != contents) {
        e.error = ControlErrorCode::kConflict;
        e.error_object = id;
      } else {
        e.replayed = true;
        e.outcome = it->second.record.outcome();
      }
      return e;
    }
  }

  const int n = request.ops_size();
  std::vector<ModelOp> ops(static_cast<size_t>(n));
  for (int i = 0; i < n; i++) {
    if (!ModelDecode(request.ops(i), &ops[i])) {
      e.outcome = v2::TransactionRecord::OUTCOME_REJECTED;
      e.failed = i;
      return e;
    }
  }
  if (request.has_expected_generation() &&
      request.expected_generation() != model.generation) {
    e.outcome = v2::TransactionRecord::OUTCOME_CONFLICT;
    return e;
  }
  if (consistency == v2::ApplyTransactionRequest::CONSISTENCY_SCOPE_SNAPSHOT) {
    for (int i = 0; i < n; i++) {
      if (ops[i].resource != kRules) {
        e.error = ControlErrorCode::kUnsupportedTransaction;
        e.error_object = ops[i].resource;
        return e;
      }
    }
  }
  auto reject = [&](int i) {
    e.outcome = v2::TransactionRecord::OUTCOME_REJECTED;
    e.failed = i;
    return e;
  };
  // One operation per key.
  std::set<std::pair<std::string, ResourceKey>> seen;
  for (int i = 0; i < n; i++) {
    if (!seen.emplace(ops[i].resource, ops[i].key).second) {
      return reject(i);
    }
  }
  // Reserve, in request order.
  size_t may_retire = 0;
  for (int i = 0; i < n; i++) {
    const State &s = model.state.at(ops[i].resource);
    const bool exists = s.contains(ops[i].key);
    if (ops[i].erase) {
      if (!exists) {
        return reject(i);
      }
      may_retire++;
    } else {
      may_retire += exists ? 1 : 0;
      if (ops[i].resource == kRoutes && ops[i].value.group != 0) {
        return reject(i);  // "groups" is declared but never registered
      }
    }
  }
  constexpr size_t kBudget =
      bess::rcu::RcuDomain::kDefaultRetireHighWater / 2;
  if (may_retire > kBudget) {
    return reject(0);
  }
  // Leftovers of the previous transaction are reclaimed at the start of
  // this one unless they do not fit the budget with it; only then BUSY.
  e.busy_possible = model.last_retired + may_retire > kBudget;

  // References the transaction leaves: every next-hop key named after it must
  // exist after it. The engine reports the lowest such key.
  *after = model.state;
  for (const ModelOp &op : ops) {
    State &s = after->at(op.resource);
    if (op.erase) {
      s.erase(op.key);
    } else {
      s.insert_or_assign(op.key, op.value);
    }
  }
  std::set<ResourceKey> named;
  for (const auto &[k, v] : after->at(kRoutes)) {
    if (v.next_hop != 0) {
      named.insert(EncodeKey(v.next_hop));
    }
  }
  for (const ResourceKey &key : named) {
    if (after->at(kNextHops).contains(key)) {
      continue;
    }
    for (int i = 0; i < n; i++) {
      if (ops[i].resource == kNextHops && ops[i].key == key) {
        return reject(i);  // erases a key still referenced
      }
    }
    for (int i = 0; i < n; i++) {
      if (ops[i].resource == kRoutes && !ops[i].erase &&
          ops[i].value.next_hop != 0 &&
          EncodeKey(ops[i].value.next_hop) == key) {
        return reject(i);  // references a missing key
      }
    }
    FUZZ_CHECK(!"a dangling reference in the committed state");
  }
  *retired = may_retire;
  e.outcome = v2::TransactionRecord::OUTCOME_APPLIED;
  return e;
}

void CheckState(const Fixture &f, const Model &model) {
  for (const auto &[name, state] : model.state) {
    FUZZ_CHECK(f.Get(name).Snapshot() == state);
  }
  FUZZ_CHECK(f.engine.generation() == model.generation);
  for (const auto &[key, value] : model.state.at(kNextHops)) {
    FUZZ_CHECK(f.engine.ReferenceCount(kNextHops, key) ==
               Model::Refs(model.state, key));
  }
  FUZZ_CHECK(f.tx->records() == model.records.size());
  // Read-back (D-129): each bound resource's entries decode, through its own
  // codec, to exactly its committed state.
  for (const char *name : {kMeters, kNextHops, kRoutes, kRules}) {
    const bess::framework::ResourceCodec *codec = f.bindings.Find(f.Get(name));
    FUZZ_CHECK(codec != nullptr);
    State read;
    codec->Entries([&](const google::protobuf::Message &key,
                       const google::protobuf::Message &value) {
      google::protobuf::Any k, v;
      FUZZ_CHECK(k.PackFrom(key) && v.PackFrom(value));
      auto decoded_key = codec->Key(k.type_url(), k.value());
      auto decoded_value = codec->Value(v.type_url(), v.value());
      FUZZ_CHECK(decoded_key.has_value() && decoded_value.has_value());
      const Val *val = std::any_cast<Val>(&*decoded_value);
      FUZZ_CHECK(val != nullptr);
      FUZZ_CHECK(read.emplace(std::move(*decoded_key), *val).second);
    });
    FUZZ_CHECK(read == f.Get(name).Snapshot());
  }
}

void ApplyOne(Fixture &f, Model &model, uint64_t epoch,
              const v2::ApplyTransactionRequest &request) {
  const std::string contents = Contents(request);
  std::map<std::string, State> after;
  size_t retired = 0;
  const Expected e = Predict(model, request, contents, &after, &retired);
  const auto result = f.tx->Apply(request);
  const std::string &id = request.request_id();

  if (e.error) {
    FUZZ_CHECK(!result.has_value());
    FUZZ_CHECK(result.error().code == *e.error);
    if (*e.error != ControlErrorCode::kInvalidArgument) {
      FUZZ_CHECK(result.error().object == e.error_object);
    } else {
      FUZZ_CHECK(result.error().field == "consistency");
    }
    CheckState(f, model);
    return;
  }
  FUZZ_CHECK(result.has_value());
  const v2::ApplyTransactionResponse &response = *result;
  FUZZ_CHECK(response.daemon_epoch() == epoch);
  FUZZ_CHECK(response.replayed() == e.replayed);
  const v2::TransactionRecord &record = response.record();
  if (e.replayed) {
    FUZZ_CHECK(MessageDifferencer::Equals(record, model.records.at(id).record));
    CheckState(f, model);
    return;
  }

  const int n = request.ops_size();
  FUZZ_CHECK(record.request_id() == id);
  FUZZ_CHECK(record.visibility() ==
             (request.consistency() ==
                      v2::ApplyTransactionRequest::CONSISTENCY_SCOPE_SNAPSHOT
                  ? v2::TransactionRecord::VISIBILITY_SCOPE_SNAPSHOT
                  : v2::TransactionRecord::VISIBILITY_DEPENDENCY_ORDERED));
  FUZZ_CHECK(record.ops_size() == n);
  const bool busy = record.outcome() == v2::TransactionRecord::OUTCOME_BUSY &&
                    e.outcome == v2::TransactionRecord::OUTCOME_APPLIED &&
                    e.busy_possible;
  const v2::TransactionRecord::Outcome outcome =
      busy ? v2::TransactionRecord::OUTCOME_BUSY : e.outcome;
  FUZZ_CHECK(record.outcome() == outcome);
  for (int i = 0; i < n; i++) {
    const auto status = record.ops(i).status();
    if (outcome == v2::TransactionRecord::OUTCOME_APPLIED) {
      FUZZ_CHECK(status == v2::TransactionOpResult::STATUS_APPLIED);
    } else if (i == e.failed) {
      FUZZ_CHECK(status == v2::TransactionOpResult::STATUS_FAILED);
      FUZZ_CHECK(!record.ops(i).error().empty());
    } else {
      FUZZ_CHECK(status == v2::TransactionOpResult::STATUS_NOT_APPLIED);
    }
  }
  if (outcome == v2::TransactionRecord::OUTCOME_APPLIED) {
    model.state = std::move(after);
    model.generation++;
    model.last_retired = retired;
  }
  FUZZ_CHECK(record.generation() == model.generation);
  const bool attempted = outcome == v2::TransactionRecord::OUTCOME_APPLIED ||
                         outcome == v2::TransactionRecord::OUTCOME_REJECTED;
  if (!id.empty() && attempted) {
    if (model.order.size() == DataplaneTransactions::kMaxRecords) {
      model.records.erase(model.order.front());
      model.order.pop_front();
    }
    model.records.emplace(id, Model::Recorded{contents, record});
    model.order.push_back(id);
  }
  CheckState(f, model);
  const v2::GetTransactionResponse got = f.tx->Get(id);
  FUZZ_CHECK(got.daemon_epoch() == epoch);
  auto known = model.records.find(id);
  FUZZ_CHECK(got.known() == (known != model.records.end()));
  if (got.known()) {
    FUZZ_CHECK(MessageDifferencer::Equals(got.record(), known->second.record));
  }
}

void CheckList(const Fixture &f, const Model &model, uint64_t epoch) {
  const v2::ListTransactionResourcesResponse list = f.tx->List();
  FUZZ_CHECK(list.daemon_epoch() == epoch);
  FUZZ_CHECK(list.generation() == model.generation);
  // Sorted by name; "hidden" is not bound.
  const struct {
    const char *name;
    const google::protobuf::Descriptor *key;
    const google::protobuf::Descriptor *value;
  } want[] = {
      {kMeters, bess::pb::MeterIdKey::descriptor(),
       bess::pb::MeterPolicyValue::descriptor()},
      {kNextHops, bess::pb::RouterNextHopIdKey::descriptor(),
       bess::pb::RouterNextHopValue::descriptor()},
      {kRoutes, bess::pb::RouterRouteKey::descriptor(),
       bess::pb::RouterRouteValue::descriptor()},
      {kRules, bess::pb::ExactMatchRuleKey::descriptor(),
       bess::pb::ExactMatchRuleValue::descriptor()},
  };
  FUZZ_CHECK(list.resources_size() == static_cast<int>(std::size(want)));
  for (int i = 0; i < list.resources_size(); i++) {
    FUZZ_CHECK(list.resources(i).name() == want[i].name);
    FUZZ_CHECK(list.resources(i).key_type() == want[i].key->full_name());
    FUZZ_CHECK(list.resources(i).value_type() == want[i].value->full_name());
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static const bool quiet = [] {
    bess::fuzz::QuietProtobufLogs();
    return true;
  }();
  (void)quiet;
  FuzzInput in(data, size);
  const uint64_t epoch = in.U64();
  Fixture f(epoch);
  FUZZ_CHECK(f.tx->epoch() == epoch);
  Model model;
  std::optional<v2::ApplyTransactionRequest> previous;
  for (int records = 0; records < 32 && !in.empty(); records++) {
    const uint8_t ctl = in.U8();
    if (ctl % 4 != 1) {
      const std::span<const uint8_t> bytes = in.Blob();
      v2::ApplyTransactionRequest request;
      if (!request.ParseFromArray(bytes.data(), static_cast<int>(bytes.size()))) {
        continue;
      }
      previous = std::move(request);
    }
    if (previous) {
      ApplyOne(f, model, epoch, *previous);
    }
  }
  CheckList(f, model, epoch);
  return 0;
}
