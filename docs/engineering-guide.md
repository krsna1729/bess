# Engineering guide

This page gives the rules that review found broken again and again, so you
can get a change right the first time. Each rule names the reason or the
record behind it. The full contracts are in the linked documents; this page
does not repeat them.

**Outcomes, not mechanisms.** The rules say what code in this repository
must guarantee. The helpers they name are the usual way to get there, not
the only way. Authors of plugins and appliances outside this repository
keep the freedom of [architecture.md](architecture.md) §0 and §4: they
choose their own programming model, which BESS libraries they use and which
they bypass, and whether they use a module graph at all. For them, a
framework facility (a helper, a metadata contract, a capability declaration)
is offered, never required. In-tree module classes must declare their
capabilities (D-180).

## 1. Read first

| You change | Read |
|---|---|
| Any code | [architecture.md](architecture.md) §1-2 (layers), §10 (fast path); [ci-parity.md](ci-parity.md) (run CI's lanes locally) |
| A module that reads or writes packet bytes | §2 and §3 of this page; `core/packet_parse.h`, `core/packet_reshape.h`, `core/packet_mutation.h` |
| A lookup table | [dataplane-tables.md](dataplane-tables.md) |
| Hot-path performance | §5 of this page; [benchmarking.md](benchmarking.md); [performance-contract.md](performance-contract.md); [assembly-shape.md](assembly-shape.md) |
| An installed header or plugin surface | [plugin-api.md](plugin-api.md); architecture.md §5 |
| A `.proto` file | [compatibility.md](compatibility.md); [control-sdk.md](control-sdk.md) (regenerate the SDKs) |
| A fixed limit (`kMax*`, `kMin*`, `kDefault*`, `*Limit*`, `MAX_*`, `MIN_*`, `DEFAULT_*`) | [limits.md](limits.md) |

## 2. Packet-path correctness

A module is a reusable unit. Users connect modules in graphs that you do not
see, and packets come from ports. So a module checks every packet it reads
or writes; it never trusts that an upstream module checked it.

1. **Check the first segment.** Before you read or write `width` bytes at
   `offset`, make sure that the first segment's `data_len` is at least
   `offset + width` (D-121). `total_len` is not this length: a chained packet
   keeps the other bytes in other segments.
2. **Parse headers with a checked parse.** `bess::packet::CheckFastIpv4`
   and `ParseFrame` (`core/packet_parse.h`, D-188) are the shared one. A
   parser of your own (a narrower protocol set, a faster special case) is
   allowed if, on the parse corpus (`core/testing/parse_corpus.h`), it
   agrees with `ParseFrame` on every frame it accepts and rejects every frame
   in its protocol set that `ParseFrame` calls `kMalformed`. Do not read
   IPv4 fields at fixed offsets before you know that the frame is IPv4.
3. **Copy a shared payload before you write.** A clone, an indirect mbuf or
   an external buffer shares bytes with other packets.
   `packet::WritableHead` (in-place writes) and `PrependWritable` (new
   headers) in `core/packet_reshape.h` do this; another way that gives the
   same guarantee is fine. The copy can replace the mbuf, so put the new
   handle back in the batch and get pointers again after the copy (D-153).
4. **Never send out a packet that is half done.** A packet with no header,
   the old header or a wrapped length must not leave on a normal output gate;
   neither may a frame that the parse calls malformed. The module drops and
   counts it (what in-tree modules do, D-153) or sends it to an error gate
   that it documents. A frame that the module does not apply to (for
   example, not IPv4 for an IPv4 rewrite) goes out unchanged, or as the
   module documents. When the outcomes change, update the proto comment.
5. **Every packet leaves.** Each packet goes to a gate or to `DropPacket`
   exactly once. Tests count "N in = N out + dropped".
6. **Do length arithmetic in `size_t` and check it.** A `uint16_t` length
   or offset wraps, and an unchecked subtraction wraps. Test each
   subtraction and each narrowing to `uint16_t`. For prepends and strips,
   `PrependWritable` and `RemovePrefixInPlace` check the lengths for you.
   Validate configured offsets and templates at `Init`, not per packet.
   Validate output-gate values with `IsValidGateValue` before converting to
   `gate_idx_t`, not after. Split applies this to packet and metadata values;
   an out-of-range value drops instead of wrapping to another output.
7. **Follow the protocol, not the common case.** Verify a checksum by
   summing over the field and comparing with all ones (RFC 1071), so that a
   checksum stored as -0 (0xffff for 0x0000, RFC 1624) passes. A UDP
   checksum covers `udp->length`, not the IP payload (RFC 768). Over IPv4, a
   received UDP checksum of 0 means none was computed, and verify accepts it;
   a computed UDP checksum of 0 is sent as 0xffff (RFC 768).
8. **A copy keeps its metadata.** Use `PacketCopyWithMetadata` or
   `PacketCloneWithMetadata`.

**Tests for a packet module.** Include these cases:
- short frames at every length up to the bytes the module touches;
- a chain split at each header boundary;
- a clone (after processing, the original's bytes do not change);
- no headroom (for prepends);
- the largest lengths, one byte below and one byte above each limit;
- tagged frames (VLAN, QinQ) and IPv4 options;
- malformed frames, and frames the module does not apply to;
- the count "N in = N out + dropped".

Run each new test against the old code and record which tests fail. A test
that passes on the old code proves nothing about the fix.

## 3. The cost of checks: check cheaply, do expensive work once

A check of a few compares costs little. A full parse costs more. When the
packet-safety fixes (#82-#90) were measured, an inline length, headroom and
sharing check cost 0.4-1.3 ns per packet; a module that started to parse the
whole frame cost 4-5.5 ns more.

- **Keep the common case inline.** Make the fast test a few compares on
  fields the module already loads, and put the copy, chain and error paths
  in a separate, out-of-line function. `WritableHead` and `PrependWritable`
  have this shape.
- **Do not add a pass over the batch.** Drop in the same loop that
  processes the packet, not in a second compaction pass. `PacketBatch::KeepIf`
  is an optional ordered, in-loop compaction helper; direct loops are equally
  valid. For multi-field writes, the first field's loop can check the extent
  of every field before writing anything, then later fields reuse that proof.
  Keep field-major order when random draws or sequence state depend on it.
- **Parse once per module.** Pass the `ParsedFrame` to the functions that
  need it; do not parse again in a helper. After a copy, use its offsets,
  not pointers taken before the copy.
- **Remove a check only when it is proven in the same code.** A type, or an
  earlier check in the same module, can prove a precondition (for example,
  `EditPlan::ApplyTrusted` after the caller's own parse, D-063). A `kRequired`
  metadata contract (D-177) proves only that a writer of the attribute exists
  at resume. No graph mechanism proves a packet's length, parse or
  exclusivity, and a value that another module computed can be stale: a
  module between the two can rewrite the packet in place.
- **Prefer cheaper local checks to graph-wide assumptions** (D-192).
  Cross-module parse reuse is unproven: `ParsedFrame` caches addresses, ports
  and flags, not just offsets. NAT rewrites addresses and ports in place
  without changing the packet handle, data pointer or length. Reuse needs a
  proven mutation-free region; a descriptor stamp alone is not proof.
- **Keep graph specializations optional.** No module must use a Parse module,
  fuse with another module or notify the framework of every rewrite. A
  future specialization must prove its preconditions on every incoming path
  and fall back to the checked path when it cannot. No D-180 descriptor
  proves private storage, contiguity or length; an unspecified one is the
  worst case, where shared or chained packets may arrive.
- **Use what the hardware gives you,** for example NIC checksum offload,
  when a port reports it. Receive packet-type and checksum flags are hints,
  not proof of the bytes a module reads. Any use to avoid software work must
  be opt-in per port, with a checked software fallback for absent or
  unsupported hints. None of these options replaces the normal checked path.

## 4. Layering and reuse

The layers and the enforced edges are in architecture.md §1-2;
`check_includes.py` and `check_link_graph.py` enforce them.

- **Where code goes:**
  - **A library** (`route/`, `flow/`, `classifier/`, `meter/`, `l2/`,
    `utils/`, `dataplane/`) has the algorithm and the state. It does not
    include `module.h`, the runtime or protobuf.
  - **An in-tree module** (`core/modules/`) is a thin adapter: it reads
    arguments, calls the library and moves packets to gates (architecture.md
    §2, rule 5).
  - **An appliance** (`examples/appliances/`) calls the same library
    functions without a module graph.
  - **A plugin or an appliance outside this repository** may use any of
    these, bypass them, or bring its own.
- **One implementation.** When two in-tree callers have the same logic, move
  it to one library function. If the copies drift apart, that is a bug.
  Examples: the forwarding chain (D-157), the checked parse (D-188), member
  selection (D-066), table plumbing (D-189), ARP resolution (D-175).
- **Search before you write.** Before you add a parser, checksum, hash,
  table, timer or COW helper, look in `core/packet_*.h`, `core/utils/`,
  `core/route/`, `core/classifier/` and `core/dataplane/`.
- **When a mechanism becomes a library:** it meets the admission criteria in
  architecture.md §8, for example that two different appliance models would
  use it unchanged. A library gives mechanism, not policy: no appliance
  semantics, and calling it directly without a module graph stays possible.
- **Low-level code stays with its owner.** Intrinsics and assembly for one
  owner go in `<owner>_kernels.{h,cc}` beside it, under `BESS_ARCH_*` guards,
  with a portable fallback. Move the code to `core/arch/` only when a second,
  unrelated owner needs it (D-190; `check_arch.py`).
- **A module does not include another module's internal header.** Share
  through a library.

## 5. Performance work

1. **Measure before and after, as a pair.** Use `tools/ab_bench.py` (ABBA
   rounds) or `tests/perf/live_perf` with the A and B binaries in the same
   session. Call a difference only outside ±3% with at least 3/4 of pairs
   in agreement (benchmarking.md).
2. **Report instructions and cycles, not only ns.** Use `live_perf
   --counters`. Nanoseconds change with frequency, IRQs and code placement;
   instructions per packet show what changed.
3. **Measure release builds on an isolated CPU.** Do not compare
   `build/fast` (buildtype `plain`) with `build/perf-release`. Report IRQ
   contamination when the tool shows it. State the machine, power profile,
   compiler, flags and command (benchmarking.md).
4. **Make the two arms do the same work.** Check the results outside the
   timed region. A module row and its control differ only in the module
   under test: the control has a Bypass in the module's slot
   (benchmarking.md, "Classified live perf tests").
5. **Add a live_perf row before you change a module's hot path,** in its
   own first commit, so that the old code has a number.
6. **Make hot-loop helpers inline and slow paths out of line.** A helper
   that the compiler does not inline costs a call per packet. Look at the
   assembly (`core/arch/asm_shape.cc`, assembly-shape.md) instead of
   guessing.
7. **Keep a change only if a paired A/B shows a gain.** Revert an
   optimization that does not show one.
8. **Never trade correctness for speed.** Measure the cost of a safety
   check and record it; recover the cost with §3, not by removing the check.
9. **Do not write a kernel for hardware you cannot test** (no AVX-512
   without AVX-512 hardware, D-126). Every kernel has a portable fallback
   with an identity test.

## 6. Change hygiene

- **One concern per commit.** The message says what was wrong, what changed
  and why, the tests that fail on the parent, and performance old → new.
- **Reference docs show the final state.** Before/after numbers for a
  change go in its commit message. A decision's Evidence section in
  `docs/decisions.md` records the measurements behind the decision. A
  changed decision gets a new D-nnn; the old entry is marked *Superseded
  by*. Check the last number on `develop` just before you merge, because
  parallel branches take numbers too.
- **Run the gates that CI runs** (ci-parity.md). The ones that surprise
  people:
  - `check_tidy.py` fails when a count goes down too. If your fix removes a
    clang-tidy finding, lower `tools/tidy_baseline.json` in the same commit.
    The baseline is clang-tidy 19's. With another version the check prints
    a NOTICE and compares nothing; `--strict` (CI) makes that a failure. CI
    runs it in the clang ASan lane's `tidy` step. Run it on the host with
    clang-tidy 19: `tools/ci_profile.py all --compiler clang --sanitize
    address`, or its configure, build and tidy steps. `tools/ci_container.sh`
    passes no `--sanitize`, so its tidy step is skipped.
  - `check_limits.py`: each new fixed limit needs a row in `limits.md`.
  - `check_arch.py`: ISA code only in `core/arch/` or `*_kernels` files.
  - `check_installed_headers.py`: the installed headers equal
    `tools/api_classes.json`.
- **A `.proto` change** regenerates the Go and Python SDKs
  (`tools/gen_go_sdk.sh`, `tools/gen_python_sdk.sh`) in the same change.
- **A flaky test** gets its timing fixed, not its assertion loosened.
