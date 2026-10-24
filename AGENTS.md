# Notes for coding agents

Read [docs/engineering-guide.md](docs/engineering-guide.md) before you
change code. It lists the rules that review enforces and links the full
contracts. The most frequent review findings, in order:

1. **Packet bytes.** Check the first segment's `data_len`, parse with
   `core/packet_parse.h`, copy a shared payload before you write
   (`core/packet_reshape.h`), and drop and count a packet you cannot finish.
   Never assume an upstream module checked the packet.
2. **Reuse.** Search `core/packet_*.h`, `core/utils/`, `core/route/`,
   `core/classifier/` and `core/dataplane/` before you write a parser,
   checksum, hash, table or COW helper. A module is a thin adapter over a
   library.
3. **Evidence.** Run each new test against the old code and say which tests
   fail. Measure performance as a paired A/B with instruction counts. Never
   report a number you did not measure.
4. **Gates.** Run what CI runs (docs/ci-parity.md). Lower
   `tools/tidy_baseline.json` when you remove a clang-tidy finding, and add
   a `docs/limits.md` row for each new fixed limit.
5. **Scope.** One concern per commit. Docs show the final state; before and
   after numbers go in the commit message.
