#!/usr/bin/env python3
"""Validate the reproducible Windows x64 build prerequisites."""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def run(*args):
    try:
        return subprocess.run(args, text=True, capture_output=True, check=False)
    except OSError:
        return None


def main():
    errors = []
    print(f'Python: {sys.version.split()[0]}')
    if sys.version_info < (3, 10):
        errors.append('Python 3.10+ is required')
    for command in ('cmake', 'ninja', 'gcc', 'g++', 'ar', 'pkg-config'):
        path = shutil.which(command)
        print(f'{command}: {path or "MISSING"}')
        if not path:
            errors.append(f'{command} was not found in PATH')
    compiler = run('gcc', '-dumpmachine')
    target = compiler.stdout.strip() if compiler else ''
    print(f'compiler target: {target or "unknown"}')
    if target not in ('x86_64-w64-mingw32', 'x86_64-pc-windows-gnu'):
        errors.append('GCC is not a MinGW-w64 x86-64 compiler')
    packages = ('sdl3', 'vulkan', 'libavformat', 'libavcodec', 'libavutil',
                'libswscale', 'libswresample')
    pkg = shutil.which('pkg-config')
    if pkg:
        for package in packages:
            result = run(pkg, '--exists', package)
            ok = bool(result and result.returncode == 0)
            print(f'pkg-config {package}: {"OK" if ok else "MISSING"}')
            if not ok and not (package == 'sdl3' and os.environ.get('SDL3_DIR')):
                if not (package == 'vulkan' and os.environ.get('VULKAN_SDK')):
                    errors.append(f'pkg-config package {package} was not found')
    glslang = shutil.which('glslangValidator') or shutil.which('glslang')
    print(f'glslang: {glslang or "MISSING"}')
    if not glslang:
        errors.append('glslangValidator is required to compile host shaders')
    atrac = ROOT / 'third_party/LibAtrac9/C/src/libatrac9.c'
    print(f'LibAtrac9: {"OK" if atrac.exists() else "MISSING"}')
    if not atrac.exists():
        errors.append('third_party/LibAtrac9 is missing')
    if errors:
        print('\nBuild environment is incomplete:', file=sys.stderr)
        for error in errors:
            print(f'  - {error}', file=sys.stderr)
        return 2
    print('Build environment: OK')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
