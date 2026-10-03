// SPDX-License-Identifier: BSD-3-Clause

// M13 (D-063): one packet edited per iteration, by case and representation.
//
//   BM_Edit/<case>/<impl>
//     case: 0 NAT, 1 VXLAN encap, 2 VXLAN decap, 3 VFP-like rewrite,
//           4 UPF-like outer replacement, 5 one write
//     impl: 0 hand-written C++, 1 typed per-flow action (NAT only; the others'
//           typed form is the hand-written code with its header precomputed),
//           2 EditPlan (opcode + switch), 3 std::variant steps + std::visit,
//           4 function-pointer sequence. 2-4 share EditPlan::ApplyPrefix, so
//           they differ only in how a step is dispatched. 5 EditPlan::ApplyTrusted
//           (no per-packet checks: the caller's parse contract proves them).
//   BM_Restore/<case>  the per-iteration packet restore alone (subtract it)

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <variant>
#include <vector>

#include "packet_edit_plan.h"
#include "packet_edit_plan_cases.h"
#include "packet_pool.h"

namespace {

using bess::PacketHandle;
using bess::packet::EditPlan;
namespace ec = bess::packet::edit_cases;

bess::PlainPacketPool &Pool() {
  static bess::PlainPacketPool pool(64, -1, 512);
  return pool;
}

// The case's input packet and plan.
struct Case {
  std::vector<uint8_t> input;
  EditPlan plan;
};
Case MakeCase(int c) {
  switch (c) {
    case 0: return {ec::TcpPacket(), ec::NatPlan(ec::NatParams())};
    case 1: return {ec::TcpPacket(), ec::VxlanEncapPlan(ec::VxlanHeader())};
    case 2: {
      std::vector<uint8_t> in(ec::kVxlanOuter, 0x5a);
      const auto inner = ec::TcpPacket();
      in.insert(in.end(), inner.begin(), inner.end());
      return {in, ec::VxlanDecapPlan()};
    }
    case 3: return {ec::TcpPacket(), ec::VfpPlan(ec::VfpParams())};
    case 4: {
      std::vector<uint8_t> in(ec::kGtpOuter, 0x5a);
      const auto inner = ec::TcpPacket();
      in.insert(in.end(), inner.begin() + 14, inner.end());
      return {in, ec::UpfPlan(ec::InnerEthernet())};
    }
    default: return {ec::TcpPacket(), ec::OneWritePlan()};
  }
}

// Puts the input bytes back and the data offset where Alloc left it.
struct Restorer {
  uint16_t data_off;
  std::vector<uint8_t> bytes;
  void operator()(PacketHandle m) const {
    m->data_off = data_off;
    m->data_len = static_cast<uint16_t>(bytes.size());
    m->pkt_len = static_cast<uint32_t>(bytes.size());
    std::memcpy(rte_pktmbuf_mtod(m, uint8_t *), bytes.data(), bytes.size());
  }
};

// -- representation 3: std::variant steps ------------------------------------

struct VWrite { uint16_t off; uint8_t len; const uint8_t *src; };
struct VCopy { uint16_t to, from; uint8_t len; };
struct VCsum { uint16_t off; uint32_t delta; };
struct VLen { uint16_t off; uint32_t base; };
struct VIpv4 { uint16_t off, len_off; uint32_t partial; };
using VStep = std::variant<VWrite, VCopy, VCsum, VLen, VIpv4>;

inline uint16_t Fold(uint32_t sum) {
  sum = (sum & 0xFFFF) + (sum >> 16);
  sum = (sum & 0xFFFF) + (sum >> 16);
  return static_cast<uint16_t>(~sum);
}

struct VariantPlan {
  const EditPlan *plan;
  std::array<VStep, EditPlan::kMaxSteps> steps;
  size_t n = 0;
  explicit VariantPlan(const EditPlan &p) : plan(&p) {
    for (const auto &s : p.step_list()) {
      switch (s.op) {
        case EditPlan::Op::kWrite: steps[n++] = VWrite{s.offset, s.len, p.data().data() + s.aux}; break;
        case EditPlan::Op::kCopy: steps[n++] = VCopy{s.offset, static_cast<uint16_t>(s.aux), s.len}; break;
        case EditPlan::Op::kAddChecksum: steps[n++] = VCsum{s.offset, s.aux}; break;
        case EditPlan::Op::kSetLength: steps[n++] = VLen{s.offset, s.aux}; break;
        case EditPlan::Op::kIpv4Checksum:
          steps[n++] = VIpv4{s.offset, static_cast<uint16_t>(s.aux & 0xFFFF), p.ipv4_partial(s.aux >> 16)};
          break;
      }
    }
  }
  void Apply(PacketHandle m) const {
    uint8_t *p = plan->ApplyPrefix(bess::PacketRef(m)).value();
    const uint32_t len = m->pkt_len;
    for (size_t i = 0; i < n; i++) {
      std::visit(
          [p, len](const auto &s) {
            using T = std::decay_t<decltype(s)>;
            if constexpr (std::is_same_v<T, VWrite>) {
              std::memcpy(p + s.off, s.src, s.len);
            } else if constexpr (std::is_same_v<T, VCopy>) {
              std::memmove(p + s.to, p + s.from, s.len);
            } else if constexpr (std::is_same_v<T, VCsum>) {
              uint16_t c;
              std::memcpy(&c, p + s.off, 2);
              c = Fold(static_cast<uint16_t>(~c) + s.delta);
              std::memcpy(p + s.off, &c, 2);
            } else if constexpr (std::is_same_v<T, VLen>) {
              const uint16_t v = rte_cpu_to_be_16(static_cast<uint16_t>(len - s.base));
              std::memcpy(p + s.off, &v, 2);
            } else {
              uint16_t l;
              std::memcpy(&l, p + s.len_off, 2);
              const uint16_t c = Fold(s.partial + l);
              std::memcpy(p + s.off, &c, 2);
            }
          },
          steps[i]);
    }
  }
};

// -- representation 4: a pre-bound function-pointer sequence -----------------

struct FStep;
using StepFn = void (*)(uint8_t *, uint32_t, const FStep &);
struct FStep {
  StepFn fn;
  uint16_t off;
  uint8_t len;
  uint32_t aux;
  const uint8_t *src;
};
void FWrite(uint8_t *p, uint32_t, const FStep &s) { std::memcpy(p + s.off, s.src, s.len); }
void FCopy(uint8_t *p, uint32_t, const FStep &s) { std::memmove(p + s.off, p + s.aux, s.len); }
void FCsum(uint8_t *p, uint32_t, const FStep &s) {
  uint16_t c;
  std::memcpy(&c, p + s.off, 2);
  c = Fold(static_cast<uint16_t>(~c) + s.aux);
  std::memcpy(p + s.off, &c, 2);
}
void FLen(uint8_t *p, uint32_t len, const FStep &s) {
  const uint16_t v = rte_cpu_to_be_16(static_cast<uint16_t>(len - s.aux));
  std::memcpy(p + s.off, &v, 2);
}
void FIpv4(uint8_t *p, uint32_t, const FStep &s) {
  uint16_t l;
  std::memcpy(&l, p + s.len, 2);  // len holds the length-field offset (< 256)
  const uint16_t c = Fold(s.aux + l);
  std::memcpy(p + s.off, &c, 2);
}

struct FnPlan {
  const EditPlan *plan;
  std::array<FStep, EditPlan::kMaxSteps> steps;
  size_t n = 0;
  explicit FnPlan(const EditPlan &p) : plan(&p) {
    for (const auto &s : p.step_list()) {
      switch (s.op) {
        case EditPlan::Op::kWrite: steps[n++] = {FWrite, s.offset, s.len, 0, p.data().data() + s.aux}; break;
        case EditPlan::Op::kCopy: steps[n++] = {FCopy, s.offset, s.len, s.aux, nullptr}; break;
        case EditPlan::Op::kAddChecksum: steps[n++] = {FCsum, s.offset, 0, s.aux, nullptr}; break;
        case EditPlan::Op::kSetLength: steps[n++] = {FLen, s.offset, 0, s.aux, nullptr}; break;
        case EditPlan::Op::kIpv4Checksum:
          steps[n++] = {FIpv4, s.offset, static_cast<uint8_t>(s.aux & 0xFF),
                        p.ipv4_partial(s.aux >> 16), nullptr};
          break;
      }
    }
  }
  void Apply(PacketHandle m) const {
    uint8_t *p = plan->ApplyPrefix(bess::PacketRef(m)).value();
    const uint32_t len = m->pkt_len;
    for (size_t i = 0; i < n; i++) {
      steps[i].fn(p, len, steps[i]);
    }
  }
};

// -- hand-written ----------------------------------------------------------------

void Hand(int c, PacketHandle m) {
  switch (c) {
    case 0: {
      static const auto nat = ec::NatParams();
      ec::NatHand(rte_pktmbuf_mtod(m, uint8_t *), nat);
      break;
    }
    case 1: {
      static const auto header = ec::VxlanHeader();  // built once, as the plan's is
      ec::VxlanEncapHand(m, header);
      break;
    }
    case 2: ec::VxlanDecapHand(m); break;
    case 3: {
      static const auto vfp = ec::VfpParams();
      ec::VfpHand(rte_pktmbuf_mtod(m, uint8_t *), vfp);
      break;
    }
    case 4: {
      static const auto eth = ec::InnerEthernet();
      ec::UpfHand(m, eth);
      break;
    }
    default: rte_pktmbuf_mtod(m, uint8_t *)[22] = 32;
  }
}

void BM_Edit(benchmark::State &state) {
  const int c = static_cast<int>(state.range(0));
  const int impl = static_cast<int>(state.range(1));
  if (impl == 1 && c != 0) {
    state.SkipWithMessage("typed action is the hand-written code for this case");
    return;
  }
  const Case k = MakeCase(c);
  PacketHandle m = Pool().Alloc(k.input.size());
  const Restorer restore{m->data_off, k.input};
  const ec::NatAction nat = ec::NatAction::For(ec::NatParams());
  const VariantPlan vplan(k.plan);
  const FnPlan fplan(k.plan);
  for (auto _ : state) {
    restore(m);
    switch (impl) {
      case 0: Hand(c, m); break;
      case 1: nat.Apply(rte_pktmbuf_mtod(m, uint8_t *)); break;
      case 2: (void)k.plan.Apply(m); break;
      case 3: vplan.Apply(m); break;
      case 4: fplan.Apply(m); break;
      default: k.plan.ApplyTrusted(bess::PacketRef(m));
    }
    benchmark::ClobberMemory();
  }
  state.counters["plan_steps"] = static_cast<double>(k.plan.steps());
  bess::PacketFree(m);
}

void BM_Restore(benchmark::State &state) {
  const Case k = MakeCase(static_cast<int>(state.range(0)));
  PacketHandle m = Pool().Alloc(k.input.size());
  const Restorer restore{m->data_off, k.input};
  for (auto _ : state) {
    restore(m);
    benchmark::ClobberMemory();
  }
  bess::PacketFree(m);
}

BENCHMARK(BM_Edit)->ArgsProduct({{0, 1, 2, 3, 4, 5}, {0, 1, 2, 3, 4, 5}});
BENCHMARK(BM_Restore)->DenseRange(0, 5);

}  // namespace
