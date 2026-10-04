# Fuzzing

The fuzz harnesses in `core/fuzz/` cover the dataplane's parsers and state
machines (roadmap M22). Each harness is `core/fuzz/<name>_fuzz.cc`, defines
`LLVMFuzzerTestOneInput`, and checks an oracle (a reference model,
differential or round trip) with `FUZZ_CHECK`, which aborts on a violation.
Harnesses use only EAL-free code: packets are hand-built `rte_mbuf` chains
(`bess::fuzz::MbufChain` in `core/fuzz/fuzz_support.h`) or byte spans, never
a mempool.

## Building

The Meson option `build_fuzzers` (default false) adds the harnesses.

- Clang: each harness links libFuzzer (`-fsanitize=fuzzer`). Sanitizers come
  from `b_sanitize`. Library code needs coverage instrumentation too, which is
  passed through `cpp_args`:

      CC=clang CXX=clang++ meson setup build/fuzz --buildtype=debug \
        -Db_sanitize=address,undefined -Db_lundef=false \
        -Dbuild_fuzzers=true -Dbuild_benchmarks=false \
        "-Dcpp_args=-fsanitize=fuzzer-no-link -fno-sanitize-recover=undefined -DPROTOBUF_MESSAGE_GLOBALS_TEMPORARY_OPTOUT"
      ninja -C build/fuzz fuzzers

  `-fno-sanitize-recover=undefined` makes UBSan findings abort, so the fuzzer
  reports them. `PROTOBUF_MESSAGE_GLOBALS_TEMPORARY_OPTOUT` keeps generated
  protobuf code on the message layout the (uninstrumented) system libprotobuf
  uses; without it protobuf's headers pick an ASan-only layout and every
  descriptor is null. The DPDK EAL does not start under ASan, which is why
  the harnesses never need it. libFuzzer builds also link
  `core/fuzz/libfuzzer_options.cc`, which turns off ASan's
  alloc-dealloc-mismatch check: the packaged libFuzzer runtime brings its own
  aligned `operator delete` that is linked ahead of ASan's.

- GCC (no libFuzzer): `-Dbuild_fuzzers=true` links each harness with
  `core/fuzz/replay_main.cc`, which runs every file (or every file inside each
  directory) given on the command line through the harness once.

Either way, `meson test --suite fuzz` replays each committed seed corpus
(`<name>_fuzz_corpus`; with libFuzzer, `-runs=0 <corpus dir>`).

## Running

    mkdir -p work/nat
    build/fuzz/core/nat_fuzz -max_total_time=120 -print_final_stats=1 \
      work/nat core/fuzz/corpus/nat

New inputs go into the first directory; keep it outside the source tree. A
crash writes `crash-<sha1>`; replay it with `build/fuzz/core/nat_fuzz
crash-<sha1>` (or with the GCC replay binary), minimise it with
`-minimize_crash=1 -runs=10000`.

## Targets

| Harness | Code under test | Oracle |
|---|---|---|
| `packet_cursor` | `packet::PacketCursor` over a segmented chain | flat-buffer reference cursor |
| `packet_mutation` | in-place prepend/append/remove-prefix/trim-suffix | byte-vector packet model and expected errors |
| `checksum_plan` | `InspectChecksumPlan`, `ComputeChecksums`, `BindTxFinalizationProfile` | reference one's-complement sums; segmentation independence |
| `classifier_schema` | `RuntimeClassifierSchema::Validate`, `ExtractPlan`, `ResultPlan` | naive per-field copy and mask; batch equals scalar |
| `resource_codec` | `framework::TypedCodec` with resource protobufs | round trip of accepted values; type-URL rule |
| `route_prefix` | `utils::Ipv4Prefix::Parse`, `utils::ParseIpv4Address`, `route::Ipv4Prefix::Make` | reference grammar parser; format/reparse round trip |
| `conntrack` | `conntrack::ParseFrame`, `Conntrack` Track/Expire/Remove | offsets within the frame; flow-table invariants and a key model |
| `nat` | `Nat::Translate`, `TranslateBatch`, `Expire` | checksums valid after translation; batch equals scalar; reply restores the tuple |
| `tunnel_decap` | VXLAN/Geneve/GRE/GTP-U decapsulation | inner offsets within the frame; `Write*` then `Decap*` recovers the fields |
| `control_transaction` | `control::DataplaneTransactions` decoding `ApplyTransactionRequest` | model of the committed resource state; all-or-nothing |

## Seeds

Seeds are generated, not hand-edited: `core/fuzz/seeds/<name>.py` returns
`{file name: bytes}` in that harness's input format (the format is described
at the top of the harness), and `core/fuzz/make_seeds.py` writes them to
`core/fuzz/corpus/<name>/`. To add a seed, add an entry to the generator and
run

    python3 core/fuzz/make_seeds.py <name>

then commit the generated file. `make_seeds.py --check` fails if a committed
corpus is out of date. A crash input that found a bug is kept as a named seed
so the corpus test replays it.

## Adding a harness

Write `core/fuzz/<name>_fuzz.cc` and `core/fuzz/seeds/<name>.py`, add
`<name>` to `fuzz_targets` in `core/meson.build` (with any library beyond
`component_runtime_whole` it needs), generate the corpus, and run the
harness for a few minutes. Keep it deterministic: no clocks, no unseeded
randomness, no state carried between inputs.
