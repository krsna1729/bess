// SPDX-License-Identifier: BSD-3-Clause

#ifndef BESS_DATAPLANE_BATCH_TUNING_H_
#define BESS_DATAPLANE_BATCH_TUNING_H_

#include <cstddef>
#include <cstdint>
#include <optional>

namespace bess::dataplane {

// Data-cache sizes of one CPU (K4.6). Read directly from sysfs
// (/sys/devices/system/cpu/cpuN/cache), falling back to sysconf and then to
// conservative defaults. Hybrid CPUs differ per core type (on the i9-13900H
// here: P-core L1d 48 KiB / L2 1.25 MiB; sysconf reports 32 KiB / 2 MiB), so
// geometry is per CPU, and Smallest() is the safe choice when the CPUs that
// will run a table are not known yet.
struct CacheGeometry {
  size_t line_bytes = 64;
  size_t l1d_bytes = 32 * 1024;
  size_t l2_bytes = 1024 * 1024;
  size_t l3_bytes = 8 * 1024 * 1024;

  static CacheGeometry ForCpu(int cpu);
  // Element-wise minimum over the online CPUs (cached after the first call).
  static const CacheGeometry &Smallest();
};

// Which batch-lookup body a table uses.
enum class LookupBody : uint8_t;
// BESS_LOOKUP_BODY=plain|staged, read once per process; kAuto when unset.
LookupBody LookupBodyOverride();

enum class LookupBody : uint8_t {
  kAuto,    // decide from the table's shape and the cache geometry
  kPlain,   // one lookup at a time
  kStaged,  // RunStages: hash/resolve + prefetch the batch, then probe
};

const char *LookupBodyName(LookupBody body);

// What a batch lookup walks: the table footprint it spreads over, how many
// cache lines one lookup touches *in sequence* (each address depends on the
// previous line's contents), and whether its control flow branches on the
// loaded data. rte_lpm's tbl24: 1 line, no such branch. A cuckoo probe: 2
// lines (bucket, entry) and branches (which slot matched). L2Forward's inline
// 4-way bucket: 1 line, but branches (hit? primary or alternate bucket?).
struct LookupShape {
  size_t table_bytes = 0;
  unsigned dependent_lines = 1;
  bool branches_on_loaded_data = false;
};

// Resolves kAuto (explicit kPlain/kStaged pass through). Decision D-006
// (docs/decisions.md). The rule is the one
// the K4.6 measurements support; see ChooseLookupBody()'s definition for the
// evidence. BESS_LOOKUP_BODY=plain|staged in the environment overrides kAuto
// for experiments.
LookupBody ResolveLookupBody(LookupBody requested, const LookupShape &shape,
                             const CacheGeometry &cache =
                                 CacheGeometry::Smallest());

}  // namespace bess::dataplane

#endif  // BESS_DATAPLANE_BATCH_TUNING_H_
