#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""wineboot (patch 2103) counts a prefix as set up only once the service wine.inf's DefaultInstall registers last is
there; a prefix whose DefaultInstall never finished had no nsiproxy, so no \\\\.\\Nsi, and Steam never showed its
sign-in (a tester's Alpha 4 prefix, 2026-10-29). This holds wine.inf to that: the sentinel is the last AddService of
every DefaultInstall services section, and AddService starts it (0x800), so the repairing session has it at once."""
import re
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCH = next((ROOT / 'wine/patches').glob('2103-*.patch'))
SPSVCINST_STARTSERVICE = 0x800


def sentinel():
    match = re.search(r'^\+.*L"System\\\\CurrentControlSet\\\\Services\\\\(\w+)"', PATCH.read_text(), re.MULTILINE)
    if not match:
        raise AssertionError(f'{PATCH.name}: no Services key named')
    return match.group(1)


def sections(inf):
    """{section name: its lines}, comments and blank lines left out."""
    found, name = {}, None
    for line in inf.splitlines():
        line = line.split(';', 1)[0].strip()
        if re.fullmatch(r'\[[^\]]+\]', line):
            name = line[1:-1]
            found[name] = []
        elif line and name:
            found[name].append(line)
    return found


class WinebootInstallSentinel(unittest.TestCase):
    def test_the_patch_checks_nsiproxy(self):
        self.assertEqual(sentinel(), 'nsiproxy')

    def test_the_sentinel_is_what_default_install_registers_last_and_starts(self):
        inf = ROOT / '.deps/wine-ps5/source/loader/wine.inf.in'
        if not inf.is_file():
            self.skipTest('no Wine source: run tools/build-wine-ps5.sh')
        found = sections(inf.read_text())
        services = {name: lines for name, lines in found.items() if re.fullmatch(r'DefaultInstall(\.\w+)?\.Services', name)}
        self.assertGreaterEqual(len(services), 4, sorted(found))
        for name, lines in services.items():
            added = [line for line in lines if line.startswith('AddService=')]
            self.assertTrue(added, name)
            service, flags = added[-1].split('=', 1)[1].split(',')[:2]
            self.assertEqual(service.lower(), sentinel(), name)
            self.assertTrue(int(flags, 0) & SPSVCINST_STARTSERVICE, name)


if __name__ == '__main__':
    unittest.main()
