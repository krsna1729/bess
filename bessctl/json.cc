// SPDX-License-Identifier: BSD-3-Clause

#include "json.h"

#include <cstdint>
#include <format>

namespace bessctl::json {

namespace {

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  std::expected<Value, std::string> Document() {
    Value v;
    if (!ParseValue(v, 0)) {
      return std::unexpected(error_);
    }
    SkipSpace();
    if (pos_ != text_.size()) {
      Fail("unexpected text after the value");
      return std::unexpected(error_);
    }
    return v;
  }

 private:
  bool Fail(std::string_view what) {
    if (error_.empty()) {
      size_t line = 1;
      size_t column = 1;
      for (size_t i = 0; i < pos_ && i < text_.size(); i++) {
        if (text_[i] == '\n') {
          line++;
          column = 1;
        } else {
          column++;
        }
      }
      error_ = std::format("line {}, column {}: {}", line, column, what);
    }
    return false;
  }

  void SkipSpace() {
    while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' ||
                                   text_[pos_] == '\n' || text_[pos_] == '\r')) {
      pos_++;
    }
  }

  bool Literal(std::string_view word) {
    if (text_.substr(pos_, word.size()) != word) {
      return Fail("invalid literal");
    }
    pos_ += word.size();
    return true;
  }

  bool ParseValue(Value &out, int depth) {
    if (depth > kMaxDepth) {
      return Fail("nested too deeply");
    }
    SkipSpace();
    if (pos_ == text_.size()) {
      return Fail("unexpected end of input");
    }
    switch (text_[pos_]) {
      case '{':
        return ParseObject(out, depth);
      case '[':
        return ParseArray(out, depth);
      case '"': {
        std::string s;
        if (!ParseString(s)) {
          return false;
        }
        out.v = std::move(s);
        return true;
      }
      case 't':
        out.v = true;
        return Literal("true");
      case 'f':
        out.v = false;
        return Literal("false");
      case 'n':
        out.v = nullptr;
        return Literal("null");
      default:
        return ParseNumber(out);
    }
  }

  bool ParseObject(Value &out, int depth) {
    pos_++;  // '{'
    Object members;
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      pos_++;
      out.v = std::move(members);
      return true;
    }
    while (true) {
      SkipSpace();
      if (pos_ == text_.size() || text_[pos_] != '"') {
        return Fail("expected a member name");
      }
      std::string key;
      if (!ParseString(key)) {
        return false;
      }
      for (const auto &[existing, _] : members) {
        if (existing == key) {
          return Fail(std::format("duplicate member \"{}\"", key));
        }
      }
      SkipSpace();
      if (pos_ == text_.size() || text_[pos_] != ':') {
        return Fail("expected ':'");
      }
      pos_++;
      Value value;
      if (!ParseValue(value, depth + 1)) {
        return false;
      }
      members.emplace_back(std::move(key), std::move(value));
      SkipSpace();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        pos_++;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == '}') {
        pos_++;
        out.v = std::move(members);
        return true;
      }
      return Fail("expected ',' or '}'");
    }
  }

  bool ParseArray(Value &out, int depth) {
    pos_++;  // '['
    Array items;
    SkipSpace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      pos_++;
      out.v = std::move(items);
      return true;
    }
    while (true) {
      Value value;
      if (!ParseValue(value, depth + 1)) {
        return false;
      }
      items.push_back(std::move(value));
      SkipSpace();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        pos_++;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == ']') {
        pos_++;
        out.v = std::move(items);
        return true;
      }
      return Fail("expected ',' or ']'");
    }
  }

  static void AppendUtf8(std::string &out, uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool Hex4(uint32_t &out) {
    if (pos_ + 4 > text_.size()) {
      return Fail("truncated \\u escape");
    }
    out = 0;
    for (int i = 0; i < 4; i++) {
      const char c = text_[pos_++];
      out <<= 4;
      if (c >= '0' && c <= '9') {
        out |= static_cast<uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        out |= static_cast<uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        out |= static_cast<uint32_t>(c - 'A' + 10);
      } else {
        return Fail("invalid \\u escape");
      }
    }
    return true;
  }

  bool ParseString(std::string &out) {
    pos_++;  // '"'
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == '"') {
        pos_++;
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        return Fail("control character in a string");
      }
      if (c != '\\') {
        out += c;
        pos_++;
        continue;
      }
      pos_++;
      if (pos_ == text_.size()) {
        break;
      }
      const char e = text_[pos_++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          uint32_t cp = 0;
          if (!Hex4(cp)) {
            return false;
          }
          if (cp >= 0xD800 && cp <= 0xDBFF) {  // a surrogate pair's first half
            uint32_t low = 0;
            if (text_.substr(pos_, 2) != "\\u") {
              return Fail("unpaired surrogate");
            }
            pos_ += 2;
            if (!Hex4(low)) {
              return false;
            }
            if (low < 0xDC00 || low > 0xDFFF) {
              return Fail("unpaired surrogate");
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return Fail("unpaired surrogate");
          }
          AppendUtf8(out, cp);
          break;
        }
        default:
          pos_--;
          return Fail("invalid escape");
      }
    }
    return Fail("unterminated string");
  }

  bool Digits() {
    const size_t start = pos_;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      pos_++;
    }
    return pos_ > start;
  }

  // -?(0|[1-9][0-9]*)(\.[0-9]+)?([eE][+-]?[0-9]+)?
  bool ParseNumber(Value &out) {
    const size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') {
      pos_++;
    }
    if (pos_ < text_.size() && text_[pos_] == '0') {
      pos_++;
    } else if (!Digits()) {
      pos_ = start;
      return Fail("expected a value");
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      pos_++;
      if (!Digits()) {
        return Fail("expected digits after '.'");
      }
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      pos_++;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        pos_++;
      }
      if (!Digits()) {
        return Fail("expected digits in the exponent");
      }
    }
    out.v = Number{std::string(text_.substr(start, pos_ - start))};
    return true;
  }

  std::string_view text_;
  size_t pos_ = 0;
  std::string error_;
};

void DumpString(std::string &out, std::string_view s) {
  out += '"';
  for (const char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
          out += c;
        }
    }
  }
  out += '"';
}

void DumpValue(std::string &out, const Value &value, int indent, int level) {
  const auto newline = [&](int l) {
    if (indent > 0) {
      out += '\n';
      out.append(static_cast<size_t>(indent * l), ' ');
    }
  };
  if (std::holds_alternative<std::nullptr_t>(value.v)) {
    out += "null";
  } else if (const bool *b = std::get_if<bool>(&value.v)) {
    out += *b ? "true" : "false";
  } else if (const Number *n = std::get_if<Number>(&value.v)) {
    out += n->text;
  } else if (const std::string *s = value.string()) {
    DumpString(out, *s);
  } else if (const Array *a = value.array()) {
    if (a->empty()) {
      out += "[]";
      return;
    }
    out += '[';
    for (size_t i = 0; i < a->size(); i++) {
      if (i > 0) {
        out += ',';
      }
      newline(level + 1);
      DumpValue(out, (*a)[i], indent, level + 1);
    }
    newline(level);
    out += ']';
  } else if (const Object *o = value.object()) {
    if (o->empty()) {
      out += "{}";
      return;
    }
    out += '{';
    for (size_t i = 0; i < o->size(); i++) {
      if (i > 0) {
        out += ',';
      }
      newline(level + 1);
      DumpString(out, (*o)[i].key);
      out += indent > 0 ? ": " : ":";
      DumpValue(out, (*o)[i].value, indent, level + 1);
    }
    newline(level);
    out += '}';
  }
}

}  // namespace

const Value *Find(const Value &object, std::string_view key) {
  if (const Object *o = object.object()) {
    for (const auto &[k, v] : *o) {
      if (k == key) {
        return &v;
      }
    }
  }
  return nullptr;
}

Value *Find(Value &object, std::string_view key) {
  return const_cast<Value *>(Find(static_cast<const Value &>(object), key));
}

void Erase(Value &object, std::string_view key) {
  if (Object *o = object.object()) {
    std::erase_if(*o, [&](const Member &member) { return member.key == key; });
  }
}

std::expected<Value, std::string> Parse(std::string_view text) {
  return Parser(text).Document();
}

std::string Dump(const Value &value, int indent) {
  std::string out;
  DumpValue(out, value, indent, 0);
  return out;
}

}  // namespace bessctl::json
