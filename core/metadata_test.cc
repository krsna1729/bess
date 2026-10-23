// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "metadata.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "module.h"
#include "module_graph.h"

namespace {

class Foo : public Module {
 public:
  static const gate_idx_t kNumIGates = MAX_GATES;
  static const gate_idx_t kNumOGates = MAX_GATES;

  static const Commands cmds;
};

const Commands Foo::cmds = {};

Module *create_foo(const std::string name = "") {
  const ModuleBuilder &builder =
      ModuleBuilder::all_module_builders().find("Foo")->second;

  Module *m;
  pb_error_t perr;

  bess::pb::EmptyArg arg_;
  google::protobuf::Any arg;
  EXPECT_TRUE(arg.PackFrom(arg_));

  if (name.size() == 0) {
    const std::string &mod_name = ModuleGraph::GenerateDefaultName(
        builder.class_name(), builder.name_template());
    m = ModuleGraph::CreateModule(builder, mod_name, arg, &perr);
  } else {
    m = ModuleGraph::CreateModule(builder, name, arg, &perr);
  }

  return m;
}

DEF_MODULE(Foo, "foo", "bip");

// The metadata framework tests read and write the packet's private metadata
// without bringing up the EAL/packet pools: a zeroed buffer large enough for
// an mbuf plus its private area, with priv_size set as DPDK would.
class FakePacket {
 public:
  FakePacket() {
    mbuf()->priv_size = bess::kPacketPrivateSize;
  }

  bess::PacketRef ref() {
    return bess::PacketRef(mbuf());
  }

 private:
  rte_mbuf *mbuf() { return reinterpret_cast<rte_mbuf *>(storage_.data()); }

  alignas(64)
      std::array<char, sizeof(rte_mbuf) + bess::kPacketPrivateSize> storage_{};
};

}  // namespace

namespace bess {
namespace metadata {

class MetadataTest : public ::testing::Test {
 protected:
  MetadataTest() : m0(), m1(), Foo_singleton() {}

  virtual void SetUp() {
    default_pipeline.CleanupMetadataComputation();
    default_pipeline.registered_attrs_.clear();
    m0 = ::create_foo();
    m1 = ::create_foo();
    ASSERT_TRUE(m0);
    ASSERT_TRUE(m1);
  }

  virtual void TearDown() { ModuleGraph::DestroyAllModules(); }

  Module *m0;
  Module *m1;
  Foo_class Foo_singleton;

 private:
  DISALLOW_COPY_AND_ASSIGN(MetadataTest);
};

TEST(Metadata, RegisterSizeMismatchFails) {
  ASSERT_EQ(0, default_pipeline.RegisterAttribute("attr0", 1));
  ASSERT_EQ(-EINVAL, default_pipeline.RegisterAttribute("attr0", 2));

  default_pipeline.DeregisterAttribute("attr0");
  ASSERT_EQ(0, default_pipeline.RegisterAttribute("attr0", 2));

  default_pipeline.DeregisterAttribute("attr0");
}

TEST(Metadata, RegisterCount) {
  ASSERT_EQ(0, default_pipeline.RegisterAttribute("a", 4));
  ASSERT_EQ(0, default_pipeline.RegisterAttribute("a", 4));
  ASSERT_EQ(0, default_pipeline.RegisterAttribute("a", 4));
  // here the count should be 3

  ASSERT_EQ(-EINVAL, default_pipeline.RegisterAttribute("a", 8));
  default_pipeline.DeregisterAttribute("a");

  ASSERT_EQ(-EINVAL, default_pipeline.RegisterAttribute("a", 8));
  default_pipeline.DeregisterAttribute("a");

  ASSERT_EQ(-EINVAL, default_pipeline.RegisterAttribute("a", 8));
  default_pipeline.DeregisterAttribute("a");

  // now the count should be 0
  ASSERT_EQ(0, default_pipeline.RegisterAttribute("a", 8));
  default_pipeline.DeregisterAttribute("a");
}

TEST_F(MetadataTest, DisconnectedFails) {
  ASSERT_EQ(0, m0->AddMetadataAttr("a", 1, Attribute::AccessMode::kWrite));
  ASSERT_EQ(0, m1->AddMetadataAttr("a", 1, Attribute::AccessMode::kRead));
  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());
  ASSERT_LT(m1->attr_offset(0), 0);
}

TEST_F(MetadataTest, SingleAttrSimplePipe) {
  ASSERT_EQ(0, m0->AddMetadataAttr("a", 1, Attribute::AccessMode::kWrite));
  ASSERT_EQ(0, m1->AddMetadataAttr("a", 1, Attribute::AccessMode::kRead));
  ModuleGraph::ConnectModules(m0, 0, m1, 0);

  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());

  // Check that m0 was assigned a valid offset
  ASSERT_GE(m1->attr_offset(0), 0);

  // Check that m0 and m1 agree on where to read/write a
  ASSERT_EQ(m0->attr_offset(0), m1->attr_offset(0));
}

// Check that the "error" offsets arre assigned correctly
TEST_F(MetadataTest, SingleAttrSimplePipeBackwardsFails) {
  ASSERT_EQ(0, m0->AddMetadataAttr("a", 1, Attribute::AccessMode::kRead));
  ASSERT_EQ(0, m1->AddMetadataAttr("a", 1, Attribute::AccessMode::kWrite));

  ModuleGraph::ConnectModules(m0, 0, m1, 0);

  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());

  ASSERT_EQ(kMetadataOffsetNoRead, m0->attr_offset(0));
  ASSERT_EQ(kMetadataOffsetNoWrite, m1->attr_offset(0));
}

// A module that matches on an attribute and writes it back (a classifier key
// field and a result field of one name) is one kUpdate attribute: it reads the
// upstream writer's value and its write is what the downstream reader sees.
TEST_F(MetadataTest, AReadAndAWriteOfOneNameAreAnUpdate) {
  Module *reader = ::create_foo("downstream");
  ASSERT_EQ(0, m0->AddMetadataAttr("a", 4, Attribute::AccessMode::kWrite));
  ASSERT_EQ(0, m1->AddMetadataAttr("a", 4, Attribute::AccessMode::kRead));
  ASSERT_EQ(0, m1->AddMetadataAttr("a", 4, Attribute::AccessMode::kWrite));
  ASSERT_EQ(1u, m1->all_attrs().size());
  EXPECT_EQ(Attribute::AccessMode::kUpdate, m1->all_attrs()[0].mode);
  // Only one read and one write merge, and only at one size.
  EXPECT_EQ(-EEXIST, m1->AddMetadataAttr("a", 4, Attribute::AccessMode::kRead));
  ASSERT_EQ(1, m0->AddMetadataAttr("b", 2, Attribute::AccessMode::kWrite));
  EXPECT_EQ(-EEXIST, m0->AddMetadataAttr("b", 2, Attribute::AccessMode::kWrite));
  EXPECT_EQ(-EEXIST, m0->AddMetadataAttr("b", 4, Attribute::AccessMode::kRead));
  ASSERT_EQ(0, reader->AddMetadataAttr("a", 4, Attribute::AccessMode::kRead));

  ModuleGraph::ConnectModules(m0, 0, m1, 0);
  ModuleGraph::ConnectModules(m1, 0, reader, 0);
  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());
  ASSERT_GE(m1->attr_offset(0), 0);
  EXPECT_EQ(m0->attr_offset(0), m1->attr_offset(0));
  EXPECT_EQ(m1->attr_offset(0), reader->attr_offset(0));
}

// A contract is orthogonal to the access mode: an existing three-argument
// declaration is best-effort, so an unresolved reader keeps the historical
// kMetadataOffsetNoRead and the report names it.
TEST_F(MetadataTest, BestEffortIsTheDefaultAndUnchanged) {
  ASSERT_EQ(0, m1->AddMetadataAttr("a", 4, Attribute::AccessMode::kRead));
  ASSERT_EQ(Attribute::Contract::kBestEffort, m1->all_attrs()[0].contract);

  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());
  EXPECT_EQ(kMetadataOffsetNoRead, m1->attr_offset(0));

  const MetadataReport &report = default_pipeline.report();
  ASSERT_EQ(1u, report.readers_without_writer.size());
  EXPECT_EQ(m1->name(), report.readers_without_writer[0].module);
  EXPECT_EQ("a", report.readers_without_writer[0].attribute);
  EXPECT_EQ(Attribute::Contract::kBestEffort,
            report.readers_without_writer[0].contract);
  EXPECT_TRUE(report.unsatisfied_required.empty());
  EXPECT_TRUE(report.defaults_applied.empty());
  EXPECT_TRUE(report.writers_without_reader.empty());
  EXPECT_EQ(1u, report.declarations);
  EXPECT_EQ(0u, report.resolved);
}

// A required reader with no provider refuses the resume and names the module
// and attribute, even though the offsets were still filled for diagnostics.
TEST_F(MetadataTest, RequiredWithoutProviderFailsResume) {
  ASSERT_EQ(0, m1->AddMetadataAttr("must_have", 4,
                                   Attribute::AccessMode::kRead,
                                   Attribute::Contract::kRequired));

  EXPECT_EQ(-EINVAL, default_pipeline.ComputeMetadataOffsets());
  EXPECT_EQ(kMetadataOffsetNoRead, m1->attr_offset(0));

  const MetadataReport &report = default_pipeline.report();
  ASSERT_EQ(1u, report.unsatisfied_required.size());
  EXPECT_EQ(m1->name(), report.unsatisfied_required[0].module);
  EXPECT_EQ("must_have", report.unsatisfied_required[0].attribute);
  EXPECT_EQ(Attribute::Contract::kRequired,
            report.unsatisfied_required[0].contract);
  ASSERT_EQ(1u, report.readers_without_writer.size());
  EXPECT_NE(nullptr, default_pipeline.first_unsatisfied_required());
}

TEST_F(MetadataTest, RequiredWithProviderResolves) {
  ASSERT_EQ(0, m0->AddMetadataAttr("must_have", 4,
                                   Attribute::AccessMode::kWrite));
  ASSERT_EQ(0, m1->AddMetadataAttr("must_have", 4,
                                   Attribute::AccessMode::kRead,
                                   Attribute::Contract::kRequired));
  ModuleGraph::ConnectModules(m0, 0, m1, 0);

  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());
  ASSERT_GE(m1->attr_offset(0), 0);
  EXPECT_EQ(m0->attr_offset(0), m1->attr_offset(0));
  EXPECT_TRUE(default_pipeline.report().unsatisfied_required.empty());
  EXPECT_EQ(nullptr, default_pipeline.first_unsatisfied_required());
}

// A defaulted reader with no provider resolves to the declared value without
// reading packet metadata; when a provider appears at a later resume, the same
// resolved attribute reads the packet instead.
TEST_F(MetadataTest, DefaultAppliesUntilAProviderAppears) {
  using Bytes4 = std::array<uint8_t, 4>;
  const Bytes4 kDefault{0x11, 0x22, 0x33, 0x44};

  ASSERT_EQ(0, m1->AddMetadataAttrWithDefault("a", sizeof(Bytes4),
                                              Attribute::AccessMode::kRead,
                                              kDefault));
  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());
  EXPECT_EQ(kMetadataOffsetDefault, m1->attr_offset(0));

  const MetadataReport &report = default_pipeline.report();
  ASSERT_EQ(1u, report.defaults_applied.size());
  EXPECT_EQ(m1->name(), report.defaults_applied[0].module);
  EXPECT_EQ("a", report.defaults_applied[0].attribute);
  EXPECT_EQ(std::vector<uint8_t>(kDefault.begin(), kDefault.end()),
            report.defaults_applied[0].default_value);
  EXPECT_TRUE(report.unsatisfied_required.empty());

  FakePacket packet;
  ResolvedAttr<Bytes4> resolved = m1->ResolveAttr<Bytes4>(0);
  EXPECT_FALSE(resolved.resolved());
  EXPECT_TRUE(resolved.has_default);
  EXPECT_EQ(kDefault, resolved.Get(packet.ref()));

  // A provider appears in a later graph revision.
  ASSERT_EQ(0, m0->AddMetadataAttr("a", sizeof(Bytes4),
                                   Attribute::AccessMode::kWrite));
  ModuleGraph::ConnectModules(m0, 0, m1, 0);
  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());

  ASSERT_GE(m1->attr_offset(0), 0);
  EXPECT_EQ(m0->attr_offset(0), m1->attr_offset(0));
  EXPECT_TRUE(default_pipeline.report().defaults_applied.empty());

  resolved = m1->ResolveAttr<Bytes4>(0);
  EXPECT_TRUE(resolved.resolved());
  const Bytes4 written{9, 8, 7, 6};
  set_attr_with_offset<Bytes4>(m0->attr_offset(0), packet.ref(), written);
  EXPECT_EQ(written, resolved.Get(packet.ref()));
}

// The report distinguishes a writer nothing consumes, and flags declarations
// of one name that disagree about the reader contract or default.
TEST_F(MetadataTest, ReportListsUnreadWritersAndConflicts) {
  ASSERT_EQ(0, m0->AddMetadataAttr("unread", 2,
                                   Attribute::AccessMode::kWrite));
  using Bytes4 = std::array<uint8_t, 4>;
  ASSERT_EQ(1, m0->AddMetadataAttrWithDefault("a", sizeof(Bytes4),
                                              Attribute::AccessMode::kRead,
                                              Bytes4{1, 2, 3, 4}));
  ASSERT_EQ(0, m1->AddMetadataAttrWithDefault("a", sizeof(Bytes4),
                                              Attribute::AccessMode::kRead,
                                              Bytes4{5, 6, 7, 8}));

  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());

  const MetadataReport &report = default_pipeline.report();
  ASSERT_EQ(1u, report.writers_without_reader.size());
  EXPECT_EQ(m0->name(), report.writers_without_reader[0].module);
  EXPECT_EQ("unread", report.writers_without_reader[0].attribute);
  EXPECT_EQ(kMetadataOffsetNoWrite, m0->attr_offset(0));

  ASSERT_EQ(1u, report.conflicts.size());
  EXPECT_EQ("a", report.conflicts[0].attribute);
  EXPECT_NE(std::string::npos,
            report.conflicts[0].reason.find("default values differ"));
  ASSERT_EQ(2u, report.conflicts[0].modules.size());
  EXPECT_EQ(2u, report.defaults_applied.size());
}

// Two readers of one name that disagree on the contract are a conflict too;
// the stricter one decides the resume.
TEST_F(MetadataTest, ReportFlagsContractDisagreement) {
  ASSERT_EQ(0, m0->AddMetadataAttr("a", 4, Attribute::AccessMode::kRead,
                                   Attribute::Contract::kBestEffort));
  ASSERT_EQ(0, m1->AddMetadataAttr("a", 4, Attribute::AccessMode::kRead,
                                   Attribute::Contract::kRequired));

  EXPECT_EQ(-EINVAL, default_pipeline.ComputeMetadataOffsets());
  const MetadataReport &report = default_pipeline.report();
  ASSERT_EQ(1u, report.conflicts.size());
  EXPECT_EQ("a", report.conflicts[0].attribute);
  EXPECT_NE(std::string::npos,
            report.conflicts[0].reason.find("different contract"));
  ASSERT_EQ(1u, report.unsatisfied_required.size());
}

// One reader reachable from two writer scopes: each writer gets its own
// component and the allocator gives the reader one of their offsets, so the
// report names the attribute rather than failing silently.
TEST_F(MetadataTest, ReportFlagsAReaderReachedFromTwoWriterScopes) {
  Module *left = ::create_foo("left");
  Module *right = ::create_foo("right");
  Module *reader = ::create_foo("reader");
  ASSERT_NE(nullptr, left);
  ASSERT_NE(nullptr, right);
  ASSERT_NE(nullptr, reader);

  ASSERT_EQ(0, m0->AddMetadataAttr("a", 2, Attribute::AccessMode::kWrite));
  ASSERT_EQ(0, m1->AddMetadataAttr("a", 2, Attribute::AccessMode::kWrite));
  ASSERT_EQ(0, reader->AddMetadataAttr("a", 2, Attribute::AccessMode::kRead));

  ModuleGraph::ConnectModules(m0, 0, left, 0);
  ModuleGraph::ConnectModules(m1, 0, right, 0);
  ModuleGraph::ConnectModules(left, 0, reader, 0);
  ModuleGraph::ConnectModules(right, 0, reader, 1);

  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());
  const MetadataReport &report = default_pipeline.report();
  ASSERT_EQ(1u, report.conflicts.size());
  EXPECT_EQ("a", report.conflicts[0].attribute);
  EXPECT_NE(std::string::npos,
            report.conflicts[0].reason.find("more than one writer scope"));
  EXPECT_EQ(3u, report.conflicts[0].modules.size())
      << "both writers and the reader declare the name";
  EXPECT_EQ(3u, report.declarations);
  EXPECT_EQ(3u, report.resolved) << "all three resolved to some packet offset";
}

// A reader also reached by a path with no writer is an invalid scope: the
// reader gets kMetadataOffsetNoRead, and the writer, whose value does have a
// reader, kMetadataOffsetInvalidScope - not kMetadataOffsetNoWrite, which
// means nothing reads it. The writer is not reported as unread.
TEST_F(MetadataTest, AWriterInAnInvalidScopeIsNotUnread) {
  Module *reader = ::create_foo("reader");
  ASSERT_NE(nullptr, reader);
  ASSERT_EQ(0, m0->AddMetadataAttr("a", 2, Attribute::AccessMode::kWrite));
  ASSERT_EQ(0, reader->AddMetadataAttr("a", 2, Attribute::AccessMode::kRead));
  // m1 declares nothing and has no input: a path to the reader without a
  // writer.
  ModuleGraph::ConnectModules(m0, 0, reader, 0);
  ModuleGraph::ConnectModules(m1, 0, reader, 0);

  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());
  EXPECT_EQ(kMetadataOffsetInvalidScope, m0->attr_offset(0));
  EXPECT_EQ(kMetadataOffsetNoRead, reader->attr_offset(0));
  const MetadataReport &report = default_pipeline.report();
  EXPECT_TRUE(report.writers_without_reader.empty());
  ASSERT_EQ(1u, report.readers_without_writer.size());
  EXPECT_EQ("reader", report.readers_without_writer[0].module);
}

// Check that offsets are properly assigned when there are too many attributes.
TEST_F(MetadataTest, MultipleAttrSimplePipeNoSpaceFails) {
  size_t sz = kMetadataAttrMaxSize;
  size_t n = kMetadataTotalSize / sz;
  for (size_t i = 0; i <= n; i++) {
    std::string s = "attr" + std::to_string(i);
    ASSERT_EQ(i, m0->AddMetadataAttr(s, sz, Attribute::AccessMode::kWrite));
    ASSERT_EQ(i, m1->AddMetadataAttr(s, sz, Attribute::AccessMode::kRead));
  }
  ModuleGraph::ConnectModules(m0, 0, m1, 0);

  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());

  ASSERT_EQ(kMetadataOffsetNoSpace, m0->attr_offset(n));
  ASSERT_EQ(kMetadataOffsetNoSpace, m1->attr_offset(n));
}

TEST_F(MetadataTest, MultipeAttrSimplePipe) {
  bool dummy_meta[kMetadataTotalSize] = {};
  ASSERT_EQ(0, m0->AddMetadataAttr("a", 2, Attribute::AccessMode::kWrite));
  ASSERT_EQ(1, m0->AddMetadataAttr("b", 3, Attribute::AccessMode::kWrite));
  ASSERT_EQ(2, m0->AddMetadataAttr("c", 5, Attribute::AccessMode::kWrite));
  ASSERT_EQ(3, m0->AddMetadataAttr("d", 8, Attribute::AccessMode::kWrite));
  ASSERT_EQ(0, m1->AddMetadataAttr("a", 2, Attribute::AccessMode::kRead));
  ASSERT_EQ(1, m1->AddMetadataAttr("b", 3, Attribute::AccessMode::kRead));
  ASSERT_EQ(2, m1->AddMetadataAttr("c", 5, Attribute::AccessMode::kRead));
  ASSERT_EQ(3, m1->AddMetadataAttr("d", 8, Attribute::AccessMode::kRead));
  ModuleGraph::ConnectModules(m0, 0, m1, 0);

  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());

  size_t i = 0;
  for (const auto &attr : m0->all_attrs()) {
    // Check that m1 is reading from where m0 is writing
    ASSERT_EQ(m1->attr_offset(i), m0->attr_offset(i));

    if (attr.mode != Attribute::AccessMode::kRead) {
      // Check that m0 was assigned non-overlapping offsets for writes
      mt_offset_t offset = m0->attr_offset(i);
      ASSERT_LE(0, offset);
      for (size_t j = 0; j < attr.size; j++) {
        ASSERT_FALSE(dummy_meta[offset + j]);
        dummy_meta[offset + j] = true;
      }
    }
    i++;
  }
}

TEST_F(MetadataTest, MultipeAttrComplexPipe) {
  ModuleGraph::DestroyAllModules();
  std::vector<Module *> mods;
  for (int i = 0; i < 10; i++) {
    mods.push_back(create_foo());
  }

  mods[0]->AddMetadataAttr("foo", 2, Attribute::AccessMode::kWrite);
  mods[1]->AddMetadataAttr("bar", 2, Attribute::AccessMode::kWrite);
  mods[2]->AddMetadataAttr("foo", 2, Attribute::AccessMode::kRead);
  mods[2]->AddMetadataAttr("bar", 2, Attribute::AccessMode::kRead);
  mods[3]->AddMetadataAttr("foo", 2, Attribute::AccessMode::kWrite);
  mods[4]->AddMetadataAttr("foo", 2, Attribute::AccessMode::kRead);
  mods[5]->AddMetadataAttr("bar", 2, Attribute::AccessMode::kWrite);
  mods[6]->AddMetadataAttr("bar", 2, Attribute::AccessMode::kRead);
  mods[6]->AddMetadataAttr("foo", 2, Attribute::AccessMode::kWrite);
  mods[7]->AddMetadataAttr("bar", 2, Attribute::AccessMode::kWrite);
  mods[8]->AddMetadataAttr("foo", 2, Attribute::AccessMode::kWrite);
  mods[9]->AddMetadataAttr("foo", 2, Attribute::AccessMode::kRead);
  mods[9]->AddMetadataAttr("bar", 2, Attribute::AccessMode::kRead);

  ModuleGraph::ConnectModules(mods[0], 0, mods[1], 0);
  ModuleGraph::ConnectModules(mods[1], 0, mods[2], 0);
  ModuleGraph::ConnectModules(mods[1], 1, mods[4], 0);
  ModuleGraph::ConnectModules(mods[0], 1, mods[4], 0);
  ModuleGraph::ConnectModules(mods[3], 0, mods[4], 0);
  ModuleGraph::ConnectModules(mods[4], 0, mods[5], 0);
  ModuleGraph::ConnectModules(mods[5], 0, mods[6], 0);
  ModuleGraph::ConnectModules(mods[7], 0, mods[6], 0);
  ModuleGraph::ConnectModules(mods[7], 1, mods[8], 0);
  ModuleGraph::ConnectModules(mods[8], 0, mods[9], 0);

  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());

  // Check that every module was assigned valid, non-overlapping offsets
  for (const auto &m : mods) {
    bool dummy_meta[kMetadataTotalSize] = {};
    size_t i = 0;
    for (const auto &attr : m->all_attrs()) {
      if (attr.mode != Attribute::AccessMode::kRead) {
        mt_offset_t offset = m->attr_offset(i);
        if (offset < 0) {
          EXPECT_EQ(m, mods[6]);
          EXPECT_EQ(1, i);
          EXPECT_STREQ("foo", attr.name.c_str());
        } else {
          for (size_t j = 0; j < attr.size; j++) {
            ASSERT_FALSE(dummy_meta[offset + j]);
            dummy_meta[offset + j] = true;
          }
        }
      }
      i++;
    }
  }

  // This write is never read by anyone
  ASSERT_EQ(kMetadataOffsetNoWrite, mods[6]->attr_offset(1));

  // Check that those assignments conform to the way the modules are connected
  ASSERT_NE(mods[0]->attr_offset(0), mods[1]->attr_offset(0));
  ASSERT_EQ(mods[0]->attr_offset(0), mods[2]->attr_offset(0));
  ASSERT_NE(mods[1]->attr_offset(0), mods[4]->attr_offset(0));
  ASSERT_EQ(mods[0]->attr_offset(0), mods[4]->attr_offset(0));
  ASSERT_EQ(mods[3]->attr_offset(0), mods[4]->attr_offset(0));
  ASSERT_EQ(mods[5]->attr_offset(0), mods[6]->attr_offset(0));
  ASSERT_NE(mods[5]->attr_offset(0), mods[6]->attr_offset(1));
  ASSERT_EQ(mods[7]->attr_offset(0), mods[6]->attr_offset(0));
  ASSERT_NE(mods[7]->attr_offset(0), mods[6]->attr_offset(1));
  ASSERT_NE(mods[7]->attr_offset(0), mods[8]->attr_offset(0));
  ASSERT_EQ(mods[7]->attr_offset(0), mods[9]->attr_offset(1));
  ASSERT_EQ(mods[8]->attr_offset(0), mods[9]->attr_offset(0));
}

// In this strange edge case, m4 should not clobber m3's write of attribute "h".
// We force a strange ordering of lexographic ordering of modules so to yield a
// non-monotonic ordering of the degrees of the scope componenets corresponding
// to each metadata attribute. ComputeMetadataOffsets() should sort them before
// handing them to AssignOffsets(). If it doesn't, bad things happen.
TEST_F(MetadataTest, ScopeComponentDegreeOrder) {
  ModuleGraph::DestroyAllModules();
  m0 = create_foo("foo5");
  m1 = create_foo("foo3");
  Module *m2 = create_foo("foo6");
  Module *m3 = create_foo("foo2");
  Module *m4 = create_foo("foo4");
  Module *m5 = create_foo("foo1");
  ASSERT_NE(nullptr, m0);
  ASSERT_NE(nullptr, m1);
  ASSERT_NE(nullptr, m2);
  ASSERT_NE(nullptr, m3);
  ASSERT_NE(nullptr, m4);
  ASSERT_NE(nullptr, m5);

  m0->AddMetadataAttr("a", 4, Attribute::AccessMode::kWrite);
  m0->AddMetadataAttr("b", 4, Attribute::AccessMode::kWrite);
  m0->AddMetadataAttr("c", 4, Attribute::AccessMode::kWrite);
  ModuleGraph::ConnectModules(m0, 0, m1, 0);

  m1->AddMetadataAttr("a", 4, Attribute::AccessMode::kWrite);
  m1->AddMetadataAttr("b", 4, Attribute::AccessMode::kWrite);
  m1->AddMetadataAttr("c", 4, Attribute::AccessMode::kWrite);
  ModuleGraph::ConnectModules(m1, 0, m2, 0);

  m2->AddMetadataAttr("a", 4, Attribute::AccessMode::kRead);
  m2->AddMetadataAttr("b", 4, Attribute::AccessMode::kRead);
  m2->AddMetadataAttr("c", 4, Attribute::AccessMode::kRead);
  m2->AddMetadataAttr("d", 4, Attribute::AccessMode::kWrite);
  m2->AddMetadataAttr("e", 4, Attribute::AccessMode::kWrite);
  m2->AddMetadataAttr("f", 1, Attribute::AccessMode::kWrite);
  ModuleGraph::ConnectModules(m2, 0, m3, 0);

  m3->AddMetadataAttr("d", 4, Attribute::AccessMode::kRead);
  m3->AddMetadataAttr("e", 4, Attribute::AccessMode::kRead);
  m3->AddMetadataAttr("f", 1, Attribute::AccessMode::kRead);
  m3->AddMetadataAttr("g", 4, Attribute::AccessMode::kWrite);
  m3->AddMetadataAttr("h", 2, Attribute::AccessMode::kWrite);
  ModuleGraph::ConnectModules(m3, 0, m4, 0);

  m4->AddMetadataAttr("i", 6, Attribute::AccessMode::kWrite);
  m4->AddMetadataAttr("j", 6, Attribute::AccessMode::kWrite);
  ModuleGraph::ConnectModules(m4, 0, m5, 0);

  m5->AddMetadataAttr("i", 6, Attribute::AccessMode::kRead);
  m5->AddMetadataAttr("j", 6, Attribute::AccessMode::kRead);
  m5->AddMetadataAttr("h", 2, Attribute::AccessMode::kRead);

  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());

  ASSERT_TRUE((m4->attr_offset(0) >= m3->attr_offset(4) + 2) ||
              (m4->attr_offset(0) + 6 <= m3->attr_offset(4)));

  ASSERT_TRUE((m4->attr_offset(1) >= m3->attr_offset(4) + 2) ||
              (m4->attr_offset(1) + 6 <= m3->attr_offset(4)));
}

// The pipelines of the old bessctl/conf/metadata/*.bess scripts (which only
// logged what the allocator chose), checked against what a packet sees: every
// path from a module nothing feeds is walked (a module at most twice, for
// cycles) with the metadata bytes owned by the write that last stored them.
// A read with an offset must find its attribute's latest write on that path
// in every byte: no path without a writer, no other attribute's write over it,
// no older write of the attribute. A reader that every path reaches through a
// writer must get an offset (the allocator must not give up on it).
struct GraphUse {
  const char* attr;
  size_t size;
  Attribute::AccessMode mode;
};
struct GraphNode {
  const char* name;
  std::vector<GraphUse> uses;
};
struct GraphEdge {
  const char* from;
  gate_idx_t ogate;
  const char* to;
  gate_idx_t igate;
};
struct MetadataGraph {
  const char* name;
  std::vector<GraphNode> nodes;
  std::vector<GraphEdge> edges;
};
void PrintTo(const MetadataGraph& graph, std::ostream* os) {
  *os << graph.name;
}

constexpr auto kR = Attribute::AccessMode::kRead;
constexpr auto kW = Attribute::AccessMode::kWrite;
constexpr auto kU = Attribute::AccessMode::kUpdate;

const std::vector<MetadataGraph>& MetadataGraphs() {
  static const std::vector<MetadataGraph> graphs = {
      {"simplest",
       {{"a", {{"foo", 2, kW}}}, {"b", {{"foo", 2, kR}}}},
       {{"a", 0, "b", 0}}},
      {"read_before_write",
       {{"a", {{"foo", 2, kR}}}, {"b", {{"foo", 2, kW}}}},
       {{"a", 0, "b", 0}}},
      {"two_scopes_in_a_chain",
       {{"a", {{"foo", 2, kW}}},
        {"b", {{"foo", 2, kR}}},
        {"c", {}},
        {"d", {{"foo", 2, kW}}},
        {"e", {{"foo", 2, kR}}}},
       {{"a", 0, "b", 0},
        {"b", 0, "c", 0},
        {"c", 0, "d", 0},
        {"d", 0, "e", 0}}},
      {"four_32_byte_attrs",
       {{"a", {{"a", 32, kW}, {"b", 32, kW}, {"c", 32, kW}, {"d", 32, kW}}},
        {"b", {{"a", 32, kR}, {"b", 32, kR}, {"c", 32, kR}, {"d", 32, kR}}}},
       {{"a", 0, "b", 0}}},
      {"one_scope_three_writers",
       {{"a", {{"foo", 2, kW}}},
        {"b", {{"foo", 2, kW}}},
        {"c", {{"foo", 2, kW}}},
        {"d", {{"foo", 2, kR}}},
        {"e", {{"foo", 2, kR}}},
        {"f", {{"foo", 2, kR}}},
        {"g", {{"foo", 2, kR}}}},
       {{"a", 0, "d", 0},
        {"a", 1, "e", 0},
        {"b", 0, "e", 0},
        {"b", 1, "f", 0},
        {"c", 0, "f", 0},
        {"c", 1, "g", 0}}},
      {"update",
       {{"a", {{"foo", 2, kW}}},
        {"b", {{"foo", 2, kU}}},
        {"c", {{"foo", 2, kR}}}},
       {{"a", 0, "b", 0}, {"b", 0, "c", 0}}},
      {"update_branches",
       {{"a", {{"foo", 2, kW}}},
        {"b", {{"foo", 2, kU}}},
        {"c", {{"foo", 2, kR}}},
        {"d", {{"foo", 2, kU}}},
        {"e", {{"foo", 2, kR}}},
        {"f", {{"foo", 2, kW}}},
        {"g", {{"foo", 2, kU}}},
        {"h", {{"foo", 2, kR}}}},
       {{"a", 0, "b", 0},
        {"b", 0, "c", 0},
        {"a", 1, "d", 0},
        {"d", 0, "e", 0},
        {"f", 0, "d", 0},
        {"f", 1, "g", 0},
        {"g", 0, "h", 0}}},
      {"reader_alone", {{"a", {{"foo", 2, kR}}}}, {}},
      {"complex_single_attr",
       {{"a", {{"foo", 2, kW}}},
        {"b", {{"foo", 2, kW}}},
        {"c", {{"foo", 2, kR}}},
        {"d", {{"foo", 2, kW}}},
        {"e", {{"foo", 2, kR}}},
        {"f", {{"foo", 2, kW}}},
        {"g", {}},
        {"h", {{"foo", 2, kW}}},
        {"i", {{"foo", 2, kR}}},
        {"j", {}}},
       {{"a", 0, "b", 0},
        {"b", 0, "c", 0},
        {"b", 1, "e", 0},
        {"a", 1, "e", 0},
        {"d", 0, "e", 0},
        {"f", 0, "g", 0},
        {"g", 0, "e", 0},
        {"h", 0, "g", 0},
        {"h", 1, "i", 0},
        {"i", 0, "j", 0}}},
      {"complex_two_attrs",
       {{"a", {{"foo", 2, kW}}},
        {"b", {{"bar", 2, kW}}},
        {"c", {{"foo", 2, kR}, {"bar", 2, kR}}},
        {"d", {{"foo", 2, kW}}},
        {"e", {{"foo", 2, kR}}},
        {"f", {{"bar", 2, kW}}},
        {"g", {{"bar", 2, kR}, {"foo", 2, kW}}},
        {"h", {{"bar", 2, kW}}},
        {"i", {{"foo", 2, kW}}},
        {"j", {{"foo", 2, kR}, {"bar", 2, kR}}}},
       {{"a", 0, "b", 0},
        {"b", 0, "c", 0},
        {"b", 1, "e", 0},
        {"a", 1, "e", 0},
        {"d", 0, "e", 0},
        {"f", 0, "g", 0},
        {"g", 0, "e", 0},
        {"h", 0, "g", 0},
        {"h", 1, "i", 0},
        {"i", 0, "j", 0}}},
      {"complex_five_attrs",
       {{"a", {{"x1", 2, kW}, {"x2", 2, kW}}},
        {"b", {{"x1", 2, kW}}},
        {"c", {}},
        {"d", {{"x1", 2, kR}}},
        {"e", {{"x2", 2, kW}}},
        {"f", {}},
        {"g", {{"x2", 2, kR}}},
        {"h", {{"x2", 2, kW}, {"x3", 2, kW}}},
        {"i", {{"x2", 2, kR}}},
        {"j", {{"x2", 2, kW}, {"x3", 2, kW}, {"x4", 2, kW}}},
        {"k", {{"x4", 2, kR}}},
        {"l", {{"x5", 2, kW}}},
        {"m", {{"x5", 2, kR}}}},
       {{"a", 0, "b", 0},
        {"b", 0, "c", 0},
        {"c", 0, "d", 0},
        {"e", 0, "b", 0},
        {"b", 1, "f", 0},
        {"f", 0, "g", 0},
        {"f", 1, "i", 0},
        {"j", 0, "h", 0},
        {"h", 0, "f", 0},
        {"h", 1, "k", 0},
        {"j", 1, "k", 0},
        {"k", 0, "i", 0},
        {"k", 1, "l", 0},
        {"l", 0, "m", 0}}},
      {"two_paths_to_a_reader",
       {{"a", {{"foo", 2, kW}}}, {"b", {}}, {"c", {{"foo", 2, kR}}}},
       {{"a", 0, "b", 0}, {"b", 0, "c", 0}, {"a", 1, "c", 0}}},
      {"complex_single_attr_components",
       {{"a", {{"foo", 2, kW}}},
        {"b", {{"foo", 2, kW}}},
        {"c", {{"foo", 2, kR}}},
        {"d", {{"foo", 2, kW}}},
        {"e", {{"foo", 2, kR}}},
        {"f", {{"foo", 2, kW}}},
        {"g", {}},
        {"h", {{"foo", 2, kW}}},
        {"i", {{"foo", 2, kR}}},
        {"j", {}}},
       {{"a", 0, "b", 0},
        {"b", 0, "c", 0},
        {"a", 1, "e", 0},
        {"d", 0, "e", 0},
        {"f", 0, "g", 0},
        {"g", 0, "e", 0},
        {"h", 0, "g", 0},
        {"h", 1, "i", 0},
        {"i", 0, "j", 0}}},
      {"update_then_overwrite",
       {{"a", {{"foo", 2, kW}}},
        {"b", {{"foo", 2, kU}}},
        {"c", {{"foo", 2, kW}}}},
       {{"a", 0, "b", 0}, {"b", 0, "c", 0}}},
      // Not from a script: an update that nothing upstream wrote.
      {"update_without_writer",
       {{"a", {{"foo", 2, kU}}}, {"b", {{"foo", 2, kR}}}},
       {{"a", 0, "b", 0}}},
      {"unread_write",
       {{"a", {{"foo", 2, kW}}},
        {"b", {{"bar", 2, kW}}},
        {"c", {{"bar", 2, kR}}}},
       {{"a", 0, "b", 0}, {"b", 0, "c", 0}}},
      {"four_sizes",
       {{"a", {{"foo", 2, kW}, {"bar", 3, kW}, {"baz", 5, kW}, {"quz", 8, kW}}},
        {"b",
         {{"foo", 2, kR}, {"bar", 3, kR}, {"baz", 5, kR}, {"quz", 8, kR}}}},
       {{"a", 0, "b", 0}}},
      // Input gates that are not contiguous (gate 4, no gate 3) once crashed
      // the traversal on a null gate.
      {"sparse_gates",
       {{"a", {{"foo", 2, kW}, {"bar", 3, kW}, {"baz", 5, kW}, {"quz", 8, kW}}},
        {"b", {{"foo", 2, kR}, {"bar", 3, kR}}},
        {"c", {{"baz", 5, kR}, {"quz", 8, kR}}}},
       {{"a", 1, "b", 2}, {"b", 3, "c", 4}}},
      {"two_writers_one_read",
       {{"a", {{"foo", 2, kW}}},
        {"b", {{"bar", 2, kW}}},
        {"c", {{"bar", 2, kR}}}},
       {{"a", 0, "c", 0}, {"b", 0, "c", 0}}},
      // No module is unfed, so no path is walked: this checks termination.
      {"cycle",
       {{"a", {{"foo", 2, kW}}},
        {"b", {{"foo", 2, kR}}},
        {"c", {{"foo", 2, kR}}}},
       {{"a", 0, "b", 0}, {"b", 0, "c", 0}, {"c", 0, "a", 0}}},
      {"cycle_with_an_entry",
       {{"a", {{"foo", 2, kW}}},
        {"b", {{"foo", 2, kR}}},
        {"c", {}},
        {"d", {{"foo", 2, kR}}},
        {"e", {}}},
       {{"a", 0, "b", 0},
        {"b", 0, "c", 0},
        {"c", 0, "d", 0},
        {"c", 1, "a", 0},
        {"d", 1, "a", 0},
        {"e", 0, "a", 0}}},
  };
  return graphs;
}

// MetadataTest (a friend of Pipeline) resets the pipeline; its two modules
// have no attributes or connections and are not walked.
class MetadataGraphTest : public MetadataTest,
                          public ::testing::WithParamInterface<MetadataGraph> {
};

struct Arrival {
  bool every_path_written = true;
  bool reached = false;
};

struct PathState {
  std::vector<int> owner = std::vector<int>(kMetadataTotalSize, -1);
  std::map<std::string, int> last_write;  // attr -> write id
  std::map<const Module*, int> visits;
  int next_id = 0;
};

TEST_P(MetadataGraphTest, ReadsFindTheirLatestWrite) {
  const MetadataGraph& graph = GetParam();
  std::map<std::string, Module*> modules;
  std::map<const Module*, std::string> names;
  for (const GraphNode& node : graph.nodes) {
    Module* m = ::create_foo(std::string(graph.name) + "_" + node.name);
    ASSERT_NE(nullptr, m);
    for (const GraphUse& use : node.uses) {
      ASSERT_GE(m->AddMetadataAttr(use.attr, use.size, use.mode), 0);
    }
    modules[node.name] = m;
    names[m] = node.name;
  }
  std::map<const Module*, std::vector<const Module*>> next;
  std::set<const Module*> fed;
  for (const GraphEdge& e : graph.edges) {
    Module* from = modules.at(e.from);
    Module* to = modules.at(e.to);
    ASSERT_EQ(0, ModuleGraph::ConnectModules(from, e.ogate, to, e.igate));
    next[from].push_back(to);
    fed.insert(to);
  }
  ASSERT_EQ(0, default_pipeline.ComputeMetadataOffsets());

  std::map<std::pair<const Module*, std::string>, Arrival> arrivals;
  std::function<void(const Module*, PathState)> walk = [&](const Module* m,
                                                           PathState s) {
    if (++s.visits[m] > 2) {
      return;
    }
    size_t i = 0;
    for (const auto& attr : m->all_attrs()) {
      const mt_offset_t offset = m->attr_offset(i++);
      bool written_upstream = false;
      if (attr.mode == kR || attr.mode == kU) {
        Arrival& a = arrivals[{m, attr.name}];
        a.reached = true;
        const auto written = s.last_write.find(attr.name);
        written_upstream = written != s.last_write.end();
        a.every_path_written &= written_upstream;
        if (offset >= 0) {
          ASSERT_NE(written, s.last_write.end())
              << names[m] << " reads " << attr.name << " at " << int{offset}
              << " on a path with no writer";
          for (size_t b = 0; b < attr.size; b++) {
            ASSERT_EQ(written->second, s.owner[offset + b])
                << names[m] << " reads " << attr.name << " at " << int{offset}
                << ": byte " << offset + b
                << " holds another write than its latest";
          }
        }
      }
      // An update with nothing written upstream writes nothing valid: the
      // allocator starts scopes at writes only.
      if (attr.mode == kU && !written_upstream) {
        s.last_write.erase(attr.name);
      } else if (attr.mode == kW || attr.mode == kU) {
        const int id = s.next_id++;
        s.last_write[attr.name] = id;
        if (offset >= 0) {
          for (size_t b = 0; b < attr.size; b++) {
            s.owner[offset + b] = id;
          }
        }
      }
    }
    const auto out = next.find(m);
    if (out != next.end()) {
      for (const Module* n : out->second) {
        walk(n, s);
      }
    }
  };
  for (const auto& [name, m] : modules) {
    if (fed.count(m) == 0) {
      walk(m, PathState{});
    }
  }

  for (const auto& [key, arrival] : arrivals) {
    const Module* m = key.first;
    size_t i = 0;
    for (const auto& attr : m->all_attrs()) {
      if (attr.name == key.second && arrival.every_path_written) {
        EXPECT_GE(m->attr_offset(i), 0)
            << names[m] << " reads " << attr.name
            << " after a write on every path but got no offset";
      }
      i++;
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    Pipelines, MetadataGraphTest, ::testing::ValuesIn(MetadataGraphs()),
    [](const ::testing::TestParamInfo<MetadataGraph>& info) {
      return std::string(info.param.name);
    });

}  // namespace metadata
}  // namespace bess
