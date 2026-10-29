#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Keep the console's kernel log on the PC while someone uses the console by hand: klogsrv's stream, read only, written
to klog/NAME.log (console addresses masked) as it comes, until Ctrl-C or --seconds. Two kernel panics of 2026-10-27 left
no klog at all: what the kernel said before a panic is only on the PC if a stream was open. Reconnects while the
console restarts, so the lines before and after a panic land in the same file.

  tools/klog-stream.py --name NAME [--seconds S]
"""
import argparse
import re
import socket
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console  # noqa: E402

ADDRESS = re.compile(r'\b\d{1,3}(?:\.\d{1,3}){3}\b')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--seconds', type=float, default=3600)
    args = parser.parse_args()
    if not args.name.replace('-', '').replace('_', '').isalnum():
        parser.error('use a plain name')
    settings = ps5_console.load_settings()
    path = ROOT / 'klog' / (args.name + '.log')
    path.parent.mkdir(exist_ok=True)
    end = time.monotonic() + args.seconds
    with path.open('a') as output:
        while time.monotonic() < end:
            try:
                with socket.create_connection((settings['host'], settings['klog_port']), timeout=5) as connection:
                    output.write(f'--- klog stream connected {time.strftime("%H:%M:%S")}\n')
                    connection.settimeout(1.0)
                    while time.monotonic() < end:
                        try:
                            chunk = connection.recv(65536)
                        except socket.timeout:
                            continue
                        if not chunk:
                            break
                        output.write(ADDRESS.sub('<console>', chunk.decode(errors='replace')))
                        output.flush()
            except OSError as error:
                output.write(f'--- klog stream lost {time.strftime("%H:%M:%S")}: {type(error).__name__}\n')
                output.flush()
                time.sleep(2)
            except KeyboardInterrupt:
                break
    print(f'klog: {path}')


if __name__ == '__main__':
    main()
