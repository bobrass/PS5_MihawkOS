#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The title launches every program with the native graphics DLLs the conformance runners verified (Proton's default
set). They once drifted: the title kept d3d12 builtin, so Wine's own d3d12 met DXVK's DXGI and no Direct3D 12 program
could create a device, while the runners' Direct3D 12 suites passed."""
import re
import subprocess
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def literal(path, pattern):
    match = re.search(pattern, (ROOT / path).read_text(), re.MULTILINE)
    if not match:
        raise AssertionError(f'{path}: no match for {pattern}')
    return match.group(1)


class DllOverrides(unittest.TestCase):
    def test_title_loads_what_the_runners_verified(self):
        vkd3d = literal('tools/run-vkd3d-tests.py', r"^OVERRIDES = '([^']+)'")
        wine = literal('tools/run-wine-tests.py', r"^DXVK_OVERRIDES = '([^']+)'")
        title = literal('native/proton_wine.cpp', r'default_overrides\[\] = "([^"]+)"')
        self.assertEqual(vkd3d, wine)
        self.assertEqual(title, vkd3d + ';bcrypt=b')
        self.assertIn('d3d12', title.split('=')[0].split(','))

    def test_every_module_drawing_through_direct3d_has_the_dxvk_oracle(self):
        # the modules in scope whose tests create a Direct3D device run on DXVK on the console (default_overrides), so
        # tools/run-wine-tests.py compares them with the PC on DXVK; a test file shared through PARENTSRC counts too
        source = ROOT / '.deps/wine-ps5/source/dlls'
        if not source.is_dir():
            self.skipTest('no Wine source: run tools/build-wine-ps5.sh')
        scope = subprocess.run([sys.executable, str(ROOT / 'tools/wine-test-scope.py')], capture_output=True, text=True,
                               check=True).stdout.split()
        device = re.compile(r'Direct3DCreate[89]|D3D1[012]CreateDevice|CreateDXGIFactory|IDirect3DDevice[89]_|ID3D1[01]Device_')
        drawing = set()
        for dll in scope:
            tests = source / dll / 'tests'
            files = list(tests.glob('*.c'))
            parent = re.search(r'PARENTSRC\s*=\s*\.\./\.\./(\S+)/tests', (tests / 'Makefile.in').read_text()) \
                if not files and (tests / 'Makefile.in').is_file() else None
            if parent:
                files = list((source / parent.group(1) / 'tests').glob('*.c'))
            if any(device.search(f.read_text(errors='replace')) for f in files):
                drawing.add(dll)
        d3d = set(literal('tools/run-wine-tests.py', r"^(D3D_DLLS = \([^)]*\))").split('=', 1)[1]
                  .replace('(', ' ').replace(')', ' ').replace(',', ' ').replace("'", ' ').split())
        self.assertEqual(drawing, d3d)


if __name__ == '__main__':
    unittest.main()
