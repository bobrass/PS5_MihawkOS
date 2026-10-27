#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Register the games the Aurelia client installed as launcher catalog entries.

The launcher starts a game by its catalog profile (game.json under
/data/ps5proton/games), and the client installs into its own Steam library inside
the mihawk prefix. This reads that library over FTP, takes each installed game's
name from the snapshot the client session wrote (aurelia-library.txt), picks its
executable, and writes a game.json under the game's app id - so the game appears
in the launcher and the Aurelia page's Play finds it by app id.

    python3 tools/register-aurelia-games.py [--dry-run] [--only APPID]

Idempotent: an entry that already names the same executable is left alone.
"""
import argparse
import ftplib
import io
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
LIBRARY = '/data/ps5proton/prefixes/mihawk/drive_c/users/Mihawk/Games/Aurelia/steamapps'
CATALOG = '/data/ps5proton/games'
SNAPSHOT = '/data/ps5proton/aurelia-library.txt'
WINDOWS_PREFIX = r'C:\users\Mihawk\Games\Aurelia\steamapps\common'


def settings():
    sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan' / 'tools'))
    import ps5_console
    return ps5_console.load_settings()


def listing(ftp, path):
    lines = []
    ftp.retrlines('LIST ' + path, lines.append)
    return lines


def names(ftp, path):
    out = []
    for line in listing(ftp, path):
        fields = line.split()
        if len(fields) >= 9 and fields[-1] not in ('.', '..'):
            out.append((line.startswith('d'), fields[-1], line))
    return out


def snapshot_names(ftp):
    b = bytearray()
    try:
        ftp.retrbinary('RETR ' + SNAPSHOT, b.extend)
    except ftplib.error_perm:
        return {}
    out = {}
    for line in bytes(b).decode('utf-8', 'replace').splitlines():
        parts = line.split('\t')
        if len(parts) >= 2 and parts[0].isdigit():
            out[int(parts[0])] = parts[1]
    return out


def manifest_installdir(ftp, path):
    b = bytearray()
    ftp.retrbinary('RETR ' + path, b.extend)
    m = re.search(r'"installdir"\s+"([^"]+)"', bytes(b).decode('utf-8', 'replace'))
    return m.group(1) if m else None


def find_executable(ftp, path, depth=3):
    """The game's .exe: the shallowest one, then the largest at that depth.

    A game's own launcher sits at its root; helpers such as tools/ModUploader are deeper. The old "largest anywhere"
    picked a game's 9.4 MB tools/ModUploader/ModUploader.exe over its 9.36 MB root executable
    (2026-10-22), so the catalog would have started the uploader instead of the game.
    """
    best = None
    def walk(current, level):
        nonlocal best
        for is_dir, name, line in names(ftp, current):
            if name in ('.', '..'):
                continue
            sub = current + '/' + name
            if is_dir and level < depth:
                walk(sub, level + 1)
            elif not is_dir and name.lower().endswith('.exe'):
                candidate = (level, -int(line.split()[4]), sub)  # lower level, then larger size
                if best is None or candidate < best:
                    best = candidate
    walk(path, 0)
    return best[2] if best else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--dry-run', action='store_true')
    parser.add_argument('--only', type=int)
    args = parser.parse_args()
    s = settings()
    ftp = ftplib.FTP()
    ftp.connect(s['host'], int(s.get('ftp_port', 2121)), timeout=30)
    ftp.login('anonymous', '')
    try:
        labels = snapshot_names(ftp)
        registered = []
        for is_dir, name, _ in names(ftp, LIBRARY):
            if is_dir or not name.startswith('appmanifest_'):
                continue
            app_id = int(re.search(r'appmanifest_(\d+)\.acf', name).group(1))
            if args.only and app_id != args.only:
                continue
            installdir = manifest_installdir(ftp, LIBRARY + '/' + name)
            if not installdir:
                print(f'{app_id}: no installdir in its manifest, skipped')
                continue
            folder = LIBRARY + '/common/' + installdir
            executable = find_executable(ftp, folder)
            if not executable:
                print(f'{app_id}: no executable under {installdir}, skipped')
                continue
            relative = executable[len(LIBRARY + '/common/'):]
            windows = WINDOWS_PREFIX + '\\' + relative.replace('/', '\\')
            entry = {'name': labels.get(app_id, installdir), 'executable': windows,
                     'directory': windows.rsplit('\\', 1)[0], 'prefix': 'mihawk'}
            target = f'{CATALOG}/{app_id}/game.json'
            existing = None
            try:
                b = bytearray()
                ftp.retrbinary('RETR ' + target, b.extend)
                existing = json.loads(bytes(b))
            except ftplib.error_perm:
                pass
            if existing == entry:
                print(f'{app_id}: {entry["name"]}: already registered')
                continue
            print(f'{app_id}: {entry["name"]}: {windows}')
            if not args.dry_run:
                try:
                    ftp.mkd(f'{CATALOG}/{app_id}')
                except ftplib.error_perm:
                    pass
                ftp.storbinary('STOR ' + target, io.BytesIO((json.dumps(entry, indent=2) + '\n').encode()))
            registered.append(app_id)
        print(f'registered {len(registered)} game(s)')
    finally:
        ftp.quit()


if __name__ == '__main__':
    main()
