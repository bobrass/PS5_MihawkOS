#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Install a game the signed-in Steam client owns, with nobody at the console.

    tools/steam-install-app.py --appid 1809700 --name "A Game" --installdir "P3P" --run RUN [--minutes 60]

The Steam client downloads a game whose app manifest (steamapps/appmanifest_<appid>.acf in its own library) says it
needs an update, as soon as it runs: this writes that manifest (StateFlags 1026, nothing downloaded) when the game has
none, then runs the client headless through the 250900 entry's prefix and settings (tools/run-console.py --profile
250900 --program steam.exe), reads the manifest over FTP every 30 s and prints its progress, and asks the title to
return (stop-game) once the manifest says installed (StateFlags 4) or the minutes are up. Nothing else is written; the
run's klog and evidence are tools/run-console.py's.
"""
import argparse
import io
import re
import subprocess
import sys
import threading
import time
from ftplib import FTP, error_perm
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

STEAMAPPS = '/data/ps5proton/prefixes/mihawk/drive_c/Program Files (x86)/Steam/steamapps'
STEAM = r'C:\Program Files (x86)\Steam\steam.exe'


def manifest_text(appid, name, installdir):
    fields = [('appid', appid), ('Universe', '1'), ('name', name), ('StateFlags', '1026'), ('installdir', installdir),
              ('LastUpdated', '0'), ('UpdateResult', '0'), ('SizeOnDisk', '0'), ('buildid', '0'),
              ('BytesToDownload', '0'), ('BytesDownloaded', '0'), ('AutoUpdateBehavior', '0'),
              ('AllowOtherDownloadsWhileRunning', '0'), ('ScheduledAutoUpdate', '0')]
    return '"AppState"\n{\n' + ''.join(f'\t"{k}"\t\t"{v}"\n' for k, v in fields) + '}\n'


def field(text, name):
    m = re.search(r'"' + name + r'"\s+"([^"]*)"', text)
    return m.group(1) if m else None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--appid', required=True)
    parser.add_argument('--name', required=True)
    parser.add_argument('--installdir', required=True)
    parser.add_argument('--run', required=True, help='the run name for tools/run-console.py')
    parser.add_argument('--minutes', type=int, default=60)
    args = parser.parse_args()
    settings = ps5_console.load_settings()

    def ftp():
        connection = FTP()
        connection.connect(settings['host'], settings['ftp_port'], timeout=30)
        connection.login(settings['ftp_user'], settings['ftp_password'] or 'codex')
        return connection

    path = f'{STEAMAPPS}/appmanifest_{args.appid}.acf'

    def read_manifest():
        sink = io.BytesIO()
        with ftp() as connection:
            try:
                connection.retrbinary('RETR ' + path, sink.write)
            except error_perm:
                return None
        return sink.getvalue().decode('utf-8', 'replace')

    if read_manifest() is None:
        with ftp() as connection:
            connection.storbinary('STOR ' + path, io.BytesIO(manifest_text(args.appid, args.name, args.installdir).encode()))
        print(f'wrote appmanifest_{args.appid}.acf (StateFlags 1026)', flush=True)
    else:
        print(f'appmanifest_{args.appid}.acf is there already', flush=True)

    run = subprocess.Popen([sys.executable, str(ROOT / 'tools/run-console.py'), '--profile', '250900', '--program', STEAM,
                            '--args=-silent -noverifyfiles -no-browser', '--seconds', str(args.minutes * 60),
                            '--watchdog', str(args.minutes * 60 + 300), '--name', args.run],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    lines = []
    reader = threading.Thread(target=lambda: lines.extend(run.stdout), daemon=True)
    reader.start()
    started, installed = time.monotonic(), False
    while run.poll() is None:
        time.sleep(30)
        text = read_manifest() or ''
        flags, done, total = field(text, 'StateFlags'), field(text, 'BytesDownloaded'), field(text, 'BytesToDownload')
        print(f'{time.monotonic() - started:6.0f} s StateFlags={flags} downloaded={done} of {total} '
              f'size={field(text, "SizeOnDisk")} build={field(text, "buildid")}', flush=True)
        if flags == '4' and not installed:
            installed = True
            with ftp() as connection:
                connection.storbinary('STOR /data/ps5proton/stop-game', io.BytesIO(b'steam-install-app: installed\n'))
            print('installed: asked the title to return', flush=True)
    reader.join(timeout=10)
    print(''.join(lines[-6:]), end='')
    print('result:', 'installed' if installed else 'not installed', flush=True)
    return 0 if installed else 1


if __name__ == '__main__':
    sys.exit(main())
