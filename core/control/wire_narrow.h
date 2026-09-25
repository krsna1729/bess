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
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#ifndef BESS_CONTROL_WIRE_NARROW_H_
#define BESS_CONTROL_WIRE_NARROW_H_

#include <cerrno>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "control/control_error.h"

namespace bess {
namespace control {

// Narrows a wire integer into a smaller C++ field, or reports it out of range.
// Every API boundary (v1 and v2) converts through this, never a bare cast or
// an implicit conversion: once a value is narrowed the evidence is gone, and a
// wrapped value is usually a *valid* one (gate 65536 arrives as gate 0, 256
// queues as "one queue"), so no later validator can catch it.
//
// `index` (>= 0) names which element of a repeated field was wrong.
template <typename To, typename From>
ControlResult<To> WireNarrow(From value, const char *object, const char *field,
                             int index = -1) {
  static_assert(std::is_integral_v<To> && std::is_integral_v<From>);
  static_assert(std::is_unsigned_v<To> && std::is_unsigned_v<From>,
                "signed wire fields need their own range rule");
  if (value > std::numeric_limits<To>::max()) {
    ControlError error =
        index >= 0
            ? Err(EINVAL, "%s %d: '%s' %llu is out of range (at most %llu)",
                  object, index, field, static_cast<unsigned long long>(value),
                  static_cast<unsigned long long>(
                      std::numeric_limits<To>::max()))
            : Err(EINVAL, "%s: '%s' %llu is out of range (at most %llu)",
                  object, field, static_cast<unsigned long long>(value),
                  static_cast<unsigned long long>(
                      std::numeric_limits<To>::max()));
    error.object = object;
    error.field = field;
    return std::unexpected(error);
  }
  return static_cast<To>(value);
}

}  // namespace control
}  // namespace bess

#endif  // BESS_CONTROL_WIRE_NARROW_H_
