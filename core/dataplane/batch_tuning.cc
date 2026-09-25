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

#include "dataplane/batch_tuning.h"

#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

namespace bess::dataplane {

namespace {

// "48K", "1280K", "24576K", "2M" -> bytes; 0 if unreadable.
size_t ParseSize(const std::string &text) {
  if (text.empty()) {
    return 0;
  }
  char *end = nullptr;
  const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
  switch (end != nullptr ? *end : '\0') {
    case 'K':
      return value * 1024;
    case 'M':
      return value * 1024 * 1024;
    case 'G':
      return value * 1024 * 1024 * 1024;
    default:
      return value;
  }
}

std::string ReadLine(const std::string &path) {
  std::ifstream in(path);
  std::string line;
  std::getline(in, line);
  return line;
}

}  // namespace

CacheGeometry CacheGeometry::ForCpu(int cpu) {
  CacheGeometry g;
  bool from_sysfs = false;
  const std::string base =
      "/sys/devices/system/cpu/cpu" + std::to_string(cpu) + "/cache/index";
  for (int i = 0; i < 8; i++) {
    const std::string dir = base + std::to_string(i) + "/";
    const std::string level = ReadLine(dir + "level");
    if (level.empty()) {
      break;
    }
    const std::string type = ReadLine(dir + "type");
    const size_t size = ParseSize(ReadLine(dir + "size"));
    if (size == 0 || type == "Instruction") {
      continue;
    }
    from_sysfs = true;
    if (const size_t line = ParseSize(ReadLine(dir + "coherency_line_size"))) {
      g.line_bytes = line;
    }
    if (level == "1") {
      g.l1d_bytes = size;
    } else if (level == "2") {
      g.l2_bytes = size;
    } else if (level == "3") {
      g.l3_bytes = size;
    }
  }
  if (!from_sysfs) {
    // Not per-CPU on hybrid parts, but better than nothing.
    if (const long v = sysconf(_SC_LEVEL1_DCACHE_SIZE); v > 0) g.l1d_bytes = v;
    if (const long v = sysconf(_SC_LEVEL2_CACHE_SIZE); v > 0) g.l2_bytes = v;
    if (const long v = sysconf(_SC_LEVEL3_CACHE_SIZE); v > 0) g.l3_bytes = v;
    if (const long v = sysconf(_SC_LEVEL1_DCACHE_LINESIZE); v > 0) {
      g.line_bytes = v;
    }
  }
  return g;
}

const CacheGeometry &CacheGeometry::Smallest() {
  static const CacheGeometry smallest = [] {
    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    CacheGeometry min = ForCpu(0);
    for (long cpu = 1; cpu < cpus; cpu++) {
      const CacheGeometry g = ForCpu(static_cast<int>(cpu));
      min.l1d_bytes = std::min(min.l1d_bytes, g.l1d_bytes);
      min.l2_bytes = std::min(min.l2_bytes, g.l2_bytes);
      min.l3_bytes = std::min(min.l3_bytes, g.l3_bytes);
    }
    return min;
  }();
  return smallest;
}

const char *LookupBodyName(LookupBody body) {
  switch (body) {
    case LookupBody::kAuto:
      return "auto";
    case LookupBody::kPlain:
      return "plain";
    case LookupBody::kStaged:
      return "staged";
  }
  return "unknown";
}

namespace {

// BESS_LOOKUP_BODY is read once: ResolveLookupBody may run per batch.
LookupBody EnvironmentOverride() {
  static const LookupBody forced = [] {
    const char *env = std::getenv("BESS_LOOKUP_BODY");
    if (env != nullptr && std::strcmp(env, "plain") == 0) {
      return LookupBody::kPlain;
    }
    if (env != nullptr && std::strcmp(env, "staged") == 0) {
      return LookupBody::kStaged;
    }
    return LookupBody::kAuto;
  }();
  return forced;
}

}  // namespace

LookupBody ResolveLookupBody(LookupBody requested, const LookupShape &shape,
                             const CacheGeometry &cache) {
  if (requested != LookupBody::kAuto) {
    return requested;
  }
  if (const LookupBody forced = EnvironmentOverride();
      forced != LookupBody::kAuto) {
    return forced;
  }
  // The rule K4.6's sweep supports (ns per 32-key batch, isolated P-core,
  // medians of 3; MODERNIZATION.md K4.6 has the tables):
  //  - L1d-resident tables: staging never helped (cuckoo 256 entries: -1%;
  //    WildcardMatch tuples of 512-1K entries: +3..+5%).
  //  - Bigger tables whose lookups walk dependent lines or branch on loaded
  //    data: staging always helped (cuckoo -16..-48%, NAT -18..-27%,
  //    L2Forward -18..-33%, WildcardMatch tuples up to -49%).
  //  - One branch-free independent load per lookup (rte_lpm tbl24): no
  //    consistent effect at any size (-15..+15%), so plain.
  const bool dependent =
      shape.dependent_lines >= 2 || shape.branches_on_loaded_data;
  return dependent && shape.table_bytes > cache.l1d_bytes
             ? LookupBody::kStaged
             : LookupBody::kPlain;
}

}  // namespace bess::dataplane
