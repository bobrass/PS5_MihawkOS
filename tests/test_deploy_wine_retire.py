#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""tools/deploy-wine.py removes from the console only runtime files the previous deploy wrote and this build no longer
ships (runtime_files_gone): never a prefix, drive_c, a save, a game folder or a path that is not plain, whatever the
manifest read back from the console says."""
import importlib.util
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('deploy_wine', ROOT / 'tools/deploy-wine.py')
deploy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(deploy)


class RuntimeFilesGone(unittest.TestCase):
    def test_only_runtime_files_the_build_dropped(self):
        manifest = {'runtime/ge11-7/share/wine/fonts/msyh.ttf': 'a', 'prefixes/probe/drive_c/x.exe': 'b'}
        previous = {
            'runtime/ge11-7/share/wine/fonts/msyh.ttf': 'a',        # still shipped: kept
            'runtime/ge11-7/share/wine/fonts/msyhbd.ttf': 'c',      # dropped by this build: removed
            'runtime/ge11-7/lib/wine/prefix/drive_c/windows/Fonts/msyhbd.ttf': 'c',
            'runtime/ge11-7/deploy-manifest.json': 'd',             # the manifest itself: kept
            'prefixes/probe/drive_c/old-probe.exe': 'e',            # a prefix: never
            'prefixes/mihawk/drive_c/Games/A Game/game.exe': 'f',
            'run/proton-payload-child.elf': 'g',                    # outside the runtime: never
        }
        self.assertEqual(deploy.runtime_files_gone(previous, manifest), [
            'runtime/ge11-7/lib/wine/prefix/drive_c/windows/Fonts/msyhbd.ttf',
            'runtime/ge11-7/share/wine/fonts/msyhbd.ttf'])

    def test_a_path_that_leaves_the_runtime_is_never_removed(self):
        previous = dict.fromkeys([
            'runtime/ge11-7/../../prefixes/mihawk/user.reg', 'runtime/ge11-7/./x', 'runtime/ge11-7//x',
            'runtime/ge11-7/', 'runtime/ge11-7/a\\..\\..\\saves', '/data/ps5proton/runtime/ge11-7/x',
            'runtime/ge11-70/x', 'Runtime/ge11-7/x', 7, None], 'x')
        self.assertEqual(deploy.runtime_files_gone(previous, {}), [])

    def test_no_previous_deploy_removes_nothing(self):
        self.assertEqual(deploy.runtime_files_gone({}, {'runtime/ge11-7/x': 'a'}), [])


if __name__ == '__main__':
    unittest.main()
