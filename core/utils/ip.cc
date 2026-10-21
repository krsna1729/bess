// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include <array>
#include <cctype>
#include <charconv>
#include <string_view>
#include "ip.h"

#include "utils/logging.h"

#include "bits.h"
#include "format.h"

namespace bess {
namespace utils {

bool ParseIpv4Address(const std::string &str, be32_t *addr) {
  // Four decimal parts, each optionally preceded by whitespace; anything after
  // the fourth part is ignored (the leniency of the sscanf("%u.%u.%u.%u") this
  // replaces). sscanf's %u wrapped out-of-range parts ("4294967297" read as 1),
  // so "4294967296.0.0.1" used to parse as 0.0.0.1.
  const char *p = str.data();
  const char *const end = p + str.size();
  uint32_t value = 0;
  for (int i = 0; i < 4; i++) {
    if (i > 0) {
      if (p == end || *p != '.') {
        return false;
      }
      p++;
    }
    while (p != end && std::isspace(static_cast<unsigned char>(*p))) {
      p++;
    }
    unsigned part = 0;
    const auto [next, ec] = std::from_chars(p, end, part);
    if (ec != std::errc() || part > 255) {
      return false;
    }
    value = (value << 8) | part;
    p = next;
  }
  *addr = be32_t(value);
  return true;
}

std::string ToIpv4Address(be32_t addr) {
  const union {
    be32_t addr;
    char bytes[4];
  } &t = {.addr = addr};

  return bess::utils::Format("%hhu.%hhu.%hhu.%hhu", t.bytes[0], t.bytes[1],
                             t.bytes[2], t.bytes[3]);
}

std::optional<Ipv4Prefix> Ipv4Prefix::Parse(const std::string &prefix) {
  const size_t slash = prefix.find('/');
  if (slash == std::string::npos || slash + 1 >= prefix.size()) {
    return std::nullopt;
  }
  // Strictly "d.d.d.d": ParseIpv4Address would also accept whitespace before
  // each part and trailing junk.
  const std::string address = prefix.substr(0, slash);
  int dots = 0;
  char prev = '.';
  for (const char c : address) {
    if (c == '.') {
      if (prev == '.') {
        return std::nullopt;  // an empty part
      }
      dots++;
    } else if (c < '0' || c > '9') {
      return std::nullopt;
    }
    prev = c;
  }
  be32_t addr;
  if (dots != 3 || prev == '.' || !ParseIpv4Address(address, &addr)) {
    return std::nullopt;
  }
  unsigned len = 0;
  const char *first = prefix.data() + slash + 1;
  const char *last = prefix.data() + prefix.size();
  const auto [end, ec] = std::from_chars(first, last, len);
  if (ec != std::errc() || end != last || len > 32) {
    return std::nullopt;
  }
  Ipv4Prefix out("");
  out.addr = addr;
  out.mask = be32_t(SetBitsLow<uint32_t>(len));
  return out;
}

namespace {

// The groups of one side of a "::": 1..4 hex digits each, separated by single
// colons, with no empty group (a lone leading/trailing colon is malformed).
bool ParseIpv6Groups(std::string_view part, uint16_t *groups, size_t *count) {
  *count = 0;
  if (part.empty()) {
    return true;
  }
  size_t start = 0;
  while (true) {
    const size_t colon = part.find(':', start);
    const std::string_view group =
        part.substr(start, colon == std::string_view::npos
                               ? std::string_view::npos
                               : colon - start);
    if (group.empty() || group.size() > 4 || *count >= 8) {
      return false;
    }
    unsigned value = 0;
    const auto [end, error] =
        std::from_chars(group.data(), group.data() + group.size(), value, 16);
    if (error != std::errc() || end != group.data() + group.size()) {
      return false;
    }
    groups[(*count)++] = static_cast<uint16_t>(value);
    if (colon == std::string_view::npos) {
      return true;
    }
    start = colon + 1;
    if (start == part.size()) {
      return false;
    }
  }
}

}  // namespace

std::optional<Ipv6Address> ParseIpv6Address(const std::string &str) {
  if (str.empty()) {
    return std::nullopt;
  }
  std::array<uint16_t, 8> left{};
  std::array<uint16_t, 8> right{};
  size_t left_count = 0;
  size_t right_count = 0;
  const size_t compression = str.find("::");
  size_t zeros = 0;
  if (compression == std::string::npos) {
    if (!ParseIpv6Groups(str, left.data(), &left_count) || left_count != 8) {
      return std::nullopt;
    }
  } else {
    if (str.find("::", compression + 2) != std::string::npos) {
      return std::nullopt;
    }
    if (!ParseIpv6Groups(std::string_view(str).substr(0, compression),
                         left.data(), &left_count) ||
        !ParseIpv6Groups(std::string_view(str).substr(compression + 2),
                         right.data(), &right_count) ||
        left_count + right_count >= 8) {
      return std::nullopt;
    }
    zeros = 8 - left_count - right_count;
  }
  Ipv6Address address;
  size_t group = 0;
  for (size_t i = 0; i < left_count; i++, group++) {
    address.bytes[group * 2] =
        static_cast<uint8_t>(static_cast<uint32_t>(left[i]) >> 8u);
    address.bytes[group * 2 + 1] = static_cast<uint8_t>(left[i]);
  }
  group += zeros;
  for (size_t i = 0; i < right_count; i++, group++) {
    address.bytes[group * 2] =
        static_cast<uint8_t>(static_cast<uint32_t>(right[i]) >> 8u);
    address.bytes[group * 2 + 1] = static_cast<uint8_t>(right[i]);
  }
  return address;
}

std::string ToIpv6Address(const Ipv6Address &addr) {
  std::array<uint16_t, 8> groups{};
  for (size_t i = 0; i < 8; i++) {
    groups[i] = static_cast<uint16_t>(
        static_cast<uint32_t>(addr.bytes[i * 2]) << 8u |
        static_cast<uint32_t>(addr.bytes[i * 2 + 1]));
  }
  // The longest run of at least two zero groups is compressed; RFC 5952 order
  // picks the first on a tie, which the strict `>` below does.
  size_t best_start = 8;
  size_t best_length = 0;
  for (size_t i = 0; i < 8;) {
    if (groups[i] != 0) {
      i++;
      continue;
    }
    size_t end = i;
    while (end < 8 && groups[end] == 0) {
      end++;
    }
    if (end - i > best_length && end - i >= 2) {
      best_start = i;
      best_length = end - i;
    }
    i = end;
  }
  std::string out;
  out.reserve(39);
  for (size_t i = 0; i < 8;) {
    if (i == best_start) {
      out += "::";
      i += best_length;
      continue;
    }
    if (!out.empty() && out.back() != ':') {
      out += ':';
    }
    const auto append = [&out](uint16_t value) {
      static constexpr char kHex[] = "0123456789abcdef";
      char digits[4];
      size_t n = 0;
      do {
        digits[n++] = kHex[value & 0xfu];
        value = static_cast<uint16_t>(static_cast<uint32_t>(value) >> 4u);
      } while (value != 0);
      while (n > 0) {
        out += digits[--n];
      }
    };
    append(groups[i++]);
  }
  return out.empty() ? "::" : out;
}

Ipv4Prefix::Ipv4Prefix(const std::string &prefix)
    : addr(be32_t(0)), mask(be32_t(0)) {
  // Default values on parser failure (the historical contract); std::stoi
  // used to throw out of here on a non-numeric length.
  if (prefix.empty()) {
    return;
  }
  if (const auto parsed = Parse(prefix)) {
    addr = parsed->addr;
    mask = parsed->mask;
  }
}

}  // namespace utils
}  // namespace bess
