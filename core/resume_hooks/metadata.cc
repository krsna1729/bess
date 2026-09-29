// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "../metadata.h"
#include "metadata.h"

const std::string SetupMetadata::kName = "setup_metadata";

SetupMetadata::SetupMetadata() : bess::ResumeHook(kName, kPriority, true) {}

CommandResponse SetupMetadata::Init(const bess::pb::EmptyArg &) {
  return CommandSuccess();
}

void SetupMetadata::Run() {
  bess::metadata::default_pipeline.ComputeMetadataOffsets();
}

ADD_RESUME_HOOK(SetupMetadata)

bool __enable_SetupMetadata = []() {
  bool ret = bess::global_resume_hooks.emplace(new SetupMetadata()).second;
  if (!ret) {
    LOG(ERROR) << "Failed to enable SetupMetadata hook by default";
  }
  return ret;
}();
