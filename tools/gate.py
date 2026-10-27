#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The commit gate: nothing but notes and evidence is committed unless the console passed the gate battery on a
build of exactly that code.

On 2026-10-06 a change that broke helpers starting programs was nearly committed; the check that would have shown
it was not run. So:

  tools/build-production.sh   ends with `gate.py record`: the fingerprint of the source the build was made from.
  tools/gate.py run --name N  runs the gate battery (GATE_CASES in tools/run-battery.py: every case a commit needs;
                              new required checks go there; the short probes of each of its GROUPS in one launch,
                              each with its own verdict) on the console, against the deployed build, and on a
                              pass writes .deps/gate/passed.json with that build's fingerprint. It first reads the
                              deployed title back by sha256 (tools/verify-title.py) and compares the console's runtime
                              deploy manifest with the staged one: a deploy from another checkout, or one that did not
                              finish, is refused.
  tools/gate.py check         the pre-commit hook (tools/git-hooks/pre-commit, core.hooksPath): the code about to be
                              committed must have the fingerprint of a build that passed. A commit that changes only
                              notes and evidence (docs/, evidence/, prototypes/, *.md) passes without one.

The fingerprint covers every file but those notes and evidence: the tracked and untracked files git does not ignore,
as the build saw them, and, at commit time, the tree the commit will have. So a commit that leaves out part of what
was tested, or adds something that was not, is refused too: commit exactly what passed.

  tools/gate.py fingerprint [--index]
"""
import argparse
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STATE = ROOT / '.deps' / 'gate'


def notes(path):
    # prototypes/ holds browser-only design prototypes that no build or console run reads.
    return path.startswith(('docs/', 'evidence/', 'prototypes/')) or path.endswith('.md')


def git(*args, env=None):
    return subprocess.check_output(['git', *args], cwd=ROOT, text=True, env=env)


def tree_entries(tree):
    """Every file of a git tree: (path, mode and blob), notes and evidence left out."""
    entries = []
    for line in git('ls-tree', '-r', '-z', tree).split('\0'):
        if not line:
            continue
        meta, path = line.split('\t', 1)
        mode, _, blob = meta.split()
        if not notes(path):
            entries.append((path, f'{mode} {blob}'))
    return sorted(entries)


def working_tree():
    """The tree of the working directory as it is: tracked and untracked files, without ignored ones."""
    with tempfile.TemporaryDirectory() as scratch:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(scratch) / 'index'))
        git('read-tree', 'HEAD', env=env)
        git('add', '-A', env=env)
        return git('write-tree', env=env).strip()


def fingerprint(tree):
    digest = hashlib.sha256()
    for path, entry in tree_entries(tree):
        digest.update(f'{path}\0{entry}\n'.encode())
    return digest.hexdigest()


def record(_args):
    STATE.mkdir(parents=True, exist_ok=True)
    tree = working_tree()
    built = {'fingerprint': fingerprint(tree), 'tree': tree, 'head': git('rev-parse', 'HEAD').strip(),
             'time': time.strftime('%Y-%m-%dT%H:%M:%S%z')}
    (STATE / 'build.json').write_text(json.dumps(built, indent=1) + '\n')
    print(f'gate: build fingerprint {built["fingerprint"][:16]}')
    return 0


def run(args):
    built_path = STATE / 'build.json'
    if not built_path.exists():
        sys.exit('gate: no build recorded: run tools/build-production.sh, then deploy it')
    built = json.loads(built_path.read_text())
    if fingerprint(working_tree()) != built['fingerprint']:
        sys.exit('gate: the code changed since the last build: build, deploy, then run the gate')
    spec = importlib.util.spec_from_file_location('verify_title', ROOT / 'tools/verify-title.py')
    verify = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(verify)
    settings = verify.ps5_console.load_settings()
    try:
        problems, digest = verify.check(settings, ROOT / 'dist/PPSA99170', ROOT / '.deps/staged/manifest.json')
    except (OSError, verify.ftplib.Error) as error:
        sys.exit('gate: the deployed build could not be read back: ' +
                 verify.redact(str(error).replace(settings['host'], '<console>')))
    if problems:
        sys.exit('gate: the console does not hold this build (' + '; '.join(problems[:5]) +
                 f'{"; ..." if len(problems) > 5 else ""}): bash ps5/tools/deploy.sh && python3 tools/deploy-wine.py')
    verify.console_guard.record_verified('title', digest, 'every file read back by sha256 with SELF off (gate)')
    status = subprocess.run([sys.executable, str(ROOT / 'tools/run-battery.py'), '--name', args.name, '--gate', '--grouped']
                            + (['--grouped-launches'] if args.grouped_launches else []),
                            cwd=ROOT).returncode
    evidence = json.loads((ROOT / 'evidence' / f'{args.name}.json').read_text())
    if status or not evidence.get('passed'):
        print(f'gate: {args.name} did not pass; nothing may be committed', file=sys.stderr)
        return 1
    passed = dict(built, battery=args.name, cases=[c['case'] for c in evidence['cases']],
                  passed_at=time.strftime('%Y-%m-%dT%H:%M:%S%z'))
    (STATE / 'passed.json').write_text(json.dumps(passed, indent=1) + '\n')
    print(f'gate: {args.name} passed on build {built["fingerprint"][:16]}: it may be committed')
    return 0


def check(_args):
    staged = git('diff', '--cached', '--name-only', '-z', 'HEAD').split('\0')
    staged = [path for path in staged if path]
    if all(notes(path) for path in staged):
        return 0
    tree = git('write-tree').strip()
    wanted = fingerprint(tree)
    passed_path = STATE / 'passed.json'
    passed = json.loads(passed_path.read_text()) if passed_path.exists() else {}
    if passed.get('fingerprint') == wanted:
        print(f'gate: this code passed {passed["battery"]} ({", ".join(passed["cases"])})')
        return 0
    print('gate: refused. This commit changes code, and no gate battery passed on a build of exactly this code.\n'
          '  1. bash tools/build-production.sh (with exactly what this commit holds, nothing more or less)\n'
          '  2. bash ps5/tools/deploy.sh && python3 tools/deploy-wine.py\n'
          '  3. python3 tools/gate.py run --name <new run name>\n'
          f'  this commit: {wanted[:16]}; last pass: {passed.get("fingerprint", "none")[:16]}', file=sys.stderr)
    return 1


def show(args):
    print(fingerprint(git('write-tree').strip() if args.index else working_tree()))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('record').set_defaults(handler=record)
    runner = commands.add_parser('run')
    runner.add_argument('--name', required=True)
    runner.add_argument('--grouped-launches', action='store_true',
                        help="the groups' probes one after another in one launch per group, as before 2026-10-25 (for "
                             'comparison); the default runs them in one persistent session, 4 at a time')
    runner.set_defaults(handler=run)
    commands.add_parser('check').set_defaults(handler=check)
    shower = commands.add_parser('fingerprint')
    shower.add_argument('--index', action='store_true')
    shower.set_defaults(handler=show)
    args = parser.parse_args()
    return args.handler(args)


if __name__ == '__main__':
    sys.exit(main())
