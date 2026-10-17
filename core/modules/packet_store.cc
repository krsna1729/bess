// SPDX-License-Identifier: BSD-3-Clause

#include "packet_store.h"

#include "utils/format.h"
#include "utils/time.h"

namespace {

using bess::utils::be32_t;

}  // namespace

const Commands PacketStore::cmds = {
    {"release", "PacketStoreCommandReleaseArg",
     MODULE_CMD_FUNC(&PacketStore::CommandRelease), Command::THREAD_UNSAFE},
    {"drop", "PacketStoreCommandDropArg",
     MODULE_CMD_FUNC(&PacketStore::CommandDrop), Command::THREAD_UNSAFE},
    {"clear", "PacketStoreCommandClearArg",
     MODULE_CMD_FUNC(&PacketStore::CommandClear), Command::THREAD_UNSAFE},
};

CommandResponse PacketStore::Init(const bess::pb::PacketStoreArg &arg) {
  using AccessMode = bess::metadata::Attribute::AccessMode;

  max_packets_ = arg.max_packets() ? arg.max_packets() : 4096;
  max_packets_per_flow_ =
      arg.max_packets_per_flow() ? arg.max_packets_per_flow() : 64;
  timeout_ns_ = arg.timeout_ms()
                    ? static_cast<uint64_t>(arg.timeout_ms()) * 1'000'000ULL
                    : 5'000'000'000ULL;

  task_id_t tid = RegisterTask(nullptr);
  if (tid == INVALID_TASK_ID) {
    return CommandFailure(ENOMEM, "Task creation failed");
  }

  // The flow key is the first of these the graph provides; the default pair is
  // the historical behaviour, store_id over vif_id.
  if (arg.key_attrs().empty()) {
    key_attrs_ = {"store_id", "vif_id"};
  } else {
    key_attrs_.assign(arg.key_attrs().begin(), arg.key_attrs().end());
  }
  key_attr_ids_.clear();
  for (const std::string &name : key_attrs_) {
    const int attr_id = AddMetadataAttr(name, sizeof(be32_t), AccessMode::kRead);
    if (attr_id < 0) {
      // Without this the id is an error code, and the packet path would index
      // attr_offsets_ with it: a duplicate, an empty name, more than
      // kMaxAttrsPerModule names, or a size clash with another module.
      return CommandFailure(-attr_id, "add_metadata_attr(%s) failed",
                            name.c_str());
    }
    key_attr_ids_.push_back(attr_id);
  }

  return CommandSuccess();
}

void PacketStore::DeInit() {
  for (auto &[id, queue] : store_) {
    while (!queue.empty()) {
      bess::PacketFree(queue.front().pkt.handle());
      queue.pop_front();
    }
  }
  while (!release_queue_.empty()) {
    bess::PacketFree(release_queue_.front().pkt.handle());
    release_queue_.pop_front();
  }
  store_.clear();
  total_packets_ = 0;
}

CommandResponse PacketStore::CommandRelease(
    const bess::pb::PacketStoreCommandReleaseArg &arg) {
  auto it = store_.find(arg.id());
  if (it == store_.end()) {
    return CommandFailure(ENOENT, "flow id %u not found in packet store",
                          arg.id());
  }

  const gate_idx_t out_gate = static_cast<gate_idx_t>(arg.gate());
  std::deque<StoredPacket> &queue = it->second;

  while (!queue.empty()) {
    release_queue_.push_back(ReleasedPacket{
        .pkt = queue.front().pkt,
        .gate = out_gate,
    });
    queue.pop_front();
    total_packets_--;
  }

  store_.erase(it);
  return CommandSuccess();
}

CommandResponse PacketStore::CommandDrop(
    const bess::pb::PacketStoreCommandDropArg &arg) {
  auto it = store_.find(arg.id());
  if (it == store_.end()) {
    return CommandFailure(ENOENT, "flow id %u not found in packet store",
                          arg.id());
  }

  std::deque<StoredPacket> &queue = it->second;
  while (!queue.empty()) {
    bess::PacketFree(queue.front().pkt.handle());
    queue.pop_front();
    total_packets_--;
  }

  store_.erase(it);
  return CommandSuccess();
}
struct task_result PacketStore::RunTask(Context *ctx, bess::PacketBatch *batch,
                                       void *) {
  if (release_queue_.empty()) {
    return {.block = true, .packets = 0, .bits = 0};
  }

  const gate_idx_t out_gate = release_queue_.front().gate;
  int count = 0;
  batch->clear();

  while (!release_queue_.empty() &&
         release_queue_.front().gate == out_gate &&
         count < static_cast<int>(bess::PacketBatch::kMaxBurst)) {
    batch->add(release_queue_.front().pkt);
    release_queue_.pop_front();
    count++;
  }

  if (count > 0) {
    RunChooseModule(ctx, out_gate, batch);
    return {.block = false,
            .packets = static_cast<uint32_t>(count),
            .bits = static_cast<uint64_t>(count * 64 * 8)};
  }

  return {.block = true, .packets = 0, .bits = 0};
}


CommandResponse PacketStore::CommandClear(
    const bess::pb::PacketStoreCommandClearArg &) {
  DeInit();
  return CommandSuccess();
}

void PacketStore::EvictExpired(uint64_t now_ns) {
  for (auto it = store_.begin(); it != store_.end();) {
    auto &queue = it->second;
    while (!queue.empty() && (now_ns - queue.front().timestamp_ns > timeout_ns_)) {
      bess::PacketFree(queue.front().pkt.handle());
      queue.pop_front();
      total_packets_--;
    }
    if (queue.empty()) {
      it = store_.erase(it);
    } else {
      ++it;
    }
  }
}

void PacketStore::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  while (!release_queue_.empty() && ctx && ctx->task) {
    bess::PacketBatch *release_batch = ctx->task->AllocPacketBatch();
    if (!release_batch) {
      break;
    }
    RunTask(ctx, release_batch, nullptr);
  }
  const uint64_t now_ns = tsc_to_ns(current_worker.current_tsc());

  // Which of the configured names is the key is fixed when the graph is
  // compiled, so it is decided once per run: the first one this graph provides.
  bess::metadata::mt_offset_t key_offset = bess::metadata::kMetadataOffsetNoRead;
  for (const int attr_id : key_attr_ids_) {
    const bess::metadata::mt_offset_t offset = attr_offset(attr_id);
    if (bess::metadata::IsValidOffset(offset)) {
      key_offset = offset;
      break;
    }
  }

  // Evict expired packets if store is growing
  if (total_packets_ >= max_packets_) {
    EvictExpired(now_ns);
  }

  for (bess::PacketRef pkt : batch->packets()) {
    uint32_t flow_id = 0;
    if (bess::metadata::IsValidOffset(key_offset)) {
      flow_id = get_attr_with_offset<be32_t>(key_offset, pkt).value();
    }

    auto &queue = store_[flow_id];

    // Check per-flow limit
    if (queue.size() >= max_packets_per_flow_) {
      // Evict oldest packet in this flow to gate 1
      bess::PacketRef oldest = queue.front().pkt;
      queue.pop_front();
      total_packets_--;
      EmitPacket(ctx, oldest, 1);
    }

    // Check global limit
    if (total_packets_ >= max_packets_) {
      // Global overflow -> drop to gate 1
      EmitPacket(ctx, pkt, 1);
      continue;
    }

    // Buffer packet
    queue.push_back(StoredPacket{
        .pkt = pkt,
        .timestamp_ns = now_ns,
    });
    total_packets_++;
  }
}

std::string PacketStore::GetDesc() const {
  return bess::utils::Format("%zu packets stored across %zu flows (max %u)",
                             total_packets_, store_.size(), max_packets_);
}

ADD_MODULE(PacketStore, "packet_store",
           "Bounded, ownership-safe packet buffer for paging and pending queues")
