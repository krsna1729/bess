// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "generic_encap.h"

#include <bit>
#include <cstring>

#include "packet_reshape.h"
#include "utils/endian.h"

static_assert(MAX_FIELD_SIZE <= sizeof(uint64_t),
              "field cannot be larger than 8 bytes");

#define MAX_HEADER_SIZE (MAX_FIELDS * MAX_FIELD_SIZE)

// A field's bytes are the low bytes of its uint64_t value, written with one
// 8-byte store (the next field overwrites the excess).
static_assert(std::endian::native == std::endian::little,
              "GenericEncap writes fields as little-endian words");

CommandResponse GenericEncap::AddFieldOne(
    const bess::pb::GenericEncapArg_EncapField &field, struct Field *f,
    int idx) {
  f->size = field.size();
  if (f->size < 1 || f->size > MAX_FIELD_SIZE) {
    return CommandFailure(EINVAL, "idx %d: 'size' must be 1-%d", idx,
                          MAX_FIELD_SIZE);
  }

  if (field.insertion_case() ==
      bess::pb::GenericEncapArg_EncapField::kAttribute) {
    const char *attr = field.attribute().c_str();
    f->attr_id = AddMetadataAttr(attr, f->size,
                                 bess::metadata::Attribute::AccessMode::kRead);
    if (f->attr_id < 0) {
      return CommandFailure(-f->attr_id, "idx %d: add_metadata_attr() failed",
                            idx);
    }
  } else if (field.insertion_case() ==
             bess::pb::GenericEncapArg_EncapField::kValue) {
    f->attr_id = -1;
    if (field.value().encoding_case() == bess::pb::FieldData::kValueInt) {
      if (!bess::utils::uint64_to_bin(&f->value, field.value().value_int(),
                                      f->size, 1)) {
        return CommandFailure(EINVAL,
                              "idx %d: "
                              "not a correct %d-byte value",
                              idx, f->size);
      }
    } else if (field.value().encoding_case() ==
               bess::pb::FieldData::kValueBin) {
      if (field.value().value_bin().size() != (size_t)f->size) {
        return CommandFailure(EINVAL, "idx %d: not a correct %d-byte mask", idx,
                              f->size);
      }
      bess::utils::Copy(reinterpret_cast<uint8_t *>(&(f->value)),
                        field.value().value_bin().c_str(),
                        field.value().value_bin().size());
    }
  } else {
    return CommandFailure(EINVAL, "idx %d: must specify 'value' or 'attribute'",
                          idx);
  }

  return CommandSuccess();
}

/* Takes a list of fields. Each field is either:
 *
 *  1. {'size': X, 'value': Y}		(for constant values)
 *  2. {'size': X, 'attribute': Y}	(for metadata attributes)
 *
 * e.g.: GenericEncap([{'size': 4, 'value': 0xdeadbeef},
 *                     {'size': 2, 'attribute': 'foo'},
 *                     {'size': 2, 'value': 0x1234}])
 * will prepend a 8-byte header:
 *    de ad be ef <xx> <xx> 12 34
 * where the 2-byte <xx> <xx> comes from the value of metadata attribute 'foo'
 * for each packet.
 */
CommandResponse GenericEncap::Init(const bess::pb::GenericEncapArg &arg) {
  if (arg.fields_size() > MAX_FIELDS) {  // fields_ holds MAX_FIELDS
    return CommandFailure(EINVAL, "at most %d fields", MAX_FIELDS);
  }
  int size_acc = 0;

  for (int i = 0; i < arg.fields_size(); i++) {
    const auto &field = arg.fields(i);
    CommandResponse err;
    struct Field *f = &fields_[i];

    f->pos = size_acc;

    err = AddFieldOne(field, f, i);
    if (err.error().code() != 0) {
      return err;
    }

    size_acc += f->size;
  }

  encap_size_ = size_acc;
  num_fields_ = arg.fields_size();

  return CommandSuccess();
}

// Prepends the header into each packet's headroom with
// packet::PrependWritable: a shared payload (a clone, an indirect mbuf) is
// copied first, as its headroom is the other holders' buffer too. A packet
// without the headroom, or whose copy fails, is dropped, never sent on
// without its header.
void GenericEncap::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  int cnt = batch->cnt();

  const auto encap_size = static_cast<size_t>(encap_size_);

  alignas(32) char headers[bess::PacketBatch::kMaxBurst][MAX_HEADER_SIZE];

  for (int i = 0; i < num_fields_; i++) {
    uint64_t value = fields_[i].value;

    int attr_id = fields_[i].attr_id;
    int offset = (attr_id >= 0) ? attr_offset(attr_id) : 0;

    // Index the packet's row: a pointer stepped from headers[0] leaves that
    // row, and GCC (at -mtune=graniterapids, for one) then reports a write
    // past it (-Wstringop-overflow, D-127).
    const int pos = fields_[i].pos;

    for (int j = 0; j < cnt; j++) {
      bess::PacketRef pkt = batch->packet(j);
      const uint64_t word =
          (attr_id < 0) ? value : get_attr_with_offset<uint64_t>(offset, pkt);
      std::memcpy(&headers[j][pos], &word, sizeof(word));
    }
  }

  const char (*next_header)[MAX_HEADER_SIZE] = headers;
  batch->KeepIf([&](bess::PacketHandle &handle, int) {
    const auto &packet_header = *next_header++;
    const auto header = bess::packet::PrependWritable(handle, encap_size);
    if (unlikely(!header)) {
      DropPacket(ctx, bess::PacketRef(handle));
      return false;
    }
    bess::utils::CopyInlined(header->data(), packet_header, encap_size);
    return true;
  });

  RunNextModule(ctx, batch);
}

ADD_MODULE(GenericEncap, "generic_encap",
           "encapsulates packets with constant values and metadata attributes")
