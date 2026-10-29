#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run a build stage only when the content of its inputs or of its outputs changed since its last good run.

  tools/build-stamp.py run NAME [--repo-except PREFIX]... [--input PATH]... [--text S]... --output PATH...
                       -- COMMAND [ARG]...

The stage's key is the SHA-256 of:
  the repository as the build sees it: every tracked and untracked file git does not ignore, by blob id (the same
    working tree tools/gate.py fingerprints), less docs/, evidence/, prototypes/, *.md and each --repo-except
    prefix (what the stage provably does not read);
  each --input outside the repository (a pinned SDK, another build's tree): every file's path and SHA-256, and each
    link inside it by its target, unfollowed (a Wine prefix links z: to /: name a linked-to input itself);
  each --text (arguments, environment) and the command line itself.
Its outputs are digested the same way. When every part (the repository, each input, the text, each output) matches
the last good run, recorded in .deps/build-stamps/NAME.json, the stage is skipped; otherwise it says which parts
changed, the stamp is removed, the command runs, and the stamp is written only when it succeeds. So an interrupted or failed run, a changed input or an output someone changed all
run the stage again. Content, not times: the theme generators rewrite identical files every run, which must not
make the next stage run. A stage keeps its own idempotence beneath this (Wine's make, Ninja): the skip saves the
fixed cost of a stage whose inputs did not move.

File digests are cached in .deps/build-stamps/digests.pickle by path, size, mtime, ctime and inode, as git's index
caches blob ids; a file changed in the last two seconds is hashed again next time (git's "racy" rule), so a write
in the same clock tick as the hash is never missed.

BUILD_STAMP_FORCE=1 runs every stage (the stamp is still recorded).
"""
import argparse
import hashlib
import json
import os
import pickle
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STAMPS = ROOT / '.deps' / 'build-stamps'
NOTES = ('docs/', 'evidence/', 'prototypes/')
RACY_SECONDS = 2


def git(*args, env=None):
    return subprocess.check_output(['git', *args], cwd=ROOT, env=env)


def repo_entries(excluded):
    """(path, mode blob) for the working tree as it is; the real index's stat cache makes it quick."""
    with tempfile.TemporaryDirectory(dir=STAMPS) as scratch:
        index = Path(scratch) / 'index'
        real = Path(git('rev-parse', '--git-path', 'index').decode().strip())
        real = real if real.is_absolute() else ROOT / real
        if real.is_file():
            index.write_bytes(real.read_bytes())
        env = dict(os.environ, GIT_INDEX_FILE=str(index))
        git('add', '-A', env=env)
        listing = git('ls-files', '-s', '-z', env=env).split(b'\0')
    skip = NOTES + tuple(excluded)
    for line in listing:
        if not line:
            continue
        meta, path = line.decode('utf-8', 'surrogateescape').split('\t', 1)
        if path.startswith(skip) or path.endswith('.md'):
            continue
        mode, blob, _stage = meta.split()
        yield path, f'{mode} {blob}'


class Digests:
    """SHA-256 of files, cached by (size, mtime, ctime, inode)."""

    def __init__(self, path):
        self.path = path
        try:
            self.cache = pickle.loads(path.read_bytes())
        except (OSError, ValueError, pickle.UnpicklingError, EOFError):
            self.cache = {}
        self.now = time.time()

    def file(self, name):
        info = os.lstat(name)
        key = (info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_ino)
        cached = self.cache.get(name)
        if cached and cached[0] == key:
            return cached[1]
        with open(name, 'rb') as stream:
            value = hashlib.file_digest(stream, 'sha256').hexdigest()
        if self.now - max(info.st_mtime_ns, info.st_ctime_ns) / 1e9 > RACY_SECONDS:
            self.cache[name] = (key, value)
        else:
            self.cache.pop(name, None)
        return value

    def tree(self, path):
        """Every file under PATH as path and digest, a link inside it as its target; PATH itself may be a link."""
        path = Path(path)
        if not path.exists():
            yield f'{path}\0missing'
            return
        if not path.is_dir():
            yield f'{path}\0{self.file(str(path.resolve()))}'
            return
        for directory, subdirectories, names in os.walk(path.resolve()):
            subdirectories.sort()
            links = [d for d in subdirectories if os.path.islink(os.path.join(directory, d))]
            for name in sorted(names + links):
                file = os.path.join(directory, name)
                if os.path.islink(file):
                    yield f'{file}\0link\0{os.readlink(file)}'
                else:
                    yield f'{file}\0{self.file(file)}'

    def save(self):
        # Two stages can run at once (the Wine builds): each writes its own file, and the last rename wins.
        partial = self.path.with_suffix(f'.{os.getpid()}.partial')
        partial.write_bytes(pickle.dumps(self.cache, protocol=pickle.HIGHEST_PROTOCOL))
        partial.replace(self.path)


def digest(lines):
    h = hashlib.sha256()
    for line in lines:
        h.update(line.encode('utf-8', 'surrogateescape') + b'\n')
    return h.hexdigest()


def run(args):
    STAMPS.mkdir(parents=True, exist_ok=True)
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        sys.exit('build-stamp: no command')
    started = time.monotonic()
    digests = Digests(STAMPS / 'digests.pickle')
    # One digest per part, so a run can say what changed (a stage that runs when nothing should is a bug to find).
    parts = {'repository': digest(f'{p}\0{e}' for p, e in repo_entries(args.repo_except)),
             **{f'input {path}': digest(digests.tree(path)) for path in args.input},
             'text': digest([*args.text, '\0'.join(['command', *command])])}
    outputs = lambda: {f'output {path}': digest(digests.tree(path)) for path in args.output}
    stamp = STAMPS / f'{args.name}.json'
    try:
        last = json.loads(stamp.read_text())
    except (OSError, ValueError):
        last = {}
    now = {**parts, **outputs()}
    changed = sorted(name for name in now.keys() | last.get('parts', {}).keys()
                     if now.get(name) != last.get('parts', {}).get(name))
    if os.environ.get('BUILD_STAMP_FORCE') != '1' and not changed:
        digests.save()
        print(f'build-stamp: {args.name} unchanged, skipped ({time.monotonic() - started:.1f} s to check)',
              flush=True)
        return 0
    print(f'build-stamp: {args.name} runs: ' + ('forced' if not changed else
          'first run' if not last else ', '.join(changed)), flush=True)
    stamp.unlink(missing_ok=True)
    digests.save()
    status = subprocess.call(command)
    if status != 0:
        return status
    digests = Digests(STAMPS / 'digests.pickle')
    stamp.write_text(json.dumps({'parts': {**parts, **outputs()},
                                 'seconds': round(time.monotonic() - started, 1)}, indent=1) + '\n')
    digests.save()
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    commands = parser.add_subparsers(dest='verb', required=True)
    runner = commands.add_parser('run')
    runner.add_argument('name')
    runner.add_argument('--repo-except', action='append', default=[])
    runner.add_argument('--input', action='append', default=[])
    runner.add_argument('--text', action='append', default=[])
    runner.add_argument('--output', action='append', default=[], required=True)
    argv = sys.argv[1:]
    if '--' not in argv:
        parser.error('the command follows --')
    split = argv.index('--')
    args = parser.parse_args(argv[:split])
    args.command = argv[split + 1:]
    sys.exit(run(args))


if __name__ == '__main__':
    main()
