# Parallel Zstandard (PZstandard)

Parallel Zstandard is a Pigz-like tool for Zstandard.
It provides Zstandard format compatible compression and decompression that is able to utilize multiple cores.
It breaks the input up into equal sized chunks and compresses each chunk independently into a Zstandard frame.
It then concatenates the frames together to produce the final compressed output.
Pzstandard will write a 12 byte header for each frame that is a skippable frame in the Zstandard format, which tells PZstandard the size of the next compressed frame.
PZstandard supports parallel decompression of independent standard frames,
including ordinary concatenated Zstandard frames without PZstandard headers.
Frame boundaries are read from the standard frame/block headers, including
when reading a pipe. Skippable payloads are skipped in bounded pieces; their
optional size hints are not trusted as frame indexes.

A single ordinary frame still uses one decoder because its blocks share
history. Its I/O and output handling can overlap decoding. Dictionaries remain
unsupported by this tool; use the standard `zstd` CLI for dictionary workflows.
The public libzstd ABI and standard CLI defaults are unchanged.

## Usage

PZstandard supports the same command line interface as Zstandard, but also provides the `-p` option to specify the number of threads.
Dictionary mode is not currently supported.

Basic usage

    pzstd input-file -o output-file -p num-threads -#          # Compression
    pzstd -d input-file -o output-file -p num-threads          # Decompression

PZstandard also supports piping and fifo pipes

    cat input-file | pzstd -p num-threads -# -c > /dev/null

For more options

    pzstd --help

PZstandard tries to pick a smart default number of threads if not specified (displayed in `pzstd --help`).
If this number is not suitable, during compilation you can define `PZSTD_NUM_THREADS` to the number of threads you prefer.

## Multicore and Ryzen fork changes

Regular files are split into power-of-two frames small enough to occupy the
requested workers, with a 1 MiB minimum and the existing window-based maximum.
Unknown-size streams retain the previous frame-size policy. More independent
frames can reduce compression ratio; `-p1` retains the previous single-worker
frame-size policy. The output remains standard concatenated `.zst` frames with
per-frame checksums enabled by default. Old Zstandard decoders can read it.

The reader batches block headers/payloads into 1 MiB input messages. Decoders
use 1 MiB output buffers and bounded queues (four messages per queue), and
contexts are initialized concurrently. This reduces allocator and queue
synchronization overhead. These queue limits do not impose a global memory
limit: each decoder also needs a frame window, and compression contexts depend
on level and window settings.

Build the library, CLI, and parallel tool with explicit Zen 2 scheduling tuning:

```sh
meson setup build build/meson -Dbin_contrib=true -Dcpu_tune=znver2
ninja -C build
build/contrib/pzstd/pzstd -p8 input -o input.zst
build/contrib/pzstd/pzstd -d -p8 input.zst -o restored
```

Other supported tuning names are `generic`, `znver1`, `znver3`, `znver4`, and
`znver5`, when supported by the selected compilers. The default adds no tuning
flag. `-mtune` changes instruction scheduling, not the ISA baseline. Existing
runtime BMI2 dispatch and upstream assembly remain enabled. User-provided
`-march` flags can still change the ISA; this option does not override them.
No unmeasured inline assembly was added.

Equivalent Make and CMake settings are `ZSTD_CPU_TUNE=znver2` and
`-DZSTD_CPU_TUNE=znver2`. Clean pzstd's Make objects before changing compiler
flags, as that existing build does not track flag changes for C++ objects.

See [the validation record](../../tests/lpm/VALIDATION.md) for measurements,
ABI checks, sanitizer coverage, reproduction commands, and limitations.

## Benchmarks

As a reference, PZstandard and Pigz were compared on an Intel Core i7 @ 3.1 GHz, each using 4 threads, with the [Silesia compression corpus](https://sun.aei.polsl.pl//~sdeor/index.php?page=silesia).

Compression Speed vs Ratio with 4 Threads | Decompression Speed with 4 Threads
------------------------------------------|-----------------------------------
![Compression Speed vs Ratio](images/Cspeed.png "Compression Speed vs Ratio") | ![Decompression Speed](images/Dspeed.png "Decompression Speed")

The test procedure was to run each of the following commands 2 times for each compression level, and take the minimum time.

    time pzstd -# -p 4    -c silesia.tar     > silesia.tar.zst
    time pzstd -d -p 4    -c silesia.tar.zst > /dev/null

    time pigz  -# -p 4 -k -c silesia.tar     > silesia.tar.gz
    time pigz  -d -p 4 -k -c silesia.tar.gz  > /dev/null

PZstandard was tested using compression levels 1-19, and Pigz was tested using compression levels 1-9.
Pigz cannot do parallel decompression, it simply does each of reading, decompression, and writing on separate threads.

## Tests

Tests require that you have [gtest](https://github.com/google/googletest) installed.
Set `GTEST_INC` and `GTEST_LIB` in `Makefile` to specify the location of the gtest headers and libraries.
Alternatively, run `make googletest`, which will clone googletest and build it.
Run `make tests && make check` to run tests.
