#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/package-tester-probe.py: a probe result that identifies a console or a person is refused before packaging,
and a real one (fmem-tester-base-2026-10-25-05) passes."""
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('package_tester_probe', ROOT / 'tools/package-tester-probe.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)

REAL = """{"model": "unknown-model", "firmware_installed": "13.60", "firmware_reported": "13.59", "direct_pool_mib": 11712,
 "sony_helper": {"spawned": 1, "spawn_status": "0x12018f31", "jit_create": "0x80020001"},
 "save_errno": "/app0=0 /mnt/usb0=2 /mnt/usb1=2 /download0=0 "}"""


class PackageTesterProbe(unittest.TestCase):
    def test_real_result_passes(self):
        self.assertEqual(package.private_findings(REAL), [])

    def test_identifiers_refused(self):
        for text, what in (('host 192.0.2.20', 'an IPv4 address'), ('mac 00:00:5E:00:53:01', 'a MAC address'),
                           ('owner@example.com', 'an e-mail address'), ('id 0123456789abcdef0123', 'a long hexadecimal identifier')):
            self.assertIn(what, package.private_findings(text))

    def test_never_mihawkos_title(self):
        self.assertEqual(package.TITLE, 'PPSA99171')


if __name__ == '__main__':
    unittest.main()
