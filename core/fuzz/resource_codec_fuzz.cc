// SPDX-License-Identifier: BSD-3-Clause

// Fuzzes framework::TypedCodec (framework/resource_codec.h), the decoder the
// transaction RPC runs on every operation's key and value, instantiated with
// the message types modules bind (ActionTable, Meter, ExactMatch, Router).
// The conversion functions mirror those modules' codec lambdas.
//
// Input: byte 0 selects the codec (bits 0-1: actions, meters, exact-match
// rules, routes) and the side (bit 2: key or value); then a u16-length-
// prefixed type_url; the rest of the input is the serialized message.
//
// Oracle, per call through the ResourceCodec interface:
//   - key_type()/value_type() are the messages' full names;
//   - the call is accepted at the decoding stage iff the reference rule
//     accepts the type_url (the text after its last '/' is the full name; a
//     url without '/' names nothing) and the bytes parse as that message;
//     otherwise it fails with "<key|value> is not a <full name>", without
//     calling the conversion function, and never throws;
//   - when accepted the conversion function runs exactly once, on a message
//     equal (MessageDifferencer) to an independent parse of the bytes, and
//     its result is returned unchanged;
//   - round trip: the decoded message re-serializes to bytes that parse back
//     to an equal message and re-serialize identically, and decoding those
//     bytes packed in a google.protobuf.Any (as a controller sends them)
//     yields the same result.

#include <any>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <absl/log/globals.h>
#include <google/protobuf/any.pb.h>
#include <google/protobuf/util/message_differencer.h>

#include "dataplane/resource.h"
#include "framework/resource_codec.h"
#include "fuzz/fuzz_support.h"
#include "gate.h"
#include "pb/module_msg.pb.h"
#include "utils/ip.h"

namespace {

using bess::dataplane::EncodeKey;
using bess::dataplane::ResourceKey;
using bess::framework::ResourceCodec;
using bess::framework::TypedCodec;
using bess::fuzz::FuzzInput;
using google::protobuf::util::MessageDifferencer;

using KeyResult = std::expected<ResourceKey, std::string>;
using ValueResult = std::expected<std::any, std::string>;

// The rule as the header implements it, written independently: the message
// is named by everything after the url's last '/'; no '/' names nothing.
bool UrlNames(std::string_view url, std::string_view full_name) {
  size_t start = url.size();
  while (start > 0 && url[start - 1] != '/') {
    start--;
  }
  if (start == 0) {
    return false;
  }
  return url.size() - start == full_name.size() &&
         std::memcmp(url.data() + start, full_name.data(), full_name.size()) ==
             0;
}

// Conversion results are compared through a canonical string: keys are bytes
// already; each codec projects its value type.
using Project = std::string (*)(const std::any &);

template <typename T>
std::string Bytes(const T &v) {
  return std::string(reinterpret_cast<const char *>(&v), sizeof(v));
}

bool SameKey(const KeyResult &a, const KeyResult &b, Project) {
  return a.has_value() == b.has_value() &&
         (a ? *a == *b : a.error() == b.error());
}
bool SameValue(const ValueResult &a, const ValueResult &b, Project project) {
  return a.has_value() == b.has_value() &&
         (a ? project(*a) == project(*b) : a.error() == b.error());
}

// ActionTable (modules/action_table.cc).
struct Action {
  uint32_t meter;
  uint32_t next_hop;
};
KeyResult ActionKey(const bess::pb::ActionIdKey &key) {
  if (key.id() == 0) {
    return std::unexpected("action id 0 is invalid");
  }
  return EncodeKey(uint32_t{key.id()});
}
ValueResult ActionVal(const bess::pb::ActionValue &value) {
  return std::any(Action{value.meter_id(), value.next_hop_id()});
}
std::string ProjectAction(const std::any &v) {
  const Action &a = std::any_cast<const Action &>(v);
  return Bytes(a.meter) + Bytes(a.next_hop);
}

// Meter (modules/meter.cc, PolicyFromPb).
struct Policy {
  int kind;
  uint64_t p[4];
};
KeyResult MeterKey(const bess::pb::MeterIdKey &key) {
  if (key.id() == 0) {
    return std::unexpected("meter id 0 is invalid");
  }
  return EncodeKey(uint32_t{key.id()});
}
ValueResult MeterVal(const bess::pb::MeterPolicyValue &value) {
  switch (value.profile_case()) {
    case bess::pb::MeterPolicyValue::kSrTcm:
      return std::any(Policy{1,
                             {value.sr_tcm().committed_rate(),
                              value.sr_tcm().committed_burst(),
                              value.sr_tcm().excess_burst(), 0}});
    case bess::pb::MeterPolicyValue::kTrTcm:
      return std::any(Policy{2,
                             {value.tr_tcm().committed_rate(),
                              value.tr_tcm().committed_burst(),
                              value.tr_tcm().peak_rate(),
                              value.tr_tcm().peak_burst()}});
    case bess::pb::MeterPolicyValue::kTrTcmRfc4115:
      return std::any(Policy{3,
                             {value.tr_tcm_rfc4115().committed_rate(),
                              value.tr_tcm_rfc4115().committed_burst(),
                              value.tr_tcm_rfc4115().excess_rate(),
                              value.tr_tcm_rfc4115().excess_burst()}});
    case bess::pb::MeterPolicyValue::PROFILE_NOT_SET:
      break;
  }
  return std::unexpected("meter policy has no profile set");
}
std::string ProjectPolicy(const std::any &v) {
  const Policy &p = std::any_cast<const Policy &>(v);
  return Bytes(p.kind) + Bytes(p.p);
}

// ExactMatch (modules/exact_match.cc, RuleFieldsFromPb + PackKey) for a
// module configured with a 4-byte and a 2-byte field.
constexpr size_t kFieldSizes[] = {4, 2};
KeyResult ExactKey(const bess::pb::ExactMatchRuleKey &key) {
  if (key.fields_size() != static_cast<int>(std::size(kFieldSizes))) {
    return std::unexpected("rule should have 2 fields");
  }
  ResourceKey packed;
  for (int i = 0; i < key.fields_size(); i++) {
    const bess::pb::FieldData &f = key.fields(i);
    const size_t size = kFieldSizes[i];
    if (f.encoding_case() == bess::pb::FieldData::kValueBin) {
      if (f.value_bin().size() != size) {
        return std::unexpected("field has the wrong size");
      }
      packed += f.value_bin();
    } else {
      uint64_t v = f.value_int();
      for (size_t j = 0; j < size; j++) {
        packed.push_back(static_cast<char>(v & 0xFF));
        v >>= 8;
      }
    }
  }
  return packed;
}
ValueResult ExactVal(const bess::pb::ExactMatchRuleValue &value) {
  if (!bess::IsValidGateValue(value.gate())) {
    return std::unexpected("invalid gate " + std::to_string(value.gate()));
  }
  if (value.action_id() != 0) {
    return std::unexpected("'action_id' needs an 'action_resource'");
  }
  return std::any(uint64_t{value.gate()});
}
std::string ProjectGate(const std::any &v) {
  return Bytes(std::any_cast<uint64_t>(v));
}

// Router routes (modules/router.cc).
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
  return Bytes(addr.value()) + Bytes(key.prefix_length()) +
         Bytes(key.domain());
}
ValueResult RouteVal(const bess::pb::RouterRouteValue &value) {
  if (value.next_hop_id() == 0) {
    return std::unexpected("next hop id 0 is invalid");
  }
  return std::any(uint32_t{value.next_hop_id()});
}
std::string ProjectNextHop(const std::any &v) {
  return Bytes(std::any_cast<uint32_t>(v));
}

// One side (key or value) of one codec: `call` goes through the
// ResourceCodec interface; `seen`/`calls` are what the conversion function
// observed; `direct` is the conversion function called on a message.
template <typename Msg, typename Result, typename Call, typename Direct>
void CheckSide(std::string_view url, const std::string &bytes,
               const char *side, const Call &call, const Direct &direct,
               const Msg &seen, const int &calls,
               bool (*same)(const Result &, const Result &, Project),
               Project project) {
  const std::string full_name(Msg::descriptor()->full_name());
  Result result = std::unexpected("not run");
  try {
    result = call(url, bytes);
  } catch (...) {
    FUZZ_CHECK(!"the codec threw");
  }

  Msg ref;
  const bool accepted = UrlNames(url, full_name) && ref.ParseFromString(bytes);
  if (!accepted) {
    FUZZ_CHECK(!result.has_value());
    FUZZ_CHECK(calls == 0);
    FUZZ_CHECK(result.error() == std::string(side) + " is not a " + full_name);
    return;
  }
  FUZZ_CHECK(calls == 1);
  FUZZ_CHECK(MessageDifferencer::Equals(seen, ref));
  FUZZ_CHECK(same(result, direct(ref), project));

  // Round trip through serialization, then through an Any.
  std::string again;
  FUZZ_CHECK(ref.SerializeToString(&again));
  Msg back;
  FUZZ_CHECK(back.ParseFromString(again));
  FUZZ_CHECK(MessageDifferencer::Equals(back, ref));
  FUZZ_CHECK(back.SerializeAsString() == again);
  google::protobuf::Any any;
  FUZZ_CHECK(any.PackFrom(back));
  Result packed = call(any.type_url(), any.value());
  FUZZ_CHECK(calls == 2);
  FUZZ_CHECK(MessageDifferencer::Equals(seen, ref));
  FUZZ_CHECK(same(packed, result, project));
}

template <typename KeyMsg, typename ValueMsg>
void Run(KeyResult (*key_fn)(const KeyMsg &),
         ValueResult (*value_fn)(const ValueMsg &), Project project,
         bool value_side, std::string_view url, const std::string &bytes) {
  KeyMsg seen_key;
  ValueMsg seen_value;
  int calls = 0;
  const TypedCodec<KeyMsg, ValueMsg> typed(
      [&](const KeyMsg &m) {
        calls++;
        seen_key = m;
        return key_fn(m);
      },
      [&](const ValueMsg &m) {
        calls++;
        seen_value = m;
        return value_fn(m);
      });
  const ResourceCodec &codec = typed;
  FUZZ_CHECK(codec.key_type() == KeyMsg::descriptor()->full_name());
  FUZZ_CHECK(codec.value_type() == ValueMsg::descriptor()->full_name());

  if (value_side) {
    CheckSide<ValueMsg, ValueResult>(
        url, bytes, "value",
        [&](std::string_view u, const std::string &b) {
          return codec.Value(u, b);
        },
        value_fn, seen_value, calls, SameValue, project);
  } else {
    CheckSide<KeyMsg, KeyResult>(
        url, bytes, "key",
        [&](std::string_view u, const std::string &b) {
          return codec.Key(u, b);
        },
        key_fn, seen_key, calls, SameKey, project);
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static const bool quiet = [] {
    // Malformed input is expected; protobuf's per-parse error logs are noise.
    absl::SetMinLogLevel(absl::LogSeverityAtLeast::kInfinity);
    return true;
  }();
  (void)quiet;
  FuzzInput in(data, size);
  const uint8_t selector = in.U8();
  const bool value_side = (selector & 4) != 0;
  // The url in its own allocation, so a read past its end is reported.
  const std::span<const uint8_t> url_bytes = in.Blob();
  const std::vector<char> url_buf(url_bytes.begin(), url_bytes.end());
  const std::string_view url(url_buf.data(), url_buf.size());
  const std::span<const uint8_t> rest = in.Rest();
  const std::string bytes(rest.begin(), rest.end());

  switch (selector & 3) {
    case 0:
      Run(ActionKey, ActionVal, ProjectAction, value_side, url, bytes);
      break;
    case 1:
      Run(MeterKey, MeterVal, ProjectPolicy, value_side, url, bytes);
      break;
    case 2:
      Run(ExactKey, ExactVal, ProjectGate, value_side, url, bytes);
      break;
    default:
      Run(RouteKey, RouteVal, ProjectNextHop, value_side, url, bytes);
      break;
  }
  return 0;
}
