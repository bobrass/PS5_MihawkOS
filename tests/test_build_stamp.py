#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/build-stamp.py: a stage runs again exactly when the content of a repository file it reads, of an outside
input or of an output changed, or its text, or its last run failed; it is skipped otherwise, also when a file was
rewritten with the same bytes. Checked in a scratch git repository."""
import argparse
import importlib.util
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('build_stamp', ROOT / 'tools/build-stamp.py')
stamp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stamp)


class StageTest(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        base = Path(self.scratch.name)
        self.repo, self.outside, self.out = base / 'repo', base / 'sdk', base / 'out'
        for directory in (self.repo / 'wine', self.repo / 'ps5', self.repo / 'docs', self.outside, self.out):
            directory.mkdir(parents=True)
        (self.repo / 'wine/a.patch').write_text('a\n')
        (self.repo / 'ps5/title.cpp').write_text('t\n')
        (self.outside / 'lib.a').write_text('l\n')
        subprocess.check_call(['git', 'init', '-q'], cwd=self.repo)
        subprocess.check_call(['git', 'add', '-A'], cwd=self.repo)
        stamp.ROOT, stamp.STAMPS = self.repo, self.repo / '.git/build-stamps'
        self.runs = base / 'runs'

    def tearDown(self):
        self.scratch.cleanup()

    def stage(self, text='x', fail=False):
        script = f'echo run >> {self.runs}; echo built > {self.out}/thing; exit {1 if fail else 0}'
        args = argparse.Namespace(name='wine', repo_except=['ps5/'], input=[str(self.outside)], text=[text],
                                  output=[str(self.out)], command=['--', 'sh', '-c', script])
        status = stamp.run(args)
        return status, len(self.runs.read_text().splitlines()) if self.runs.exists() else 0

    def test_skips_and_reruns(self):
        self.assertEqual(self.stage(), (0, 1))
        self.assertEqual(self.stage(), (0, 1), 'nothing changed: skipped')
        (self.repo / 'ps5/title.cpp').write_text('t2\n')
        (self.repo / 'docs/x.md').write_text('note\n')
        self.assertEqual(self.stage(), (0, 1), 'an excluded prefix and notes do not count')
        (self.repo / 'wine/a.patch').write_text('a2\n')
        self.assertEqual(self.stage(), (0, 2), 'a tracked input changed')
        (self.repo / 'wine/b.patch').write_text('b\n')
        self.assertEqual(self.stage(), (0, 3), 'an untracked input appeared')
        os.utime(self.outside / 'lib.a', ns=(1, 1))
        self.assertEqual(self.stage(), (0, 3), 'an outside input rewritten with its own bytes')
        (self.outside / 'lib.a').write_text('L\n')
        self.assertEqual(self.stage(), (0, 4), 'an outside input changed, at the same size, in the same second')
        self.assertEqual(self.stage(text='y'), (0, 5), 'the arguments changed')
        (self.out / 'thing').write_text('built\n')
        self.assertEqual(self.stage(text='y'), (0, 5), 'an output rewritten with its own bytes')
        (self.out / 'thing').write_text('touched\n')
        self.assertEqual(self.stage(text='y'), (0, 6), 'an output changed')
        self.assertEqual(self.stage(text='y'), (0, 6))

    def test_link_inside_an_input_is_not_followed(self):
        (self.outside / 'z:').symlink_to('/')
        self.assertEqual(self.stage(), (0, 1))
        self.assertEqual(self.stage(), (0, 1))

    def test_failure_is_never_trusted(self):
        self.assertEqual(self.stage(), (0, 1))
        (self.repo / 'wine/a.patch').write_text('a2\n')
        self.assertEqual(self.stage(fail=True), (1, 2))
        (self.repo / 'wine/a.patch').write_text('a\n')
        self.assertEqual(self.stage(), (0, 3), 'back to the old inputs, but the stamp went with the failed run')

    def test_force(self):
        self.stage()
        os.environ['BUILD_STAMP_FORCE'] = '1'
        try:
            self.assertEqual(self.stage(), (0, 2))
        finally:
            del os.environ['BUILD_STAMP_FORCE']


if __name__ == '__main__':
    unittest.main(argv=[sys.argv[0]])
