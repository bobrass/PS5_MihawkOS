# SPDX-License-Identifier: GPL-3.0-or-later
"""Where a helper's start goes, step by step, from one run's lines (installers start a helper per custom action; each
cost about 1.7 s on the Pro in 2026-10-27's gates, against well under 0.1 s on a PC).

Every stamp is CLOCK_MONOTONIC in ms, the clock the title's spawn line, the helper's start lines and Wine's +timestamp
share:
  title  "wine-ps5: spawn native=P request_ms=R lock=.. read=.. answered=.. connected=.. attempts=N held=.. sent=.. ready=..
         warm=W" (each after R; -1: not reached or, warm=1, done before the request)  wine/ps5/pw_wine_process.c
  helper "wine-child: start at=<step> ms=M"                                   native/proton_wine_child.c start_stamp
         "wine-child: life summary ms=M server_calls=.. server_ms=.. ..."     at its end
         "<s>.<ms>:<pid>:<tid>:trace:loaddll:..."                             Wine's first and last module lines
         "wine-child: payload ending monotonic_ms=M"
A helper's points become ms after its request; its steps are the gaps between them in time order, each named by where
it ends. A warm start's helper was made before its request (src/pw_warm_helper.h): its earlier points are negative and
not steps, and warm and cold starts have their own medians. summary() is the run record (tools/run-console.py writes it as "start_stages"); the medians are the verdict to
compare between runs:

  python3 tools/start_stages.py run <klog-dir> <run>     one run's logs, as run-console.py names them
  python3 tools/start_stages.py compare <a.json> <b.json>  two evidence records' medians side by side
"""
import json
import re
import statistics
import sys
from pathlib import Path

SPAWN = re.compile(r'wine-ps5: spawn native=(\d+) request_ms=(\d+) (.*)$')
FIELD = re.compile(r'(\w+)=(-?\d+)')
START = re.compile(r'^wine-child: start at=([\w-]+) ms=(-?\d+)$', re.M)
LOADDLL = re.compile(r'^(\d+)\.(\d{3}):[0-9a-f]+:[0-9a-f]+:trace:loaddll:', re.M)
LIFE = re.compile(r'^wine-child: life summary (.*)$', re.M)
ENDING = re.compile(r'^wine-child: payload ending monotonic_ms=(\d+)$', re.M)
TITLE_POINTS = ('lock', 'read', 'answered', 'connected', 'held', 'sent', 'ready')


def helper(request_ms, title_fields, text):
    """One start: its points (ms after the request), its steps in time order, and its life summary's counters."""
    points = {name: title_fields[name] for name in TITLE_POINTS if title_fields.get(name, -1) >= 0}
    if text:
        for name, ms in START.findall(text):
            if int(ms) >= 0:
                points[name] = int(ms) - request_ms
        dlls = LOADDLL.findall(text)
        if dlls:
            points['first-dll'] = int(dlls[0][0]) * 1000 + int(dlls[0][1]) - request_ms
            points['last-dll'] = int(dlls[-1][0]) * 1000 + int(dlls[-1][1]) - request_ms
        if (ending := ENDING.search(text)):
            points['ending'] = int(ending.group(1)) - request_ms
    ordered = sorted(points.items(), key=lambda item: item[1])
    after = [(name, at) for name, at in ordered if at >= 0]
    steps = {name: at - before for (_, before), (name, at) in zip([('request', 0)] + after, after)}
    life = None
    if text and (found := LIFE.findall(text)):
        life = {key: int(value) for key, value in FIELD.findall(found[-1]) if key != 'ms'}
    return {'warm': bool(title_fields.get('warm')), 'attempts': title_fields.get('attempts'), 'points': dict(ordered),
            'steps': steps, 'life': life}


def summary(owner_lines, child_texts):
    """owner_lines: the title's klog lines; child_texts: {pid: helper log text}. None when no start was stamped."""
    starts = {}
    for line in owner_lines:
        if (m := SPAWN.search(line)):
            fields = {key: int(value) for key, value in FIELD.findall(m.group(3))}
            starts[int(m.group(1))] = helper(int(m.group(2)), fields, child_texts.get(int(m.group(1)), ''))
    if not starts:
        return None
    record = {'starts': len(starts), 'warm_starts': sum(start['warm'] for start in starts.values())}
    for key, warm in (('median', False), ('median_warm', True)):
        chosen = [start for start in starts.values() if start['warm'] == warm]
        record[key] = {kind: {name: statistics.median(start[kind][name] for start in chosen if name in start[kind])
                              for name in sorted({name for start in chosen for name in start[kind]})}
                       for kind in ('points', 'steps')} if chosen else None
    record['helpers'] = {str(pid): start for pid, start in sorted(starts.items())}
    return record


def from_run(klog_dir, run):
    folder = Path(klog_dir)
    owner = (folder / f'{run}.log').read_text(errors='replace').splitlines()
    children = {int(re.search(r'-(\d+)\.log$', path.name).group(1)): path.read_text(errors='replace')
                for path in folder.glob(f'{run}-wine-child-*.log')}
    return summary(owner, children)


def compare(a, b, key='median'):
    lines = []
    for kind in ('steps', 'points'):
        left, right = (a.get(key) or {}).get(kind, {}), (b.get(key) or {}).get(kind, {})
        for name in sorted(set(left) | set(right), key=lambda n: right.get(n, left.get(n, 0))):
            lines.append(f'{kind[:-1]:<6}{name:<16}{left.get(name, "-")!s:>10}{right.get(name, "-")!s:>10}')
    return '\n'.join(lines)


def main(argv):
    if len(argv) == 4 and argv[1] == 'run':
        record = from_run(argv[2], argv[3])
        print(json.dumps(record['median'] if record else None, indent=1))
        return 0 if record else 1
    if len(argv) == 4 and argv[1] == 'compare':
        records = [json.loads(Path(path).read_text()).get('start_stages') for path in argv[2:]]
        if not all(records):
            print('a record has no start_stages')
            return 1
        print('cold starts\n' + compare(*records) + '\nwarm starts\n' + compare(*records, key='median_warm'))
        return 0
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
