#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Where a game's start goes: one line a second from a run's flight log (klog/<run>-flight.log), with the frames the
display flipped and the CPU each Windows program used in that second, its name taken from the helper's own log
(klog/<run>-wine-child-<pid>.log).

    tools/start-timeline.py RUN [--from S] [--to S] [--min-ms N]

Then the milestones: each program's first second, the first second with frames, and the first of five seconds in a row
at 50 or more flips (the game running). Only reads files.
"""
import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def names(base):
    """pid -> the Windows program a helper ran: the first .exe its own log (klog/<run>-wine-child-<pid>.log) names."""
    found = {}
    for log in base.parent.glob(base.name + '-wine-child-*.log'):
        m = re.search(rb'([A-Za-z0-9_.-]+\.exe)', log.read_bytes())
        if m:
            found[int(log.stem.rsplit('-', 1)[1])] = m.group(1).decode()
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('run')
    parser.add_argument('--from', dest='start', type=float, default=0.0)
    parser.add_argument('--to', type=float, default=1e9)
    parser.add_argument('--min-ms', type=int, default=150, help='a program shows in a second from this much CPU')
    args = parser.parse_args()
    base = ROOT / 'klog' / args.run
    pids = names(base)
    seconds, now = [], None
    for line in Path(str(base) + '-flight.log').read_text(errors='replace').splitlines():
        m = re.match(r'fr t=([\d.]+) .*flips=\+(\d+)', line)
        if m:
            now = {'t': float(m.group(1)), 'flips': int(m.group(2)), 'cpu': {}}
            seconds.append(now)
            continue
        m = re.match(r'p pid=(\d+) role=(\w+) .*run_ms=\+(\d+)', line)
        if m and now is not None:
            pid, role, ms = int(m.group(1)), m.group(2), int(m.group(3))
            name = pids.get(pid, role if role == 'title' else f'pid{pid}')
            now['cpu'][name] = now['cpu'].get(name, 0) + ms
    first, frames_at, running_at = {}, None, None
    for i, s in enumerate(seconds):
        for name, ms in s['cpu'].items():
            if ms and name not in first:
                first[name] = s['t']
        if frames_at is None and s['flips'] and s['t'] > 8:
            frames_at = s['t']
        if running_at is None and all(x['flips'] >= 50 for x in seconds[i:i + 5]) and len(seconds[i:i + 5]) == 5:
            running_at = s['t']
        if args.start <= s['t'] <= args.to:
            busy = ' '.join(f'{n}={ms}' for n, ms in sorted(s['cpu'].items(), key=lambda x: -x[1]) if ms >= args.min_ms)
            print(f"{s['t']:6.1f} flips={s['flips']:3d} {busy}")
    print('first CPU second:', ', '.join(f'{n} {t:.0f}' for n, t in sorted(first.items(), key=lambda x: x[1])))
    print(f'first frames after 8 s: {frames_at}; running (5 s at 50+ flips) from: {running_at}')


if __name__ == '__main__':
    main()
