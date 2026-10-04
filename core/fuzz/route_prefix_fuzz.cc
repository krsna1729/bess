// SPDX-License-Identifier: BSD-3-Clause

// Fuzzes the IPv4 address/prefix parsers used by route configuration:
// utils::Ipv4Prefix::Parse (strict "a.b.c.d/len"), the lenient
// utils::Ipv4Prefix(string) constructor, utils::ParseIpv4Address (lenient)
// and route::Ipv4Prefix::Make (address, length) validation.
//
// Input: first byte selects the stage.
//   even: the rest is a string fed to the three string parsers.
//   odd:  u32 address, u8 length for route::Ipv4Prefix::Make.
//
// Oracles:
//   - Parse accepts exactly the strings a reference parser of the grammar
//     octet "." octet "." octet "." octet "/" len accepts (octet: 1+ decimal
//     digits with value <= 255, len: 1+ decimal digits with value <= 32), and
//     yields the same address and mask; formatting the result as
//     "a.b.c.d/len" and parsing it again gives the same prefix.
//   - The constructor yields Parse's result, or 0.0.0.0/0 when Parse rejects.
//   - ParseIpv4Address leaves its output untouched on rejection and accepts
//     exactly when the string starts with four dot-separated octets, each
//     optionally preceded by whitespace (trailing input is ignored), with
//     that value.
//   - Make succeeds iff length <= 32 and no host bit is set, with the
//     documented error otherwise; an accepted prefix formats to a string that
//     Parse accepts with the same address and length, and matches exactly the
//     addresses that share its first `length` bits.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "fuzz/fuzz_support.h"
#include "route/route_table.h"
#include "utils/endian.h"
#include "utils/ip.h"

namespace {

using bess::utils::be32_t;

struct RefPrefix {
  uint32_t addr;
  uint32_t len;
};

// Parses a run of decimal digits at s[*pos]; saturates at 1000 so overlong
// runs stay out of range. Returns nullopt if there is no digit.
std::optional<uint32_t> DigitRun(const std::string &s, size_t *pos) {
  const size_t start = *pos;
  uint32_t value = 0;
  while (*pos < s.size() && s[*pos] >= '0' && s[*pos] <= '9') {
    value = value * 10 + static_cast<uint32_t>(s[*pos] - '0');
    if (value > 1000) {
      value = 1000;
    }
    (*pos)++;
  }
  if (*pos == start) {
    return std::nullopt;
  }
  return value;
}

// Four octets from s[*pos]; nullopt unless each is a digit run <= 255. With
// `lenient`, whitespace may precede each octet (ParseIpv4Address's grammar).
std::optional<uint32_t> RefAddress(const std::string &s, size_t *pos,
                                   bool lenient = false) {
  uint32_t addr = 0;
  for (int i = 0; i < 4; i++) {
    if (i > 0) {
      if (*pos >= s.size() || s[*pos] != '.') {
        return std::nullopt;
      }
      (*pos)++;
    }
    while (lenient && *pos < s.size() &&
           std::string_view(" \t\n\v\f\r").find(s[*pos]) != std::string_view::npos) {
      (*pos)++;
    }
    const auto octet = DigitRun(s, pos);
    if (!octet || *octet > 255) {
      return std::nullopt;
    }
    addr = (addr << 8) | *octet;
  }
  return addr;
}

std::optional<RefPrefix> RefParsePrefix(const std::string &s) {
  size_t pos = 0;
  const auto addr = RefAddress(s, &pos);
  if (!addr || pos >= s.size() || s[pos] != '/') {
    return std::nullopt;
  }
  pos++;
  const auto len = DigitRun(s, &pos);
  if (!len || *len > 32 || pos != s.size()) {
    return std::nullopt;
  }
  return RefPrefix{*addr, *len};
}

uint32_t MaskOf(uint32_t len) {
  return len == 0 ? 0 : ~uint32_t{0} << (32 - len);
}

std::string Format(uint32_t addr, uint32_t len) {
  return std::to_string(addr >> 24) + "." + std::to_string((addr >> 16) & 255) +
         "." + std::to_string((addr >> 8) & 255) + "." +
         std::to_string(addr & 255) + "/" + std::to_string(len);
}

void FuzzStrings(const std::string &s) {
  using bess::utils::Ipv4Prefix;

  const std::optional<Ipv4Prefix> parsed = Ipv4Prefix::Parse(s);
  const std::optional<RefPrefix> ref = RefParsePrefix(s);
  FUZZ_CHECK(parsed.has_value() == ref.has_value());
  if (parsed) {
    FUZZ_CHECK(parsed->addr.value() == ref->addr);
    FUZZ_CHECK(parsed->mask.value() == MaskOf(ref->len));
    FUZZ_CHECK(parsed->prefix_length() == ref->len);
    // The textual form of a parsed prefix parses back to itself, also through
    // the address formatter the control plane uses to print it.
    const std::string printed =
        bess::utils::ToIpv4Address(parsed->addr) + "/" +
        std::to_string(parsed->prefix_length());
    FUZZ_CHECK(printed == Format(ref->addr, ref->len));
    const auto again = Ipv4Prefix::Parse(printed);
    FUZZ_CHECK(again.has_value());
    FUZZ_CHECK(again->addr == parsed->addr && again->mask == parsed->mask);
  }

  const Ipv4Prefix lenient(s);
  FUZZ_CHECK(lenient.addr.value() == (parsed ? parsed->addr.value() : 0));
  FUZZ_CHECK(lenient.mask.value() == (parsed ? parsed->mask.value() : 0));

  constexpr uint32_t kSentinel = 0xDEADBEEF;
  be32_t addr(kSentinel);
  const bool ok = bess::utils::ParseIpv4Address(s, &addr);
  if (!ok) {
    FUZZ_CHECK(addr.value() == kSentinel);
  }
  size_t pos = 0;
  const auto ref_addr = RefAddress(s, &pos, /*lenient=*/true);
  FUZZ_CHECK(ok == ref_addr.has_value());
  if (ok) {
    FUZZ_CHECK(addr.value() == *ref_addr);
  }
}

void FuzzMake(uint32_t addr, uint8_t length) {
  using bess::route::Ipv4Prefix;
  using bess::route::RouteError;

  const auto made = Ipv4Prefix::Make(addr, length);
  if (length > 32) {
    FUZZ_CHECK(!made && made.error() == RouteError::kInvalidPrefixLength);
    return;
  }
  const uint32_t mask = MaskOf(length);
  if ((addr & ~mask) != 0) {
    FUZZ_CHECK(!made && made.error() == RouteError::kHostBitsSet);
    return;
  }
  FUZZ_CHECK(made.has_value());
  FUZZ_CHECK(made->addr() == addr && made->length() == length);

  const auto parsed = bess::utils::Ipv4Prefix::Parse(Format(addr, length));
  FUZZ_CHECK(parsed.has_value());
  FUZZ_CHECK(parsed->addr.value() == addr);
  FUZZ_CHECK(parsed->prefix_length() == length);
  FUZZ_CHECK(parsed->Match(be32_t(addr)));
  // Flipping a bit inside the prefix leaves it; flipping a host bit does not.
  if (length > 0) {
    FUZZ_CHECK(!parsed->Match(be32_t(addr ^ (1u << (32 - length)))));
  }
  if (length < 32) {
    FUZZ_CHECK(parsed->Match(be32_t(addr | ~mask)));
  }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  bess::fuzz::FuzzInput in(data, size);
  if ((in.U8() & 1) == 0) {
    const auto rest = in.Rest();
    FuzzStrings(std::string(rest.begin(), rest.end()));
  } else {
    const uint32_t addr = in.U32();
    FuzzMake(addr, in.U8());
  }
  return 0;
}
