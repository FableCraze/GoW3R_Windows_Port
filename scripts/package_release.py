#!/usr/bin/env python3
"""Create a game-free binary release from an explicit allowlist."""
import argparse
import shutil
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FORBIDDEN_NAMES = {'eboot.bin', 'param.sfo', 'boot.bin', 'boot-linked.bin',
                   'eboot.elf', 'entry.bin'}
FORBIDDEN_SUFFIXES = {'.prx', '.self', '.wad', '.xvag', '.bsf', '.sfo', '.pkg'}
ROOT_FILES = ('LICENSE', 'README.md', 'CONTRIBUTING.md',
              'gow3r.ini', 'run.bat', 'launch_gui.bat', 'launcher.py')
SCRIPT_FILES = ('prepare.py', 'link_modules.py', 'game_check.py', 'cache_key.py',
                'content_profile.py', 'import_coverage.py', 'patches.py')
PATCH_FILES = ('God_of_War_III_Remastered.xml',)


def reject(path):
    lower = path.name.lower()
    return lower in FORBIDDEN_NAMES or path.suffix.lower() in FORBIDDEN_SUFFIXES


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out', type=Path, default=ROOT / 'dist/gow3r-port-windows-x64.zip')
    args = parser.parse_args()
    executable = ROOT / 'out/gow3r-port.exe'
    if not executable.is_file():
        parser.error('out/gow3r-port.exe does not exist; build first')
    files = [ROOT / name for name in ROOT_FILES]
    files += [ROOT / 'scripts' / name for name in SCRIPT_FILES]
    files += [ROOT / 'patches' / name for name in PATCH_FILES]
    files += [executable]
    files += sorted((ROOT / 'out').glob('*.dll'))
    files += sorted((ROOT / 'docs').glob('*.md'))
    files += [ROOT / 'gpu/VENDOR.txt']
    missing = [path for path in files if not path.is_file()]
    if missing:
        parser.error('allowlisted file is missing: ' + str(missing[0]))
    blocked = [path for path in files if reject(path)]
    if blocked:
        parser.error('proprietary file matched the release allowlist: ' + str(blocked[0]))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='gow3r-release-') as tmp:
        stage = Path(tmp) / 'gow3r-port'
        for source in files:
            if source.parent == ROOT:
                relative = Path(source.name)
            elif source.parent == ROOT / 'out':
                relative = Path(source.name)
            else:
                relative = source.relative_to(ROOT)
            target = stage / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
        offenders = [path for path in stage.rglob('*') if path.is_file() and reject(path)]
        if offenders:
            parser.error('release contains forbidden game content: ' + str(offenders[0]))
        with zipfile.ZipFile(args.out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for path in sorted(stage.rglob('*')):
                if path.is_file():
                    archive.write(path, path.relative_to(stage.parent))
    print(f'Created {args.out} ({len(files)} allowlisted files; no game content)')


if __name__ == '__main__':
    main()
