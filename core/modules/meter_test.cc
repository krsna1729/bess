// SPDX-License-Identifier: BSD-3-Clause

// Meter's `get_status` action: read-only inspection of the per-meter profile,
// token buckets and last update. The action is THREAD_SAFE, so the tests drive
// it both on an idle meter and while a batch path updates the same meter's
// tokens, and check that a reconfigured meter reports its new profile.

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "framework/resource_codec.h"
#include "metadata.h"
#include "module.h"
#include "modules/meter.h"
#include "packet_pool.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "utils/endian.h"

namespace {

using bess::testing::ModuleHarness;

bess::pb::MeterIdKey Key(uint32_t id) {
  bess::pb::MeterIdKey key;
  key.set_id(id);
  return key;
}

bess::pb::MeterPolicyValue TrTcmPolicy(uint64_t cir, uint64_t cbs, uint64_t pir,
                                       uint64_t pbs) {
  bess::pb::MeterPolicyValue value;
  auto *tr = value.mutable_tr_tcm();
  tr->set_committed_rate(cir);
  tr->set_committed_burst(cbs);
  tr->set_peak_rate(pir);
  tr->set_peak_burst(pbs);
  return value;
}

bess::pb::MeterPolicyValue SrTcmPolicy(uint64_t cir, uint64_t cbs,
                                       uint64_t ebs) {
  bess::pb::MeterPolicyValue value;
  auto *sr = value.mutable_sr_tcm();
  sr->set_committed_rate(cir);
  sr->set_committed_burst(cbs);
  sr->set_excess_burst(ebs);
  return value;
}

bess::pb::MeterCommandGetStatusResponse GetStatus(
    ModuleHarness &h, Module *m, const std::vector<uint32_t> &ids = {}) {
  bess::pb::MeterCommandGetStatusArg arg;
  for (uint32_t id : ids) {
    arg.add_meter_ids(id);
  }
  CommandResponse response = h.Action(m, "get_status", arg);
  EXPECT_EQ(0, response.error().code()) << response.error().errmsg();
  bess::pb::MeterCommandGetStatusResponse status;
  EXPECT_TRUE(response.data().UnpackTo(&status));
  return status;
}

// Packets whose meter id the test writes into the meter's attribute at
// metadata offset 0.
class Packets {
 public:
  explicit Packets(size_t n) : pool_(n + 8) {
    for (size_t i = 0; i < n; i++) {
      bess::PacketHandle pkt = pool_.Alloc(100);
      EXPECT_NE(pkt, nullptr);
      std::memset(bess::PacketRef(pkt).head_data<uint8_t *>(), 0, 100);
      handles_.push_back(pkt);
    }
  }
  ~Packets() {
    for (bess::PacketHandle pkt : handles_) {
      bess::PacketFree(pkt);
    }
  }

  void SetMeter(size_t i, uint32_t id) {
    _set_attr_with_offset<bess::utils::be32_t>(
        0, bess::PacketRef(handles_[i]), bess::utils::be32_t(id));
  }

  bess::PacketBatch Batch(size_t n) const {
    bess::PacketBatch batch;
    batch.clear();
    for (size_t i = 0; i < n; i++) {
      batch.add(bess::PacketRef(handles_[i]));
    }
    return batch;
  }

  size_t size() const { return handles_.size(); }

 private:
  bess::PlainPacketPool pool_;
  std::vector<bess::PacketHandle> handles_;
};

const bess::pb::MeterBucketState *Bucket(
    const bess::pb::MeterStatus &status, bess::pb::MeterBucketKind kind) {
  for (const auto &bucket : status.buckets()) {
    if (bucket.kind() == kind) {
      return &bucket;
    }
  }
  ADD_FAILURE() << "meter " << status.meter_id() << " has no bucket "
                << static_cast<int>(kind);
  return nullptr;
}

TEST(MeterStatusTest, ListsNeverUpdatedMetersWithFullBuckets) {
  ModuleHarness h;
  Meter *m = static_cast<Meter *>(
      h.Create("Meter", bess::pb::MeterArg(), "mt"));
  ASSERT_NE(nullptr, m);
  ASSERT_EQ("", h.UpsertResource("mt/meters", Key(1),
                                 TrTcmPolicy(1, 1000, 1, 2000)));
  ASSERT_EQ("", h.UpsertResource("mt/meters", Key(2),
                                 SrTcmPolicy(1, 3000, 4000)));

  bess::pb::MeterCommandGetStatusResponse status = GetStatus(h, m);
  ASSERT_EQ(2, status.meters_size());
  // std::map order: by id.
  EXPECT_EQ(1u, status.meters(0).meter_id());
  EXPECT_EQ(2u, status.meters(1).meter_id());

  const bess::pb::MeterStatus &tr = status.meters(0);
  EXPECT_TRUE(tr.shared());
  EXPECT_TRUE(tr.policy().has_tr_tcm());
  EXPECT_EQ(1000u, tr.policy().tr_tcm().committed_burst());
  EXPECT_GT(tr.last_update_tsc(), 0u);
  ASSERT_EQ(2, tr.buckets_size());
  const auto *c = Bucket(tr, bess::pb::METER_BUCKET_COMMITTED);
  const auto *p = Bucket(tr, bess::pb::METER_BUCKET_PEAK);
  ASSERT_NE(nullptr, c);
  ASSERT_NE(nullptr, p);
  // Never checked: rte_meter's initial condition is a full bucket.
  EXPECT_EQ(1000u, c->tokens());
  EXPECT_EQ(1000u, c->limit());
  EXPECT_EQ(2000u, p->tokens());
  EXPECT_EQ(2000u, p->limit());

  const bess::pb::MeterStatus &sr = status.meters(1);
  EXPECT_TRUE(sr.policy().has_sr_tcm());
  const auto *e = Bucket(sr, bess::pb::METER_BUCKET_EXCESS);
  ASSERT_NE(nullptr, e);
  EXPECT_EQ(4000u, e->tokens());
  EXPECT_EQ(4000u, e->limit());
}

TEST(MeterStatusTest, ReadsSaturatedAndOverLimitBuckets) {
  ModuleHarness h;
  Meter *m = static_cast<Meter *>(
      h.Create("Meter", bess::pb::MeterArg(), "mt"));
  ASSERT_NE(nullptr, m);
  ASSERT_EQ("", h.UpsertResource("mt/meters", Key(1),
                                 TrTcmPolicy(1, 100, 1, 200)));
  // The module is the only writer of meter_id, at metadata offset 0.
  m->set_attr_offset(0, 0);

  Packets packets(3);
  for (size_t i = 0; i < packets.size(); i++) {
    packets.SetMeter(i, 1);
  }
  bess::PacketBatch batch = packets.Batch(3);
  std::array<gate_idx_t, 3> gates;
  m->MeterBatch(&batch, gates.data());
  EXPECT_EQ(Meter::kGreenGate, gates[0]);   // committed 0, peak 100
  EXPECT_EQ(Meter::kYellowGate, gates[1]);  // peak 0
  EXPECT_EQ(Meter::kRedGate, gates[2]);     // over both, charges nothing

  bess::pb::MeterCommandGetStatusResponse status = GetStatus(h, m, {1});
  ASSERT_EQ(1, status.meters_size());
  const auto *c = Bucket(status.meters(0), bess::pb::METER_BUCKET_COMMITTED);
  const auto *p = Bucket(status.meters(0), bess::pb::METER_BUCKET_PEAK);
  ASSERT_NE(nullptr, c);
  ASSERT_NE(nullptr, p);
  EXPECT_EQ(0u, c->tokens());
  EXPECT_EQ(0u, p->tokens());
  EXPECT_EQ(100u, c->limit());
  EXPECT_EQ(200u, p->limit());
  EXPECT_GT(status.meters(0).last_update_tsc(), 0u);
}

TEST(MeterStatusTest, FollowsAProfileChange) {
  ModuleHarness h;
  Meter *m = static_cast<Meter *>(
      h.Create("Meter", bess::pb::MeterArg(), "mt"));
  ASSERT_NE(nullptr, m);
  ASSERT_EQ("", h.UpsertResource("mt/meters", Key(1),
                                 TrTcmPolicy(1, 100, 1, 100)));
  const uint64_t before = GetStatus(h, m, {1}).meters(0).last_update_tsc();

  // A profile change starts fresh state (rte_meter has no rebinding), so the
  // reply must show the new policy and full new buckets, not the old tokens.
  ASSERT_EQ("", h.UpsertResource("mt/meters", Key(1),
                                 SrTcmPolicy(1, 500, 700)));
  bess::pb::MeterCommandGetStatusResponse status = GetStatus(h, m, {1});
  ASSERT_EQ(1, status.meters_size());
  const bess::pb::MeterStatus &meter = status.meters(0);
  EXPECT_TRUE(meter.policy().has_sr_tcm());
  EXPECT_EQ(500u, meter.policy().sr_tcm().committed_burst());
  const auto *c = Bucket(meter, bess::pb::METER_BUCKET_COMMITTED);
  const auto *e = Bucket(meter, bess::pb::METER_BUCKET_EXCESS);
  ASSERT_NE(nullptr, c);
  ASSERT_NE(nullptr, e);
  EXPECT_EQ(500u, c->tokens());
  EXPECT_EQ(500u, c->limit());
  EXPECT_EQ(700u, e->tokens());
  EXPECT_EQ(700u, e->limit());
  EXPECT_GE(meter.last_update_tsc(), before);
}

TEST(MeterStatusTest, RefusesAnUnknownMeter) {
  ModuleHarness h;
  Meter *m = static_cast<Meter *>(
      h.Create("Meter", bess::pb::MeterArg(), "mt"));
  ASSERT_NE(nullptr, m);
  bess::pb::MeterCommandGetStatusArg arg;
  arg.add_meter_ids(9);
  CommandResponse response = h.Action(m, "get_status", arg);
  EXPECT_NE(0, response.error().code());
}

// A meter erased but not yet through the removal cascade -- it may still be
// resolved by packets -- is no longer live to the control side: the all-meters
// form omits it and naming it is ENOENT, matching the resource read-back.
TEST(MeterStatusTest, RefusesAnErasedMeterBeforeTheCascade) {
  ModuleHarness h;
  Meter *m = static_cast<Meter *>(
      h.Create("Meter", bess::pb::MeterArg(), "mt"));
  ASSERT_NE(nullptr, m);
  ASSERT_EQ("", h.UpsertResource(
                    "mt/meters", Key(1),
                    TrTcmPolicy(1'000'000, 1000, 1'000'000, 1000)));
  ASSERT_EQ("", h.EraseResource("mt/meters", Key(1)));

  EXPECT_EQ(0, GetStatus(h, m).meters_size());
  bess::pb::MeterCommandGetStatusArg arg;
  arg.add_meter_ids(1);
  CommandResponse response = h.Action(m, "get_status", arg);
  EXPECT_NE(0, response.error().code()) << "a retiring meter has no live status";
}

// MZ-5: with 100K meters, get_status without ids returns one page (at most
// kStatusPage meters, a bounded reply) and where the next one starts; walking
// the pages lists every meter once, in id order.
TEST(MeterStatusTest, PagesTheConfiguredMeters) {
  constexpr uint32_t kMeters = 100'000;
  ModuleHarness h;
  bess::pb::MeterArg meter_arg;
  meter_arg.set_capacity(kMeters);
  Meter *m = static_cast<Meter *>(h.Create("Meter", meter_arg, "mt"));
  ASSERT_NE(nullptr, m);
  const bess::framework::ResourceCodec *codec = h.Codec("mt/meters");
  ASSERT_NE(nullptr, codec);
  google::protobuf::Any packed_value;
  ASSERT_TRUE(packed_value.PackFrom(SrTcmPolicy(1, 3000, 4000)));
  const auto value = codec->Value(packed_value.type_url(), packed_value.value());
  ASSERT_TRUE(value.has_value()) << value.error();
  std::vector<bess::dataplane::Op> ops;
  for (uint32_t id = 1; id <= kMeters; id++) {
    google::protobuf::Any packed_key;
    ASSERT_TRUE(packed_key.PackFrom(Key(id)));
    auto key = codec->Key(packed_key.type_url(), packed_key.value());
    ASSERT_TRUE(key.has_value()) << key.error();
    ops.push_back(bess::dataplane::Op::Upsert("mt/meters", std::move(*key), *value));
    if (ops.size() == 1000) {
      ASSERT_EQ("", h.Apply(ops));
      ops.clear();
    }
  }

  auto page = [&](uint32_t start_id, uint32_t limit) {
    bess::pb::MeterCommandGetStatusArg arg;
    arg.set_start_id(start_id);
    arg.set_limit(limit);
    CommandResponse response = h.Action(m, "get_status", arg);
    EXPECT_EQ(0, response.error().code()) << response.error().errmsg();
    bess::pb::MeterCommandGetStatusResponse status;
    EXPECT_TRUE(response.data().UnpackTo(&status));
    return status;
  };
  const bess::pb::MeterCommandGetStatusResponse first = page(0, 0);
  EXPECT_EQ(Meter::kStatusPage, static_cast<size_t>(first.meters_size()));
  EXPECT_EQ(Meter::kStatusPage + 1, first.next_id());
  EXPECT_LE(first.ByteSizeLong(), size_t{3} << 20);
  const bess::pb::MeterCommandGetStatusResponse mid = page(5, 10);
  ASSERT_EQ(10, mid.meters_size());
  EXPECT_EQ(5u, mid.meters(0).meter_id());
  EXPECT_EQ(15u, mid.next_id());

  uint32_t next = 0, expected = 1;
  size_t pages = 0;
  do {
    const bess::pb::MeterCommandGetStatusResponse status = page(next, 4096);  // clamped
    ASSERT_LE(static_cast<size_t>(status.meters_size()), Meter::kStatusPage);
    for (const bess::pb::MeterStatus &meter : status.meters()) {
      ASSERT_EQ(expected++, meter.meter_id());
    }
    next = status.next_id();
    pages++;
  } while (next != 0 && pages <= kMeters);
  EXPECT_EQ(kMeters + 1, expected);
  EXPECT_EQ((kMeters + Meter::kStatusPage - 1) / Meter::kStatusPage, pages);
}

// The command's read path while the packet path updates the same shared
// meter: every reading is a coherent pair of buckets (a lock-protected
// snapshot, not two independent word loads), and the read is exercised
// through the real action, not only the core API.
TEST(MeterStatusTest, IsCoherentUnderTraffic) {
  ModuleHarness h;
  Meter *m = static_cast<Meter *>(
      h.Create("Meter", bess::pb::MeterArg(), "mt"));
  ASSERT_NE(nullptr, m);
  ASSERT_EQ("", h.UpsertResource("mt/meters", Key(1),
                                 TrTcmPolicy(1'000'000, 1000, 1'000'000, 1000)));
  m->set_attr_offset(0, 0);

  Packets packets(8);
  for (size_t i = 0; i < packets.size(); i++) {
    packets.SetMeter(i, 1);
  }
  bess::PacketBatch batch = packets.Batch(packets.size());

  std::atomic<bool> stop{false};
  std::thread writer([&] {
    std::array<gate_idx_t, 8> gates;
    while (!stop.load(std::memory_order_relaxed)) {
      m->MeterBatch(&batch, gates.data());
    }
  });

  constexpr int kReads = 1000;
  bool torn = false;
  for (int i = 0; i < kReads; i++) {
    bess::pb::MeterCommandGetStatusResponse status = GetStatus(h, m, {1});
    ASSERT_EQ(1, status.meters_size());
    uint64_t committed = 0;
    uint64_t peak = 0;
    for (const auto &bucket : status.meters(0).buckets()) {
      if (bucket.tokens() > bucket.limit()) {
        torn = true;
      }
      if (bucket.kind() == bess::pb::METER_BUCKET_COMMITTED) {
        committed = bucket.tokens();
      } else if (bucket.kind() == bess::pb::METER_BUCKET_PEAK) {
        peak = bucket.tokens();
      }
    }
    // The test profile has CIR == PIR and equal bursts, so a coherent
    // snapshot can never have the peak bucket above the committed one; a
    // pair torn between a green check's two stores can.
    if (peak > committed) {
      torn = true;
    }
  }
  EXPECT_FALSE(torn);
  stop.store(true, std::memory_order_relaxed);
  writer.join();
}

}  // namespace
