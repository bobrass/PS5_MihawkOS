#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""MihawkBrowser on the console (docs/MIHAWK_BROWSER.md C1-C5): one scripted run through tools/run-console.py that
ends in a verdict.

Starts the runtime's one copy, Z:\\data\\ps5proton\\runtime\\ge11-7\\lib\\mihawk-browser\\MihawkBrowser.exe, on the
window-surfaces route the desktop uses, drives it with MIHAWK_BROWSER_SCRIPT, takes pictures with
tests/pad-scripts/browser.txt, and adds to run-console's evidence/<name>.json a "browser" record: its own lines (the
program's output, klog/<name>-stdout.txt; they carry no query, path or address), each step's outcome, the download,
the hand-off, and per
browser process its type with the helper's census (run-console's "helpers" rows: flexible resident memory, images held,
code converted by patch 1127). The run passes when run-console's verdict and the browser's both do.

    tools/run-browser.py --name browser-local-2026-10-16-01 [--script "load ...; wait 10000; exit"] [--limit MIB]
    tools/run-browser.py --name browser-local-2026-10-16-01 --reparse   (the record again, from the run's files)

--limit sets PROTON_SYSTEM_MEMORY_LIMIT (the guard's close threshold, MiB) for the launch, with the steep-climb rule
left on (§5: C0's idle desktop census plus the 200 MiB ceiling)."""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import browser_lines  # noqa: E402
from console_log import redact  # noqa: E402

PATH = 'Z:\\data\\ps5proton\\runtime\\ge11-7\\lib\\mihawk-browser\\MihawkBrowser.exe'
DEFAULT_SCRIPT = 'load https://example.com/; wait 10000; load wine download; wait 10000; exit'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--script', default=DEFAULT_SCRIPT)
    parser.add_argument('--expect-sha256', help="the download's SHA-256, when --script downloads")
    parser.add_argument('--seconds', type=int, default=120)
    parser.add_argument('--limit', type=int, help='PROTON_SYSTEM_MEMORY_LIMIT for the launch, MiB')
    parser.add_argument('--env', action='append', default=[], help='NAME=VALUE for the launch')
    parser.add_argument('--reparse', action='store_true', help='rebuild the browser record from an earlier run')
    args = parser.parse_args()
    environment = [f'MIHAWK_BROWSER_SCRIPT={args.script}', 'WINE_PS5_WINDOW_SURFACES=1', *args.env]
    if args.limit: environment.append(f'PROTON_SYSTEM_MEMORY_LIMIT={args.limit}')
    command = [sys.executable, str(ROOT / 'tools/run-console.py'), '--name', args.name, '--path', PATH,
               '--seconds', str(args.seconds), '--pad-script', str(ROOT / 'tests/pad-scripts/browser.txt')]
    for item in environment: command += ['--env', item]
    status = 0 if args.reparse else subprocess.run(command, cwd=ROOT).returncode
    evidence = ROOT / 'evidence' / f'{args.name}.json'
    if not evidence.exists():
        print('run-console wrote no evidence', file=sys.stderr)
        return status or 1
    record = json.loads(evidence.read_text())
    if args.reparse:
        args.script = record.get('browser', {}).get('script', args.script)
        args.limit = record.get('browser', {}).get('limit_mib', args.limit)
        record['passed'] = record.get('run_console_passed', record.get('passed'))
    record['run_console_passed'] = record.get('passed')
    output = ROOT / 'klog' / f'{args.name}-stdout.txt'
    text = output.read_text(errors='replace') if output.exists() else ''
    lines = [redact(line) for line in text.splitlines() if line.startswith('browser: ')]
    # Each MihawkBrowser helper, in start order, with the process type it said (its pid inside Wine is not the
    # helper's, so the two lists are paired by order: the browser first, then its children as they started).
    kinds = browser_lines.PROCESS.findall('\n'.join(lines))
    ours = [h for h in record.get('helpers', []) if h.get('program') == 'MihawkBrowser.exe' or h.get('libcef')]
    processes = [{'type': kind[0] if kind else None, 'service': (kind[1] or None) if kind else None, **helper}
                 for kind, helper in zip(kinds + [None] * len(ours), ours)]
    browser = browser_lines.summary(lines, args.script, args.expect_sha256)
    browser.update({'path': PATH, 'script': args.script, 'limit_mib': args.limit, 'processes': processes,
                    'converted_kib_max': max((p['exec'] or {}).get('converted_kib', 0) for p in processes) if processes else None,
                    'browser_lines': lines})
    record['browser'] = browser
    record['passed'] = bool(record.get('passed')) and browser['passed']
    evidence.write_text(json.dumps(record, indent=1) + '\n')
    print(json.dumps({'passed': record['passed'], 'startup': browser['startup'], 'loads': len(browser['loads']),
                      'download': browser['download'], 'handoff': browser['handoff']}, indent=1))
    for p in processes:
        print(f"  {p['type'] or '-':9} {p['service'] or '':32} resident peak {p['resident_peak_mib']} MiB  images "
              f"{p['images_held_mib']} MiB  converted {(p['exec'] or {}).get('converted_kib')} KiB")
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
