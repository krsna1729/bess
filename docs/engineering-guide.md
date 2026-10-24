# Engineering guide

This page gives the rules that review found broken again and again, so you
can get a change right the first time. Each rule names the reason or the
record behind it. The full contracts are in the linked documents; this page
does not repeat them.

## 1. Read first

| You change | Read |
|---|---|
| Any code | [architecture.md](architecture.md) §1-2 (layers), §10 (fast path); [ci-parity.md](ci-parity.md) (run CI's lanes locally) |
| A module that reads or writes packet bytes | §2 and §3 of this page; `core/packet_parse.h`, `core/packet_reshape.h`, `core/packet_mutation.h` |
| A lookup table | [dataplane-tables.md](dataplane-tables.md) |
| Hot-path performance | §5 of this page; [benchmarking.md](benchmarking.md); [performance-contract.md](performance-contract.md); [assembly-shape.md](assembly-shape.md) |
| An installed header or plugin surface | [plugin-api.md](plugin-api.md); architecture.md §5 |
| A `.proto` file | [compatibility.md](compatibility.md); [control-sdk.md](control-sdk.md) (regenerate the SDKs) |
| A fixed limit (`kMax*`, `MAX_*`) | [limits.md](limits.md) |

## 2. Packet-path correctness

A module is a reusable unit. Users connect modules in graphs that you do not
see, so a module never trusts that an upstream module checked the packet.

1. **Check the first segment.** Before you read or write `width` bytes at
   `offset`, make sure that the first segment's `data_len` is at least
   `offset + width` (D-121). `total_len` is not this length: a chained packet
   keeps the other bytes in other segments.
2. **Parse headers with the checked parse.** Use
   `bess::packet::CheckFastIpv4`, then `ParseFrame` (`core/packet_parse.h`,
   D-188). Do not write a new walk of EtherType, VLAN tags, IHL or IPv6
   extension headers. Do not read IPv4 fields at fixed offsets before you
   know that the frame is IPv4.
3. **Copy a shared payload before you write.** A clone, an indirect mbuf or
   an external buffer shares bytes with other packets. Use
   `packet::WritableHead` for in-place writes and `PrependWritable` for new
   headers (`core/packet_reshape.h`). The copy can replace the mbuf, so put
   the new handle back in the batch and get pointers again after the call
   (D-153).
4. **Drop a packet that you cannot finish, and count it.** Do not forward a
   packet with half of the change done: no header, the old header, or a
   wrapped length (D-153). A packet that your module does not apply to (for
   example, not IPv4 for an IPv4 rewrite) goes out unchanged, as the module's
   proto comment says.
5. **Every packet leaves.** Each packet goes to a gate or to `DropPacket`
   exactly once. Tests count "N in = N out + dropped".
6. **Do length arithmetic in `size_t` and check it.** A `uint16_t` length
   or offset wraps; an unchecked subtraction wraps. Use `MutationLengthFits`
   and `MutationRemovalLengthFits` (`core/packet_mutation.h`). Validate
   configured offsets and templates at `Init`, not per packet.
7. **Follow the protocol, not the common case.** Recent mistakes: a stored
   checksum of 0xffff is a correct -0 (RFC 1624); a UDP checksum covers
   `udp->length`, not the IP payload (RFC 768); a computed UDP checksum of 0
   is sent as 0xffff.
8. **A copy keeps its metadata.** Use `PacketCopyWithMetadata` or
   `PacketCloneWithMetadata`.

**Tests for a packet module.** Include these cases:
- short frames at every length up to the bytes the module touches;
- a chain split at each header boundary;
- a clone (after processing, the original's bytes do not change);
- no headroom (for prepends);
- the largest lengths, one byte below and one byte above each limit;
- tagged frames (VLAN, QinQ) and IPv4 options;
- frames the module does not apply to;
- the count "N in = N out + dropped".

Run each new test against the old code and record which tests fail. A test
that passes on the old code proves nothing about the fix.

## 3. The cost of checks: check cheaply, do expensive work once

A check that is one compare costs almost nothing. A full parse costs more.
When the packet-safety fixes (#82-#90) were measured, an inline headroom and
sharing check cost 0.4-0.8 ns per packet; a module that started to parse the
whole frame cost 4-5.5 ns more.

- **Keep the common case inline.** Make the fast test one inline compare,
  and put the copy, chain and error paths out of line (`[[gnu::cold,
  gnu::noinline]]`). `WritableHead` and `PrependWritable` have this shape.
- **Do not add a pass over the batch.** Drop in the same loop that
  processes the packet, not in a second compaction pass.
- **Parse once per module.** Pass the `ParsedFrame` (offsets only) to the
  functions that need it. Do not parse again in a helper.
- **Remove a check only when a mechanism makes it true.** A type, a
  `kRequired` metadata contract (D-177) or a validator check can prove a
  precondition; a comment or "our graph does not do that" cannot. If the
  proof can change with the graph, keep the checked path as the fallback.
- **Use what the hardware gives you,** for example NIC checksum offload,
  when a port reports it. Keep the software path for ports that do not.

## 4. Layering and reuse

The layers and the enforced edges are in architecture.md §1-2;
`check_includes.py` and `check_link_graph.py` enforce them.

- **Where code goes:**
  - **A library** (`route/`, `flow/`, `classifier/`, `meter/`, `l2/`,
    `utils/`, `dataplane/`) has the algorithm and the state. It does not
    include `module.h`, the runtime or protobuf.
  - **A module** (`core/modules/`) is a thin adapter: it reads arguments,
    calls the library and moves packets to gates.
  - **An appliance** (`examples/appliances/`) calls the same library
    functions without a module graph.
- **One implementation.** When two callers have the same logic, move it to
  one library function. If the copies drift apart, that is a bug. Examples:
  the forwarding chain (D-157), the checked parse (D-188), member selection
  (D-066), table plumbing (D-189), ARP resolution (D-175).
- **Search before you write.** Before you add a parser, checksum, hash,
  table, timer or COW helper, look in `core/packet_*.h`, `core/utils/`,
  `core/route/`, `core/classifier/` and `core/dataplane/`.
- **When a mechanism becomes a library:** it must meet the admission
  criteria in architecture.md §8. At a minimum, two different appliance
  models must use it without change.
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
3. **Use release builds on an isolated CPU.** Do not compare a
   `debugoptimized` build with a release build. Report IRQ contamination
   when the tool shows it. State the machine, compiler, flags and command.
4. **Make the two arms do the same work.** Check the results outside the
   timed region. A control arm must use the same frames and graph as the
   measured arm (benchmarking.md, "Classified live perf tests").
5. **Add a live_perf row before you change a module's hot path,** in its
   own first commit, so that the old code has a number.
6. **Make hot-loop helpers inline and cold paths cold.** A helper that the
   compiler does not inline costs a call per packet. Look at the assembly
   (`core/arch/asm_shape.cc`, assembly-shape.md) instead of guessing.
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
- **Docs show the final state.** Put before/after numbers in commit
  messages, not in repository docs. A changed decision gets a new D-nnn in
  `docs/decisions.md`; the old entry is marked *Superseded by*. Check the
  last number on `develop` just before you merge, because parallel branches
  take numbers too.
- **Run the gates that CI runs** (ci-parity.md). The ones that surprise
  people:
  - `check_tidy.py --strict` fails when a count goes down too. If your fix
    removes a clang-tidy finding, lower `tools/tidy_baseline.json` in the
    same commit. CI uses clang-tidy 19.
  - `check_limits.py`: each new fixed limit needs a row in `limits.md`.
  - `check_arch.py`: ISA code only in `core/arch/` or `*_kernels` files.
  - `check_installed_headers.py`: the installed headers equal
    `tools/api_classes.json`.
- **A `.proto` change** regenerates the Go and Python SDKs
  (`tools/gen_go_sdk.sh`, `tools/gen_python_sdk.sh`) in the same change.
- **A flaky test** gets its timing fixed, not its assertion loosened.
