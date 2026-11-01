#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0-only
"""Benchmark real CLI processes; report time and compressed-size tradeoffs."""
import argparse
import csv
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('baseline', 'candidate', 'zstd'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--input', type=Path, help='Optional corpus; default is deterministic mixed data')
    parser.add_argument('--repeats', type=int, default=5)
    parser.add_argument('--discard-output', action='store_true',
                        help='Time /dev/null output after a verified file-output warmup')
    args = parser.parse_args()
    rng = random.Random(47021)
    # Repeated records with fresh noise, not a zero-filled synthetic speed test.
    payload = args.input.read_bytes() if args.input else b''.join(
        (b'{"package":"lpm","operation":"install","path":"/usr/lib/"}\n' * 1200
         + rng.randbytes(32768)) for _ in range(384))
    binaries = {'baseline': str(args.baseline.resolve()), 'candidate': str(args.candidate.resolve())}
    output = csv.writer(sys.stdout)
    output.writerow(['operation', 'implementation', 'workers', 'output_sink', 'input_bytes',
                     'compressed_bytes', 'median_seconds', 'input_MiB_per_second'])
    with tempfile.TemporaryDirectory(prefix='pzstd-benchmark-') as temp:
        directory = Path(temp)
        source = directory / 'corpus'
        source.write_bytes(payload)
        # One identical ordinary concatenated stream isolates decoder changes.
        frames = b''.join(subprocess.check_output([str(args.zstd.resolve()), '-qc', '--check'],
                          input=payload[i:i + (1 << 20)])
                          for i in range(0, len(payload), 1 << 20))
        compressed = directory / 'ordinary-frames.zst'
        compressed.write_bytes(frames)
        for operation in ('compress', 'decompress'):
            for workers in (1, 2, 4, 8):
                timings = {name: [] for name in binaries}
                sizes = {}
                for trial in range(args.repeats + 1):
                    # Alternate order to reduce thermal / scheduling bias.
                    order = list(binaries) if trial % 2 else list(reversed(binaries))
                    for name in order:
                        dest = directory / ('out-' + name)
                        target = os.devnull if args.discard_output and trial else str(dest)
                        command = [binaries[name], '-qf', '-p' + str(workers), '-o', target]
                        command += ['-3', str(source)] if operation == 'compress' else ['-d', str(compressed)]
                        start = time.perf_counter()
                        subprocess.run(command, check=True, stderr=subprocess.DEVNULL)
                        elapsed = time.perf_counter() - start
                        if trial:
                            timings[name].append(elapsed)
                        if trial == 0:
                            sizes[name] = dest.stat().st_size if operation == 'compress' else len(frames)
                        if operation == 'decompress' and (not args.discard_output or trial == 0):
                            assert dest.read_bytes() == payload
                        elif trial == 0:
                            restored = subprocess.check_output(
                                [str(args.zstd.resolve()), '-dqc', str(dest)])
                            assert restored == payload
                for name in binaries:
                    seconds = statistics.median(timings[name])
                    output.writerow([operation, name, workers, 'discard' if args.discard_output else 'file',
                                     len(payload), sizes[name],
                                     f'{seconds:.6f}', f'{len(payload)/(1<<20)/seconds:.3f}'])
                    sys.stdout.flush()


if __name__ == '__main__':
    main()
