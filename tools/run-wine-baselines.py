#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The PC's baselines for the whole scope, several modules at a time (tools/run-wine-tests.py host does one).

  tools/run-wine-baselines.py [MODULE...] [--workers 6] [--force]

MODULE: test modules (default: tools/wine-test-scope.py). Each runs in both architectures on the display-capable host
Wine (tools/build-wine-host-display.sh), every worker in a prefix of its own (PW_HOST_WORKER). The modules whose files
share windows, focus, devices or the display (tests/wine-tests-serial-ps5.json) run one after another on a worker of
their own: on the PC they share one X display. dxgi is left out: its tests switch the real display's modes, and run on
the host only when the owner has said so (run-wine-tests.py --allow-dxgi-on-host).
"""
import argparse
import json
import queue
import subprocess
import sys
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HOST_WINE = '.deps/wine-host-display/build/wine'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('modules', nargs='*')
    parser.add_argument('--workers', type=int, default=6)
    parser.add_argument('--force', action='store_true')
    args = parser.parse_args()
    modules = args.modules or subprocess.run([sys.executable, str(ROOT / 'tools/wine-test-scope.py')], capture_output=True,
                                             text=True, check=True).stdout.split()
    modules = [m for m in modules if m != 'dxgi' and (ROOT / '.deps/wine-tests/exe' / f'{m}_test.exe').is_file()]
    serial_names = {k.split(':')[0] for k in json.loads((ROOT / 'tests/wine-tests-serial-ps5.json').read_text()) if ':' not in k}
    shared, alone = queue.Queue(), queue.Queue()
    for m in modules:
        for arch in ('x86_64', 'i386'):
            (alone if m in serial_names else shared).put((m, arch))
    lock = threading.Lock()

    def work(q, worker):
        while True:
            try:
                module, arch = q.get_nowait()
            except queue.Empty:
                return
            command = [sys.executable, str(ROOT / 'tools/run-wine-tests.py'), 'host', module, '--arch', arch, '--timeout', '300',
                       '--host-wine', HOST_WINE] + (['--force'] if args.force else [])
            result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True,
                                    env=dict(__import__('os').environ, PW_HOST_WORKER=str(worker)))
            with lock:
                lines = [l for l in result.stdout.splitlines() if 'running on the host' not in l and 'already run' not in l]
                print(f'== {module} {arch}: exit {result.returncode}' + ''.join('\n' + l for l in lines[-200:]), flush=True)

    threads = [threading.Thread(target=work, args=(alone, 0))] + \
              [threading.Thread(target=work, args=(shared, w)) for w in range(1, max(1, args.workers - 1) + 1)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    print('== all done', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
