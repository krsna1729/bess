// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include <charconv>
#include "ip.h"

#include <glog/logging.h>

#include "bits.h"
#include "format.h"

namespace bess {
namespace utils {

bool ParseIpv4Address(const std::string &str, be32_t *addr) {
  unsigned int a, b, c, d;
  int cnt;

  cnt = bess::utils::Parse(str, "%u.%u.%u.%u", &a, &b, &c, &d);
  if (cnt != 4 || a >= 256 || b >= 256 || c >= 256 || d >= 256) {
    return false;
  }

  *addr = be32_t((a << 24) | (b << 16) | (c << 8) | d);
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
  // Strictly "d.d.d.d": ParseIpv4Address (sscanf) would also accept
  // surrounding whitespace and trailing junk.
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
