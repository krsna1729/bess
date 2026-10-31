// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_FRAMEWORK_PACKET_STORE_CODEC_H_
#define BESS_FRAMEWORK_PACKET_STORE_CODEC_H_

#include <any>
#include <expected>
#include <memory>
#include <string>

#include "dataplane/packet_store.h"
#include "framework/resource_codec.h"
#include "pb/module_msg.pb.h"

namespace bess {
namespace dataplane {

// The wire codec of a dataplane::PacketStoreFlowsResource (D-201), in BESS's
// own messages: bess.pb.PacketStoreFlowKey{id} ->
// bess.pb.PacketStoreFlowValue{timeout_ns?, max_packets_per_flow?}. Flow id
// 0 is refused (it is every packet without a key); readback reports each
// entry with exactly the fields it set, in flow id order (a walk of the
// table's capacity). `table` is the owner's, which must outlive the binding.
inline std::shared_ptr<framework::ResourceCodec> MakePacketStoreFlowsCodec(
    const PacketStoreLimitsTable &table) {
  return std::make_shared<framework::TypedCodec<
      bess::pb::PacketStoreFlowKey, bess::pb::PacketStoreFlowValue>>(
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
      [&table](const auto &visit) {
        for (size_t i = 1; i <= table.capacity(); i++) {
          const auto id = static_cast<uint32_t>(i);
          const PacketStoreLimits *found = table.Current(PacketStoreFlowId(id));
          if (found == nullptr) {
            continue;
          }
          const PacketStoreLimits &entry = *found;
          bess::pb::PacketStoreFlowKey key;
          key.set_id(id);
          bess::pb::PacketStoreFlowValue value;
          if (entry.timeout_ns.has_value()) {
            value.set_timeout_ns(*entry.timeout_ns);
          }
          if (entry.max_packets_per_flow.has_value()) {
            value.set_max_packets_per_flow(*entry.max_packets_per_flow);
          }
          visit(key, value);
        }
      });
}

}  // namespace dataplane
}  // namespace bess

#endif  // BESS_FRAMEWORK_PACKET_STORE_CODEC_H_
