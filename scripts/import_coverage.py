#!/usr/bin/env python3
"""Audit GOW3IMG1 host imports against the runtime and vendored GPU exports."""
import argparse
import json
import re
from pathlib import Path


def source_text(root, pattern):
    return '\n'.join(path.read_text(encoding='utf-8', errors='replace')
                      for path in root.glob(pattern) if path.is_file())


def audit(root, link_report):
    root = Path(root)
    report = json.loads(Path(link_report).read_text())
    imports = report['unresolved_imports']

    names_text = (root / 'src/import_names.inc').read_text(encoding='utf-8')
    symbols = {}
    for scoped, symbol in re.findall(r'\{"([^"]+)","([^"]+)"\},', names_text):
        symbols.setdefault(scoped.split('#', 1)[0], symbol)

    runtime = source_text(root / 'src', 'runtime*.c')
    direct_nids = set(re.findall(r'nid_matches\(name,\s*"([^"]+)"\)', runtime))
    direct_nids.update(value.split('#', 1)[0] for value in
                       re.findall(r'(?:strcmp|strncmp)\(name,\s*"([^"]+)"', runtime)
                       if '#' in value)
    export_keys = set(re.findall(r'\{\s*"([^"]+)"\s*,', runtime))
    # Some legacy tables are keyed directly by a scoped NID while canonical
    # tables are keyed by the public symbol name.  Account for both forms.
    direct_nids.update(key.split('#', 1)[0] for key in export_keys if '#' in key)
    implemented_symbols = {key for key in export_keys if '#' not in key}

    gpu = source_text(root / 'gpu/shadps4', '**/*.cpp') + source_text(root / 'gpu/shim', '**/*.cpp')
    gpu_nids = set(re.findall(r'LIB_FUNCTION\("([^"]+)"', gpu))

    covered, missing = [], []
    for scoped in imports:
        nid = scoped.split('#', 1)[0]
        symbol = symbols.get(nid)
        provider = None
        if nid in direct_nids:
            provider = 'runtime-direct'
        elif nid in gpu_nids:
            provider = 'shadps4-gpu'
        elif symbol in implemented_symbols:
            provider = 'runtime-hle'
        row = {'import': scoped, 'nid': nid, 'symbol': symbol, 'provider': provider}
        (covered if provider else missing).append(row)
    return {
        'format': report.get('format'),
        'native_bindings': report.get('bindings', 0),
        'host_imports': len(imports),
        'covered_host_imports': len(covered),
        'missing_host_imports': len(missing),
        'covered': covered,
        'missing': missing,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('link_report', type=Path)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parent.parent)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--strict', action='store_true')
    args = parser.parse_args()
    result = audit(args.root, args.link_report)
    text = json.dumps(result, indent=2) + '\n'
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text)
    print(f"Host import coverage: {result['covered_host_imports']}/{result['host_imports']}; "
          f"missing={result['missing_host_imports']}; native bindings={result['native_bindings']}")
    for row in result['missing']:
        print(f"MISSING {row['import']} ({row['symbol'] or 'unknown symbol'})")
    if args.strict and result['missing']:
        raise SystemExit(3)


if __name__ == '__main__':
    main()
