// SPDX-License-Identifier: BSD-3-Clause

// A JSON tree that keeps numbers exactly as written, so bessctl can rewrite a
// manifest before protobuf's JSON parser reads it. (protobuf's own tree,
// google.protobuf.Struct, holds numbers as doubles: a 64-bit value would
// change.) Strict RFC 8259 input; objects keep their members' order.

#ifndef BESSCTL_JSON_H_
#define BESSCTL_JSON_H_

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace bessctl::json {

struct Value;
struct Member;
using Array = std::vector<Value>;
// A struct, not std::pair: a vector of pairs holding the incomplete Value
// fails to instantiate inside Value's variant with clang and libstdc++.
using Object = std::vector<Member>;

// A number's literal text, already checked against the JSON grammar.
struct Number {
  std::string text;
  bool operator==(const Number &) const = default;
};

struct Value {
  std::variant<std::nullptr_t, bool, Number, std::string, Array, Object> v;

  bool operator==(const Value &) const = default;

  const Object *object() const { return std::get_if<Object>(&v); }
  Object *object() { return std::get_if<Object>(&v); }
  const Array *array() const { return std::get_if<Array>(&v); }
  Array *array() { return std::get_if<Array>(&v); }
  const std::string *string() const { return std::get_if<std::string>(&v); }
};

struct Member {
  std::string key;
  Value value;

  bool operator==(const Member &) const = default;
};

// The member `key` of an object; nullptr if absent or not an object.
const Value *Find(const Value &object, std::string_view key);
Value *Find(Value &object, std::string_view key);

// Removes the member `key` of an object, if present.
void Erase(Value &object, std::string_view key);

// "line L, column C: what" on malformed input. Nesting deeper than
// kMaxDepth is refused.
inline constexpr int kMaxDepth = 256;
std::expected<Value, std::string> Parse(std::string_view text);

// Compact with `indent` 0, else pretty with that many spaces per level.
std::string Dump(const Value &value, int indent = 0);

}  // namespace bessctl::json

#endif  // BESSCTL_JSON_H_
