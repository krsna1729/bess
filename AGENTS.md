# Notes for coding agents

Read [docs/engineering-guide.md](docs/engineering-guide.md) before you
change code. It lists the rules that review enforces and links the full
contracts. The rules state outcomes; the helpers they name are the usual
way to meet them, not the only way. Plugin and appliance authors outside
this repository keep their own programming model (docs/architecture.md §0
and §4); in-tree module classes must declare their capabilities (D-180).

The most frequent review findings, in order:

1. **Packet bytes.** Check the first segment's `data_len`, use a checked
   parse (`core/packet_parse.h` is the shared one), copy a shared payload
   before you write (`core/packet_reshape.h`), and never send out a packet
   that is half done or malformed on a normal gate. Never assume that an
   upstream module checked the packet.
2. **Reuse.** Search `core/packet_*.h`, `core/utils/`, `core/route/`,
   `core/classifier/` and `core/dataplane/` before you write a parser,
   checksum, hash, table or COW helper. An in-tree module is a thin adapter
   over a library; a library stays usable without a module graph.
3. **Evidence.** Run each new test against the old code and say which tests
   fail. Measure performance as a paired A/B with instruction counts. Never
   report a number you did not measure.
4. **Gates.** Run what CI runs (docs/ci-parity.md). Lower
   `tools/tidy_baseline.json` when you remove a clang-tidy finding, and add
   a `docs/limits.md` row for each new fixed limit.
5. **Scope.** One concern per commit. Reference docs show the final state;
   a change's before and after numbers go in its commit message.
6. **Scale.** A change to a table, queue, loop over entries, tick or packet
   path runs the scale review checklist (engineering guide §8): cost follows
   what changed, proven by a count, not a time.
