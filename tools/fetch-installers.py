#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch the third-party installers of the installer slice, pinned by URL, size and SHA-256.

  tools/fetch-installers.py [--print-digests]

They go to .deps/installers/downloads/ (ignored) and are never committed or vendored (AGENTS.md rule 6): the
Microsoft redistributables are run, as a game's installer runs them, and Inno Setup's and NSIS's compilers build the
slice's own fixtures on the PC (tools/build-installer-fixtures.sh). A file already there with the right digest is kept;
a download whose digest differs is refused and removed. --print-digests prints what the files that are there hash to.
"""
import hashlib
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / '.deps/installers/downloads'
PINS = {
    # Inno Setup 7.1.0 (2026-08-12), its compiler run under the PC's Wine; digest of the first fetch (2026-10-25)
    'innosetup-7.1.0-x64.exe': ('https://github.com/jrsoftware/issrc/releases/download/is-7_1_0/innosetup-7.1.0-x64.exe', 14304168,
                                '0362a383ed217d4c4239b5933866dd96d3eb2102737da92f80f6057a4b40df2f'),
    # NSIS 3.13 (2026-04-10), its makensis.exe on the PC's Wine when makensis is not installed; digest of the first fetch
    'nsis-3.13.zip': ('https://downloads.sourceforge.net/project/nsis/NSIS%203/3.13/nsis-3.13.zip', 2363925,
                      'ba63dffc4410ee89193e1cb5a41989991bd77c61068da17e3156d136b7b0b3d8'),
    # Visual C++ 2015-2022 redistributables, the WiX Burn kind (Microsoft's own URLs name the file's SHA-256)
    'vc_redist.x64.exe': ('https://download.visualstudio.microsoft.com/download/pr/990c720a-46ce-4dc7-87b9-c2eb615d34b9/'
                          'CC0FF0EB1DC3F5188AE6300FAEF32BF5BEEBA4BDD6E8E445A9184072096B713B/VC_redist.x64.exe', 25635768,
                          'cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b'),
    'vc_redist.x86.exe': ('https://download.visualstudio.microsoft.com/download/pr/990c720a-46ce-4dc7-87b9-c2eb615d34b9/'
                          '0C09F2611660441084CE0DF425C51C11E147E6447963C3690F97E0B25C55ED64/VC_redist.x86.exe', 13953392,
                          '0c09f2611660441084ce0df425c51c11e147e6447963c3690f97e0b25c55ed64'),
    # DirectX End-User Runtimes (June 2010), DXSETUP; its size is the one Microsoft publishes, its digest the first fetch (2026-10-25)
    'directx_Jun2010_redist.exe': ('https://download.microsoft.com/download/8/4/A/84A35BF1-DAFE-4AE8-82AF-AD2AE20B6B14/'
                                   'directx_Jun2010_redist.exe', 100275120, '053f76dcbb28802e23341b6a787e3b0791c0fa5c8d4d011b1044172dbf89c73b'),
}


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def fetch(name):
    url, size, sha = PINS[name]
    path = OUT / name
    if path.is_file() and path.stat().st_size == size and digest(path) == sha:
        return path
    partial = path.with_suffix('.part')
    with urllib.request.urlopen(url, timeout=60) as answer, partial.open('wb') as f:
        while block := answer.read(1 << 20):
            f.write(block)
    if partial.stat().st_size != size or digest(partial) != sha:
        got = digest(partial)
        partial.unlink()
        raise SystemExit(f'{name}: downloaded {got}, pinned {sha}: refused')
    partial.rename(path)
    return path


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    if '--print-digests' in sys.argv:
        for name in PINS:
            path = OUT / name
            print(name, path.stat().st_size if path.is_file() else '-', digest(path) if path.is_file() else '-')
        return 0
    for name in PINS:
        print(name, fetch(name).stat().st_size, 'bytes, sha256 matches the pin')
    return 0


if __name__ == '__main__':
    sys.exit(main())
