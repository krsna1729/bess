# The assembly shape of `core/arch`'s kernels

Appendix J's "assembly review complete for critical kernels". The kernels are
header-only, so their shape is what the *release* flags make of them: this page
records the instructions each one compiles to, and checks the header's claim
against them. It is a review, not a benchmark.

Reproduce with a throwaway translation unit that calls each kernel and

```sh
g++ -O3 -march=x86-64-v3 -DNDEBUG -std=c++23 -I core -S -o kernels.s kernels.cc
```

which is the release shape (D-127: releases stay `-march=x86-64-v3`; AVX2 is in
that floor, AVX-512 is not).

## What each kernel is

| kernel | source | the emitted body | header's claim |
|---|---|---|---|
| `Crc32c(uint64_t, uint32_t)` | `arch/crc32c.h` | `movl` + `crc32q %rdi, %rax` + `ret` | the instruction |
| `SumWords32` (AVX2 path) | `arch/checksum_kernels.h` | `vmovdqu`/`vpunpck{h,l}dq` + `vpaddq` stream, `vextracti128`/`vpextrq` fold, then the `addq`/`adcq` asm chain | "two ymm streams", "8 then 2 words per step" |
| `AddWords32<5>` | `arch/checksum_kernels.h` | the inline asm: `addl (%rsi), %eax` + four `adcl` + `adcl $0` | "one add/adc chain" |
| `SkipBytesOutsideRanges` | `arch/byte_match.h` | `vmovdqu` + `vpcmpestri $4, (%rdi), %xmm0` + the pointer arithmetic | "SSE4.2 PCMPESTRI in ranges mode" |
| `MaskedWordCandidates64x4` | `arch/word_probe.h` | `vmovdqa (%rdi), %ymm0` (the inline asm's 32-byte load) + two `vpbroadcastq` + the compare and mask | "one 32-byte vector load, issued as inline assembly" |
| `MatchTags16x8` | `arch/tag_match.h` | `vpbroadcastd` + `vpcmpeqd` + `vmovmskps` | "one vector compare, one mask" |
| `MatchHashes32x4` | `arch/tag_match.h` | `vpbroadcastd` + `vpcmpeqd` + `vpand` + `vpsrldq`/`vpor` folds + `vmovd` | the dispatched AVX2 compare |
| `RemoveVlanTag` | `arch/vlan.h` | `vmovdqu` + `vpslldq $4` + `vmovdqu` | "one 16-byte load, one byte shift, one store" |
| `InsertVlanTag` | `arch/vlan.h` | `vmovdqu 4(%rdi)` + `vpinsrd $3` + `vmovdqu` | "one 16-byte load, one lane insert, one store" |

Every body is three to twenty instructions, with no call, no branch and no
scalar fallback on this target. The three that are a single instruction plus
their loads and stores (`Crc32c`, `RemoveVlanTag`, `InsertVlanTag`) are as short
as the operation allows.

## What this settles, and what it does not

- **Settled:** each kernel's claim in its header matches the emitted code, and
  the critical kernels are *not* left to the compiler's choice: the ones whose
  shape matters are intrinsics or inline assembly (`crc32q`, `pcmpestri`,
  `vmovdqa`, the `add/adc` chains), and the two that are written portably
  (`MatchTags16x8`, `RemoveVlanTag`/`InsertVlanTag`) compile to a single vector
  operation as well.
- **The dispatched one:** `MatchHashes32x4` is an indirect call through
  `RuntimeDispatch` (D-034, D-126), so its *callee* is one of the two bodies in
  `tag_match.h`; this review checked the AVX2 body's shape, not the dispatch's
  cost. The dispatch is one load and one indirect call, and D-034's measurement
  is the reason it exists.
- **Not settled here:** how the *callers* inline these (the review is of the
  kernels, not of each module's hot loop), and any target other than
  x86-64-v3. The arm64 paths (`__crc32cd`, NEON shuffles) are the same source
  under a different `#if`, and the CI's arm64 lane compiles them.

## Why this is a page rather than a script

An earlier note said binary inspection was "outside this program's tooling".
That is wrong for a *review*: the reproduction above is two commands, and the
comparison is against a header comment that already states the expected
instructions and the measured alternative (each kernel's header records the
delta it was chosen for - 11-25% for the checksum kernels, 7% for the IPv4
header, +10%/+56% for the word probe's atomic-load alternative). What is *not*
here is a checked-in disassembly of a whole binary, which would be a build
artifact rather than a review.
