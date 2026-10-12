// SPDX-License-Identifier: BSD-3-Clause

// YAML manifests: a YAML document read into, and written from, the same tree
// as JSON (json.h), so a manifest means the same in either and numbers stay
// as written. Read with libyaml; plain scalars resolve as YAML 1.2's core
// schema does:
//   null, Null, NULL, ~, empty      null
//   true/True/TRUE, false/...       booleans
//   decimal integers and floats     numbers, as written (a leading "+" and
//                                   leading zeros dropped, ".5" -> 0.5)
//   0x.. and 0o..                   numbers, in decimal (64 bits at most)
//   .inf, -.inf, .nan (any case     "Infinity", "-Infinity", "NaN": protobuf's
//   form the schema allows)         JSON names for them
//   anything else                   a string
// Quoted and block scalars, and those tagged !!str or !, are strings. Aliases
// are copies of their anchor's node. What a document expands to is bounded:
// kMaxBytes in total, counting kValueBytes per value plus its text (keys
// too), aliases' copies and anchors' kept copies included, each charged
// before it is made (an alias bomb is refused); and json::kMaxDepth deep,
// aliased subtrees included. Refused: other tags, merge keys (<<), keys that
// are not scalars, duplicate keys, more than one document.

#ifndef BESSCTL_YAML_IO_H_
#define BESSCTL_YAML_IO_H_

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include "json.h"

namespace bessctl::yaml {

inline constexpr size_t kValueBytes = 64;
inline constexpr size_t kMaxBytes = size_t{256} << 20;

// "line L, column C: what" on malformed or refused input.
std::expected<json::Value, std::string> Parse(std::string_view text);

// Block style, two spaces per level, ending in a newline. A string is
// written plain when it reads back as that string, else double-quoted.
std::string Dump(const json::Value &value);

}  // namespace bessctl::yaml

#endif  // BESSCTL_YAML_IO_H_
