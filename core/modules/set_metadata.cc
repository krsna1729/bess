// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "set_metadata.h"

#include "utils/bits.h"
#include "utils/endian.h"

using bess::metadata::mt_offset_t;

const Commands SetMetadata::cmds = {
    {"get_initial_arg", "EmptyArg",
     MODULE_CMD_FUNC(&SetMetadata::GetInitialArg), Command::THREAD_SAFE, "",
     Command::LEGACY_ONLY, Command::READS},
};

CommandResponse SetMetadata::AddAttrOne(
    const bess::pb::SetMetadataArg_Attribute &attr) {
  std::string name;
  size_t size = 0;
  int offset = -1;
  int rshift_bytes = attr.rshift_bits() / 8;
  value_t value;
  mask_t mask;
  bool do_mask = false;

  int ret;

  if (!attr.name().length()) {
    return CommandFailure(EINVAL, "'name' field is missing");
  }
  name = attr.name();
  size = attr.size();
  do_mask = attr.mask().length() > 0;

  if (size < 1 || size > kMetadataAttrMaxSize) {
    return CommandFailure(EINVAL, "'size' must be 1-%zu", kMetadataAttrMaxSize);
  }

  if (do_mask && attr.offset() < 0) {
    return CommandFailure(
        EINVAL, "'mask' may only be set when copying metadata from a packet.");
  }

  if (attr.rshift_bits() && attr.offset() < 0) {
    return CommandFailure(
        EINVAL,
        "'rshift_bits' may only be set when copying metadata from a packet.");
  }

  // All metadata values are stored in a reserved area of packet data.
  // Note they are stored in network order. This does not mean that you need
  // to pass endian-swapped values in value_int to the module. Value is just
  // value, and it has nothing to do with endianness (how an integer value is
  // stored in memory). value_bin is a short stream of bytes, which means that
  // its data will never be reordered.
  if (attr.value_case() == bess::pb::SetMetadataArg_Attribute::kValueInt) {
    if (!bess::utils::uint64_to_bin(&value, attr.value_int(), size, true)) {
      return CommandFailure(EINVAL,
                            "'value_int' field has not a "
                            "correct %zu-byte value",
                            size);
    }
  } else if (attr.value_case() ==
             bess::pb::SetMetadataArg_Attribute::kValueBin) {
    if (attr.value_bin().length() != size) {
      return CommandFailure(EINVAL,
                            "'value_bin' field has not a "
                            "correct %zu-byte value",
                            size);
    }
    bess::utils::Copy(&value, attr.value_bin().data(), size);
  } else {
    offset = attr.offset();
    if (offset < 0 || offset + size >= SNBUF_DATA) {
      return CommandFailure(EINVAL, "invalid packet offset");
    }
    if (rshift_bytes * 8 != attr.rshift_bits()) {
      return CommandFailure(EINVAL, "'rshift_bits' must be a multiple of 8");
    }
    if (static_cast<size_t>(std::abs(rshift_bytes)) >= size) {
      return CommandFailure(EINVAL, "'rshift_bits' must be in (-%zu, %zu)",
                            8 * size, 8 * size);
    }
    if (do_mask) {
      if (attr.mask().length() != size) {
        return CommandFailure(EINVAL,
                              "'mask' field has not a "
                              "correct %zu-byte value",
                              size);
      }
      bess::utils::Copy(&mask, attr.mask().data(), size);
    }
  }

  ret = AddMetadataAttr(name, size,
                        bess::metadata::Attribute::AccessMode::kWrite);
  if (ret < 0) {
    return CommandFailure(-ret, "add_metadata_attr() failed");
  }

  attrs_.push_back({.name = name,
                    .value = value,
                    .mask = mask,
                    .offset = offset,
                    .size = size,
                    .do_mask = do_mask,
                    .shift = rshift_bytes});

  return CommandSuccess();
}

CommandResponse SetMetadata::Init(const bess::pb::SetMetadataArg &arg) {
  if (!arg.attrs_size()) {
    return CommandFailure(EINVAL, "'attrs' must be specified");
  }

  for (int i = 0; i < arg.attrs_size(); i++) {
    const auto &attr = arg.attrs(i);
    CommandResponse err;

    err = AddAttrOne(attr);
    if (err.error().code() != 0) {
      return err;
    }
  }

  init_arg_ = arg;
  return CommandSuccess();
}

CommandResponse SetMetadata::GetInitialArg(const bess::pb::EmptyArg &) {
  return CommandSuccess(init_arg_);
}

template <size_t N>  // N == 0: attr->size at run time
void SetMetadata::FromValue(bess::PacketBatch *batch, const Op &op) {
  const mt_offset_t mt_off = op.mt_offset;
  if constexpr (N != 0) {
    // A local copy: the stores below cannot alias it, so it stays in a
    // register.
    uint8_t value[N];
    std::memcpy(value, op.attr->value.bytes, N);
    for (bess::PacketRef pkt : batch->packets()) {
      std::memcpy(_ptr_attr_with_offset<uint8_t>(mt_off, pkt), value, N);
    }
  } else {
    const size_t size = op.attr->size;
    const value_t *value = &op.attr->value;
    for (bess::PacketRef pkt : batch->packets()) {
      bess::utils::CopySmall(_ptr_attr_with_offset<value_t>(mt_off, pkt), value, size);
    }
  }
}

template <size_t N, bool do_shift, bool do_mask>  // N == 0: attr->size at run time
void SetMetadata::FromPacket(bess::PacketBatch *batch, const Op &op) {
  const Attr &attr = *op.attr;
  const size_t size = N != 0 ? N : attr.size;
  const int shift = attr.shift;
  const int pkt_off = attr.offset;
  const mt_offset_t mt_off = op.mt_offset;

  for (bess::PacketRef pkt : batch->packets()) {
    const uint8_t *head = pkt.head_data<uint8_t *>(pkt_off);
    uint8_t *mt_ptr = _ptr_attr_with_offset<uint8_t>(mt_off, pkt);
    if constexpr (N != 0) {
      std::memcpy(mt_ptr, head, N);
    } else {
      bess::utils::CopySmall(mt_ptr, head, size);
    }
    if constexpr (do_shift) {
      if (shift > 0) {
        bess::utils::ShiftBytesRight(mt_ptr, size, shift);
      } else {
        bess::utils::ShiftBytesLeft(mt_ptr, size, -shift);
      }
    }
    if constexpr (do_mask) {
      bess::utils::MaskBytes(mt_ptr, attr.mask.bytes, size);
    }
  }
}

template <size_t N>
decltype(SetMetadata::Op::fn) SetMetadata::SelectSized(const Attr &attr) {
  if (attr.offset < 0) {
    return &FromValue<N>;
  }
  const bool shift = attr.shift != 0;
  if (shift && attr.do_mask) {
    return &FromPacket<N, true, true>;
  }
  if (shift) {
    return &FromPacket<N, true, false>;
  }
  if (attr.do_mask) {
    return &FromPacket<N, false, true>;
  }
  return &FromPacket<N, false, false>;
}

decltype(SetMetadata::Op::fn) SetMetadata::Select(const Attr &attr) {
  switch (attr.size) {
    case 1:
      return SelectSized<1>(attr);
    case 2:
      return SelectSized<2>(attr);
    case 4:
      return SelectSized<4>(attr);
    case 8:
      return SelectSized<8>(attr);
    default:
      return SelectSized<0>(attr);
  }
}

int SetMetadata::OnEvent(bess::Event event) {
  if (event != bess::Event::PreResume) {
    return -ENOTSUP;
  }
  std::vector<Op> ops;
  for (size_t i = 0; i < attrs_.size(); i++) {
    const mt_offset_t mt_offset = attr_offset(i);
    if (bess::metadata::IsValidOffset(mt_offset)) {
      ops.push_back({Select(attrs_[i]), mt_offset, &attrs_[i]});
    }
  }
  // PreResume also reaches a module whose worker is running (a resume of all
  // workers when some already run); the offsets are then the ones compiled,
  // and the list a worker may be reading is left alone.
  const auto same = [](const Op &a, const Op &b) {
    return a.fn == b.fn && a.mt_offset == b.mt_offset && a.attr == b.attr;
  };
  if (!std::equal(ops.begin(), ops.end(), ops_.begin(), ops_.end(), same)) {
    ops_ = std::move(ops);
  }
  return 0;
}

void SetMetadata::ProcessBatch(Context *ctx, bess::PacketBatch *batch) {
  for (const Op &op : ops_) {
    op.fn(batch, op);
  }
  RunNextModule(ctx, batch);
}

ADD_MODULE(SetMetadata, "setattr", "Set metadata attributes to packets")
