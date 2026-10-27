#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Upload owned runtime files; create test prefixes without overwriting user state."""
import argparse
import ftplib
import hashlib
import io
import json
import re
import socket
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console
sys.path.insert(0, str(ROOT / 'tools'))
import console_guard


# A prefix this tool makes from the probe one gets the user folders programs expect from here, as one the title makes
# on the console gets them from the title (wineboot in a helper, patch 0933, and pw_wine_launch_profile_dirs).
# A Java launcher's single-instance lock file, for one, is created in AppData\Local\Temp and it quits when it is not
# there. Creating a folder that exists is a no-op.
PROFILE_USER = 'Mihawk'   # Wine's GetUserNameW (patch 1240, src/pw_wine_profile.h)
PROFILE_DIRECTORIES = (
    'AppData/Local/Temp', 'AppData/Local/Microsoft/Windows/INetCache', 'AppData/LocalLow', 'AppData/Roaming',
    'Desktop', 'Documents', 'Downloads', 'Music', 'Pictures', 'Videos', 'Saved Games',
)
PUBLIC_DIRECTORIES = ('Desktop', 'Documents', 'Downloads', 'Music', 'Pictures', 'Videos')


def command(settings, text):
    with socket.create_connection((settings['host'], settings['ctl_port']), timeout=15) as sock:
        sock.sendall((text + '\n').encode())
        return sock.recv(65536).decode().strip()


def mkdirs(ftp, path):
    ftp.cwd('/')
    for part in path.strip('/').split('/'):
        if not part:
            continue
        try:
            ftp.cwd(part)
        except ftplib.error_perm:
            ftp.mkd(part)
            ftp.cwd(part)


def remote_sizes(ftp, directory, known):
    """The sizes of the files in a console folder, from one listing, kept per folder. The deploy manifest on the console
    says what the last deploy wrote, not what is there now: a file a test replaced by hand (a candidate Mesa build in
    the runtime, docs/WINDOWS_PROCESSES.md) kept its manifest digest, and was not put back. Its size, as a rule, did
    not stay the same."""
    if directory not in known:
        lines = []
        try:
            ftp.retrlines(f'LIST {directory}', lines.append)
        except ftplib.error_perm:
            pass
        known[directory] = {fields[8]: int(fields[4]) for fields in (line.split(None, 8) for line in lines)
                            if len(fields) == 9 and fields[0].startswith('-') and fields[4].isdigit()}
    return known[directory]


def probe_owned(prefix, relative):
    """Files of the probe prefix that are the runtime's, not state: its own test programs and their data (every file the
    stage puts in drive_c itself: C:\\mf-video.mp4 and its expectations reached no prefix made before them) and the
    Windows modules in system32 and syswow64 (the runtime's PE builds, its i386 ones for WoW64, DXVK's and
    VKD3D-Proton's DLLs). They are replaced when they
    change. An older deploy left them as they were at the prefix's creation, so the PE files Wine loaded (from the
    runtime's own folder) and the ones the prefix held differed, which kernel32's loader test notices, and a DLL added
    later (d3d9, d3d12, d3d12core) never reached the prefix at all. A game's prefix is state and is never touched."""
    if prefix != 'probe' or not relative.startswith('prefixes/probe/drive_c/'):
        return False
    if relative.count('/') == 3:
        return True
    return (relative.startswith(('prefixes/probe/drive_c/windows/system32/', 'prefixes/probe/drive_c/windows/syswow64/')) and
            relative.endswith(('.dll', '.drv', '.sys', '.exe', '.ocx')))


RUNTIME = 'runtime/ge11-7/'


def runtime_files_gone(previous, manifest):
    """The only files a deploy removes from the console: runtime files the previous deploy wrote (its manifest, read
    back from the console) that this build no longer ships. Nothing outside runtime/ge11-7/ (prefixes, drive_c,
    saves, game folders, /data), no path that is not plain (absolute, empty, '.' or '..' segments, a backslash) and
    not the manifest itself, whatever the console's manifest says (tests/test_deploy_wine_retire.py)."""
    gone = []
    for path in previous:
        if not isinstance(path, str) or not path.startswith(RUNTIME) or path in manifest:
            continue
        if '\\' in path or any(part in ('', '.', '..') for part in path.split('/')):
            continue
        if path == RUNTIME + 'deploy-manifest.json':
            continue
        gone.append(path)
    return sorted(gone)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--prefix', default='probe', type=lambda text: text if re.fullmatch(r'[A-Za-z0-9_-]+', text)
                        else parser.error('--prefix is probe or a game id: letters, digits, - or _'),
                        help='probe, or the id of a game under games/ (a new prefix is created from the probe one)')
    args = parser.parse_args()
    settings = ps5_console.load_settings()
    try:
        state = command(settings, 'procs')
        if 'count=0' not in state:
            raise RuntimeError('Console is not idle; deployment refused')
        console_guard.begin_write('runtime', settings)
        stage = ROOT / '.deps/staged'
        manifest = json.loads((stage / 'manifest.json').read_text())
        ftp = ftplib.FTP()
        ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
        ftp.login(settings['ftp_user'], settings['ftp_password'])
        for _ in range(2):
            if 'disabled' in ftp.sendcmd('SELF').lower():
                break
        base = '/data/ps5proton'
        try:
            ftp.cwd(f'{base}/prefixes/{args.prefix}')
            prefix_exists = True
        except ftplib.error_perm:
            prefix_exists = False
        install_prefix = f'.install-{args.prefix}-' + hashlib.sha256(
            json.dumps(manifest, sort_keys=True).encode()).hexdigest()[:12]
        previous = {}
        try:
            data = io.BytesIO()
            ftp.retrbinary(f'RETR {base}/runtime/ge11-7/deploy-manifest.json', data.write)
            previous = json.loads(data.getvalue())
        except ftplib.error_perm:
            pass
        uploaded = changed_on_console = 0
        sent = []   # what this deploy uploaded: only files whose digest changed since the last deploy, or gone from the console
        last_directory = None
        sizes = {}

        def current(relative, remote, digest):
            # The last deploy wrote this file, and the console still holds a file of its size.
            if previous.get(relative) != digest:
                return False
            folder, name = f'{base}/{remote}'.rsplit('/', 1)
            return remote_sizes(ftp, folder, sizes).get(name) == (stage / relative).stat().st_size

        for relative, digest in manifest.items():
            remote = relative
            if relative.startswith('prefixes/probe/'):
                if prefix_exists:
                    # A preserved prefix predates newly introduced runtime drivers.
                    # Add the PS5 loader only when missing; never overwrite a DLL,
                    # registry file, setting or save in an existing prefix.
                    # The probe prefix's own test programs and their data (drive_c/*) are
                    # ours, not user state: they are replaced whenever they change.
                    probe_program = probe_owned(args.prefix, relative)
                    if not probe_program and relative not in (
                            'prefixes/probe/drive_c/windows/system32/wineps5.drv',
                            'prefixes/probe/drive_c/windows/system32/drivers/winebus.sys',
                            'prefixes/probe/drive_c/windows/system32/drivers/winehid.sys'):
                        continue
                    remote = relative.replace('prefixes/probe/', f'prefixes/{args.prefix}/', 1)
                    if probe_program:
                        if current(relative, remote, digest):
                            continue
                        changed_on_console += previous.get(relative) == digest
                    else:
                        try:
                            ftp.retrbinary(f'RETR {base}/{remote}', lambda data: None)
                            continue
                        except ftplib.error_perm as error:
                            if not str(error).startswith('550'):
                                raise
                else:
                    remote = relative.replace('prefixes/probe/', f'prefixes/{install_prefix}/', 1)
            elif not relative.startswith('runtime/ge11-7/') and relative != 'run/proton-payload-child.elf':
                continue
            elif current(relative, remote, digest):
                continue
            else:
                changed_on_console += previous.get(relative) == digest
            path = stage / relative
            if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
                raise RuntimeError(f'Staged file changed: {relative}')
            destination = f'{base}/{remote}'
            directory = destination.rsplit('/', 1)[0]
            if directory != last_directory:
                mkdirs(ftp, directory)
                last_directory = directory
            with path.open('rb') as data:
                ftp.storbinary(f'STOR {destination}', data, blocksize=262144)
            check = hashlib.sha256()
            ftp.retrbinary(f'RETR {destination}', check.update, blocksize=262144)
            if check.hexdigest() != digest:
                raise RuntimeError(f'Readback mismatch: {relative}')
            uploaded += 1
            sent.append(relative)
            if uploaded % 100 == 0:
                print(f'Uploaded and verified {uploaded} files', flush=True)
        print(f'sent {len(sent)} of {len(manifest)} files' + (': ' + ', '.join(p.rsplit('/', 1)[-1] for p in sent[:30]) +
                                                               (' ...' if len(sent) > 30 else '') if sent else ''), flush=True)
        # Runtime files the last deploy wrote and this build no longer ships (a font left out: tools/stage-wine.py
        # LEFT_OUT_FONTS): Wine would still load them from the console.
        gone = runtime_files_gone(previous, manifest)
        for relative in gone:
            try:
                ftp.delete(f'{base}/{relative}')
            except ftplib.error_reply:   # the console's FTP answers DELE with 226, which ftplib raises
                pass
            except ftplib.error_perm as error:
                if not str(error).startswith('550'):
                    raise
        if gone:
            print(f'removed {len(gone)} runtime files this build no longer ships: ' +
                  ', '.join(p.rsplit('/', 1)[-1] for p in gone[:30]), flush=True)
        if not prefix_exists:
            ftp.rename(f'{base}/prefixes/{install_prefix}', f'{base}/prefixes/{args.prefix}')
        users = f'{base}/prefixes/{args.prefix}/drive_c/users'
        for directory in [f'{PROFILE_USER}/{d}' for d in PROFILE_DIRECTORIES] + [f'Public/{d}' for d in PUBLIC_DIRECTORIES]:
            mkdirs(ftp, f'{users}/{directory}')
        runtime_manifest = {k: v for k, v in manifest.items() if k.startswith('runtime/') or
                            k == 'run/proton-payload-child.elf' or
                            probe_owned(args.prefix, k)}
        ftp.storbinary(f'STOR {base}/runtime/ge11-7/deploy-manifest.json',
                       io.BytesIO(json.dumps(runtime_manifest).encode()))
        for directory in ('logs', 'caches'):
            mkdirs(ftp, f'{base}/{directory}')
        ftp.quit()
        console_guard.end_write(settings)
        console_guard.record_verified('runtime', hashlib.sha256(json.dumps(runtime_manifest).encode()).hexdigest(),
                                      'each uploaded file read back by sha256; the others by size and the last manifest')
        print(f'Uploaded and verified {uploaded} files' +
              (f' ({changed_on_console} changed on the console since the last deploy)' if changed_on_console else '') +
              f'; prefix {args.prefix}: ' + ('preserved existing state' if prefix_exists else 'created'))
    except Exception as error:
        raise SystemExit(str(error).replace(settings['host'], '<console>')) from None


if __name__ == '__main__':
    main()
