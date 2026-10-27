#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A slice of Wine's conformance tests as a battery case: tools/run-battery.py runs it like any other case.

  tools/run-wine-slice.py --name RUN DLL ARCH FILE...

The named test files of DLL run on the console in one launch (tools/run-wine-tests.py console --force), then are
compared with the committed host baseline (compare FILE...), qualified failures included (<arch>/<dll>-expected.json).
evidence/RUN.json says whether the launch came back and the comparison passed. Only files that complete on both sides
with no console-only failure belong in a gate slice (docs/CONFORMANCE.md, "Gate").
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('dll')
    parser.add_argument('arch', choices=['x86_64', 'i386'])
    parser.add_argument('files', nargs='+')
    args = parser.parse_args()
    started = time.monotonic()
    runner = [sys.executable, str(ROOT / 'tools/run-wine-tests.py')]
    console = subprocess.run(runner + ['console', args.dll, *args.files, '--arch', args.arch, '--force',
                                       '--per-launch', str(len(args.files)), '--file-timeout', '120', '--gap', '0'], cwd=ROOT)
    record = json.loads((ROOT / f'evidence/wine-tests/{args.arch}/{args.dll}-console.json').read_text())
    runs = sorted({record[f].get('run') for f in args.files if record.get(f, {}).get('run')})
    launches = [json.loads(p.read_text()) for p in (ROOT / 'evidence' / (r + '.json') for r in runs) if p.is_file()]
    returned = bool(launches) and all(l.get('returned_to_launcher') for l in launches)
    compare = subprocess.run(runner + ['compare', args.dll, *args.files, '--arch', args.arch], cwd=ROOT, capture_output=True)
    verdict = json.loads((ROOT / f'evidence/wine-tests/{args.arch}/{args.dll}-comparison.json').read_text())
    passed = console.returncode == 0 and compare.returncode == 0 and returned
    (ROOT / 'evidence' / (args.name + '.json')).write_text(json.dumps({
        'run': args.name, 'dll': args.dll, 'arch': args.arch, 'files': args.files, 'launches': runs,
        'returned_to_launcher': returned, 'result': 'finished' if returned else 'stopped', 'passed': passed,
        'comparison': {k: verdict.get(k) for k in ('passed', 'totals', 'new_failures', 'unfinished', 'missing')},
        'seconds': round(time.monotonic() - started, 1)}, indent=1) + '\n')
    print(f'{args.name}: {"PASS" if passed else "FAIL"} ({args.dll} {args.arch}: {", ".join(args.files)})')
    return 0 if passed else 1


if __name__ == '__main__':
    sys.exit(main())
