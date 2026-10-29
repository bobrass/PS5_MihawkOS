#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Build a pinned one-request Lapy validation payload; never modify sibling sources."""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
REV = '5b8397b9f2b5f12a7bc2f9c8745a00d1c2dd01ad'
SDK_SHA = '8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da'
LOG_SHA = '394af67d0f8b60b3335deb53396e52855ea2daa50ca914a456ea7663f48900c6'
PATCHES = ['stdout-mirror.patch', 'donor-wait.patch', 'donor-release.patch', 'service-lifecycle.patch',
           'deterministic-build.patch', 'resident-log.patch', 'sdk042-ucred-attrs.patch', 'optional-log-channel.patch']


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--sdk-zip', required=True, type=Path)
    mode = p.add_mutually_exclusive_group()
    mode.add_argument('--service-requests', type=int, default=0, choices=(0, 2))
    mode.add_argument('--resident', action='store_true')
    a = p.parse_args()
    # The logger header: the read-only sibling when it is at the pin, else the copy tools/fetch-all.sh fetches at
    # prospero-win e665a2c. Both give the same daemon: deterministic-build.patch names inputs by file, not by path.
    candidates = [ROOT.parent / 'prospero-win-main/native/ps5log', ROOT / '.deps/prospero-win-e665a2c/native/ps5log']
    logger = next((c for c in candidates if (c / 'ps5log.h').is_file() and
                   hashlib.sha256((c / 'ps5log.h').read_bytes()).hexdigest() == LOG_SHA), candidates[-1])
    for path, digest in [(a.sdk_zip, SDK_SHA), (logger / 'ps5log.h', LOG_SHA)]:
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise RuntimeError('Pinned input mismatch: ' + str(path))
    work = ROOT / '.deps/lapy-validation'
    work.mkdir(parents=True, exist_ok=True)
    source = work / 'source'
    source.mkdir(exist_ok=True)
    archive = subprocess.check_output(['git', '-C', str(ROOT.parent / 'PS5-Lapy-JB-Daemon'), 'archive', REV])
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(source, filter='data')
    patches = PATCHES
    for patch in patches:
        subprocess.run(['patch', '-p1', '-i', str(ROOT / 'platform/lapy' / patch)], cwd=source, check=True)
    sdk = work / 'sdk'
    sdk.mkdir(exist_ok=True)
    with zipfile.ZipFile(a.sdk_zip) as z:
        z.extractall(sdk)
        for entry in z.infolist():
            mode = entry.external_attr >> 16
            if mode:
                (sdk / entry.filename).chmod(mode)
    sdk_root = next(sdk.rglob('bin/prospero-clang')).parent.parent
    subprocess.run(['python3', str(source / 'tools/build_owned_daemon.py'), '--sdk', str(sdk_root),
                    '--logging-client', str(logger), '--title', 'PPSA99170'] +
                   (['--service', '--require-client-result', '--max-requests', str(a.service_requests)]
                    if a.service_requests or a.resident else []), check=True)

    build = source / 'build' / ('owned_root_daemon-service-verified-client-max' + str(a.service_requests)
                               if a.service_requests else 'owned_root_daemon-service-verified-client'
                               if a.resident else 'owned_root_daemon')
    path = build / 'manifest.json'
    manifest = json.loads(path.read_text())
    manifest.update(proton_upstream_revision=REV,
                    proton_patches=['platform/lapy/' + name for name in patches],
                    proton_patch_sha256={name: hashlib.sha256((ROOT / 'platform/lapy' / name).read_bytes()).hexdigest()
                                         for name in patches},
                    sdk_archive_sha256=SDK_SHA, logging_header_sha256=LOG_SHA)
    path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + '\n')


if __name__ == '__main__':
    main()
