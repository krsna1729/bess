// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include <cctype>
#include <charconv>
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
