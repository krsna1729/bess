// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_PACKET_STORE_CODEC_H_
#define BESS_FRAMEWORK_PACKET_STORE_CODEC_H_

#include <any>
#include <array>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

#include "dataplane/packet_store.h"
#include "framework/resource_codec.h"
#include "pb/module_msg.pb.h"

namespace bess {
namespace dataplane {

// The wire codec of a dataplane::PacketStoreFlowsResource (D-201), in BESS's
// own messages: bess.pb.PacketStoreFlowKey{id} ->
// bess.pb.PacketStoreFlowValue{timeout_ns?, max_packets_per_flow?}. Flow id
// 0 is refused (it is every packet without a key); readback reports each
// entry with exactly the fields it set. `published` is the owner's RcuPtr,
// which must outlive the binding.
inline std::shared_ptr<framework::ResourceCodec> MakePacketStoreFlowsCodec(
    const rcu::RcuPtr<PacketStoreLimitsMap> &published) {
  return std::make_shared<framework::TypedCodec<bess::pb::PacketStoreFlowKey,
                                                bess::pb::PacketStoreFlowValue>>(
      [](const bess::pb::PacketStoreFlowKey &key)
          -> std::expected<dataplane::ResourceKey, std::string> {
        // The flow id is the metadata value the classifier wrote, and 0 is
        // every packet that has none: all keyless sessions share that bucket,
        // so a per-session entry for it would apply one session's limits to
        // all of them.
        if (key.id() == 0) {
          return std::unexpected("flow id 0 is every keyless packet");
        }
        return dataplane::EncodeKey(key.id());
      },
      [](const bess::pb::PacketStoreFlowValue &value)
          -> std::expected<std::any, std::string> {
        // Field presence is the point: an absent field leaves the store-wide
        // number in force, and a present 0 is a decision.
        dataplane::PacketStoreLimits limits;
        if (value.has_timeout_ns()) {
          limits.timeout_ns = value.timeout_ns();
        }
        if (value.has_max_packets_per_flow()) {
          limits.max_packets_per_flow = value.max_packets_per_flow();
        }
        return std::any(limits);
      },
      // Read back as given: an entry carries exactly the fields it set.
      // Resumable by flow id (the cursor is the next id).
      [&published](std::string_view cursor, const auto &visit) -> std::string {
        const dataplane::PacketStoreLimitsMap *limits = published.Read();
        if (limits == nullptr) {
          return {};
        }
        std::array<uint32_t, 1> from{};
        (void)framework::CursorFields(cursor, &from);
        for (auto it = limits->lower_bound(from[0]); it != limits->end(); ++it) {
          bess::pb::PacketStoreFlowKey key;
          key.set_id(it->first);
          bess::pb::PacketStoreFlowValue value;
          if (it->second.timeout_ns.has_value()) {
            value.set_timeout_ns(*it->second.timeout_ns);
          }
          if (it->second.max_packets_per_flow.has_value()) {
            value.set_max_packets_per_flow(*it->second.max_packets_per_flow);
          }
          if (!visit(key, value)) {
            const auto next = std::next(it);
            return next == limits->end() ? std::string() : framework::CursorOf<1>({next->first});
          }
        }
        return {};
      });
}

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_FRAMEWORK_PACKET_STORE_CODEC_H_
