#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0-only
"""Check exported symbols/SONAME/headers and run a baseline-linked consumer."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

CLIENT = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zstd.h"
#define CHECK(x) do { size_t r = (x); if (ZSTD_isError(r)) { \
    fprintf(stderr, "%s\n", ZSTD_getErrorName(r)); return 1; } } while (0)
int main(void) {
    const size_t size = 4 * 1024 * 1024;
    unsigned char *input = malloc(size), *output = malloc(size);
    void *compressed = malloc(ZSTD_compressBound(size));
    ZSTD_CCtx *cctx = ZSTD_createCCtx();
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    size_t i, csize, dsize;
    if (!input || !output || !compressed || !cctx || !dctx) return 2;
    for (i = 0; i < size; i++) input[i] = (unsigned char)(i * 37 + i / 4096);
    CHECK(ZSTD_CCtx_setParameter(cctx, ZSTD_c_nbWorkers, 2));
    CHECK(ZSTD_CCtx_setParameter(cctx, ZSTD_c_checksumFlag, 1));
    csize = ZSTD_compress2(cctx, compressed, ZSTD_compressBound(size), input, size);
    CHECK(csize);
    dsize = ZSTD_decompressDCtx(dctx, output, size, compressed, csize);
    CHECK(dsize);
    if (dsize != size || memcmp(input, output, size)) return 3;
    CHECK(ZSTD_CCtx_reset(cctx, ZSTD_reset_session_and_parameters));
    csize = ZSTD_compress_usingDict(cctx, compressed, ZSTD_compressBound(size),
                                  input, size, input, 65536, 3);
    CHECK(csize);
    dsize = ZSTD_decompress_usingDict(dctx, output, size, compressed, csize, input, 65536);
    CHECK(dsize);
    if (dsize != size || memcmp(input, output, size)) return 4;
    ZSTD_freeCCtx(cctx); ZSTD_freeDCtx(dctx);
    free(input); free(output); free(compressed);
    puts("baseline-linked compression, MT compression, dictionary and decoding passed");
    return 0;
}
'''


def symbols(library):
    data = subprocess.check_output(['nm', '-D', '--defined-only', str(library)], text=True)
    return {line.split()[-1] for line in data.splitlines()}


def soname(library):
    data = subprocess.check_output(['readelf', '-d', str(library)], text=True)
    return re.search(r'\(SONAME\).*?\[(.*?)\]', data).group(1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', type=Path, required=True, help='Baseline shared library')
    parser.add_argument('--candidate', type=Path, required=True, help='Candidate shared library')
    parser.add_argument('--baseline-source', type=Path, required=True)
    parser.add_argument('--source', type=Path, default=Path(__file__).resolve().parents[2])
    args = parser.parse_args()
    base, candidate = args.baseline.resolve(), args.candidate.resolve()
    assert symbols(base) == symbols(candidate), 'exported symbol set changed'
    assert soname(base) == soname(candidate), 'SONAME changed'
    for name in ('zstd.h', 'zdict.h', 'zstd_errors.h'):
        assert (args.baseline_source / 'lib' / name).read_bytes() == (args.source / 'lib' / name).read_bytes(), name
    with tempfile.TemporaryDirectory(prefix='zstd-abi-') as temp:
        source = Path(temp) / 'consumer.c'
        binary = Path(temp) / 'consumer'
        source.write_text(CLIENT)
        subprocess.run(['cc', '-O2', '-I' + str(args.baseline_source / 'lib'), str(source),
                        '-L' + str(base.parent), '-Wl,-rpath,' + str(base.parent),
                        '-lzstd', '-o', str(binary)], check=True)
        for directory in (base.parent, candidate.parent):
            env = dict(os.environ, LD_LIBRARY_PATH=str(directory))
            subprocess.run([str(binary)], env=env, check=True)
    print(f'PASS: {len(symbols(base))} identical exports, SONAME {soname(base)}, '
          'unchanged public headers and baseline-linked consumer')


if __name__ == '__main__':
    main()
