// SPDX-License-Identifier: BSD-3-Clause

#include "modules/conntrack.h"

#include <span>

#include "event.h"
#include "modules/symmetric_inputs.h"
#include "packet_parse.h"
#include "utils/format.h"
#include "utils/time.h"

namespace {

namespace ct = bess::conntrack;

constexpr size_t kExpireBudget = 256;  // wheel work per batch
constexpr unsigned kGranularityShift = 24;  // about 17 ms

// The wire status is the library's, plus one (0 is unspecified, D-182).
constexpr uint8_t WireStatus(ct::TrackStatus s) { return static_cast<uint8_t>(s) + 1; }
static_assert(WireStatus(ct::TrackStatus::kNew) == bess::pb::CONNTRACK_STATUS_NEW);
static_assert(WireStatus(ct::TrackStatus::kExisting) == bess::pb::CONNTRACK_STATUS_EXISTING);
static_assert(WireStatus(ct::TrackStatus::kRelated) == bess::pb::CONNTRACK_STATUS_RELATED);
static_assert(WireStatus(ct::TrackStatus::kInvalid) == bess::pb::CONNTRACK_STATUS_INVALID);
static_assert(WireStatus(ct::TrackStatus::kUntracked) == bess::pb::CONNTRACK_STATUS_UNTRACKED);
static_assert(WireStatus(ct::TrackStatus::kFull) == bess::pb::CONNTRACK_STATUS_FULL);

}  // namespace

const Commands ConnTrack::cmds = {};

std::unique_ptr<ConnTrack::Ct> ConnTrack::MakeTable() const {
  auto made = Ct::Create(capacity_, policy_, tsc_to_ns(rdtsc()), kGranularityShift);
  return made ? std::move(*made) : nullptr;
}

ModuleCapabilities ConnTrack::Capabilities(const bess::pb::ConnTrackArg &arg) {
  using State = ModuleCapabilities::State;
  using Affinity = ModuleCapabilities::FlowAffinity;
  switch (arg.mode()) {
    case bess::pb::ConnTrackArg::SHARED:
      return {State::kShared, Affinity::kNone};
    case bess::pb::ConnTrackArg::PER_WORKER:
      return {State::kPerWorker,
              arg.fallback_shared() ? Affinity::kPreferred : Affinity::kRequired};
    default:
      return {State::kSingleWorker, Affinity::kNone};
  }
}

CommandResponse ConnTrack::Init(const bess::pb::ConnTrackArg &arg) {
  table_full_ = bess::stats::EventThrottle(init_context().events(), "bess.table_full", name());
  mode_ = arg.mode();
  capacity_ = arg.capacity() != 0 ? arg.capacity() : 65536;
  ct::TimeoutPolicy policy;
  policy.tcp_pickup = arg.tcp_pickup();
  policy_ = policy.Scaled(1000ull * 1000 * 1000);  // the packet clock is ns
  fallback_shared_ = arg.fallback_shared();
  status_attr_name_ = arg.status_attr();
  if (!status_attr_name_.empty()) {
    status_attr_ = AddMetadataAttr(status_attr_name_, sizeof(uint8_t),
                                   bess::metadata::Attribute::AccessMode::kWrite);
    if (status_attr_ < 0) {
      return CommandFailure(-status_attr_, "add_metadata_attr() failed");
    }
  }
  if (mode_ == bess::pb::ConnTrackArg::SHARED || fallback_shared_) {
    auto made = SharedCt::Create(capacity_, init_context().rcu(), policy_, tsc_to_ns(rdtsc()),
                                 kGranularityShift);
    if (!made) {
      return CommandFailure(ENOMEM, "cannot create the connection table");
    }
    shared_ = std::move(*made);
    if (mode_ == bess::pb::ConnTrackArg::SHARED) {
      max_allowed_workers_ = Worker::kMaxWorkers;
      return CommandSuccess();
    }
  }
  if (mode_ == bess::pb::ConnTrackArg::PER_WORKER) {
    // Tables come with the workers, before resume; until checked, closed.
    max_allowed_workers_ = Worker::kMaxWorkers;
    refused_ = "not yet checked";
    return CommandSuccess();
  }
  tables_[0] = MakeTable();
  if (tables_[0] == nullptr) {
    return CommandFailure(ENOMEM, "cannot create the connection table");
  }
  return CommandSuccess();
}

// Before workers resume (the graph and the workers that run this module are
// settled): check the inputs, and give every worker that runs the module its
// own table.
int ConnTrack::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  // Offsets are assigned just before this: a configured status attribute
  // promises a reader (D-182).
  if (status_attr_ >= 0 && !bess::metadata::IsValidOffset(StatusOffset())) {
    LOG(WARNING) << "ConnTrack '" << name() << "': nothing reads '" << status_attr_name_
                 << "' (a module downstream reading it would); packets carry no status";
  }
  if (mode_ != bess::pb::ConnTrackArg::PER_WORKER) {
    return 0;
  }
  refused_ = bess::modules::AsymmetricInputs(*this);
  for (bess::dataplane::WorkerId wid : bess::dataplane::AllWorkerIds()) {
    if (refused_) {
      break;
    }
    if (active_workers()[wid.value()] && tables_[wid.value()] == nullptr) {
      tables_[wid.value()] = MakeTable();
      if (tables_[wid.value()] == nullptr) {
        refused_ = bess::utils::Format("no memory for worker %u's table",
                                       unsigned{wid.value()});
      }
    }
  }
  if (refused_) {
    LOG(ERROR) << "ConnTrack '" << name() << "' (PER_WORKER): " << *refused_
               << (fallback_shared_ ? "; tracking with one shared table"
                                    : "; every packet goes to ogate 1");
  }
  return 0;
}

void ConnTrack::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  if (mode_ == bess::pb::ConnTrackArg::SHARED) {
    Track(*shared_, ctx, batch);
    return;
  }
  Ct *table = mode_ == bess::pb::ConnTrackArg::PER_WORKER
                  ? tables_[ctx->wid.value()].get()
                  : tables_[0].get();
  if (refused_ || table == nullptr) [[unlikely]] {
    if (fallback_shared_) {
      Track(*shared_, ctx, batch);
    } else {
      // Fail closed: not tracked, and marked so (D-182).
      const bess::metadata::mt_offset_t status = StatusOffset();
      if (bess::metadata::IsValidOffset(status)) {
        for (bess::PacketRef pkt : batch->packets()) {
          set_attr_with_offset<uint8_t>(status, pkt, bess::pb::CONNTRACK_STATUS_UNTRACKED);
        }
      }
      RunChooseModule(ctx, 1, batch);
    }
    return;
  }
  Track(*table, ctx, batch);
}

template <typename T>
void ConnTrack::Track(T &table, Context *ctx, bess::PacketBatch *batch) {
  const int cnt = batch->cnt();
  const uint64_t now = ctx->current_ns;
  table.Expire(now, kExpireBudget);
  const bool may_create = ctx->current_igate == 0;
  std::span<const uint8_t> frames[bess::PacketBatch::kMaxBurst];
  bess::packet::ParsedFrame parsed[bess::PacketBatch::kMaxBurst];
  typename T::Result results[bess::PacketBatch::kMaxBurst];
  for (int i = 0; i < cnt; i++) {
    bess::PacketRef pkt = batch->packet(i);
    frames[i] = std::span<const uint8_t>(pkt.head_data<const uint8_t *>(), pkt.head_len());
    if (bess::packet::ParseFrame(frames[i], parsed[i], pkt.total_len()) !=
        bess::packet::ParseStatus::kOk) {
      parsed[i] = bess::packet::ParsedFrame{};  // unkeyed: untracked
    }
  }
  table.TrackBatch(std::span(frames, cnt), std::span(parsed, cnt), now, std::span(results, cnt), 0,
                   may_create);
  uint64_t full = 0;
  const bess::metadata::mt_offset_t status = StatusOffset();
  const bool write_status = bess::metadata::IsValidOffset(status);
  for (int i = 0; i < cnt; i++) {
    const ct::TrackStatus s = results[i].status;
    const bool pass = s == ct::TrackStatus::kNew || s == ct::TrackStatus::kExisting ||
                      s == ct::TrackStatus::kRelated;
    full += s == ct::TrackStatus::kFull;
    if (write_status) {
      set_attr_with_offset<uint8_t>(status, batch->packet(i), WireStatus(s));
    }
    EmitPacket(ctx, batch->packet(i), pass ? 0 : 1);
  }
  if (full != 0) [[unlikely]] {
    table_full_.Note(ctx->wid, now, full);
  }
}

std::string ConnTrack::GetDesc() const {
  size_t n = shared_ != nullptr ? shared_->size() : 0;
  for (const auto &t : tables_) {
    n += t != nullptr ? t->size() : 0;
  }
  if (refused_ && mode_ == bess::pb::ConnTrackArg::PER_WORKER) {
    return bess::utils::Format("%s: %s (%zu connections)",
                               fallback_shared_ ? "shared" : "closed", refused_->c_str(), n);
  }
  return bess::utils::Format("%zu connections", n);
}

ADD_MODULE(ConnTrack, "conntrack",
           "Connection tracking: tracked connections to ogate 0, the rest to ogate 1")
