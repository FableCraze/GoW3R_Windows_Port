#!/usr/bin/env python3
"""Validate the only supported game image: CUSA01623 update 1.02.

The loaded-image hash is independent of dump-tool SELF wrapper differences. This
module deliberately does not decrypt, download, repair, or copy game content.
"""
import argparse
import hashlib
from pathlib import Path

SUPPORTED_TITLE = 'CUSA01623'
SUPPORTED_VERSION = '01.02'
SUPPORTED_IMAGE = '79ed34bd3ef49a63c725485c3c286d38755f3c17be2af58c0cdc2e8ab23fa1ad'


def image_sha256(game):
    from prepare import parse_self, span
    elf, _header, ph, _segments, _missing = parse_self((Path(game) / 'eboot.bin').read_bytes())
    loads = [p for p in ph if p['type'] in (1, 0x61000010)]
    image = bytearray(max(p['vaddr'] + p['memsz'] for p in loads))
    for p in loads:
        image[p['vaddr']:p['vaddr'] + p['filesz']] = span(elf, p['offset'], p['filesz'])
    return hashlib.sha256(image).hexdigest()


def inspect(game):
    from prepare import sfo
    root = Path(game)
    try:
        info = sfo((root / 'sce_sys/param.sfo').read_bytes())
    except (OSError, ValueError, IndexError) as error:
        return {'ok': False, 'kind': 'unreadable_sfo', 'error': str(error),
                'title_id': '?', 'app_version': '?', 'image_sha256': None}
    title = info.get('TITLE_ID', '?')
    version = info.get('APP_VER', '?')
    try:
        digest = image_sha256(root)
    except (OSError, ValueError, IndexError, StopIteration, KeyError) as error:
        return {'ok': False, 'kind': 'unreadable_eboot', 'error': str(error),
                'title_id': title, 'app_version': version, 'image_sha256': None}
    kind = None
    if title != SUPPORTED_TITLE:
        kind = 'other_title'
    elif version != SUPPORTED_VERSION:
        kind = 'wrong_version'
    elif digest != SUPPORTED_IMAGE:
        kind = 'wrong_eboot'
    return {'ok': kind is None, 'kind': kind, 'title_id': title,
            'app_version': version, 'image_sha256': digest}


def problem(game, image_hash=None):
    result = inspect(game)
    if image_hash is not None:
        result['image_sha256'] = image_hash
        result['ok'] = (result['title_id'] == SUPPORTED_TITLE and
                        result['app_version'] == SUPPORTED_VERSION and
                        image_hash == SUPPORTED_IMAGE)
        result['kind'] = None if result['ok'] else 'wrong_eboot'
    if result['ok']:
        return None
    return result['kind'], result['title_id'], result['app_version']


def explain(kind, title, version):
    found = f'Found {title}, version {version}.'
    return {
        'other_title': f'{found} This port only supports CUSA01623.',
        'wrong_version': f'{found} Apply the official 1.02 update to the dump before launching.',
        'wrong_eboot': f'{found} eboot.bin does not match the supported CUSA01623 1.02 image.',
        'unreadable_sfo': 'Could not read sce_sys/param.sfo from the decrypted game dump.',
        'unreadable_eboot': f'{found} eboot.bin is not a compatible decrypted PS4 SELF.',
    }[kind]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    args = parser.parse_args()
    result = inspect(args.game)
    if not result['ok']:
        parser.exit(2, explain(result['kind'], result['title_id'], result['app_version']) + '\n')
    print(f"OK: {SUPPORTED_TITLE} {SUPPORTED_VERSION} | image {result['image_sha256']}")


if __name__ == '__main__':
    main()
