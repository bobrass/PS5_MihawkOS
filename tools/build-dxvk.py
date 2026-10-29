#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build both DXVK architectures from pinned cached archives and owned patches.

No network activity. Missing inputs name their exact URL and SHA256 in pins.json.
The existing Mesa toolchain is checksum-pinned; production builds it first.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DEPS = ROOT / '.deps'
WORK = DEPS / 'dxvk-patched'
PINS = ROOT / 'platform/dxvk/pins.json'
PATCHES = sorted((ROOT / 'platform/dxvk/patches').glob('*.patch'))
MODULES = ('d3d11', 'dxgi', 'd3d10core', 'd3d9', 'd3d8')
TOOLCHAIN_SHA256 = 'bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21'


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def source_stamp():
    h = hashlib.sha256()
    for path in (PINS, Path(__file__), *PATCHES):
        h.update(path.read_bytes())
    return h.hexdigest()


def prepare_source():
    pins = json.loads(PINS.read_text())
    for pin in pins:
        archive = DEPS / pin['archive']
        if not archive.is_file() or digest(archive) != pin['sha256']:
            raise SystemExit('Missing or changed pinned DXVK input: ' + pin['archive'] +
                '\nhttps://codeload.github.com/' + pin['repo'] + '/tar.gz/' + pin['revision'] +
                '\nSHA256 ' + pin['sha256'])
    stamp = source_stamp()
    WORK.mkdir(exist_ok=True)
    source = WORK / 'source'
    if (source / '.source-stamp').is_file() and (source / '.source-stamp').read_text().strip() == stamp:
        return source, stamp
    with tempfile.TemporaryDirectory(dir=WORK) as temporary:
        staging = Path(temporary) / 'source'
        staging.mkdir()
        for pin in pins:
            destination = staging / pin['path']
            destination.mkdir(parents=True, exist_ok=True)
            with tarfile.open(DEPS / pin['archive']) as archive:
                for member in archive.getmembers():
                    parts = Path(member.name).parts
                    if len(parts) <= 1:
                        continue
                    member.name = str(Path(*parts[1:]))
                    archive.extract(member, destination, filter='data')
        for patch in PATCHES:
            with patch.open('rb') as stream:
                subprocess.run(['patch', '--batch', '--forward', '-p1'], cwd=staging, stdin=stream, check=True)
        (staging / '.source-stamp').write_text(stamp + '\n')
        if source.exists():
            shutil.rmtree(source)
        shutil.move(staging, source)
    return source, stamp


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=12)
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    source, stamp = prepare_source()
    toolchain = DEPS / 'mesa-foundation-wgl/toolchain'
    if not (toolchain / '.archive-sha256').is_file() or (toolchain / '.archive-sha256').read_text().strip() != TOOLCHAIN_SHA256:
        raise SystemExit('Build the pinned Mesa WGL toolchain first')
    env = dict(os.environ, PATH=str(toolchain / 'bin') + os.pathsep + os.environ['PATH'])
    files = {}
    for arch, cpu in (('x86_64', 'x86_64'), ('i686', 'x86')):
        cross = WORK / ('cross-' + arch + '.ini')
        cross.write_text("[binaries]\n" +
            '\n'.join(f"{key} = '{toolchain / 'bin' / name}'" for key, name in (
                ('c', arch + '-w64-mingw32-clang'), ('cpp', arch + '-w64-mingw32-clang++'),
                ('ar', 'llvm-ar'), ('strip', 'llvm-strip'), ('windres', arch + '-w64-mingw32-windres'))) +
            "\npkg-config = 'false'\n[properties]\nneeds_exe_wrapper = true\n[host_machine]\n" +
            f"system = 'windows'\ncpu_family = '{cpu}'\ncpu = '{arch}'\nendian = 'little'\n")
        build = WORK / ('build-' + arch)
        command = ['meson', 'setup'] + (['--reconfigure'] if (build / 'build.ninja').exists() else [])
        # No link time in the DLLs (--no-insert-timestamp), as Mesa's: the same source links to the same bytes.
        subprocess.run([*command, str(build), str(source), '--cross-file', str(cross),
            '--wrap-mode=nodownload', '--buildtype=release', '-Dc_link_args=-Wl,--no-insert-timestamp',
            '-Dcpp_link_args=-Wl,--no-insert-timestamp'], env=env, check=True)
        subprocess.run(['ninja', '-C', str(build), '-j', str(args.jobs)], env=env, check=True)
        destination = WORK / ('i386' if arch == 'i686' else 'x86_64')
        destination.mkdir(exist_ok=True)
        for name in MODULES:
            candidates = list((build / 'src').glob('*/' + name + '.dll'))
            if len(candidates) != 1:
                raise SystemExit('DXVK output not unique: ' + arch + '/' + name)
            output = destination / (name + '.dll')
            shutil.copy2(candidates[0], output)
            files[str(output.relative_to(WORK))] = digest(output)
    (WORK / 'manifest.json').write_text(json.dumps({'revision': json.loads(PINS.read_text())[0]['revision'],
        'version': 'v3.1-19-g601930949-ps5-1', 'source_stamp': stamp, 'toolchain_sha256': TOOLCHAIN_SHA256,
        'files': files, 'console_validated': False}, indent=2) + '\n')
    print('Built pinned patched DXVK: both architectures, 10 modules')


if __name__ == '__main__':
    main()
