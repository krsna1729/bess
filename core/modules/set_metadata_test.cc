// SPDX-License-Identifier: BSD-3-Clause

// SetMetadata and Update in process. SetMetadata's attributes are read back
// by a GenericEncap that copies each into a header in front of the packet, so
// every byte written is checked: values and packet bytes (with and without
// shift and mask) of the sizes the packet path specializes (1, 2, 4, 8) and of
// one it does not (3). Update's fields are applied to every packet in order.

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>

#include "module_graph.h"
#include "pb/module_msg.pb.h"
#include "testing/module_harness.h"
#include "testing/packet_builder.h"

namespace {

using bess::testing::Bytes;
using bess::testing::ModuleHarness;
using bess::testing::PacketBuilder;

// 40 packets (more than one burst), each with its own source address.
std::vector<Bytes> Packets() {
  std::vector<Bytes> packets;
  for (int i = 0; i < 40; i++) {
    packets.push_back(PacketBuilder()
                          .Ether("02:00:00:00:00:02", "02:00:00:00:00:01")
                          .IPv4("10.0.0." + std::to_string(i + 1), "11.0.0.2")
                          .Udp(1000, 2000)
                          .Payload("payload")
                          .Build());
  }
  return packets;
}

Bytes B(std::string_view s) { return Bytes(s.begin(), s.end()); }

struct Attr {
  std::string name;
  size_t size;
  std::string value_bin;  // empty: from the packet
  int offset = -1;
  int rshift_bits = 0;
  std::string mask;
};

// Runs Packets() through SetMetadata(attrs) -> GenericEncap(each attribute
// in order) and checks that each packet leaves with `header` in front.
void ExpectHeader(const std::vector<Attr> &attrs, const Bytes &header) {
  ModuleHarness h;
  bess::pb::SetMetadataArg md_arg;
  bess::pb::GenericEncapArg encap_arg;
  for (const Attr &a : attrs) {
    auto *m = md_arg.add_attrs();
    m->set_name(a.name);
    m->set_size(a.size);
    if (a.offset < 0) {
      m->set_value_bin(a.value_bin);
    } else {
      m->set_offset(a.offset);
      m->set_rshift_bits(a.rshift_bits);
      m->set_mask(a.mask);
    }
    auto *f = encap_arg.add_fields();
    f->set_size(a.size);
    f->set_attribute(a.name);
  }
  Module *md = h.Create("SetMetadata", md_arg);
  Module *encap = h.Create("GenericEncap", encap_arg);
  ASSERT_NE(nullptr, md);
  ASSERT_NE(nullptr, encap);
  // No default hooks: Track needs a worker thread, and this test runs on none.
  ASSERT_EQ(0, ModuleGraph::ConnectModules(md, 0, encap, 0, true));

  const std::vector<Bytes> in = Packets();
  auto out = h.RunPipeline(md, 0, in, encap, {0});
  ASSERT_EQ(in.size(), out[0].size());
  for (size_t i = 0; i < in.size(); i++) {
    Bytes want = header;
    want.insert(want.end(), in[i].begin(), in[i].end());
    EXPECT_EQ(want, out[0][i]) << "packet " << i;
  }
}

TEST(SetMetadataTest, ValuesOfEverySize) {
  ExpectHeader({{"v1", 1, "\x11"},
                {"v2", 2, "\x21\x22"},
                {"v3", 3, "\x31\x32\x33"},
                {"v4", 4, "\x41\x42\x43\x44"},
                {"v8", 8, "\x81\x82\x83\x84\x85\x86\x87\x88"}},
               B("\x11"
                 "\x21\x22"
                 "\x31\x32\x33"
                 "\x41\x42\x43\x44"
                 "\x81\x82\x83\x84\x85\x86\x87\x88"));
}

// The packet: source address at 26 (10.0.0.1 for the first, but each packet
// differs, so only fields after it are compared as constants), destination
// 11.0.0.2 at 30, ports 1000 (03 e8) and 2000 (07 d0) at 34 and 36.
TEST(SetMetadataTest, PacketBytesWithShiftAndMask) {
  ExpectHeader(
      {
          // 4 bytes as they are: 0b 00 00 02.
          {.name = "d4", .size = 4, .offset = 30},
          // 4 bytes shifted right one byte: 00 0b 00 00.
          {.name = "d4s", .size = 4, .offset = 30, .rshift_bits = 8},
          // 2 bytes masked: 03 e8 & ff 00.
          {.name = "p2m", .size = 2, .offset = 34, .mask = std::string("\xff\x00", 2)},
          // 8 bytes (0b 00 00 02 03 e8 07 d0) shifted left two bytes
          // (00 02 03 e8 07 d0 00 00), then masked.
          {.name = "d8sm",
           .size = 8,
           .offset = 30,
           .rshift_bits = -16,
           .mask = std::string("\xff\x00\xff\xff\xff\x0f\xff\xff", 8)},
          // 3 bytes (03 e8 07) shifted right one byte (00 03 e8), masked.
          {.name = "p3sm",
           .size = 3,
           .offset = 34,
           .rshift_bits = 8,
           .mask = std::string("\xff\xff\x0f", 3)},
          // 3 bytes as they are: 00 00 02 (the last three of the destination).
          {.name = "d3", .size = 3, .offset = 31},
      },
      B(std::string("\x0b\x00\x00\x02"
                    "\x00\x0b\x00\x00"
                    "\x03\x00"
                    "\x00\x00\x03\xe8\x07\x00\x00\x00"
                    "\x00\x03\x08"
                    "\x00\x00\x02",
                    4 + 4 + 2 + 8 + 3 + 3)));
}

// D-129: SetMetadata has no actions; the attributes are the init argument,
// which GetPipeline's Module.arg carries back.
TEST(SetMetadataTest, InitialArgIsADesiredStateArgument) {
  ModuleHarness h;
  bess::pb::SetMetadataArg arg;
  auto *a = arg.add_attrs();
  a->set_name("v4");
  a->set_size(4);
  a->set_value_bin("\x41\x42\x43\x44");
  Module *md = h.Create("SetMetadata", arg);
  ASSERT_NE(nullptr, md);
  EXPECT_EQ(ENOTSUP, h.Action(md, "get_initial_arg", bess::pb::EmptyArg()).error().code());
}

// Fields apply to every packet in the order given: a later field that
// overlaps an earlier one wins where they overlap, and bytes no field names
// keep each packet's own value.
TEST(UpdateTest, FieldsApplyToEveryPacketInOrder) {
  ModuleHarness h;
  bess::pb::UpdateArg arg;
  auto add = [&](int64_t offset, uint64_t size, uint64_t value) {
    auto *f = arg.add_fields();
    f->set_offset(offset);
    f->set_size(size);
    f->set_value(value);
  };
  add(22, 1, 32);                // TTL
  add(0, 6, 0x0a0b0c0d0e0f);     // destination MAC
  add(4, 4, 0xa1a2a3a4);         // its last two bytes and the source's first two
  Module *up = h.Create("Update", arg);
  ASSERT_NE(nullptr, up);

  const std::vector<Bytes> in = Packets();
  auto out = h.Run(up, 0, in, {0});
  ASSERT_EQ(in.size(), out[0].size());
  for (size_t i = 0; i < in.size(); i++) {
    Bytes want = in[i];
    const uint8_t mac[] = {0x0a, 0x0b, 0x0c, 0x0d, 0xa1, 0xa2, 0xa3, 0xa4};
    std::copy(std::begin(mac), std::end(mac), want.begin());
    want[22] = 32;
    EXPECT_EQ(want, out[0][i]) << "packet " << i;
  }
}

// A packet-source attribute reads a fixed offset. A frame whose first segment
// is shorter than offset + size is dropped: the old code read past its head.
TEST(SetMetadataTest, APacketShorterThanTheExtractionIsDropped) {
  bess::pb::SetMetadataArg arg;
  auto *a = arg.add_attrs();
  a->set_name("meta");
  a->set_size(4);
  a->set_offset(8);

  bess::pb::MeterArg meter_arg;
  meter_arg.set_id_attr("meta");  // a downstream reader, so the offset resolves

  ModuleHarness h;
  Module *md = h.Create("SetMetadata", arg);
  Module *meter = h.Create("Meter", meter_arg);
  ASSERT_NE(nullptr, md);
  ASSERT_NE(nullptr, meter);
  ASSERT_EQ(0, ModuleGraph::ConnectModules(md, 0, meter, 0, true));

  auto out = h.RunPipeline(md, 0, {Bytes(11)}, meter, {0});
  EXPECT_TRUE(out[0].empty()) << "11 bytes < offset 8 + size 4";

  out = h.RunPipeline(md, 0, {Bytes(12)}, meter, {0});
  EXPECT_EQ(1u, out[0].size()) << "12 bytes hold the whole extraction";
}

}  // namespace
