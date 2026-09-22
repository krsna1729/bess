// Copyright (c) 2026, Nefeli Networks, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// * Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// * Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// * Neither the names of the copyright holders nor the names of their
// contributors may be used to endorse or promote products derived from this
// software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_CLASSIFIER_RUNTIME_SCHEMA_H_
#define BESS_CLASSIFIER_RUNTIME_SCHEMA_H_

#include <cstddef>
#include <vector>

#include "classifier/classifier.h"

namespace bess::classifier {

struct RuntimeKeyField {
  SourceKind source = SourceKind::kPacket;
  size_t source_offset = 0;
  size_t key_offset = 0;
  size_t size = 0;
};

struct RuntimeResultField {
  size_t value_offset = 0;
  size_t destination_offset = 0;
  size_t size = 0;
};

// Resolved control-plane input. Metadata names and protobuf objects must be
// removed before constructing this value.
struct RuntimeClassifierSchema {
  size_t key_size = 0;
  size_t value_size = 0;
  BoundsPolicy bounds = BoundsPolicy::kCheck;
  std::vector<RuntimeKeyField> key_fields;
  std::vector<RuntimeResultField> result_fields;

  [[nodiscard]] ClassifierResult<void> Validate() const;
};

}  // namespace bess::classifier

#endif  // BESS_CLASSIFIER_RUNTIME_SCHEMA_H_
