// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_GATE_HOOKS_PCAPNG_
#define BESS_GATE_HOOKS_PCAPNG_

#include "message.h"
#include "module.h"

#include "utils/fifo_opener.h"

class PcapngOpener final : public bess::utils::FifoOpener {
 public:
  PcapngOpener() : FifoOpener() {}
  bool InitFifo(int fd) override;
};

// Pcapng dumps copies of the packets seen by a gate (data + metadata) in
// pcapng format.  Useful for debugging.
class Pcapng final : public bess::GateHook {
 public:
  Pcapng();

  virtual ~Pcapng(){};

  CommandResponse Init(const bess::Gate *, const bess::pb::PcapngArg &);

  void ProcessBatch(const bess::PacketBatch *batch);

  static constexpr uint16_t kPriority = 2;
  static const std::string kName;

 private:
  struct Attr {
    // Attribute offset in the packet metadata.
    int md_offset;
    // Size in bytes of the attribute.
    size_t size;
    // Offset where this attribute hex dump should go inside `attr_template_`.
    size_t tmpl_offset;
  };

  // The opener instance for the FIFO for the captured packets.
  PcapngOpener opener_;

  // List of attributes to dump.
  std::vector<Attr> attrs_;
  // Preallocated string with attribute names and values.  For each packet,
  // we will change in place the values and send the string out, without
  // doing any memory allocation.
  std::vector<char> attr_template_;
};

#endif  // BESS_GATE_HOOKS_PCAPNG_
