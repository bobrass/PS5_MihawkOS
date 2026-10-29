#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""platform/lapy's patch series applies to the pinned daemon, and the resident build it ships never exits over
missing logging.

A player's console may have no /data/homebrew/dev.conf, no log server and no /data/ps5proton/logs: the resident daemon
used to return 2 before doing any work, and the title then showed "No access to /data" (Alpha 5, fw 13.60, Payload
Manager). The daemon is a console payload, so its main() is checked as the preprocessor leaves it for the resident
build's flags (tools/build-lapy-validation.py --resident): its #if conditions use only LAPY_* macros, so empty stub
headers stand in for the SDK's. Skips without ../PS5-Lapy-JB-Daemon at the pin."""
import importlib.util
import io
import os
import re
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_lapy', ROOT / 'tools/build-lapy-validation.py')
build_lapy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(build_lapy)
LAPY = ROOT.parent / 'PS5-Lapy-JB-Daemon'
RESIDENT = {'LAPY_SERVICE': 1, 'LAPY_ELF_HELPER': 0, 'LAPY_REQUIRE_CLIENT_RESULT': 1, 'LAPY_MAX_REQUESTS': 0}


def main_body(source_dir, defines):
    """owned_root_daemon.c's main() after the preprocessor, for these LAPY_* values."""
    stubs = source_dir / 'stub-include'
    stubs.mkdir()
    for path in (source_dir / 'source').iterdir():
        for name in re.findall(r'#\s*include\s*[<"]([^>"]+)[>"]', path.read_text(errors='replace')):
            if not (source_dir / 'source' / name).exists():
                (stubs / name).parent.mkdir(parents=True, exist_ok=True)
                (stubs / name).touch()
    text = subprocess.run([os.environ.get('CC', 'cc'), '-E', '-P', '-nostdinc', '-I', str(stubs)] +
                          [f'-D{k}={v}' for k, v in defines.items()] +
                          [str(source_dir / 'source/owned_root_daemon.c')],
                          capture_output=True, text=True, check=True).stdout
    start = text.index('int main(void)')
    depth, i = 0, text.index('{', start)
    for i in range(i, len(text)):
        depth += {'{': 1, '}': -1}.get(text[i], 0)
        if depth == 0:
            return text[start:i + 1]
    raise AssertionError('main() has no closing brace')


class LapyPatchesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        archive = subprocess.run(['git', '-C', str(LAPY), 'archive', build_lapy.REV], capture_output=True)
        if archive.returncode:
            raise unittest.SkipTest(f'no {LAPY.name} at {build_lapy.REV[:7]} beside the checkout')
        cls.temp = tempfile.TemporaryDirectory()
        cls.source = Path(cls.temp.name)
        with tarfile.open(fileobj=io.BytesIO(archive.stdout)) as tar:
            tar.extractall(cls.source, filter='data')
        for patch in build_lapy.PATCHES:
            subprocess.run(['patch', '-s', '-p1', '-i', str(ROOT / 'platform/lapy' / patch)], cwd=cls.source,
                           check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def test_resident_daemon_never_exits_over_logging(self):
        body = main_body(self.source, RESIDENT)
        self.assertIn('ps5log_init_default', body)  # logging is still attempted
        self.assertIsNone(re.search(r'\breturn\s+2\s*;', body), 'the resident daemon can still return 2')


if __name__ == '__main__':
    unittest.main()
