# The assembly shape of `core/arch`'s kernels

Appendix J's "assembly review complete for critical kernels". The kernels are
header-only, so their shape is what the *release* flags make of them: this page
records the instructions each one compiles to, under its own symbol, and checks
the header's claim against them. It is a review, not a benchmark.

The probe is `core/arch/asm_shape.cc`, built by Meson as `bess_arch_asm_shape`
(so every lane compiles it and clang-tidy has its flags; nothing links or
installs it): each kernel is `inline` in a header, so no archive carries a
symbol to disassemble, and the wrappers are `noinline` with arguments the
compiler cannot fold. Read the assembly with

```sh
ninja -C build/perf-release core/libbess_arch_asm_shape.a
objdump -d --no-show-raw-insn build/perf-release/core/libbess_arch_asm_shape.a
```

or compile the same unit by hand with a `core/` translation unit's flags from
`build/perf-release/compile_commands.json` (D-127: releases stay
`-march=x86-64-v3`; AVX2 is in that floor, AVX-512 is not):

```sh
g++ -O3 -march=x86-64-v3 -DNDEBUG -D_FILE_OFFSET_BITS=64 -std=c++23 -I core \
    -S -o /tmp/asm_shape.s core/arch/asm_shape.cc
awk '/^[_a-zA-Z][A-Za-z0-9_.]*:$/{f=$1} /^\t[a-z]/{print f, $1}' \
    /tmp/asm_shape.s | sort | uniq -c
```

Split the output by symbol as above, not by `.LFB` order: a TU-wide mnemonic
count attributes one kernel's instructions to another (the first version of this
page did exactly that, and swapped two bodies).

## What each kernel is

| wrapper | kernel | the emitted body | header's claim |
|---|---|---|---|
| `k_crc64` | `Crc32c(uint64_t, uint32_t)` | `movl` + `crc32q %rdi, %rax` + `ret` | the instruction |
| `k_sumwords` | `SumWords32` | four `vmovdqu` + four `vpunpck{h,l}dq` + eight `vpaddq` + `vextracti128`/`vpextrq`, then the `addq`/`adcq` chain, with the block loop (`jbe`) | "two ymm streams", "8 then 2 words per step" |
| `k_addwords5` | `AddWords32<5>` | the inline asm: `addl (%rsi), %eax` + five `adcl` | "one add/adc chain" |
| `k_skipbytes` | `SkipBytesOutsideRanges` | `vmovdqu` + `vpcmpestri $4, (%rdi), %xmm0`, the pointer arithmetic and the block loop | "SSE4.2 PCMPESTRI in ranges mode" |
| `k_probe4` | `MaskedWordCandidates64x4` | `vmovdqa (%rdi), %ymm0` (the inline asm's 32-byte load) + two `vpbroadcastq` + `vpcmpeqq` + `vmovmskpd` + `vzeroupper` | "one 32-byte vector load, issued as inline assembly" |
| `k_match16x8` | `MatchTags16x8` | `vpbroadcastw` + `vpcmpeqw` + `vpacksswb` + `vpmovmskb` + `vmovd` | "one vector compare, one mask" |
| `k_match32x4` | `MatchHashes32x4` | `jmp` - the dispatch itself; the AVX2 callee is `vpbroadcastd` + `vpcmpeqd` + `vmovmskps`, and the scalar callee (local, no symbol) is `vpcmpeqd` + `vpand` + a `vpsrldq`/`vpor` fold | the dispatched AVX2 compare |
| `k_vlan_remove` | `RemoveVlanTag` | `vmovdqu` + `vpslldq $4` + `vmovdqu` | "one 16-byte load, one byte shift, one store" |
| `k_vlan_insert` | `InsertVlanTag` | `vmovdqu 4(%rdi)` + `vpinsrd $3` + `vmovdqu` | "one 16-byte load, one lane insert, one store" |
| `k_maskpair16x2` | `MaskedAddrPair16x2` | two `vmovdqu` mask loads, two `vpand` with the address as the memory operand, two `vmovdqu` stores | "one 16-byte load, one AND and one 16-byte store per address" |

Every body is three to forty instructions, with no call, and the only branches
are the two length loops (`SkipBytesOutsideRanges`, `SumWords32`). The three
that are a single instruction plus their loads and stores (`Crc32c`,
`RemoveVlanTag`, `InsertVlanTag`) are as short as the operation allows.

## What this settles, and what it does not

- **Settled:** each kernel's claim in its header matches the emitted code, and
  the critical kernels are *not* left to the compiler's choice: the ones whose
  shape matters are intrinsics or inline assembly (`crc32q`, `vpcmpestri`,
  `vmovdqa`, the `add/adc` chains), and the ones written portably
  (`MatchTags16x8`, `RemoveVlanTag`/`InsertVlanTag`) compile to a single vector
  operation as well. No kernel calls `rte_hash_crc` on this target: the CRC's
  fallback path is not reached where SSE4.2 exists.
- **Not covered, and why the item is not the whole of §2:** the typed hot paths
  `performance-contract.md` §2 names - `TypedExactTable`, `RangeClassifier`,
  `ScopeTable::Lookup` - are *callers* of these kernels and of their own
  generated plans, and no record measures them against a hand-written
  specialized arm. This page is the kernel half of "near-specialized assembly";
  the typed half has the matrix's throughput rows and the checked-in baseline,
  not a typed-vs-specialized ratio.
- **The dispatched one:** `MatchHashes32x4` is an indirect call through
  `RuntimeDispatch` (D-034, D-126), so its *callee* is one of the two bodies in
  `tag_match.h`; this review checked both bodies' shapes, not the dispatch's
  cost. The dispatch is one load and one indirect call, and D-034's measurement
  is the reason it exists.
- **Not settled here:** how the *callers* inline these (the review is of the
  kernels, not of each module's hot loop), and any target other than
  x86-64-v3. The arm64 paths (`__crc32cd`, NEON shuffles) are the same source
  under a different `#if`, and the CI's arm64 lane compiles them.

## Why this is a page and a wrapper rather than a script

An earlier note said binary inspection was "outside this program's tooling".
That is wrong for a *review*: the wrapper above is 60 lines, the comparison is
against a header comment that already states the expected instructions and the
measured alternative (each kernel's header records the delta it was chosen for -
11-25% for the checksum kernels, 7% for the IPv4 header, +10%/+56% for the word
probe's atomic-load alternative). What is *not* here is a checked-in disassembly
of a whole binary, which would be a build artifact rather than a review.
