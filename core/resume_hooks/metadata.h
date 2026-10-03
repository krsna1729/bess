// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_RESUME_HOOKS_METADATA_
#define BESS_RESUME_HOOKS_METADATA_

#include "message.h"
#include "resume_hook.h"
#include "worker.h"

// SetupMetadata computes read/write offsets for packet metadata attributes.
class SetupMetadata final : public bess::ResumeHook {
 public:
  SetupMetadata();

  CommandResponse Init(const bess::pb::EmptyArg &);

  void Run() override;

  static constexpr uint16_t kPriority = 0;
  static const std::string kName;
};

#endif  // BESS_RESUME_HOOKS_METADATA_
