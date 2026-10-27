#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The tester probe's zip (native/proton_tester_probe.h, docs/FLEXIBLE_MEMORY.md "The tester probe"): what a tester copies.

  tools/package-tester-probe.py BUILD OUT [--sample RESULT.json]

  BUILD    a tools/build-child-probe.py --kind service-native --tester folder (its source/dist/PPSA99171)
  OUT      a folder for the owner, outside the repository: OUT/MihawkOS-memory-probe-PPSA99171.zip holds
           PPSA99171/ (the title folder), README.txt and SHA256SUMS
  --sample a result the probe wrote on a console: refused when it carries anything that could identify a console or
           a person (an IP or MAC address, an e-mail address, a long hexadecimal identifier)
"""
import argparse, hashlib, io, json, re, sys, zipfile
from pathlib import Path

TITLE = 'PPSA99171'
NEEDED = ('eboot.bin', 'sce_sys/param.json', 'service-native-helper.bin', 'assets/hui/fonts/Inter-Regular.ttf',
          'assets/hui/fonts/Inter-SemiBold.ttf', 'assets/hui/fonts/Inter-LICENSE.txt')
README = """MihawkOS memory probe (title PPSA99171)

It answers one question for MihawkOS: which memory can run code on your console. It takes about a minute, changes
nothing on the console, and does not touch your MihawkOS install (it is a separate title, PPSA99171).

1. Copy the PPSA99171 folder into /data/homebrew on the console, the same way you installed MihawkOS (FTP from a
   phone or a PC, or a file manager from a USB stick). ShadowMountPlus shows its tile within about 45 seconds.
2. For the best result, start MihawkOS once after turning the console on, close it, then open the probe.
3. Open "MihawkOS memory probe". Leave the controller alone. After about a minute it shows its result.
4. Take a photo of the result screen. It closes itself 60 seconds later.
5. The same result is saved in /data/homebrew/PPSA99171, in two files named memprobe-<model>-<firmware>.txt and
   .json. If a USB stick is plugged in, a copy may also go there.
6. Send the photo, or the two files, to the MihawkOS owner, and say which PS5 you have (the model number on the
   sticker, for example CFI-1215A, and whether it is a Pro, a Slim or a Digital Edition).

The files hold only the console's model when the system tells it, its firmware version, memory figures and the
probe's answers: no serial number, console ID, account name or network address.

Remove it afterwards by deleting /data/homebrew/PPSA99171.
"""
PRIVATE = [
    ('an IPv4 address', re.compile(r'\b(?:\d{1,3}\.){3}\d{1,3}\b')),
    ('a MAC address', re.compile(r'\b[0-9a-fA-F]{2}(?:[:-][0-9a-fA-F]{2}){5}\b')),
    ('an e-mail address', re.compile(r'[\w.+-]+@[\w-]+\.[\w.-]+')),
    ('a long hexadecimal identifier', re.compile(r'\b[0-9a-fA-F]{16,}\b')),
]


def private_findings(text):
    return [what for what, pattern in PRIVATE if pattern.search(text)]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('build', type=Path)
    parser.add_argument('out', type=Path)
    parser.add_argument('--sample', type=Path)
    args = parser.parse_args()
    folder = args.build / 'source/dist' / TITLE
    missing = [name for name in NEEDED if not (folder / name).is_file()]
    if missing:
        sys.exit(f'{folder} lacks {", ".join(missing)}: build it with tools/build-child-probe.py --kind service-native --tester')
    if json.loads((folder / 'sce_sys/param.json').read_text())['titleId'] != TITLE:
        sys.exit('the folder is not the tester probe\'s title: it must never be MihawkOS\'s PPSA99170')
    if args.sample:
        found = private_findings(args.sample.read_text())
        if found:
            sys.exit(f'{args.sample} carries {", ".join(found)}: the probe must not write that; nothing packaged')
    files = sorted(p for p in folder.rglob('*') if p.is_file() and not p.name.startswith('memprobe-'))
    sums = ''.join(f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {TITLE}/{p.relative_to(folder).as_posix()}\n' for p in files)
    args.out.mkdir(parents=True, exist_ok=True)
    target = args.out / f'MihawkOS-memory-probe-{TITLE}.zip'
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as archive:
        for p in files:
            archive.write(p, f'{TITLE}/{p.relative_to(folder).as_posix()}')
        archive.writestr('README.txt', README)
        archive.writestr('SHA256SUMS', sums)
    (args.out / 'README.txt').write_text(README)
    print(f'{target} ({target.stat().st_size} bytes, {len(files)} files)')


if __name__ == '__main__':
    main()
