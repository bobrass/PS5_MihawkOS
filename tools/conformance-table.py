#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The conformance table of a DLL group: per test module, how many of its files (both architectures) match the PC,
were fixed, are qualified, or are still open (docs/CONFORMANCE.md).

  tools/conformance-table.py MODULE... [--json OUT]

A file, per architecture, is
  matching   complete on the console and the PC, with no failure line the PC does not have;
  fixed      matching now, and named in evidence/wine-tests/fixes.json with the change that made it so;
  qualified  complete (or accepted unfinished for a recorded cause), every console-only line qualified in <arch>/<module>-expected.json (tools/qualify-wine-tests.py);
  awaiting   qualified, but at least one line by a reason that rests on our own choice ("AWAITING OWNER": a patch, a
             service turned off, a feature not built), counted apart until the owner accepts it;
  open       anything else: an unqualified or OPEN line, a timeout, a crash, no result, or no PC result.
compare runs first for each module and architecture, so the counts are those of the records as they are.
"""
import argparse
import json
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'evidence/wine-tests'


def classify(module, arch, fixed):
    subprocess.run([sys.executable, str(ROOT / 'tools/run-wine-tests.py'), 'compare', module, '--arch', arch],
                   cwd=ROOT, capture_output=True)
    host = json.loads((OUT / arch / f'{module}-host.json').read_text()) if (OUT / arch / f'{module}-host.json').is_file() else {}
    console = json.loads((OUT / arch / f'{module}-console.json').read_text()) if (OUT / arch / f'{module}-console.json').is_file() else {}
    comparison = json.loads((OUT / arch / f'{module}-comparison.json').read_text()) if (OUT / arch / f'{module}-comparison.json').is_file() else {}
    new = Counter(line.split(':', 1)[0] for line in comparison.get('new_failures', []))
    known = Counter(line.split(':', 1)[0] for line in comparison.get('known_limitations', []))
    awaiting = {line.split(':', 1)[0] for line in comparison.get('known_limitations', []) if 'AWAITING OWNER' in line}
    unfinished = {line.split(':', 1)[0] for line in comparison.get('unfinished', []) + comparison.get('missing', [])}
    kinds = {}
    for name in sorted(set(host) | set(console)):
        c, h = console.get(name, {}), host.get(name, {})
        if c.get('status') == 'skipped':
            kinds[name] = 'open'      # the skip list is not a pass
        elif name in unfinished or new[name] or (c.get('status') != 'complete' and not known[name]):
            kinds[name] = 'open'
        elif h.get('status') != 'complete' and not known[name]:
            kinds[name] = 'open'
        elif known[name]:
            kinds[name] = 'awaiting' if name in awaiting else 'qualified'
        else:
            kinds[name] = 'fixed' if f'{module}:{name}:{arch}' in fixed else 'matching'
    return kinds


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('modules', nargs='+')
    parser.add_argument('--json')
    args = parser.parse_args()
    fixed = json.loads((OUT / 'fixes.json').read_text()) if (OUT / 'fixes.json').is_file() else {}
    rows, details = [], {}
    for module in args.modules:
        counts = Counter()
        for arch in ('x86_64', 'i386'):
            kinds = classify(module, arch, fixed)
            counts.update(kinds.values())
            details[f'{module} {arch}'] = kinds
        rows.append((module, sum(counts.values()), counts['matching'], counts['fixed'], counts['qualified'], counts['awaiting'],
                     counts['open']))
    print('| DLL | files (both builds) | matching | fixed | qualified | awaiting owner | open |')
    print('| --- | --- | --- | --- | --- | --- | --- |')
    for row in rows:
        print('| ' + ' | '.join(str(x) for x in row) + ' |')
    total = [sum(r[i] for r in rows) for i in range(1, 7)]
    print('| **all** | ' + ' | '.join(f'**{x}**' for x in total) + ' |')
    if args.json:
        Path(args.json).write_text(json.dumps({'rows': rows, 'files': details}, indent=1) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
