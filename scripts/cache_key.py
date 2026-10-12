#!/usr/bin/env python3
"""Print a cache key for all proprietary inputs consumed by the GoW3R linker."""
import argparse
import hashlib
from pathlib import Path


INPUTS = ('eboot.bin', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx',
          'sce_sys/param.sfo')
CACHE_SCHEMA = b'GOW3IMG1\0VIDEO-ARENA-2.25-GIB\0'


def cache_key(game):
    digest = hashlib.sha256(CACHE_SCHEMA)
    for relative in INPUTS:
        path = Path(game) / relative
        digest.update(relative.encode('utf-8') + b'\0')
        with path.open('rb') as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b''):
                digest.update(block)
    return digest.hexdigest()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    args = parser.parse_args()
    try:
        print(cache_key(args.game))
    except OSError as error:
        parser.exit(2, f'cache input error: {error}\n')
