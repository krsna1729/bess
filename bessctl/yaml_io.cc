// SPDX-License-Identifier: BSD-3-Clause

#include "yaml_io.h"

#include <yaml.h>

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace bessctl::yaml {

namespace {

constexpr std::string_view kStrTag = "tag:yaml.org,2002:str";
constexpr std::string_view kMapTag = "tag:yaml.org,2002:map";
constexpr std::string_view kSeqTag = "tag:yaml.org,2002:seq";

bool IsDigit(char c) {
  return c >= '0' && c <= '9';
}

bool OneOf(std::string_view s, std::initializer_list<std::string_view> all) {
  for (std::string_view a : all) {
    if (s == a) {
      return true;
    }
  }
  return false;
}

// [-+]? ( \.[0-9]+ | [0-9]+ ( \.[0-9]* )? ) ( [eE][-+]?[0-9]+ )?, as JSON's
// number grammar writes it.
std::optional<std::string> Decimal(std::string_view s) {
  size_t i = 0;
  std::string out;
  if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
    if (s[i] == '-') {
      out += '-';
    }
    i++;
  }
  const size_t int_start = i;
  while (i < s.size() && IsDigit(s[i])) {
    i++;
  }
  const std::string_view integer = s.substr(int_start, i - int_start);
  std::string_view fraction;
  if (i < s.size() && s[i] == '.') {
    const size_t start = ++i;
    while (i < s.size() && IsDigit(s[i])) {
      i++;
    }
    fraction = s.substr(start, i - start);
  }
  if (integer.empty() && fraction.empty()) {
    return std::nullopt;
  }
  std::string_view exponent;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    const size_t start = i++;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) {
      i++;
    }
    const size_t digits = i;
    while (i < s.size() && IsDigit(s[i])) {
      i++;
    }
    if (i == digits) {
      return std::nullopt;
    }
    exponent = s.substr(start, i - start);
  }
  if (i != s.size()) {
    return std::nullopt;
  }
  size_t zeros = 0;
  while (zeros + 1 < integer.size() && integer[zeros] == '0') {
    zeros++;
  }
  out += integer.empty() ? std::string_view("0") : integer.substr(zeros);
  if (!fraction.empty()) {
    out += '.';
    out += fraction;
  }
  out += exponent;
  return out;
}

// A plain scalar's value under the core schema; an error only for a 0x/0o
// integer beyond 64 bits.
std::expected<json::Value, std::string> Resolve(std::string_view s) {
  if (OneOf(s, {"", "~", "null", "Null", "NULL"})) {
    return json::Value{nullptr};
  }
  if (OneOf(s, {"true", "True", "TRUE"})) {
    return json::Value{true};
  }
  if (OneOf(s, {"false", "False", "FALSE"})) {
    return json::Value{false};
  }
  if (auto d = Decimal(s)) {
    return json::Value{json::Number{std::move(*d)}};
  }
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'o')) {
    const int base = s[1] == 'x' ? 16 : 8;
    const std::string_view digits = s.substr(2);
    uint64_t v = 0;
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v, base);
    if (end == digits.data() + digits.size()) {
      if (ec == std::errc::result_out_of_range) {
        return std::unexpected(std::format("{} does not fit in 64 bits", s));
      }
      return json::Value{json::Number{std::to_string(v)}};
    }
  }
  if (OneOf(s, {".inf", ".Inf", ".INF", "+.inf", "+.Inf", "+.INF"})) {
    return json::Value{std::string("Infinity")};
  }
  if (OneOf(s, {"-.inf", "-.Inf", "-.INF"})) {
    return json::Value{std::string("-Infinity")};
  }
  if (OneOf(s, {".nan", ".NaN", ".NAN"})) {
    return json::Value{std::string("NaN")};
  }
  return json::Value{std::string(s)};
}

// One libyaml event, deleted when it goes.
struct Event {
  yaml_event_t e{};
  bool owned = false;
  Event() = default;
  Event(const Event &) = delete;
  Event &operator=(const Event &) = delete;
  ~Event() {
    if (owned) {
      yaml_event_delete(&e);
    }
  }
};

class Reader {
 public:
  explicit Reader(std::string_view text) {
    yaml_parser_initialize(&parser_);
    yaml_parser_set_input_string(&parser_, reinterpret_cast<const unsigned char *>(text.data()),
                                 text.size());
  }
  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;
  ~Reader() { yaml_parser_delete(&parser_); }

  std::expected<json::Value, std::string> Document() {
    Event start;
    if (!Next(start, YAML_STREAM_START_EVENT)) {
      return std::unexpected(error_);
    }
    Event doc;
    if (!Next(doc)) {
      return std::unexpected(error_);
    }
    if (doc.e.type == YAML_STREAM_END_EVENT) {
      return json::Value{nullptr};  // an empty stream
    }
    Event node;
    if (!Next(node)) {
      return std::unexpected(error_);
    }
    auto value = Node(node, 0);
    if (!value) {
      return std::unexpected(error_);
    }
    Event end;
    if (!Next(end, YAML_DOCUMENT_END_EVENT)) {
      return std::unexpected(error_);
    }
    Event rest;
    if (!Next(rest)) {
      return std::unexpected(error_);
    }
    if (rest.e.type != YAML_STREAM_END_EVENT) {
      return std::unexpected(At(rest, "a manifest is one YAML document"));
    }
    return std::move(*value);
  }

 private:
  static std::string At(const yaml_mark_t &mark, std::string_view what) {
    return std::format("line {}, column {}: {}", mark.line + 1, mark.column + 1, what);
  }
  static std::string At(const Event &ev, std::string_view what) { return At(ev.e.start_mark, what); }

  bool Fail(std::string message) {
    error_ = std::move(message);
    return false;
  }

  bool Next(Event &ev) {
    if (!yaml_parser_parse(&parser_, &ev.e)) {
      std::string what = parser_.problem != nullptr ? parser_.problem : "malformed YAML";
      if (parser_.context != nullptr) {
        what += std::format(" ({})", parser_.context);
      }
      return Fail(At(parser_.problem_mark, what));
    }
    ev.owned = true;
    return true;
  }

  bool Next(Event &ev, yaml_event_type_t want) {
    if (!Next(ev)) {
      return false;
    }
    return ev.e.type == want || Fail(At(ev, "unexpected YAML structure"));
  }

  // Charges `bytes` to the budget before what they stand for is made: to the
  // document (`counter` bytes_) or to the anchors' kept copies (kept_).
  bool Charge(size_t bytes, const Event &ev, size_t &counter) {
    if (bytes > kMaxBytes - bytes_ - kept_) {
      return Fail(At(ev, std::format("expands beyond {} MiB (aliases and anchors are copies)",
                                     kMaxBytes >> 20)));
    }
    counter += bytes;
    return true;
  }
  bool Charge(size_t bytes, const Event &ev) { return Charge(bytes, ev, bytes_); }

  static std::string_view View(const yaml_char_t *s) {
    return s == nullptr ? std::string_view() : std::string_view(reinterpret_cast<const char *>(s));
  }

  bool TooDeep(int depth, const Event &ev) {
    return depth > json::kMaxDepth &&
           !Fail(At(ev, std::format("nested deeper than {}", json::kMaxDepth)));
  }

  // The node `ev` starts; consumes its events.
  std::optional<json::Value> Node(Event &ev, int depth) {
    if (TooDeep(depth, ev)) {
      return std::nullopt;
    }
    const yaml_char_t *anchor = nullptr;
    switch (ev.e.type) {
      case YAML_ALIAS_EVENT: {
        const auto it = anchors_.find(View(ev.e.data.alias.anchor));
        if (it == anchors_.end()) {
          Fail(At(ev, std::format("alias *{} names no complete node before it",
                                  View(ev.e.data.alias.anchor))));
          return std::nullopt;
        }
        const Anchored &a = it->second;
        if (TooDeep(depth + a.height, ev) || !Charge(a.bytes, ev)) {
          return std::nullopt;
        }
        deepest_ = std::max(deepest_, depth + a.height);
        return a.value;
      }
      case YAML_SCALAR_EVENT:
        anchor = ev.e.data.scalar.anchor;
        break;
      case YAML_SEQUENCE_START_EVENT:
        anchor = ev.e.data.sequence_start.anchor;
        break;
      case YAML_MAPPING_START_EVENT:
        anchor = ev.e.data.mapping_start.anchor;
        break;
      default:
        Fail(At(ev, "unexpected YAML structure"));
        return std::nullopt;
    }
    // This node's size and height, for an anchor on it.
    const size_t bytes_before = bytes_;
    const int deepest_before = deepest_;
    deepest_ = depth;
    std::optional<json::Value> value = ev.e.type == YAML_SCALAR_EVENT ? Scalar(ev)
                                       : ev.e.type == YAML_SEQUENCE_START_EVENT
                                           ? Sequence(ev, depth)
                                           : Mapping(ev, depth);
    const int height = deepest_ - depth;
    deepest_ = std::max(deepest_before, deepest_);
    if (value && anchor != nullptr) {
      // The kept copy is charged too: nested anchors would otherwise keep
      // one copy of the innermost node per level for free.
      const size_t bytes = bytes_ - bytes_before;
      if (!Charge(bytes, ev, kept_)) {
        return std::nullopt;
      }
      anchors_.insert_or_assign(std::string(View(anchor)), Anchored{*value, bytes, height});
    }
    return value;
  }

  std::optional<json::Value> Scalar(const Event &ev) {
    const auto &s = ev.e.data.scalar;
    const std::string_view text(reinterpret_cast<const char *>(s.value), s.length);
    const std::string_view tag = View(s.tag);
    std::optional<json::Value> value;
    if (!tag.empty() && tag != "!" && tag != kStrTag) {
      Fail(At(ev, std::format("tag {} is not supported (only !!str)", tag)));
      return std::nullopt;
    }
    if (!Charge(kValueBytes + text.size(), ev)) {
      return std::nullopt;
    }
    if (tag.empty() && s.style == YAML_PLAIN_SCALAR_STYLE) {
      auto resolved = Resolve(text);
      if (!resolved) {
        Fail(At(ev, resolved.error()));
        return std::nullopt;
      }
      value = std::move(*resolved);
    } else {
      value = json::Value{std::string(text)};
    }
    return value;
  }

  bool CollectionTag(const Event &ev, const yaml_char_t *tag, std::string_view standard) {
    const std::string_view t = View(tag);
    return t.empty() || t == standard ||
           Fail(At(ev, std::format("tag {} is not supported", t)));
  }

  std::optional<json::Value> Sequence(const Event &start, int depth) {
    if (!CollectionTag(start, start.e.data.sequence_start.tag, kSeqTag) ||
        !Charge(kValueBytes, start)) {
      return std::nullopt;
    }
    json::Array items;
    while (true) {
      Event ev;
      if (!Next(ev)) {
        return std::nullopt;
      }
      if (ev.e.type == YAML_SEQUENCE_END_EVENT) {
        break;
      }
      auto item = Node(ev, depth + 1);
      if (!item) {
        return std::nullopt;
      }
      items.push_back(std::move(*item));
    }
    return json::Value{std::move(items)};
  }

  std::optional<json::Value> Mapping(const Event &start, int depth) {
    if (!CollectionTag(start, start.e.data.mapping_start.tag, kMapTag) ||
        !Charge(kValueBytes, start)) {
      return std::nullopt;
    }
    json::Object members;
    std::set<std::string, std::less<>> keys;
    while (true) {
      Event key;
      if (!Next(key)) {
        return std::nullopt;
      }
      if (key.e.type == YAML_MAPPING_END_EVENT) {
        break;
      }
      if (key.e.type != YAML_SCALAR_EVENT) {
        Fail(At(key, "a key must be a scalar"));
        return std::nullopt;
      }
      const auto &s = key.e.data.scalar;
      std::string name(reinterpret_cast<const char *>(s.value), s.length);
      if (!Charge(name.size(), key)) {
        return std::nullopt;
      }
      if (name == "<<" && s.style == YAML_PLAIN_SCALAR_STYLE) {
        Fail(At(key, "merge keys (<<) are not supported"));
        return std::nullopt;
      }
      if (!keys.insert(name).second) {
        Fail(At(key, std::format("duplicate key \"{}\"", name)));
        return std::nullopt;
      }
      Event value;
      if (!Next(value)) {
        return std::nullopt;
      }
      auto v = Node(value, depth + 1);
      if (!v) {
        return std::nullopt;
      }
      members.push_back(json::Member{std::move(name), std::move(*v)});
    }
    return json::Value{std::move(members)};
  }

  // An anchored node: a copy, its size (as charged) and its height.
  struct Anchored {
    json::Value value;
    size_t bytes;
    int height;
  };

  yaml_parser_t parser_{};
  std::map<std::string, Anchored, std::less<>> anchors_;
  size_t bytes_ = 0;  // the document's values, aliases' copies included
  size_t kept_ = 0;   // the anchors' kept copies
  int deepest_ = 0;  // the deepest level reached in the node being read
  std::string error_;
};

// A string written plain reads back as itself, also to YAML 1.1 readers
// (yes/no/on/off are their booleans).
bool PlainSafe(std::string_view s) {
  if (s.empty()) {
    return false;
  }
  const char first = s[0];
  if (!((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') || first == '_' ||
        first == '/')) {
    return false;
  }
  for (const char c : s) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || IsDigit(c) ||
                    std::strchr("_./@+-", c) != nullptr;
    if (!ok) {
      return false;
    }
  }
  if (OneOf(s, {"y", "Y", "yes", "Yes", "YES", "n", "N", "no", "No", "NO", "on", "On", "ON",
                "off", "Off", "OFF"})) {
    return false;
  }
  const auto resolved = Resolve(s);
  return resolved && resolved->string() != nullptr && *resolved->string() == s;
}

// The code point of a two- or three-byte UTF-8 sequence at s[i] that a YAML
// reader folds as a line break (U+0085, U+2028, U+2029) or refuses or may
// take for a byte order mark (C1 controls, U+FEFF, U+FFFE, U+FFFF), with its
// length; 0 for anything else.
std::pair<uint32_t, size_t> Unprintable(std::string_view s, size_t i) {
  const auto at = [&](size_t k) {
    return i + k < s.size() ? static_cast<unsigned char>(s[i + k]) : 0u;
  };
  if (at(0) == 0xc2 && at(1) >= 0x80 && at(1) <= 0x9f) {
    return {at(1), 2};
  }
  if (at(0) == 0xe2 && at(1) == 0x80 && (at(2) == 0xa8 || at(2) == 0xa9)) {
    return {0x2000u | (at(2) - 0x80u), 3};
  }
  if (at(0) == 0xef && at(1) == 0xbb && at(2) == 0xbf) {
    return {0xfeff, 3};
  }
  if (at(0) == 0xef && at(1) == 0xbf && (at(2) == 0xbe || at(2) == 0xbf)) {
    return {0xff00u | (at(2) + 0x40u), 3};  // EF BF BE/BF: U+FFFE/FFFF
  }
  return {0, 0};
}

void Quote(std::string_view s, std::string &out) {
  out += '"';
  for (size_t i = 0; i < s.size(); i++) {
    const char c = s[i];
    const auto u = static_cast<unsigned char>(c);
    if (const auto [cp, len] = Unprintable(s, i); len != 0) {
      out += std::format("\\u{:04x}", cp);
      i += len - 1;
      continue;
    }
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\t':
        out += "\\t";
        break;
      case '\r':
        out += "\\r";
        break;
      default:
        if (u < 0x20 || u == 0x7f) {
          out += std::format("\\u{:04x}", u);
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

void Text(std::string_view s, std::string &out) {
  if (PlainSafe(s)) {
    out += s;
  } else {
    Quote(s, out);
  }
}

bool Inline(const json::Value &v) {
  const json::Array *a = v.array();
  const json::Object *o = v.object();
  return (a == nullptr || a->empty()) && (o == nullptr || o->empty());
}

void Scalar(const json::Value &v, std::string &out) {
  if (std::holds_alternative<std::nullptr_t>(v.v)) {
    out += "null";
  } else if (const bool *b = std::get_if<bool>(&v.v)) {
    out += *b ? "true" : "false";
  } else if (const json::Number *n = std::get_if<json::Number>(&v.v)) {
    out += n->text;
  } else if (const std::string *s = v.string()) {
    Text(*s, out);
  } else if (v.array() != nullptr) {
    out += "[]";
  } else {
    out += "{}";
  }
}

void Block(const json::Value &v, int indent, std::string &out);

// A member list or item list at `indent`; the first line's indentation is
// already written (after "- ", or at the line start).
void Members(const json::Object &o, int indent, std::string &out) {
  bool first = true;
  for (const json::Member &m : o) {
    if (!first) {
      out.append(indent, ' ');
    }
    first = false;
    Text(m.key, out);
    out += ':';
    if (Inline(m.value)) {
      out += ' ';
      Scalar(m.value, out);
      out += '\n';
    } else {
      out += '\n';
      out.append(indent + 2, ' ');
      Block(m.value, indent + 2, out);
    }
  }
}

void Items(const json::Array &a, int indent, std::string &out) {
  bool first = true;
  for (const json::Value &item : a) {
    if (!first) {
      out.append(indent, ' ');
    }
    first = false;
    out += "- ";
    if (Inline(item)) {
      Scalar(item, out);
      out += '\n';
    } else {
      Block(item, indent + 2, out);
    }
  }
}

void Block(const json::Value &v, int indent, std::string &out) {
  if (Inline(v)) {
    Scalar(v, out);
    out += '\n';
  } else if (const json::Object *o = v.object()) {
    Members(*o, indent, out);
  } else {
    Items(*v.array(), indent, out);
  }
}

}  // namespace

std::expected<json::Value, std::string> Parse(std::string_view text) {
  return Reader(text).Document();
}

std::string Dump(const json::Value &value) {
  std::string out;
  Block(value, 0, out);
  return out;
}

}  // namespace bessctl::yaml
