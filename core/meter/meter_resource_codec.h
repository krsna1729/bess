// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_METER_METER_RESOURCE_CODEC_H_
#define BESS_METER_METER_RESOURCE_CODEC_H_

#include <any>
#include <array>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "dataplane/resource.h"
#include "framework/resource_codec.h"
#include "meter/meter.h"
#include "meter/meter_resource.h"
#include "pb/module_msg.pb.h"

// BESS's wire form of a MeterResource (D-199): key bess.pb.MeterIdKey, value
// bess.pb.MeterPolicyValue, as the `Meter` module's "<module>/meters" has
// always spoken it (D-129). Header-only, so bess_meter itself stays free of
// protobuf and the framework; a module or plugin binds it with
//
//   binding_ = init_context().codecs().Bind(
//       *resource, bess::meter::MakeMeterResourceCodec(*resource));

namespace bess::meter {

// The wire policy as K5's profile specification (D-025).
inline std::expected<MeterProfileSpec, std::string> MeterPolicyFromPb(
    const bess::pb::MeterPolicyValue &value) {
  switch (value.profile_case()) {
    case bess::pb::MeterPolicyValue::kSrTcm:
      return MeterProfileSpec{SrTcmSpec{value.sr_tcm().committed_rate(),
                                        value.sr_tcm().committed_burst(),
                                        value.sr_tcm().excess_burst()}};
    case bess::pb::MeterPolicyValue::kTrTcm:
      return MeterProfileSpec{TrTcmSpec{
          value.tr_tcm().committed_rate(), value.tr_tcm().committed_burst(),
          value.tr_tcm().peak_rate(), value.tr_tcm().peak_burst()}};
    case bess::pb::MeterPolicyValue::kTrTcmRfc4115:
      return MeterProfileSpec{TrTcmRfc4115Spec{
          value.tr_tcm_rfc4115().committed_rate(),
          value.tr_tcm_rfc4115().committed_burst(),
          value.tr_tcm_rfc4115().excess_rate(),
          value.tr_tcm_rfc4115().excess_burst()}};
    case bess::pb::MeterPolicyValue::PROFILE_NOT_SET:
      break;
  }
  return std::unexpected("meter policy has no profile set");
}

// MeterPolicyFromPb's inverse: the message a client sends for `spec` (D-129's
// read-back; every field is a scalar, so the form is unique).
inline bess::pb::MeterPolicyValue MeterPolicyToPb(const MeterProfileSpec &spec) {
  bess::pb::MeterPolicyValue value;
  if (const auto *sr = std::get_if<SrTcmSpec>(&spec)) {
    auto *pb = value.mutable_sr_tcm();
    pb->set_committed_rate(sr->committed_rate);
    pb->set_committed_burst(sr->committed_burst);
    pb->set_excess_burst(sr->excess_burst);
  } else if (const auto *tr = std::get_if<TrTcmSpec>(&spec)) {
    auto *pb = value.mutable_tr_tcm();
    pb->set_committed_rate(tr->committed_rate);
    pb->set_committed_burst(tr->committed_burst);
    pb->set_peak_rate(tr->peak_rate);
    pb->set_peak_burst(tr->peak_burst);
  } else if (const auto *rfc = std::get_if<TrTcmRfc4115Spec>(&spec)) {
    auto *pb = value.mutable_tr_tcm_rfc4115();
    pb->set_committed_rate(rfc->committed_rate);
    pb->set_committed_burst(rfc->committed_burst);
    pb->set_excess_rate(rfc->excess_rate);
    pb->set_excess_burst(rfc->excess_burst);
  }
  return value;
}

// The codec for `resource`, which must outlive the binding (reset the binding
// first, D-044). Entries are the committed, non-retiring meters.
inline std::shared_ptr<const framework::ResourceCodec> MakeMeterResourceCodec(
    const MeterResource &resource) {
  return std::make_shared<
      framework::TypedCodec<bess::pb::MeterIdKey, bess::pb::MeterPolicyValue>>(
      [](const bess::pb::MeterIdKey &key)
          -> std::expected<dataplane::ResourceKey, std::string> {
        if (key.id() == 0) {
          return std::unexpected("meter id 0 is invalid");
        }
        return dataplane::EncodeKey(MeterId(key.id()));
      },
      [](const bess::pb::MeterPolicyValue &value)
          -> std::expected<std::any, std::string> {
        auto policy = MeterPolicyFromPb(value);
        if (!policy) {
          return std::unexpected(policy.error());
        }
        return std::any(*policy);
      },
      // Resumable by meter id (the cursor is the next id).
      [&resource](std::string_view cursor, const auto &visit) -> std::string {
        std::array<uint32_t, 1> from{};
        (void)framework::CursorFields(cursor, &from);
        const std::optional<MeterId> next = resource.VisitPoliciesFrom(
            MeterId(from[0]), [&visit](MeterId id, const MeterProfileSpec &policy) {
              bess::pb::MeterIdKey key;
              key.set_id(id.value());
              return visit(key, MeterPolicyToPb(policy));
            });
        return next ? framework::CursorOf<1>({next->value()}) : std::string();
      });
}

}  // namespace bess::meter

#endif  // BESS_METER_METER_RESOURCE_CODEC_H_
