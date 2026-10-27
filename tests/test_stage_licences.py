#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Every licence text a shipped binary needs: the list below is what the staged runtime's share/licenses and the title
folder's licenses/ must hold (tools/stage-wine.py, tools/stage-licences.py), the tool's tables must say exactly that,
its downloads must be pinned, and a missing source must stop the staging. With a production build present, the staged
runtime and dist/PPSA99170 are checked too."""
import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('stage_licences', ROOT / 'tools/stage-licences.py')
tool = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tool)

STAGE_WINE = tool.STAGE_WINE  # staged by tools/stage-wine.py itself
OWN = ['ps5-proton/LICENSE', 'ps5-proton/NOTICE.md', 'ps5-proton/LICENSING.md']
# Staged by tools/stage-licences.py.
RUNTIME = OWN + ['wine/COPYING.LIB', 'wine/LICENSE', 'wine/AUTHORS', 'vkd3d-proton/COPYING', 'vkd3d-proton/LICENSE',
                 'vkd3d-proton/AUTHORS', 'gnutls/COPYING', 'gnutls/COPYING.LESSERv2', 'gnutls/AUTHORS',
                 'nettle/COPYING.LESSERv3', 'nettle/COPYINGv2', 'nettle/COPYINGv3', 'nettle/AUTHORS',
                 'freetype/LICENSE.TXT', 'freetype/FTL.TXT', 'freetype/GPLv2.TXT', 'mesa/license.rst',
                 'wine-mono/COPYING', 'icu/LICENSE', 'ca-certificates/MPL-2.0.txt', 'llvm-mingw/LICENSE.TXT',
                 'mingw-w64/COPYING.MinGW-w64-runtime.txt', 'fonts/README.txt', 'fonts/Liberation-1.x-License.txt',
                 'fonts/GPL-2.0.txt', 'fonts/Apache-2.0.txt', 'wine-mono/SOURCE.txt', 'wine-mono/GPL-2.0.txt',
                 'wine-mono/LGPL-2.0.txt', 'wine-mono/LGPL-2.1.txt', 'wine-mono/MS-PL.txt', 'wine-gecko/SOURCE.txt',
                 'wine-gecko/MPL-2.0.txt'] + [f'ps5-opengl/{name}' for name in tool.OPENGL_SDK_TEXTS]
TITLE = OWN + ['lapy-daemon/LICENSE', 'mesa/license.rst', 'sascha-willems-vulkan/LICENSE.md', 'volk/LICENSE.md',
               'imgui/LICENSE.txt', 'ktx/LICENSE.md', 'tinygltf/LICENSE', 'nlohmann-json/LICENSE.MIT', 'glm/manual.md', 'zstd/LICENSE', 'mbedtls/LICENSE',
               'monocypher/LICENCE.md', 'libmicrohttpd/COPYING']


class StageLicences(unittest.TestCase):
    def test_tables_are_the_list(self):
        listed = subprocess.run(['python3', str(ROOT / 'tools/stage-licences.py'), '--list'], capture_output=True,
                                text=True, check=True).stdout.split('\n')
        self.assertEqual([line for line in listed if line],
                         [f'runtime {d}' for d in RUNTIME] + [f'title {d}' for d in TITLE])

    def test_repository_sources_exist(self):
        for table in (tool.RUNTIME, tool.TITLE):
            for name in table.values():
                if not name.startswith(('.deps/', 'download:', 'text:')):
                    self.assertTrue((ROOT / name).is_file(), name)

    def test_downloads_are_pinned(self):
        for name, (url, digest) in tool.DOWNLOADS.items():
            self.assertRegex(url, r'^https://(raw\.githubusercontent\.com/[^/]+/[^/]+/|salsa\.debian\.org/[^/]+/[^/]+/-/raw/)'
                                  r'[^/]+/', name)
            self.assertNotRegex(url, r'/(main|master|HEAD)/', name)  # a tag, not a moving branch
            self.assertRegex(digest, r'^[0-9a-f]{64}$', name)
        used = {n[len('download:'):] for t in (tool.RUNTIME, tool.TITLE) for n in t.values() if n.startswith('download:')}
        self.assertEqual(used, set(tool.DOWNLOADS))
        notes = {n[len('text:'):] for t in (tool.RUNTIME, tool.TITLE) for n in t.values() if n.startswith('text:')}
        self.assertEqual(notes, set(tool.TEXTS))

    def test_a_missing_source_stops_before_anything_is_written(self):
        with tempfile.TemporaryDirectory() as folder:
            with self.assertRaises(SystemExit):
                tool.stage({'a/LICENSE': 'LICENSE', 'b/LICENSE': 'no/such/licence'}, Path(folder))
            self.assertEqual(list(Path(folder).iterdir()), [])

    def test_a_production_build_staged_them(self):
        licences, title = ROOT / '.deps/staged/runtime/ge11-7/share/licenses', ROOT / 'dist/PPSA99170/licenses'
        if not licences.is_dir() or not title.is_dir():
            self.skipTest('no production build here')
        for name in STAGE_WINE + RUNTIME:
            self.assertTrue((licences / name).is_file(), name)
        for name in TITLE:
            self.assertTrue((title / name).is_file(), name)


if __name__ == '__main__':
    unittest.main()
