#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0-only
"""Cross-decoder and malformed-stream regressions for the parallel CLI."""
import argparse
import hashlib
from pathlib import Path
import random
import struct
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--pzstd', required=True, type=Path)
    parser.add_argument('--zstd', required=True, type=Path,
                        help='Unmodified reference decoder/compressor')
    args = parser.parse_args()
    parallel = str(args.pzstd.resolve())
    reference = str(args.zstd.resolve())
    rng = random.Random(47021)
    checks = 0

    def assert_clean(result):
        assert result.returncode >= 0, ('process terminated by signal', result.stderr)
        for marker in (b'AddressSanitizer', b'UndefinedBehaviorSanitizer', b'runtime error:'):
            assert marker not in result.stderr, result.stderr

    def run(command, data=None, success=True):
        result = subprocess.run(command, input=data, capture_output=True, timeout=60)
        assert_clean(result)
        if (result.returncode == 0) != success:
            raise AssertionError((command, result.returncode, result.stderr[-2000:]))
        return result.stdout

    def decompress(data, expected):
        nonlocal checks
        for workers in (1, 2, 4):
            actual = run([parallel, '-dqc', '-p' + str(workers)], data)
            assert actual == expected, (workers, len(actual), len(expected))
            checks += 1

    with tempfile.TemporaryDirectory(prefix='pzstd-integrity-') as temp:
        source = Path(temp) / 'source'
        payloads = [b'', b'x', bytes(range(256)) * 19,
                    rng.randbytes(1048577),
                    (rng.randbytes(32768) + b'parallel zstandard\n' * 5000) * 128]
        for payload in payloads:
            source.write_bytes(payload)
            for level in (1, 3, 9):
                for workers in (1, 2, 4):
                    # A named file exercises known-size adaptive splitting.
                    encoded = run([parallel, '-qc', '-p' + str(workers),
                                   '-' + str(level), str(source)])
                    assert run([reference, '-dqc'], encoded) == payload
                    checks += 1
                    decompress(encoded, payload)
            # A pipe retains the fixed streaming frame-size policy.
            encoded = run([parallel, '-qc', '-p4'], payload)
            assert run([reference, '-dqc'], encoded) == payload
            decompress(encoded, payload)
            checks += 1

        pieces = [b'', rng.randbytes(25000), b'a' * 400000,
                  bytes(range(256)) * 8000, b'', rng.randbytes(390001)]
        # stdin compression leaves frame content sizes unknown.
        frames = [run([reference, '-qc', '--check'], data) for data in pieces]
        encoded = b''.join(frames)
        decompress(encoded, b''.join(pieces))
        skip = lambda magic, data: struct.pack('<II', magic, len(data)) + data
        skips = [skip(0x184D2A50 + i, bytes([i]) * (i * 73)) for i in range(16)]
        mixed = b''.join(skips) + encoded + skip(0x184D2A5F, b'tail')
        decompress(mixed, b''.join(pieces))
        # Incorrect optional pzstd size hints must not control frame boundaries.
        hinted = b''.join(skip(0x184D2A50, struct.pack('<I', 1)) + frame
                          for frame in frames)
        decompress(hinted, b''.join(pieces))

        small = run([reference, '-qc', '--check'], b'a' * 4096)
        damaged = [small[:cut] for cut in range(len(small))]
        corrupt = bytearray(small)
        corrupt[-1] ^= 0x80
        damaged += [bytes(corrupt), encoded + b'junk',
                    skip(0x184D2A50, b'hello')[:-1],
                    struct.pack('<II', 0x184D2A50, 0xffffffff) + b'x']
        # Reserved block type, after a valid frame header.
        # FHD has no dictionary, no content size, a window descriptor, checksum.
        damaged.append(struct.pack('<I', 0xFD2FB528) + b'\x04\x00\x07\x00\x00')
        for data in damaged:
            run([reference, '-tq'], data, success=False)
            for workers in (1, 4):
                run([parallel, '-dqc', '-p' + str(workers)], data, success=False)
                checks += 1
        # Differential mutation checks exercise the new untrusted block scanner.
        seeds = frames + [small]
        for _ in range(256):
            data = bytearray(rng.choice(seeds))
            action = rng.randrange(3)
            if action == 0:
                data[rng.randrange(len(data))] ^= 1 << rng.randrange(8)
            elif action == 1:
                del data[rng.randrange(len(data)):]
            else:
                data += rng.randbytes(rng.randrange(1, 20))
            expected = subprocess.run([reference, '-dqc'], input=data,
                                      capture_output=True, timeout=30)
            actual = subprocess.run([parallel, '-dqc', '-p4'], input=data,
                                    capture_output=True, timeout=30)
            assert_clean(expected)
            assert_clean(actual)
            assert (expected.returncode == 0) == (actual.returncode == 0), (
                'decoder disagreement', expected.stderr, actual.stderr)
            if expected.returncode == 0:
                assert expected.stdout == actual.stdout
            checks += 1
        # An explicit overwrite must never truncate an input or its aliases.
        for decode in (False, True):
            original = small if decode else b'input must not be overwritten'
            same = Path(temp) / ('same.zst' if decode else 'same')
            same.write_bytes(original)
            for hardlink in (False, True):
                destination = same
                if hardlink:
                    destination = Path(temp) / 'alias'
                    destination.hardlink_to(same)
                command = [parallel, '-qf', '-p2', '-o', str(destination), str(same)]
                if decode:
                    command.insert(1, '-d')
                run(command, success=False)
                assert same.read_bytes() == original
                if hardlink:
                    destination.unlink()
                checks += 1
        # Failed decoding must retain a --rm input file.
        bad = Path(temp) / 'bad.zst'
        bad.write_bytes(bytes(corrupt))
        run([parallel, '-dqf', '--rm', '-p4', str(bad)], success=False)
        assert bad.exists()
        checks += 1
        # Catch buffered write failures even when fwrite initially succeeds.
        if Path('/dev/full').exists():
            with open('/dev/full', 'wb') as output:
                result = subprocess.run([parallel, '-qc', '-p2'], input=b'small',
                                        stdout=output, stderr=subprocess.PIPE, timeout=30)
            assert_clean(result)
            assert result.returncode != 0, 'buffered output error was ignored'
            checks += 1
            source.write_bytes(b'small source must survive an output error')
            run([parallel, '-qf', '--rm', '-p2', '-o', '/dev/full', str(source)], success=False)
            assert source.read_bytes() == b'small source must survive an output error'
            checks += 1
        print(f'PASS: {checks} parallel integrity checks; corpus SHA256 '
              f'{hashlib.sha256(b"".join(payloads)).hexdigest()}')


if __name__ == '__main__':
    main()
