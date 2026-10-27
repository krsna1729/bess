// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "metadata.h"
// Core metadata.h: "metadata.h" would find resume_hooks/metadata.h.
#include <metadata.h>

const std::string SetupMetadata::kName = "setup_metadata";

SetupMetadata::SetupMetadata() : bess::ResumeHook(kName, kPriority, true) {}

CommandResponse SetupMetadata::Init(const bess::pb::EmptyArg &) {
  return CommandSuccess();
}

std::optional<CommandResponse> SetupMetadata::Run() {
  const int ret = bess::metadata::default_pipeline.ComputeMetadataOffsets();
  if (ret == 0) {
    return std::nullopt;
  }

  const auto *allocation =
      bess::metadata::default_pipeline.first_blocking_allocation_failure();
  if (allocation != nullptr) {
    return CommandFailure(ENOSPC,
                          "metadata storage exhausted for attribute '%s' of "
                          "module '%s'",
                          allocation->attribute.c_str(),
                          allocation->module.c_str());
  }

  const bess::metadata::MetadataDeclaration *missing =
      bess::metadata::default_pipeline.first_unsatisfied_required();
  if (missing != nullptr) {
    return CommandFailure(EINVAL,
                          "required metadata attribute '%s' of module '%s' has "
                          "no usable graph binding",
                          missing->attribute.c_str(), missing->module.c_str());
  }
  return CommandFailure(-ret, "metadata offset computation failed");
}

ADD_RESUME_HOOK(SetupMetadata)

bool __enable_SetupMetadata = []() {
  bool ret = bess::global_resume_hooks.emplace(new SetupMetadata()).second;
  if (!ret) {
    LOG(ERROR) << "Failed to enable SetupMetadata hook by default";
  }
  return ret;
}();
