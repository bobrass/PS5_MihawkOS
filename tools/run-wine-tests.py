#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run Wine's conformance tests on the console and on the pinned Wine on Linux, and compare.

  tools/run-wine-tests.py list ntdll
  tools/run-wine-tests.py host ntdll [test ...] --arch i386  the same tests and pinned Wine, on this PC
  tools/run-wine-tests.py console ntdll [test ...]    one title launch per test file on the console
  tools/run-wine-tests.py compare ntdll               what fails on the console and not on the baseline

The programs come from tools/build-wine-tests.sh (<dll>_test.exe and <dll>_test32.exe). Console batches
run a few files per launch; a crash or watchdog timeout leaves that file incomplete, and the next launch
resumes with the remaining files. Payload children are available. A run is classified as

  complete    the program printed its summary line (the failures, if any, are in the record)
  incomplete  it ended without one: a crash, an exit in the middle of the test, or an unhandled exception
  timeout     it was still running when the budget ended (console only; the title is stopped, and killed with
              tools/kill-hung-title.py only if its own stop gave up)
  skipped     named in tests/wine-tests-skip-ps5.json, with the reason

Results go to evidence/wine-tests/<arch>/<dll>-<where>.json, one record per test file: the status, the counts the
program reports, and the "file:line: Test failed" lines. "marked todo" lines are failures Wine knows and
expects on Windows too; they are counted, not compared. compare matches failures by file and line (the text
carries addresses and sizes that differ), reports each failure the console has and the baseline does not,
each test file whose status differs, and prints the totals. A failure on both sides is Wine's or the
test's, not the port's.
"""
import argparse
from collections import Counter
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TESTS = ROOT / '.deps/wine-tests'
RUNTIME = ROOT / '.deps/wine-runtime'
OUT = Path(os.environ['PW_EVIDENCE_DIR']) if os.environ.get('PW_EVIDENCE_DIR') else ROOT / 'evidence/wine-tests'   # a dry run writes elsewhere
ARCH = 'x86_64'
CONTEXT = {}
HOST_WINE = RUNTIME / 'build/wine'
DEFAULT_HOST_WINE = HOST_WINE
SKIP = ROOT / 'tests/wine-tests-skip-ps5.json'


def skips(dll, arch=None):
    """A DLL's skipped files for one architecture: FILE skips both, FILE@i386 or FILE@x86_64 only that one."""
    arch = arch or ARCH
    found = {}
    for key, reason in load(SKIP).get(dll, {}).items():
        name, _, only = key.partition('@')
        if not only or only == arch:
            found[name] = reason
    return found
EXPECTED = ROOT / 'tests/wine-tests-expected-ps5.json'

SUMMARY = re.compile(r'^[0-9a-f]{4}:(\w+):\s*(?:[\d.]+\s)?(\d+) tests executed \((\d+) marked as todo, (\d+) as flaky, '
                     r'(\d+) failures?\), (\d+) skipped\.')
FAILED = re.compile(r'^(\S+\.c):(\d+): Test failed: (.*)$')
TODO = re.compile(r'^(\S+\.c):(\d+): Test marked todo: ')
SKIPPED = re.compile(r'^(\S+\.c):(\d+): Tests skipped: (.*)$')


# The graphics DLLs, and every module whose tests create a Direct3D device: the title runs every program with DXVK
# (native/proton_wine.cpp default_overrides), so their PC oracle runs on DXVK too. Against the PC's wined3d, d3dx9_36's
# shader test showed 75,097 lines that the PC on DXVK has line for line (2026-10-26). tests/test_dll_overrides.py
# checks the list against the test sources.
D3D_DLLS = ('d3d11', 'd3d10core', 'dxgi', 'd3d9', 'd3d12',
            'd3d8', 'd3d10', 'd3d10_1', 'd2d1', 'dxva2', 'evr', 'mf', 'mfplat', 'mfreadwrite', 'mfmediaengine', 'quartz',
            'win32u', 'd3dcompiler_42', 'd3dcompiler_43', 'd3dcompiler_46', 'd3dcompiler_47',
            'd3dx9_35', 'd3dx9_36', 'd3dx9_42', 'd3dx9_43', 'd3dx11_42', 'd3dx11_43',
            'd3dx10_33', 'd3dx10_34', 'd3dx10_35', 'd3dx10_36', 'd3dx10_37', 'd3dx10_38', 'd3dx10_39', 'd3dx10_40',
            'd3dx10_41', 'd3dx10_42', 'd3dx10_43')
DXVK_OVERRIDES = 'd3d11,dxgi,d3d10core,d3d9,d3d12,d3d12core=n'
# one test thread (the same on both sides, and gentler on the console than one per CPU), and DXVK quiet
# PW_STUB_CONSOLE=1: tools/stub-console.py stands in for the console (a dry run of this runner)
LAUNCHER = str(ROOT / ('tools/stub-console.py' if os.environ.get('PW_STUB_CONSOLE') else 'tools/run-console.py'))
COUNTS = ROOT / 'tests/d3d-guarded-counts.json'   # how many guarded calls each Direct3D test file makes (`count`)
# wined3d's Vulkan renderer, which every console program gets (native/proton_wine.cpp): Wine's d3dcompiler_47 loads
# wined3d, and d3d11's tests read a Vulkan renderer as "damavand", which decides 228 of their todo checks
D3D_ENV = {'WINETEST_NO_MT_D3D': '1', 'DXVK_LOG_LEVEL': 'none', 'WINE_D3D_CONFIG': 'renderer=vulkan'}


def test_exe(dll):
    return TESTS / 'exe' / f'{dll}_test{"32" if ARCH == "i386" else ""}.exe'


def test_context(dll):
    """An old test binary or another architecture cannot silently become this run's baseline."""
    manifest = load(TESTS / 'manifest.json')
    executable = test_exe(dll)
    digest = hashlib.sha256(executable.read_bytes()).hexdigest()
    if manifest.get('executables', {}).get(executable.name) != digest:
        raise SystemExit('test executable is not in the build manifest: run tools/build-wine-tests.sh')
    context = {'arch': ARCH, 'test_sha256': digest, 'wine_revision': manifest['wine_revision'],
               'batch_header_sha256': manifest['batch_header_sha256']}
    if dll in D3D_DLLS:
        folder = 'syswow64' if ARCH == 'i386' else 'system32'
        identity = hashlib.sha256()
        for name in DXVK_OVERRIDES.split('=')[0].split(','):
            source = ROOT / f'.deps/staged/prefixes/probe/drive_c/windows/{folder}/{name}.dll'
            identity.update(name.encode() + hashlib.sha256(source.read_bytes()).digest())
        context['graphics_sha256'] = identity.hexdigest()
    return context


def check_staged_test(dll):
    staged = ROOT / '.deps/staged/prefixes/probe/drive_c' / test_exe(dll).name
    if not staged.exists() or hashlib.sha256(staged.read_bytes()).hexdigest() != CONTEXT['test_sha256']:
        raise SystemExit('staged test differs from the baseline executable: stage and deploy the matching tests first')


def test_dir(dll):
    """dlls/NAME, or programs/NAME for a program's tests (NAME.exe), as tools/build-wine-tests.sh builds them."""
    if (TESTS / f'build/dlls/{dll}/tests').is_dir() or not dll.endswith('.exe'):
        return f'dlls/{dll}'
    return f'programs/{dll[:-4]}'


def test_names(dll):
    source = (TESTS / f'build/{test_dir(dll)}/tests/testlist.c').read_text()
    return re.findall(r'\{ "([^"]+)", func_\w+ \}', source)


def parse(text):
    """The record of one test file's output."""
    failures, todo, skipped, summary = [], 0, [], None
    for line in text.splitlines():
        line = line.rstrip('\r')
        found = FAILED.match(line)
        if found:
            failures.append({'file': found.group(1), 'line': int(found.group(2)), 'text': found.group(3)[:200]})
        elif TODO.match(line):
            todo += 1
        elif SKIPPED.match(line):
            skipped.append(line[:200])
        else:
            found = SUMMARY.match(line)
            if found:  # children print summaries of their own first; the file's own is the last
                summary = {'tests': int(found.group(2)), 'todo': int(found.group(3)), 'flaky': int(found.group(4)),
                           'failures': int(found.group(5)), 'skipped': int(found.group(6))}
    lines = text.splitlines()
    guarded = re.search(r'^winetest-batch: guarded (\d+)$', text, re.M)
    return {'status': 'complete' if summary else 'incomplete', 'summary': summary, 'failures': failures[:100000],
            'guarded': int(guarded.group(1)) if guarded else None,
            'failure_count': len(failures), 'todo_lines': todo, 'skipped_lines': skipped[:20],
            'last_line': lines[-1][:200] if lines else ''}


def load(path):
    return json.loads(path.read_text()) if path.is_file() else {}


def save(path, record):
    OUT.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(record, indent=1, sort_keys=True) + '\n')


def console_fonts(prefix):
    """The console's prefix brings Proton's fonts (arial, tahoma, times, ...); without them the PC's Wine falls back to
    the PC's own and skips or answers differently (gdi32 font.c's italic Times New Roman, 2026-10-25), or has none
    at all in a bare DXVK prefix (d3dx9 core.c:377, D3DXCreateFont 0x88760b59, then a crash, 2026-10-27)."""
    fonts = ROOT / '.deps/staged/prefixes/probe/drive_c/windows/Fonts'
    for font in sorted(fonts.glob('*')) if fonts.is_dir() else ():
        shutil.copy2(font, prefix / 'drive_c/windows/Fonts' / font.name)


def display_prefix(prefix, env):
    """A display-capable host Wine (tools/build-wine-host-display.sh) draws on the user's screen: its prefix keeps
    every window of every test inside one Wine virtual desktop."""
    if (prefix / '.pw-virtual-desktop').is_file():
        return
    subprocess.run([str(HOST_WINE), 'wineboot', '-u'], env=env, timeout=300, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for key, value, data in (('HKCU\\Software\\Wine\\Explorer', 'Desktop', 'Default'),
                             ('HKCU\\Software\\Wine\\Explorer\\Desktops', 'Default', '1280x720')):
        subprocess.run([str(HOST_WINE), 'reg', 'add', key, '/v', value, '/d', data, '/f'], env=env, timeout=120,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    # the loader resolves to <build>/tools/wine/wine; its server is <build>/server/wineserver
    console_fonts(prefix)
    subprocess.run([str(HOST_WINE.parents[2] / 'server/wineserver'), '-k'], env=env, timeout=120)
    (prefix / '.pw-virtual-desktop').write_text('tools/run-wine-tests.py\n')


def host_run(dll, name, timeout, noproc=False, dxvk=False):
    display = HOST_WINE != DEFAULT_HOST_WINE.resolve()
    # PW_HOST_WORKER: parallel host baselines, each worker in a prefix of its own
    prefix = TESTS / ((f'prefix-{ARCH}-dxvk' if dxvk else f'prefix-{ARCH}' + ('-display' if display else '')) +
                      (f"-w{os.environ['PW_HOST_WORKER']}" if os.environ.get('PW_HOST_WORKER') else ''))
    # the console's user (patch 1240) and so its profile folder, C:\\users\\Mihawk (lsa.c's case test differed)
    env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG='-all', WINETEST_PLATFORM='wine', USER='Mihawk', LOGNAME='Mihawk',
               WINETEST_FILE_TIMEOUT=str(max(timeout - 10, 10)))  # the program's own watchdog first, the kill after it
    if noproc:
        env['LD_PRELOAD'] = str(TESTS / 'libnoproc.so')  # tools/noproc.c: fork fails, as on the console
    if dxvk:
        env['WINEDLLOVERRIDES'] = DXVK_OVERRIDES
        env.update(D3D_ENV)
    elif display:
        display_prefix(prefix, env)
    started = time.monotonic()
    import signal
    import tempfile
    with tempfile.TemporaryFile() as output:
        # a file, not a pipe: Wine's other processes inherit a pipe and keep it open after the test is gone
        # a Direct3D test on the host goes into a Wine virtual desktop: dxgi's tests switch display modes and go
        # fullscreen, which turned the real screen black; inside a virtual desktop they stay in its window
        # (the program started there does not share this process's output, so cmd redirects it to a file)
        # Run as the console does: the program in the prefix's C:\ and started from there (its argv[0], DLL search path
        # and current directory are C:\; from .deps, kernel32's path, process and module tests differed by location only).
        if not (prefix / 'drive_c').is_dir():
            subprocess.run([str(HOST_WINE), 'wineboot', '-u'], env=env, timeout=300, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        source = test_exe(dll)
        staged = prefix / 'drive_c' / source.name
        if not staged.is_file() or staged.read_bytes() != source.read_bytes():
            shutil.copy2(source, staged)
        exe = 'C:\\' + source.name
        if dxvk:
            fd, filename = tempfile.mkstemp(prefix='wt-d3d-', suffix='.txt')
            os.close(fd)
            capture = Path(filename)
            command = [str(HOST_WINE), 'explorer', '/desktop=pwtests,1280x720', 'cmd', '/c',
                       f'{exe} {name}, > Z:{str(capture).replace("/", chr(92))} 2>&1']
        else:
            command = [str(HOST_WINE), exe, name + ',']
        child = subprocess.Popen(command, stdout=output,
                                 stderr=subprocess.STDOUT, env=env, cwd=prefix / 'drive_c', start_new_session=True)
        timed_out = False
        try:
            child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            os.killpg(child.pid, signal.SIGKILL)
            child.wait()
        output.seek(0)
        text = output.read().decode(errors='replace')
        if dxvk:
            text = capture.read_text(errors='replace')
            capture.unlink()
        record = parse(text)
    log = ROOT / 'klog' / f'wt-host-{ARCH}-{dll}-{name}-{time.time_ns()}.log'
    log.parent.mkdir(exist_ok=True)
    log.write_text(text)
    record['log'] = str(log.relative_to(ROOT))
    record['exit_code'] = None if timed_out else child.returncode
    if timed_out or re.search(r'^winetest-batch: timeout ', text, re.M):
        record['status'] = 'timeout'
    record['seconds'] = round(time.monotonic() - started, 1)
    record['context'] = dict(CONTEXT, host_wine_sha256=hashlib.sha256(HOST_WINE.read_bytes()).hexdigest())
    record['host_wine'] = str(HOST_WINE.relative_to(ROOT)) if HOST_WINE.is_relative_to(ROOT) else str(HOST_WINE)
    return record


def console_run(dll, name, budget):
    run = f'wt-{ARCH}-{dll}-{name}-{time.time_ns()}'
    started = time.monotonic()
    command = [sys.executable, LAUNCHER, '--exe', test_exe(dll).name, '--args', name,
               '--seconds', str(budget), '--watchdog', str(budget + 60), '--name', run,
               '--env', 'PROTON_LAYOUT=launcher', '--env', 'WINEDEBUG=err+all']
    result = subprocess.run(command, capture_output=True, text=True)
    combined = result.stdout + result.stderr
    if 'did not answer' in combined:
        raise SystemExit('the console\'s control payload does not answer: stopping the batch (' + combined.strip().splitlines()[-1][:150] + ')')
    stdout_file = ROOT / f'klog/{run}-stdout.txt'
    text = stdout_file.read_text(errors='replace') if stdout_file.is_file() else ''
    record = parse(text)
    record['launch_output'] = [line[:200] for line in combined.splitlines() if 'closing:' in line or 'refused' in line or 'Error' in line][-3:]
    if not text:
        record['launch_tail'] = combined[-600:]
    timed_out = '"result": "timed out"' in result.stdout or 'timed out' in result.stdout.split('"passed"')[0][-200:]
    if timed_out and record['status'] != 'complete':
        record['status'] = 'timeout'
    if result.returncode and not text and not timed_out:
        record['status'] = 'no output'
    record['seconds'] = round(time.monotonic() - started, 1)
    record['context'] = CONTEXT
    record['run'] = run
    execution = load(ROOT / 'evidence' / (run + '.json'))
    record['console_result'] = execution.get('result')
    if execution.get('result') not in (None, 'finished', 'exited'):
        record['status'] = 'fault'
    settle_console(run)
    return record


def settle_console(run):
    """run-console owns stop/cleanup. Never kill a title another person might have opened afterward."""
    if os.environ.get('PW_STUB_CONSOLE'):
        return None
    result = load(ROOT / 'evidence' / (run + '.json'))
    if not result.get('returned_to_launcher'):
        raise SystemExit(f'{run}: no verified return to the launcher; batch stopped without another launch')
    return None


BEGIN = re.compile(r'^winetest-batch: begin (\S+)$', re.M)


def split_batch(text):
    """Per test file: its output, and whether the program said it ended (the 'end' line); the last test that
    began and did not end is the one that crashed or hung."""
    segments, order, repeated = {}, [], set()
    matches = list(BEGIN.finditer(text))
    for index, match in enumerate(matches):
        stop = matches[index + 1].start() if index + 1 < len(matches) else len(text)
        name = match.group(1)
        body = text[match.end():stop]
        if name in segments:
            # a second "begin" of the same file: a child process ran the batch again (its argv[1] was the list,
            # 2026-10-25); the first segment is kept and the file is not trusted
            repeated.add(name)
            continue
        segments[name] = (body, f'winetest-batch: end {name} ' in body)
        order.append(name)
    return segments, order, repeated


def console_batches(dll, names, budget, gap, pause_every, pause, per_launch, force, file_timeout=60, run_skipped=False):
    """One title launch runs many test files in turn; what a launch did not get to is run by the next one. A test
    that crashed or hung is recorded as it is and the launch after it starts with the test that follows."""
    path = OUT / f'{dll}-console.json'
    record = load(path)
    skip = skips(dll)
    todo = []
    for name in names:
        if name in skip and run_skipped:
            # Asked for by name: a skipped file still reports what it checks before it needs a child
            # process (the watchdog ends the rest), which is how a patch it covers is verified.
            print(f'{dll}:{name}: run although skipped ({skip[name]})', flush=True)
            todo.append(name)
        elif name in skip:
            if record.get(name, {}).get('status') in ('complete', 'incomplete', 'timeout'):
                # an earlier run's result stands; the file is not run again (every launch costs the console)
                record[name]['skip_reason'] = skip[name]
                print(f"{dll}:{name}: not run again ({skip[name]}); keeping its {record[name]['status']} result", flush=True)
            else:
                record[name] = {'status': 'skipped', 'reason': skip[name]}
                print(f'{dll}:{name}: skipped ({skip[name]})', flush=True)
        elif force or record.get(name, {}).get('context') != CONTEXT or record.get(name, {}).get('status') not in ('complete', 'incomplete', 'timeout'):
            todo.append(name)
    save(path, record)
    launches, idle_launches = 0, 0
    while todo:
        chunk = todo[:per_launch]
        if launches:
            wait = gap + (pause if pause_every and launches % pause_every == 0 else 0)
            print(f'  pausing {wait} s between launches', flush=True)
            time.sleep(wait)
        launches += 1
        print(f'{dll}: launch {launches}: {",".join(chunk)}', flush=True)
        run = f'wt-{ARCH}-{dll}-batch-{time.time_ns()}'
        started = time.monotonic()
        command = [sys.executable, LAUNCHER, '--exe', test_exe(dll).name, '--args', ','.join(chunk) + ',',  # the comma makes a single file a list too: the program then writes its begin and end lines
                   '--seconds', str(budget), '--watchdog', str(budget + 60), '--name', run,
                   '--env', 'PROTON_LAYOUT=launcher', '--env', 'WINEDEBUG=err+all', '--env', f'WINETEST_FILE_TIMEOUT={file_timeout}'] + (
                   ['--env', f'WINEDLLOVERRIDES={DXVK_OVERRIDES}'] + [x for k, v in D3D_ENV.items() for x in ('--env', f'{k}={v}')]
                   if dll in D3D_DLLS else [])
        result = subprocess.run(command, capture_output=True, text=True)
        combined = result.stdout + result.stderr
        if 'did not answer' in combined:
            raise SystemExit('the console\'s control payload does not answer: stopping the batch (' + combined.strip().splitlines()[-1][:150] + ')')
        stdout_file = ROOT / f'klog/{run}-stdout.txt'
        text = stdout_file.read_text(errors='replace') if stdout_file.is_file() else ''
        seconds = round(time.monotonic() - started, 1)
        action = settle_console(run)
        segments, order, repeated = split_batch(text)
        execution = load(ROOT / 'evidence' / (run + '.json'))
        done_here = []
        for name in order:
            body, ended = segments[name]
            entry = parse(body)
            entry['seconds'] = seconds
            entry['context'] = CONTEXT
            entry['run'] = run
            entry['console_result'] = execution.get('result')
            if execution.get('result') not in (None, 'finished', 'exited'):
                entry['status'] = 'fault'
            if not ended and entry['status'] == 'complete':
                ended = True
            if re.search(rf'^winetest-batch: timeout {re.escape(name)} (\d+)$', body, re.M):
                ended = False  # the program's watchdog ended it: a child's summary line is not the file's own
            if name == order[-1] and not ended:
                ended_itself = re.search(rf'^winetest-batch: timeout {re.escape(name)} (\d+)$', body, re.M)
                if ended_itself:  # the program's own watchdog ended it: the title closed the normal way
                    entry['status'] = 'timeout'
                    entry['hung'] = f'still running after {ended_itself.group(1)} s; the program ended itself'
                else:
                    entry['status'] = 'timeout' if action else 'incomplete'
                    if action:
                        entry['hung'] = action
            if name in repeated:
                entry['status'] = 'duplicated'
                entry['hung'] = 'the batch began this file more than once: a child process re-ran the list'
            record[name] = entry
            done_here.append(name)
            summary = entry.get('summary') or {}
            print(f"  {name}: {entry['status']}: {entry.get('failure_count', 0)} failure lines"
                  f"{', summary ' + json.dumps(summary) if summary else ''}", flush=True)
        save(path, record)
        todo = [name for name in todo if name not in done_here]
        if not done_here:
            idle_launches += 1
            print(f"  nothing ran in this launch: {combined.strip().splitlines()[-1][:150] if combined.strip() else 'no output'}", flush=True)
            if idle_launches >= 2:
                raise SystemExit('two launches in a row ran no test: stopping the batch')
        else:
            idle_launches = 0
    return record


def run_many(where, dll, names, runner, force, gap=0, pause_every=0, pause=0):
    """gap: seconds between two console launches; every pause_every launches, pause seconds more. Back-to-back
    launches (each opens and closes the video output twice) preceded power-offs and a loss of the console's
    services, so the batch is paced."""
    path = OUT / f'{dll}-{where}.json'
    record = load(path)
    launches = 0
    skip = skips(dll) if where == 'console' else {}
    for name in names:
        if name in skip:
            record[name] = {'status': 'skipped', 'reason': skip[name]}
            print(f'{dll}:{name}: skipped ({skip[name]})', flush=True)
            continue
        previous = record.get(name, {})
        cached = all(previous.get('context', {}).get(k) == v for k, v in CONTEXT.items())
        if where.startswith('host'):
            cached = cached and previous.get('context', {}).get('host_wine_sha256') == hashlib.sha256(HOST_WINE.read_bytes()).hexdigest()
        if not force and cached and previous.get('status') in ('complete', 'incomplete', 'timeout'):
            print(f"{dll}:{name}: already run ({record[name]['status']})", flush=True)
            continue
        if where == 'console' and launches:
            wait = gap + (pause if pause_every and launches % pause_every == 0 else 0)
            print(f'  pausing {wait} s between launches', flush=True)
            time.sleep(wait)
        launches += 1
        print(f'{dll}:{name}: running on the {where}', flush=True)
        record[name] = runner(name)
        entry = record[name]
        summary = entry.get('summary') or {}
        print(f"  {entry['status']}: {entry.get('failure_count', 0)} failure lines"
              f"{', summary ' + json.dumps(summary) if summary else ''} in {entry.get('seconds')} s", flush=True)
        save(path, record)
    return record


def merge_slices(record, dll):
    """The console runs a Direct3D file in slices ('d3d11@0-39', ...), one launch each; for the comparison they are
    one record: failure multiplicity preserved, and the status of the worst slice."""
    parts = {k: v for k, v in record.items() if k.startswith(dll + '@')}
    if not parts:
        return record
    rank = {'complete': 0, 'incomplete': 1, 'timeout': 2, 'fault': 3, 'skipped': 3, 'no output': 3}
    merged = {'status': 'complete', 'failures': [], 'failure_count': 0, 'slices': len(parts), 'last_line': '', 'summary': None}
    for key in sorted(parts, key=lambda k: int(k.split('@')[1].split('-')[0])):
        part = parts[key]
        if rank.get(part['status'], 3) > rank.get(merged['status'], 3):
            merged['status'] = part['status']
            merged['last_line'] = f"{key}: {part.get('last_line', '')}"
        for failure in part.get('failures', []):
            merged['failures'].append(failure)
    contexts = [part.get('context', {}) for part in parts.values()]
    merged['context'] = contexts[0] if contexts and all(c == contexts[0] for c in contexts) else {}
    merged['failure_count'] = len(merged['failures'])
    out = {k: v for k, v in record.items() if not k.startswith(dll + '@')}
    out[dll] = merged
    return out


def compare_records(host, console, expected, oracle=None, unfinished_ok=None):
    """Fail closed on missing/stale baselines and unfinished files; known open defects still fail.
    File/line matching follows the existing runner. It is an API regression comparison, not Windows certification.
    """
    totals, new, known, statuses, missing, skipped = {}, [], [], [], [], []
    for name in sorted(set(host) | set(console)):
        there, here = console.get(name), host.get(name)
        if not there or not here:
            missing.append(name + ': missing ' + ('console' if not there else 'host') + ' result')
            continue
        totals[there['status']] = totals.get(there['status'], 0) + 1
        keys = ('arch', 'test_sha256', 'wine_revision', 'batch_header_sha256')
        if any('graphics_sha256' in entry.get('context', {}) for entry in (there, here)):
            keys += ('graphics_sha256',)
        if not all(there.get('context', {}).get(k) and there['context'][k] == here.get('context', {}).get(k) for k in keys):
            missing.append(name + ': missing or mismatched build/architecture provenance')
            continue
        if (oracle or {}).get(name) and here['status'] != 'complete':
            # The PC's own run of this file cannot complete (the reason is recorded): it passes only complete on the
            # console with no failure line at all; anything less stays unfinished.
            if there['status'] == 'complete' and not there.get('failures'):
                known.append(f"{name}: whole file: console complete without failures; the PC's oracle: {oracle[name]}")
                continue
        if there['status'] != 'complete' and (unfinished_ok or {}).get(name):
            # it cannot finish on the console for the cause recorded (a service that is off there, for one)
            known.append(f"{name}: whole file: console {there['status']}: {unfinished_ok[name]}")
            continue
        if there['status'] != 'complete' or here['status'] != 'complete':
            statuses.append(f"{name}: host={here['status']}, console={there['status']}")
        host_skips = (here.get('summary') or {}).get('skipped', 0)
        console_skips = (there.get('summary') or {}).get('skipped', 0)
        if host_skips or console_skips:
            skipped.append({'test': name, 'host': host_skips, 'console': console_skips})
        if console_skips > host_skips:
            statuses.append(f'{name}: console skipped {console_skips} checks, host skipped {host_skips}')
        seen = Counter((f['file'], f['line']) for f in here.get('failures', []))
        for failure in there.get('failures', []):
            key = f"{failure['file']}:{failure['line']}"
            location = (failure['file'], failure['line'])
            if seen[location]:
                seen[location] -= 1
                continue
            reason = expected.get(name, {}).get(key, '')
            # Historical process restrictions cannot explain failures on the payload architecture.
            if reason and not reason.startswith('OPEN:') and 'no child processes' not in reason and 'cannot start' not in reason:
                known.append(f'{name}: {key}: {reason}')
            else:
                new.append(f"{name}: {key}: {failure['text']}" + (f' ({reason})' if reason else ''))
    return {'passed': bool(host) and bool(console) and not (new or statuses or missing),
            'totals': totals, 'new_failures': new, 'known_limitations': known,
            'unfinished': statuses, 'missing': missing, 'skipped_checks': skipped}


def compare(dll, names=None):
    # Modern baselines keep child processes enabled. Old --noproc records are historical only.
    host = load(OUT / f'{dll}-host-dxvk.json' if dll in D3D_DLLS else OUT / f'{dll}-host.json')
    console = load(OUT / f'{dll}-console.json')
    coverage_mismatch = False
    if dll in D3D_DLLS:
        host_ranges = {k for k in host if k.startswith(dll + '@')}
        console_ranges = {k for k in console if k.startswith(dll + '@')}
        coverage_mismatch = host_ranges != console_ranges
        console = merge_slices(console, dll)
        host = merge_slices(host, dll)
    if names:
        host = {name: host[name] for name in names if name in host}
        console = {name: console[name] for name in names if name in console}
    # Historical line-number allowances were measured on another runtime. Requalify them per architecture/build.
    qualified = load(OUT / f'{dll}-expected.json')
    expected = qualified.get('tests', {})
    # A qualification holds for the build it was made on, and only for the files it names (a gate slice of other files
    # of the same DLL runs on every new build).
    # The build fingerprint is left out: an incremental session reruns only the files whose inputs changed, so files of
    # one DLL come from several builds (tools/run-wine-session.py keys each to the modules it runs on).
    base = lambda context: {k: v for k, v in (context or {}).items() if k != 'console_build_fingerprint'}
    if any(base(entry.get('context')) != base(qualified.get('context')) for name, entry in console.items() if expected.get(name)):
        raise SystemExit('expected-failure qualification does not match the console test build')
    verdict = compare_records(host, console, expected, qualified.get('oracle'), qualified.get('unfinished_ok'))
    if coverage_mismatch:
        verdict['missing'].append('Direct3D slice ranges differ: run the same ranges on the host and console')
        verdict['passed'] = False
    verdict.update(dll=dll, arch=ARCH, selected_tests=names)
    save(OUT / f'{dll}-comparison.json', verdict)
    print(json.dumps(verdict, indent=2))
    return 0 if verdict['passed'] else 1


def main():
    global ARCH, OUT, CONTEXT, HOST_WINE
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=['list', 'host', 'console', 'compare', 'count'])
    parser.add_argument('dll')
    parser.add_argument('--arch', choices=['x86_64', 'i386'], default='x86_64')
    parser.add_argument('--host-wine', type=Path, default=HOST_WINE,
                        help='host loader; defaults to the pinned runtime. Graphics need a display-capable host build of the same revision.')
    parser.add_argument('tests', nargs='*')
    parser.add_argument('--force', action='store_true', help='rerun test files that already have a record (a run that produced no output is always rerun)')
    parser.add_argument('--timeout', type=int, default=300, help='host: seconds per test file (the Direct3D files: use 900)')
    parser.add_argument('--allow-dxgi-on-host', action='store_true', help='host: run the dxgi tests (inside a Wine virtual desktop) on this machine')
    parser.add_argument('--noproc', action='store_true', help='historical diagnostic only: fail fork(); never used as the modern comparison baseline')
    parser.add_argument('--file-timeout', type=int, default=60, help='console: seconds after which the program ends a test file that is still running (and itself with it), instead of the title being killed')
    parser.add_argument('--slices', type=int, default=0, help='console, Direct3D files: run the file in this many slices of its guarded calls, one launch each (needs `count`)')
    parser.add_argument('--budget', type=int, default=None, help='console: seconds one launch may run (default: every file of the launch using its whole timeout, and a minute more)')
    parser.add_argument('--run-skipped', action='store_true', help='console: run the named test files even if the skip list names them')
    parser.add_argument('--per-launch', type=int, default=8, help='console: test files one title launch runs in turn')
    parser.add_argument('--gap', type=int, default=90, help='console: seconds between two launches')
    parser.add_argument('--pause-every', type=int, default=3, help='console: launches after which the batch pauses longer')
    parser.add_argument('--pause', type=int, default=120, help='console: that longer pause, in seconds')
    args = parser.parse_args()
    if not re.fullmatch(r'[a-zA-Z0-9_.-]+', args.dll):
        parser.error('test module name: letters, digits, underscores and dots (ndis.sys, cmd.exe)')
    if args.per_launch < 1 or args.slices < 0 or args.file_timeout < 1:
        parser.error('positive per-launch and file-timeout required; slices must be nonnegative')
    ARCH = args.arch
    OUT = OUT / ARCH
    if args.command == 'compare':
        return compare(args.dll, args.tests or None)
    names = args.tests or test_names(args.dll)
    if any(not re.fullmatch(r'[A-Za-z0-9_]+(?:@[0-9]+-[0-9]+)?', n) for n in names):
        parser.error('test names must be identifiers, optionally followed by @FROM-TO')
    HOST_WINE = args.host_wine.resolve()
    if args.command != 'list':
        CONTEXT = test_context(args.dll)
        if args.command == 'console' and not os.environ.get('PW_STUB_CONSOLE'):
            build = load(ROOT / '.deps/gate/build.json')
            if not build.get('fingerprint'):
                raise SystemExit('no production build identity: build and deploy first')
            CONTEXT['console_build_fingerprint'] = build['fingerprint']
    if args.command == 'list':
        print('\n'.join(names))
        return 0
    if args.command == 'count':
        counts = load(COUNTS)
        entry = host_run(args.dll, f'{args.dll}@0-0', 120, False, True)
        if not entry.get('guarded'):
            raise SystemExit(f'{args.dll}: no guarded count in the output ({entry.get("last_line", "")[:100]})')
        counts[args.dll] = entry['guarded']
        COUNTS.write_text(json.dumps(counts, indent=1, sort_keys=True) + '\n')
        print(f'{args.dll}: {entry["guarded"]} guarded calls')
        return 0
    if args.command == 'host' and args.dll in D3D_DLLS:
        if args.dll == 'dxgi' and not args.allow_dxgi_on_host:
            raise SystemExit('the dxgi tests switch display modes and went fullscreen on the real screen once, turning it black; '
                             'they run on the host only with --allow-dxgi-on-host, which I pass only after I have said so beforehand')
        # the prefix host_run uses (a worker's own when PW_HOST_WORKER is set): DXVK's DLLs go into that one
        prefix = TESTS / (f'prefix-{ARCH}-dxvk' + (f"-w{os.environ['PW_HOST_WORKER']}" if os.environ.get('PW_HOST_WORKER') else ''))
        env = dict(os.environ, WINEPREFIX=str(prefix), WINEDEBUG='-all', USER='Mihawk', LOGNAME='Mihawk')
        subprocess.run([str(HOST_WINE), 'wineboot', '-u'], env=env, check=True, timeout=180,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        folder = 'syswow64' if ARCH == 'i386' else 'system32'
        for name in DXVK_OVERRIDES.split('=')[0].split(','):
            source = ROOT / f'.deps/staged/prefixes/probe/drive_c/windows/{folder}/{name}.dll'
            shutil.copy2(source, prefix / f'drive_c/windows/{folder}/{name}.dll')
        console_fonts(prefix)
        run_many('host-dxvk', args.dll, names, lambda name: host_run(args.dll, name, args.timeout, False, True), args.force)
        return 0
    if args.command == 'host':
        where = 'host-noproc' if args.noproc else 'host'
        if args.noproc:
            # starting a wineserver is a fork too: have the prefix's server running first, outside the preload
            env = dict(os.environ, WINEPREFIX=str(TESTS / 'prefix'), WINEDEBUG='-all')
            subprocess.run([str(RUNTIME / 'build/server/wineserver'), '-p'], env=env, cwd=RUNTIME / 'build',
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            # no pipes: the prefix's service processes inherit them and keep them open, so a capture never ends
            subprocess.run([str(RUNTIME / 'build/wine'), 'cmd', '/c', 'exit'], env=env, cwd=RUNTIME / 'build',
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        run_many(where, args.dll, names, lambda name: host_run(args.dll, name, args.timeout, args.noproc), args.force)
        if args.noproc:
            subprocess.run([str(RUNTIME / 'build/server/wineserver'), '-k'], env=env, cwd=RUNTIME / 'build')
        return 0
    if args.command == 'console':
        if not os.environ.get('PW_STUB_CONSOLE'):
            check_staged_test(args.dll)
        if args.slices:
            total = load(COUNTS).get(args.dll)
            if not total:
                raise SystemExit(f'run `count {args.dll}` first: how many guarded calls the file makes')
            step = -(-total // args.slices)
            names = [f'{args.dll}@{a}-{min(a + step, total)}' for a in range(0, total, step)]
            args.per_launch = 1
        budget = args.budget or args.per_launch * args.file_timeout + 60
        console_batches(args.dll, names, budget, args.gap, args.pause_every, args.pause, args.per_launch, args.force, args.file_timeout,
                        args.run_skipped)
        return 0
    return compare(args.dll)


if __name__ == '__main__':
    sys.exit(main())
