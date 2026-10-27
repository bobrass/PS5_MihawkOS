#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Steam guide's real first run on the console, from the press to Big Picture (docs/STEAM.md, "The guide").

  tools/run-steam-guide-trial.py --name NAME [--prefix steamguide-fresh] [--budget 3600]

The trial runs in a prefix of its own (a new one: the harness refuses one that exists), named by the option's test hook
/data/ps5proton/steam-prefix, so the desktop's prefix is never touched. It presses the option at the launcher's first
frame (client-press: the guide starts its download at once), launches the title with no frame budget, and follows klog
through the chain the guide makes by itself: the download, the install session, the launcher's return, the sign-in
session and its screens, the client's QR window, the sign-in (a person with the Steam app), the countdown, the
launcher's done page and Big Picture.

- It asks the GPU broker for a picture a few seconds into each screen, and every minute of the update.
- When the phone screen shows it prints a line starting "QR NEXT", so the person who signs in can get ready.
- It never stops a session. It ends 60 s after Big Picture's first frames, or at the budget.
- At the end it removes its hooks, the prefix hook included. The trial's prefix and its client stay for a look.

evidence/NAME.json holds each stage's seconds from the press and the pictures (klog/NAME/)."""
import argparse
import io
import json
import re
import socket
import sys
import threading
import time
import uuid
from ftplib import FTP, error_perm, error_reply
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
sys.path.insert(0, str(ROOT / 'tools'))
import ps5_console
import console_guard
from console_log import LineLog, redact

BASE = '/data/homebrew/PPSA99170'
DATA = '/data/ps5proton'
CAPTURE_REQUEST = DATA + '/logs/game-frame.ppm.broker-request'   # WINE_PS5_CAPTURE's request (native/proton_wine.cpp)
# (stage, the klog words that mark it (a pattern), pictures to ask for (seconds after it), the stage it must follow).
STAGES = [
    ('pressed', r'launcher: the client-press flag pressed CROSS on the option', (), None),
    ('downloaded', r'steam: installer downloaded', (), 'pressed'),
    ('install_session', r'restart: to=steam-setup', (), 'downloaded'),
    ('install_screen', r'steam guide: screen 1 at', (6, 30), 'install_session'),
    ('install_back', r'steam guide: back from its install session', (), 'install_session'),
    ('signin_session', r'launcher: Steam sign-in session', (), 'install_back'),
    ('updating', r'steam guide: screen 2 at', (8, 60, 120, 180, 240), 'signin_session'),
    ('slow', r'steam guide: screen 3 at', (3,), 'updating'),
    ('scan', r'steam guide: screen 4 at', (3, 15), 'updating'),
    ('qr_window', r"steam guide: screen 0 at", (6, 20), 'scan'),
    ('signed_in', r'steam guide: screen 5 at', (2, 10), 'updating'),
    ('session_result', r"steam guide: the session's result is", (), 'signin_session'),
    ('done_page', r'steam guide: page 3', (), 'session_result'),
    ('big_picture', r'launcher: Steam in Big Picture', (), 'signin_session'),
    ('big_picture_frames', r'Wine: frames shown [1-9]\d* in', (20, 50), 'big_picture'),
]


def ftp_connect(settings):
    ftp = FTP()
    ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
    ftp.login(settings['ftp_user'], settings['ftp_password'] or 'codex')
    return ftp


def remove(ftp, path):
    try:
        ftp.delete(path)
    except error_perm as error:
        if not str(error).startswith('550'):
            raise
    except error_reply as error:
        if not str(error).startswith('226'):   # the console answers a delete with 226
            raise


def listing(ftp, folder):
    rows = []
    try:
        ftp.retrlines('LIST ' + folder, rows.append)
    except error_perm:
        return None
    return [row.split()[-1] for row in rows if row.split() and row.split()[-1] not in ('.', '..')]


def pictures_on_console(ftp):
    return [f'{DATA}/logs/{name}' for name in listing(ftp, DATA + '/logs') or [] if re.fullmatch(r'game-frame-\d+\.ppm', name)]


def run(settings, name, prefix, budget):
    console_guard.check_launch_allowed()
    readiness = console_guard.before_launch(readiness=lambda: console_guard.wait_ready(settings))

    def idle():
        return ' count=0 ' in ps5_console.ps5vkctl_command(settings, 'procs', timeout=15) + ' '
    if not idle():
        raise RuntimeError('Console is running an application; no launch performed')
    capture = ROOT / 'klog' / name
    capture.mkdir(parents=True, exist_ok=False)
    evidence_path = ROOT / 'evidence' / (name + '.json')
    if evidence_path.exists():
        raise RuntimeError('Evidence already exists; use a new run name')
    evidence_path.write_text(json.dumps({'passed': False, 'status': 'incomplete'}) + '\n')
    clean = False
    seen, asked, timers = {}, [], []
    started = [None]

    def ask_picture(stage, after):
        try:
            with ftp_connect(settings) as ftp:
                ftp.storbinary(f'STOR {CAPTURE_REQUEST}', io.BytesIO(b''))
            asked.append((stage, after, round(time.monotonic() - started[0], 1)))
        except Exception as error:   # a picture is a nicety: the trial goes on
            print(f'picture for {stage} not asked: {redact(error)}', flush=True)

    def on_line(line):
        for stage, words, pictures, after in STAGES:
            if stage in seen or (after and after not in seen) or not re.search(words, line):
                continue
            # A session result names its word; a stage before the press has no time to be measured from
            if started[0] is None:
                started[0] = time.monotonic()
            seen[stage] = round(time.monotonic() - started[0], 1)
            print(f'STAGE {stage} at {seen[stage]} s', flush=True)
            if stage == 'scan':
                print('QR NEXT: the phone screen is up; the client\'s QR code follows in about 25 s', flush=True)
            if stage == 'session_result':
                seen['session_result_word'] = line.split("result is", 1)[1].strip()
            for after in pictures:
                timer = threading.Timer(after, ask_picture, (stage, after))
                timer.daemon = True
                timer.start()
                timers.append(timer)
            break

    try:
        with socket.create_connection((settings['host'], settings['klog_port']), timeout=10) as sock:
            sock.settimeout(0.5)
            quiet = time.monotonic() + 20
            while time.monotonic() < quiet:
                try:
                    if not sock.recv(65536):
                        raise RuntimeError('Klog closed before launch')
                except socket.timeout:
                    break
            with ftp_connect(settings) as ftp:
                if listing(ftp, f'{DATA}/prefixes/{prefix}') is not None:
                    raise RuntimeError(f'the prefix {prefix} exists: the trial needs a new one (pick another --prefix)')
                for path in [f'{DATA}/stop-game', f'{DATA}/steam-guide.txt', f'{BASE}/wine-run.txt', f'{BASE}/test-run.txt',
                             CAPTURE_REQUEST] + pictures_on_console(ftp):
                    remove(ftp, path)
                ftp.storbinary(f'STOR {DATA}/steam-prefix', io.BytesIO((prefix + '\n').encode()))
                ftp.storbinary(f'STOR {DATA}/client-press', io.BytesIO(b''))
            console_guard.after_launch()
            ps5_console.ps5vkctl_command(settings, 'launch PPSA99170', timeout=120)
            print(f'Launched; following the chain (prefix {prefix}).', flush=True)
            began, captured, end_at = time.monotonic(), bytearray(), None
            with (capture / 'capture.log').open('w') as log:
                stream = LineLog(log, on_line)
                while time.monotonic() - began < budget:
                    try:
                        chunk = sock.recv(65536)
                        if not chunk:
                            raise RuntimeError('Klog disconnected during run')
                        captured.extend(chunk)
                        stream.write(chunk)
                    except socket.timeout:
                        pass
                    if 'big_picture_frames' in seen and end_at is None:
                        end_at = time.monotonic() + 60
                    if end_at and time.monotonic() >= end_at:
                        break
                stream.finish()
        for timer in timers:
            timer.cancel()
        (capture / 'klog.log').write_text(redact(captured.decode(errors='replace')))
        pictures = []
        with ftp_connect(settings) as ftp:
            from PIL import Image
            for path in sorted(pictures_on_console(ftp)):
                raw = io.BytesIO()
                ftp.retrbinary(f'RETR {path}', raw.write)
                target = capture / f'picture-{re.search(r"-(\d+)[.]ppm$", path).group(1)}.png'
                Image.open(io.BytesIO(raw.getvalue())).save(target)
                remove(ftp, path)
                pictures.append(target.name)
        text = redact(captured.decode(errors='replace'))
        evidence = {
            'prefix': prefix, 'launch_readiness': readiness, 'stages_seconds_from_press': seen,
            'pictures_asked': asked, 'pictures': pictures,
            'reached_big_picture': 'big_picture_frames' in seen,
            'signin_saved': re.findall(r'Client sign-in: saved, (\d+) s into the session', text),
            'guide_lines': [line for line in text.splitlines() if re.search(r'steam guide|Client sign-in|restart:', line)][:200],
            'storage_errors': len(console_guard.storage_damage(text.splitlines())), 'capture': str(capture.relative_to(ROOT)),
        }
        evidence['passed'] = evidence['reached_big_picture'] and not evidence['storage_errors']
        evidence_path.write_text(json.dumps(evidence, indent=2) + '\n')
        clean = evidence['passed']
        print(json.dumps({k: v for k, v in evidence.items() if k != 'guide_lines'}, indent=2))
        return 0 if clean else 1
    finally:
        for timer in timers:
            timer.cancel()
        try:
            with ftp_connect(settings) as ftp:
                for path in (f'{DATA}/client-press', f'{DATA}/steam-prefix', CAPTURE_REQUEST):
                    remove(ftp, path)
            console_guard.finished(clean and idle())
        except BaseException as error:
            console_guard.finished(False)
            print('Harness cleanup incomplete: ' + redact(error), file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--prefix', default='steamguide-fresh', help='a new prefix of its own (never the desktop\'s)')
    parser.add_argument('--budget', type=int, default=3600, help='seconds before the harness stops following')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('use a plain run name')
    if not re.fullmatch(r'[a-z0-9-]{1,32}', args.prefix) or args.prefix == 'mihawk':
        parser.error('the prefix is a plain word of a-z, 0-9 and -, and never mihawk')
    return run(ps5_console.load_settings(), args.name, args.prefix, args.budget)


if __name__ == '__main__':
    sys.exit(main())
