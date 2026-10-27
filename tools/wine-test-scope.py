#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The test modules the console must pass, in the order they run (tests/wine-tests-scope-ps5.json).

  tools/wine-test-scope.py [--excluded]

Every dlls/*/tests and programs/*/tests directory of the pinned Wine tree (a program's as NAME.exe), minus the excluded
ones: first the order list (what games touch most), then the rest by name. --excluded prints the excluded ones with
their reasons instead.
"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / '.deps/wine-runtime/source'


def scope():
    rules = json.loads((ROOT / 'tests/wine-tests-scope-ps5.json').read_text())
    present = sorted([p.parent.name for p in (SOURCE / 'dlls').glob('*/tests') if p.is_dir()] +
                     [p.parent.name + '.exe' for p in (SOURCE / 'programs').glob('*/tests') if p.is_dir()])
    excluded = rules['excluded']
    first = [m for m in dict.fromkeys(rules['order']) if m in present and m not in excluded]
    return first + [m for m in present if m not in excluded and m not in first], {m: r for m, r in excluded.items() if m in present}


if __name__ == '__main__':
    modules, excluded = scope()
    if '--excluded' in sys.argv:
        print('\n'.join(f'{m}\t{r}' for m, r in excluded.items()))
    else:
        print('\n'.join(modules))
