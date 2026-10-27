#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The launcher's Steam option on the console, from the press to the installed client (docs/STEAM.md).

  tools/run-steam-option.py set-aside          move the prefix's client folder to Steam.keep (a fresh-install start)
  tools/run-steam-option.py run --name NAME    stage the press, launch the launcher, follow the chain until the console is idle
  tools/run-steam-option.py run --name NAME --leg signin [--seconds N | --real] [--pad-script FILE]
                                               the installed client's own window: the press restarts into it, the run
                                               stops it after N s (stop-game) and records what the klog showed
  tools/run-steam-option.py put-back           the installed folder to Steam.fresh-N, Steam.keep back as Steam

`run` stages /data/ps5proton/client-press (the launcher presses CROSS on the option at its first frame), launches the title
with a 3600-frame launcher budget, and follows klog: the download, the restart into the silent installer, the installer's
session, the launcher's return and its end. The signin leg is the second press, on an installed client. The record (evidence/NAME.json) says which steps the klog showed and whether the
client's program is in the prefix afterwards; it passes only when every step is there and the console ended idle."""
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
STEAM = DATA + '/prefixes/mihawk/drive_c/Program Files (x86)/Steam'
BUDGET_SECONDS = 600
REAL_BUDGET_SECONDS = 2400   # a real sign-in: the first start's updates (about 5.5 minutes) and a person with a phone
PRESSED = ('pressed', 'launcher: the client-press flag pressed CROSS on the option')
BACK = ('back_to_launcher', 'restart: to=launcher')
STEPS = {'install': [PRESSED, ('downloaded', 'steam: installer downloaded'),
                     ('install_queued', 'launcher: installing from the downloaded installer'),
                     ('restart_to_installer', 'restart: to=steam-setup'), BACK],
         'signin': [PRESSED, ('signin_queued', 'launcher: Steam sign-in session'), ('restart_to_client', 'restart: to=steam cycle'), BACK]}
SESSION_START = re.compile(r'restart: to=steam(-setup)? cycle')


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
    """The numbered pictures a pad script's captures leave (game-frame-NN.ppm)."""
    rows = []
    ftp.retrlines(f'LIST {DATA}/logs', rows.append)
    return [f'{DATA}/logs/{row.split()[-1]}' for row in rows if row.split() and re.fullmatch(r'game-frame-\d+\.ppm', row.split()[-1])]


def installed_client(ftp):
    # The installer writes Steam.exe, a later update steam.exe: the file system tells them apart, Windows does not.
    return any(name.lower() == 'steam.exe' for name in listing(ftp, STEAM) or [])


def set_aside(settings):
    with ftp_connect(settings) as ftp:
        if listing(ftp, STEAM + '.keep') is not None:
            raise SystemExit('Steam.keep already exists: nothing moved')
        ftp.rename(STEAM, STEAM + '.keep')
        print('moved the client folder to Steam.keep')


def put_back(settings):
    with ftp_connect(settings) as ftp:
        if listing(ftp, STEAM + '.keep') is None:
            raise SystemExit('no Steam.keep: nothing to put back')
        n = 1
        while listing(ftp, f'{STEAM}.fresh-{n}') is not None:
            n += 1
        if listing(ftp, STEAM) is not None:
            ftp.rename(STEAM, f'{STEAM}.fresh-{n}')
        ftp.rename(STEAM + '.keep', STEAM)
        print(f'Steam.keep is Steam again; the installed folder is Steam.fresh-{n}')


VM_STATS = re.compile(r'VM Stats: RSS ([\d.]+) ([\d.]+).*?swap out ([\d.]+).*?page table CPU (\d+)/(\d+)')


def run(settings, name, leg, seconds, pad_script, real=False, raise_to=None):
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
    evidence_path.write_text(json.dumps({'passed': False, 'status': 'incomplete', 'capture': str(capture.relative_to(ROOT))}) + '\n')
    identity = uuid.uuid4().hex
    marker = '[MihawkOS] test run identity: ' + identity
    clean = False

    stop_timer = []

    def stop_session():
        with ftp_connect(settings) as ftp:
            ftp.storbinary(f'STOR {DATA}/stop-game', io.BytesIO(b'steam-option run\n'))

    brake = {'tripped': None, 'swap_base': None, 'series': []}

    def watch_kernel(line):
        # The kernel's own figures, about every 7 s: the harness brake the title cannot see. A graceful stop; the raised guard's
        # end mark is the fast one.
        m = VM_STATS.search(line)
        if not m or not raise_to:
            return
        rss, limit, swap, pt, pt_max = float(m.group(1)), float(m.group(2)), float(m.group(3)), int(m.group(4)), int(m.group(5))
        brake['swap_base'] = swap if brake['swap_base'] is None else brake['swap_base']
        brake['series'].append((rss, swap, pt))
        why = (f'page table CPU {pt} of {pt_max}' if pt >= 8800 else
               f'swap out {swap} above {brake["swap_base"]} + 8' if swap > brake['swap_base'] + 8 else
               f'RSS {rss} within 25 of the limit {limit}' if rss > limit - 25 else None)
        if why and not brake['tripped']:
            brake['tripped'] = why
            print('BRAKE: ' + why, flush=True)
            stop_session()

    def stage_second_launcher(line):
        watch_kernel(line)
        # The first launcher consumed test-run.txt; the one that follows the session needs its own budget to end.
        if SESSION_START.search(line):
            with ftp_connect(settings) as ftp:
                ftp.storbinary(f'STOR {BASE}/test-run.txt',
                               io.BytesIO(f'identity {identity}-2\nframes 300\nscreenshot\nsamples ps5proton\n'.encode()))
            if leg == 'signin' and not real:
                stop_timer.append(threading.Timer(seconds, stop_session))   # a real sign-in ends itself: the title's watcher
                stop_timer[0].daemon = True
                stop_timer[0].start()

    try:
        with socket.create_connection((settings['host'], settings['klog_port']), timeout=10) as sock:
            sock.settimeout(0.5)
            quiet = time.monotonic() + 20
            replay = bytearray()
            while time.monotonic() < quiet:
                try:
                    chunk = sock.recv(65536)
                    if not chunk:
                        raise RuntimeError('Klog closed before launch')
                    replay.extend(chunk)
                except socket.timeout:
                    break
            with ftp_connect(settings) as ftp:
                if installed_client(ftp) != (leg == 'signin'):
                    raise RuntimeError('the client is installed: run set-aside first' if leg == 'install' else 'the client is not installed: run the install leg first')
                remove(ftp, f'{DATA}/stop-game')
                if raise_to:
                    ftp.storbinary(f'STOR {DATA}/memory-raise', io.BytesIO((raise_to + '\n').encode()))
                for stale in pictures_on_console(ftp):
                    remove(ftp, stale)
                if pad_script:
                    ftp.storbinary(f'STOR {DATA}/pad-script.txt', io.BytesIO(Path(pad_script).read_bytes()))
                if leg == 'install':
                    remove(ftp, f'{DATA}/downloads/SteamSetup.exe')
                remove(ftp, f'{BASE}/wine-run.txt')
                ftp.storbinary(f'STOR {DATA}/client-press', io.BytesIO(b''))
                ftp.storbinary(f'STOR {BASE}/test-run.txt',
                               io.BytesIO(f'identity {identity}\nframes 3600\nscreenshot\nsamples ps5proton\n'.encode()))
            console_guard.after_launch()
            ps5_console.ps5vkctl_command(settings, 'launch PPSA99170', timeout=120)
            print('Launch request completed; following the chain.', flush=True)
            started, next_poll, idle_since, captured = time.monotonic(), time.monotonic() + 2, None, bytearray()
            with (capture / 'capture.log').open('w') as log:
                stream = LineLog(log, stage_second_launcher)
                while time.monotonic() - started < (REAL_BUDGET_SECONDS if real else BUDGET_SECONDS):
                    try:
                        chunk = sock.recv(65536)
                        if not chunk:
                            raise RuntimeError('Klog disconnected during run')
                        captured.extend(chunk)
                        stream.write(chunk)
                    except socket.timeout:
                        pass
                    now = time.monotonic()
                    if now >= next_poll:
                        next_poll = now + 2
                        idle_since = (idle_since or now) if idle() else None
                    if idle_since and now - idle_since >= 3 and now - started > 20:
                        break
                stream.finish()
        text = redact(captured.decode(errors='replace'))
        current = text[text.index(marker):] if marker in text else ''
        (capture / 'klog.log').write_text(current)
        with ftp_connect(settings) as ftp:
            installed = installed_client(ftp)
            installer = listing(ftp, DATA + '/downloads') or []
            leftover_flag = listing(ftp, DATA + '/client-press') is not None
            pictures = []
            for path in sorted(pictures_on_console(ftp)):
                from PIL import Image
                raw = io.BytesIO()
                ftp.retrbinary(f'RETR {path}', raw.write)
                target = capture / f'capture-{re.search(r"-(\d+)[.]ppm$", path).group(1)}.png'
                Image.open(io.BytesIO(raw.getvalue())).save(target)
                remove(ftp, path)
                pictures.append(target.name)
        found = {key: line in current for key, line in STEPS[leg]}
        census = [int(m) for m in re.findall(r'Wine: system memory (\d+) MiB', current)]
        saved = re.search(r'Client sign-in: saved, (\d+) s into the session', current)
        logon = re.search(r'Client sign-in: the client.s connection log shows a logon, (\d+) s into the session', current)
        if real:
            found['signin_saved'] = bool(saved)
        interesting = [line for line in current.splitlines() if re.search(r'steam|restart:|launcher:|ends:|installer', line, re.I)]
        evidence = {'raise': raise_to, 'brake': brake['tripped'], 'kernel_series_rss_swap_pt': brake['series'][-60:], 'real_signin': real, 'signin_saved_at_s': int(saved.group(1)) if saved else None,
                    'logon_line_at_s': int(logon.group(1)) if logon else None,
                    'census_peak_mib': max(census) if census else None, 'census_series_mib': census[-40:],
                    'leg': leg, 'launch_readiness': readiness, 'identity_seen': marker in text, 'steps': found, 'installed_after': installed,
                    'downloads_folder': installer, 'pictures': pictures, 'press_flag_left': leftover_flag, 'shell_idle_after_run': bool(idle_since),
                    'seconds': round(time.monotonic() - started, 1), 'lines': interesting[:120],
                    'storage_errors': len(console_guard.storage_damage(text.splitlines())), 'capture': str(capture.relative_to(ROOT))}
        evidence['passed'] = all(found.values()) and installed and evidence['shell_idle_after_run'] and not evidence['storage_errors']
        evidence_path.write_text(json.dumps(evidence, indent=2) + '\n')
        clean = evidence['passed']
        print(json.dumps({k: v for k, v in evidence.items() if k != 'lines'}, indent=2))
        return 0 if clean else 1
    finally:
        for timer in stop_timer:
            timer.cancel()   # a stop-game written after the run would close the next session at its first tick
        try:
            if idle():
                with ftp_connect(settings) as ftp:
                    for path in (f'{DATA}/client-press', f'{BASE}/test-run.txt', f'{BASE}/wine-run.txt', f'{DATA}/stop-game',
                                 f'{DATA}/steam-guide.txt',
                                 f'{DATA}/pad-script.txt', f'{DATA}/memory-raise'):
                        remove(ftp, path)
            else:
                clean = False
            console_guard.finished(clean)
        except BaseException as error:
            console_guard.finished(False)
            print('Harness cleanup incomplete: ' + redact(error), file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('action', choices=('set-aside', 'run', 'put-back'))
    parser.add_argument('--name')
    parser.add_argument('--leg', choices=('install', 'signin'), default='install')
    parser.add_argument('--seconds', type=int, default=240, help='signin: how long the client session runs before stop-game')
    parser.add_argument('--real', action='store_true', help='signin: wait for a real sign-in with a phone (no stop timer; up to 40 minutes)')
    parser.add_argument('--raise', dest='raise_to', metavar='LIMIT:END', help='signin: the guard for this session only, in MiB (870..990 : up to 1010); the harness brakes on the kernel figures')
    parser.add_argument('--pad-script', help='signin: a pad script (src/pw_pad_script.h) for the session; its captures stay on the console')
    args = parser.parse_args()
    settings = ps5_console.load_settings()
    if args.action == 'set-aside':
        return set_aside(settings)
    if args.action == 'put-back':
        return put_back(settings)
    if args.real and args.leg != 'signin':
        parser.error('--real is for --leg signin')
    if not args.name or not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('run needs --name with a plain run name')
    return run(settings, args.name, args.leg, args.seconds, args.pad_script, args.real, args.raise_to)


if __name__ == '__main__':
    sys.exit(main())
