# Multicore and Ryzen tuning validation

Date: 2026-11-01. Baseline: `49cf51799ea051457f57243527aa2058f999f005`
(zstd 1.6.0). Changes and tests were AI-assisted by OpenAI Codex at the
repository owner's request.

## Implemented scope

The existing `pzstd` tool now splits ordinary concatenated standard frames into
independent decoder jobs, including unknown-size frames and pipes. It scans
standard frame/block headers instead of depending on optional pzstd size hints.
Skippable payloads are consumed in bounded pieces. Only the writer emits bytes,
in frame order. Checksums are still checked by the unmodified zstd decoder.

Compression uses adaptive frame sizes for known-size files, preserving the old
upper bound and using a 1 MiB minimum. Compression input and decoded output
are batched in 1 MiB messages, empty output messages are suppressed, and decode
queues are bounded at four messages. Context factories can initialize workers
concurrently rather than holding the resource-pool mutex during allocation.
Null factories do not incorrectly register an outstanding borrowed resource.

Output flush/close errors are checked before success or `--rm`. Input/output
identity checks reject direct paths and hard-link aliases before output is
opened for overwrite. A stalled decoder cannot spin indefinitely on a truncated
stream after input has ended.

`cpu_tune` (Meson) and `ZSTD_CPU_TUNE` (Make/CMake) select compiler scheduling
for generic or Zen 1–5 processors, subject to compiler support. They do not
raise the instruction-set baseline. Existing BMI2 runtime dispatch and assembly
are retained. No new inline assembly was justified by these measurements; the
measured bottleneck was queueing/allocation and worker utilization.

The standard `zstd` CLI and libzstd compression/decompression APIs retain their
existing behavior. **Use `pzstd` for parallel frame decoding. A single ordinary
frame still has one decoder**, because its blocks share history. Dictionary
workflows remain supported by the standard CLI/library, not by pzstd.

## Correctness and ABI results

- GCC 13.3 builds with Meson 1.3.2/Ninja 1.11.1, Make, and CMake 3.28.3 passed
  using explicit `znver2` tuning. Unsupported `znver5` tuning was rejected by
  GCC 13 rather than silently falling back.
- All nine configured upstream Meson test groups passed: CLI/play tests,
  fullbench variants, streaming API tests, long matches, invalid dictionaries,
  decodecorpus, and thread pool tests. The slow/large-data suite and the
  zlib-gated standalone fuzzer were not enabled in this configuration.
- All 42 enabled pzstd/unit utility tests passed. The extremely-large disabled
  test was excluded. New resource-pool tests cover concurrent factories and
  null-return accounting.
- All 526 custom integrity checks passed against an untouched reference CLI,
  in normal, CMake, and ASan/UBSan builds. Coverage includes 1/2/4 workers,
  multiple levels and sizes, empty files, streaming input, unknown frame sizes,
  mixed standard/skippable frames, incorrect optional size hints, truncation,
  checksum damage, reserved block types, huge truncated skip lengths, and 256
  differential mutations. Also checked: `--rm` preservation on failures,
  `/dev/full` errors, and input/output hard-link aliases.
- The four ResourcePool tests passed under GCC ThreadSanitizer. This was a
  focused test of the changed shared-state code, not a full CLI TSan run.
- Public headers `zstd.h`, `zdict.h`, and `zstd_errors.h` are byte-identical to
  the baseline. Both shared libraries expose the same **188 symbols** and
  **SONAME `libzstd.so.1`**. A consumer compiled/linked against baseline headers
  and library ran with both libraries, exercising compression, MT compression,
  dictionary APIs, and decompression. These are ABI regression checks, not an
  exhaustive ABI proof for every platform or caller.
- No sanitizer errors were reported. LeakSanitizer was disabled locally because
  this execution environment blocks its process-inspection mechanism. Existing
  warnings in upstream invalid-dictionary fixtures/gtest/tmpnam tests were not
  part of the changed production code.

## Performance measurements

Builder: Intel Xeon Platinum 8573C virtual host, 9 logical CPUs visible, cgroup
quota equivalent to 8 CPUs. **No physical Ryzen benchmark was performed.**
Baseline and candidate used the same Meson release configuration; the candidate
added `-mtune=znver2`. Results combine the pipeline changes and compiler tuning;
they do not isolate or establish a Ryzen-specific speedup.

Each configuration used a warmup followed by **13 timed processes**, alternating
baseline/candidate order, reporting median elapsed time. Levels were 3 for
compression. Decoder comparisons use the exact same ordinary concatenation of
1 MiB frames for both implementations. Warmup output is decoded/compared using
the baseline tool; decoded outputs are also verified against the input.

Two output modes are recorded. `discard` writes timed output to `/dev/null`
after the verified file-output warmup, reducing storage bottlenecks. `file`
writes temporary files and includes filesystem/page-cache/writeback effects.
The virtual host has timing noise; these measurements are not production
throughput guarantees. Both improvements and regressions are retained below
and in the raw CSV files.

Corpora:

- Mixed: 39,770,112 bytes of deterministic records plus fresh pseudo-random
  noise, generated by `benchmark_parallel.py` with seed 47021.
- Source tar: 6,307,840 bytes from `git archive HEAD lib programs tests` in
  the baseline checkout. SHA256:
  `adb3a63d01c6f4fb1dfc048b4b2031178f0a203d903d9e106e2be2b15aec2dac`.

### Four-worker results

| Corpus | Output | Operation | Baseline MiB/s | Candidate MiB/s | Speed ratio | Compressed-size change |
|---|---|---|---:|---:|---:|---:|
| mixed | discard | compress | 830.7 | 910.6 | 1.10× | +0.00% |
| mixed | discard | decompress | 1521.9 | 2239.2 | 1.47× | +0.00% |
| mixed | file | compress | 447.9 | 450.4 | 1.01× | +0.00% |
| mixed | file | decompress | 763.3 | 723.4 | 0.95× | +0.00% |
| source | discard | compress | 131.3 | 267.3 | 2.04× | +7.52% |
| source | discard | decompress | 426.2 | 621.4 | 1.46× | +0.00% |
| source | file | compress | 147.9 | 306.5 | 2.07× | +7.52% |
| source | file | decompress | 401.9 | 478.2 | 1.19× | +0.00% |

The source corpus at four workers compressed roughly twice as fast in the
CPU-oriented test, at a **7.52% larger** compressed size. The four-worker
CPU-oriented decoder improved about 1.46–1.47× on both corpora. This does not
mean a single frame becomes parallel, or that files get a universal speedup.

Regressions matter: the source corpus's one-worker discard-output compression
was about 11% slower; its one-worker decoding was about 5% slower. The file-output
mixed corpus had regressions at several worker counts (including approximately
12% slower compression at eight workers and 26% slower decoding at two workers).
At eight workers, source-tar file-output decoding was approximately unchanged.
More workers are not always faster. Preserve ratio and benchmark a workload
before selecting a worker count. `-p1` retains the old frame-size policy.

### Reproduction

```sh
git fetch --depth=1 origin 49cf51799ea051457f57243527aa2058f999f005
git worktree add --detach ../baseline 49cf51799ea051457f57243527aa2058f999f005
meson setup baseline-build ../baseline/build/meson -Dbin_contrib=true -Dzlib=disabled -Dlzma=disabled -Dlz4=disabled
ninja -C baseline-build
meson setup build build/meson -Dbin_contrib=true -Dbin_tests=true -Dcpu_tune=znver2 -Dzlib=disabled -Dlzma=disabled -Dlz4=disabled
ninja -C build
meson test -C build --no-rebuild --print-errorlogs --num-processes 2
python3 tests/lpm/parallel_integrity.py --pzstd build/contrib/pzstd/pzstd --zstd baseline-build/programs/zstd
python3 tests/lpm/check_abi.py --baseline baseline-build/lib/libzstd.so.1.6.0 --candidate build/lib/libzstd.so.1.6.0 --baseline-source ../baseline
python3 tests/lpm/benchmark_parallel.py --baseline baseline-build/contrib/pzstd/pzstd --candidate build/contrib/pzstd/pzstd --zstd baseline-build/programs/zstd --repeats=13 --discard-output
```

For source-corpus measurements, archive the baseline checkout and pass the tar
with `--input`. Omit `--discard-output` to include temporary output-file I/O.
The GitHub workflow reproduces GCC/Clang builds, upstream/unit tests, integrity,
ABI checks, sanitizers, and CMake builds. Check its actual results separately.

## Integrity and compatibility limits

The changes preserve the standard format, default per-frame checksums, decoded
bytes in the tested cases, public headers, exports, and SONAME. They do not
constitute a 100% data-preservation guarantee. Output streams can contain a
valid prefix before an error is detected; callers must check exit status.
Power-loss durability, atomic file replacement, and cryptographic corruption
protection are not added. The existing `--no-check` option remains available.

Still needed: physical Ryzen generations, Windows/macOS/ARM, 32-bit builds,
full CLI ThreadSanitizer, longer fuzzing, injected allocation failures, and
large/adversarial real-world corpora. Compiler tuning is optional and does not
change the default portable build. Other architectures retain the portable
implementation; selecting an unsupported x86 tuning flag is an explicit error.
