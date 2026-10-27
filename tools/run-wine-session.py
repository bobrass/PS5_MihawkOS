#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Wine's conformance tests on the console in one persistent session (tests/wt-session.c), many files at once.

  tools/run-wine-session.py DLL... [--arch x86_64 --arch i386] [--files NAME...] [--jobs 4] [--force] [--name RUN]

The per-launch mode (tools/run-wine-tests.py console) pays a title start, a Wine session start and a cleanup for
every few files, 20-30 s each time. Here one title launch runs wt-session.exe, which takes the files from a queue this
runner writes over FTP (C:\\wt\\RUN\\queue.txt), runs each as its own process from C:\\ (as the PC's oracle does: no file
inherits another's process state), JOBS at a time, and appends each verdict to C:\\wt\\RUN\\results.txt; the raw output
of each is C:\\wt\\RUN\\out\\ID.txt. This runner reads them as they land and records each file exactly as the per-launch
mode does (evidence/wine-tests/<arch>/<dll>-console.json, the same parser, context and statuses), so compare and
tools/qualify-wine-tests.py read either. The per-launch mode stays the fallback and qualifies single files.

  timeouts  each file's own: 30 times its duration on the PC plus 60 s, 120 s at least, 900 s at most (300 s
            when the PC did not complete it), or tests/wine-tests-timeouts-ps5.json's with its reason; its watchdog ends it the normal way first, the session 30 s later. A timeout fails.
  serial    tests/wine-tests-serial-ps5.json: files that share the session's state run alone.
  restart   a crashed test is one result; when Wine itself (the title) ends early, the files not yet done are queued
            again in a new session; a file that was running when two sessions ended is recorded as a fault.
  skips     tests/wine-tests-skip-ps5.json, as the per-launch mode.
  reruns    incremental: a file runs again only when its test program, the Wine source or the modules it runs on
            (its import closure in the staged runtime, the module under test, their Unix sides, the core: "inputs")
            changed, or its last result was not final; plus --sentinels files of each DLL every time; --force: all.
"""
import argparse
import hashlib
import importlib.util
import io
import json
import os
import runpy
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('wine_tests', ROOT / 'tools/run-wine-tests.py')
wt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wt)
SERIAL = ROOT / 'tests/wine-tests-serial-ps5.json'
BASE = '/data/ps5proton/prefixes/probe/drive_c'
# The session program as the build staged it, which the deploy put on the console: .deps/probes is a build's working
# folder that another checkout's build can share and rewrite (gate-b5-session-2026-10-25-01 ran another tree's copy).
SESSION_EXE = next((p for p in (ROOT / '.deps/staged/prefixes/probe/drive_c/wt-session.exe',
                                ROOT / '.deps/probes/wt-session.exe') if p.is_file()), ROOT / '.deps/probes/wt-session.exe')
HOST_RECORDS = ROOT / 'evidence/wine-tests'
# a dry run (PW_STUB_CONSOLE) keeps its console records and session records elsewhere
RECORDS = Path(os.environ['PW_EVIDENCE_DIR']) if os.environ.get('PW_EVIDENCE_DIR') else HOST_RECORDS
SESSIONS = RECORDS if os.environ.get('PW_EVIDENCE_DIR') else ROOT / 'evidence'


class StubFtp:
    """PW_STUB_CONSOLE=1: the console's FTP is a folder of the PC (PW_STUB_PREFIX/drive_c for BASE), for dry runs."""
    def __init__(self, prefix):
        self.root = Path(prefix) / 'drive_c'
    def local(self, path):
        return self.root / path[len(BASE):].lstrip('/')
    def __enter__(self):
        return self
    def __exit__(self, *_):
        return False
    def mkd(self, path):
        self.local(path).mkdir(parents=True, exist_ok=False)
    def storbinary(self, command, f):
        self.local(command.split(' ', 1)[1]).write_bytes(f.read())
    def rename(self, a, b):
        self.local(a).replace(self.local(b))
    def retrbinary(self, command, write):
        write(self.local(command.split(' ', 1)[1]).read_bytes())


def stub_command(run, directory, jobs):
    """The session on the PC's display-capable Wine, recorded as a console run would be."""
    prefix = os.environ['PW_STUB_PREFIX']
    wine = ROOT / '.deps/wine-host-display/build/wine'
    env = dict(os.environ, WINEPREFIX=prefix, WINEDEBUG='-all')
    for exe in sorted((ROOT / '.deps/wine-tests/exe').glob('*.exe')):
        target = Path(prefix) / 'drive_c' / exe.name
        if not target.is_file() or target.stat().st_size != exe.stat().st_size:
            target.write_bytes(exe.read_bytes())
    script = (f"import subprocess,json,sys; r=subprocess.run({[str(wine), 'C:\\wt-session.exe', str(jobs), '30', directory]!r}, env={env!r}, "
              f"cwd={str(Path(prefix) / 'drive_c')!r}); json.dump({{'returned_to_launcher': True, 'result': 'finished'}}, "
              f"open({str(SESSIONS / (run + '.json'))!r}, 'w'))")
    return [sys.executable, '-c', script]


RUNTIME = ROOT / '.deps/staged/runtime/ge11-7/lib/wine'
PREFIX = ROOT / '.deps/staged/prefixes/probe/drive_c'
# What every file runs on, whatever it loads: the Unix sides of ntdll and win32u, the helper's loader, the title.
CORE = [RUNTIME / 'x86_64-unix/ntdll.prx', RUNTIME / 'x86_64-unix/win32u.prx', RUNTIME / 'x86_64-unix/win32u-helper.prx',
        ROOT / '.deps/staged/run/proton-payload-child.elf', ROOT / 'dist/PPSA99170/eboot.bin']
_digests, _imports = {}, {}


def digest(path):
    path = Path(path)
    if path not in _digests:
        _digests[path] = hashlib.sha256(path.read_bytes()).hexdigest()[:16] if path.is_file() else None
    return _digests[path]


def imports_of(path, delayed=False):
    """The DLL names a PE file imports (and with delayed, its delay imports too), lower case."""
    path = Path(path)
    if (path, delayed) in _imports:
        return _imports[path, delayed]
    import struct
    names = set()
    try:
        data = path.read_bytes()
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        sections, optional = struct.unpack_from('<H', data, pe + 6)[0], struct.unpack_from('<H', data, pe + 20)[0]
        magic = struct.unpack_from('<H', data, pe + 24)[0]
        directories = pe + 24 + (112 if magic == 0x20b else 96)
        table = [(struct.unpack_from('<IIIII', data, pe + 24 + optional + 40 * i + 8)) for i in range(sections)]
        def offset(rva):
            for size, address, raw_size, raw, _ in table:
                if address <= rva < address + max(size, raw_size):
                    return rva - address + raw
            return None
        for index, stride, name_field in ((1, 20, 12), (13, 32, 4))[:2 if delayed else 1]:   # imports, delay imports
            rva = struct.unpack_from('<I', data, directories + 8 * index)[0]
            at = offset(rva) if rva else None
            while at is not None:
                name_rva = struct.unpack_from('<I', data, at + name_field)[0]
                if not name_rva:
                    break
                name_at = offset(name_rva)
                names.add(data[name_at:data.index(b'\0', name_at)].decode(errors='replace').lower())
                at += stride
    except (struct.error, ValueError, IndexError):
        pass
    _imports[path, delayed] = names
    return names


def inputs(arch, dll, exe):
    """The digests of what a test program runs on: its import closure in the runtime (and the prefix, for native DLLs),
    (the program's delay imports, not its modules'), the module under test (loaded at run time by many tests), the Unix side of each, and the core. Modules a test loads
    by name at run time are not all here: the sentinel files of each group are rerun whatever their inputs."""
    folder = RUNTIME / ('i386-windows' if arch == 'i386' else 'x86_64-windows')
    target = dll if '.' in dll else dll + '.dll'
    todo, found = list(imports_of(exe, delayed=True)) + [target], {}
    while todo:
        name = todo.pop()
        if name in found:
            continue
        path = folder / name
        if not digest(path):
            path = PREFIX / 'windows' / ('syswow64' if arch == 'i386' else 'system32') / name
        found[name] = digest(path)
        if found[name]:
            todo += [n for n in imports_of(path) if n not in found]
        unix = RUNTIME / 'x86_64-unix' / (name.rsplit('.', 1)[0] + '.prx')
        if digest(unix):
            found[unix.name] = digest(unix)
    for path in CORE + ([RUNTIME / 'x86_64-windows' / n for n in ('wow64.dll', 'wow64win.dll', 'wow64cpu.dll', 'ntdll.dll')]
                        if arch == 'i386' else []):
        found[('x86_64-windows/' if arch == 'i386' and path.parent.name == 'x86_64-windows' else '') + path.name] = digest(path)
    return {k: v for k, v in sorted(found.items()) if v}


def inputs_key(test_sha256, found):
    return hashlib.sha256((test_sha256 + json.dumps(found, sort_keys=True)).encode()).hexdigest()[:16]


def timeout_for(host_entry):
    """From the PC's time for the file: only a run that completed there says how long the file takes (ntdll's PE32
    exception test crashes early on the PC; 60 s from that ended it on the console, where it completes in 90)."""
    seconds = (host_entry or {}).get('seconds')
    if not isinstance(seconds, (int, float)) or host_entry.get('status') != 'complete':
        return 300
    # 30x: the console's file and system calls cost 4-30 times the PC's (platform-stress-2026-10-25); ole32
    # storage32, all small reads and writes, outran 10x + 30 s
    return int(min(900, max(120, 30 * seconds + 60)))


BASE_CONTEXT = ('arch', 'test_sha256', 'wine_revision', 'batch_header_sha256')


def plan(dlls, arches, files, force, sentinels):
    """The entries to run, in the order asked, and the records they go to. A file runs again when its test program,
    the modules it runs on (inputs) or the Wine source changed, when its last result is not final, with --force, or
    as one of the sentinels: the fastest complete files of each DLL and architecture, rerun every time."""
    serial = json.loads(SERIAL.read_text())
    overrides = json.loads((ROOT / 'tests/wine-tests-timeouts-ps5.json').read_text())
    build = json.loads((ROOT / '.deps/gate/build.json').read_text())
    entries, records = [], {}
    for dll in dlls:
        for arch in arches:
            wt.ARCH = arch
            wt.OUT = RECORDS / arch
            context = dict(wt.test_context(dll), console_build_fingerprint=build['fingerprint'])
            wt.CONTEXT = context
            if not os.environ.get('PW_STUB_CONSOLE'):
                wt.check_staged_test(dll)
            path = wt.OUT / f'{dll}-console.json'
            record = wt.load(path)
            records[(dll, arch)] = (path, record, context)
            host = wt.load(HOST_RECORDS / arch / (f'{dll}-host-dxvk.json' if dll in wt.D3D_DLLS else f'{dll}-host.json'))
            skip = wt.skips(dll, arch)
            exe = ROOT / '.deps/wine-tests/exe' / wt.test_exe(dll).name
            # what the console runs now: the staged runtime when the run is planned (a build during the run changes the
            # staged files, not the console's)
            found = inputs(arch, dll, exe)
            key = inputs_key(context['test_sha256'], found)
            fastest = sorted((v.get('seconds') or 0, n) for n, v in record.items() if v.get('status') == 'complete')
            sentinel = {n for _, n in fastest[:sentinels]}
            for name in files or wt.test_names(dll):
                if name in skip and not (files and name in files):   # a skipped file named in --files runs
                    if record.get(name, {}).get('status') not in ('complete', 'incomplete', 'timeout'):
                        record[name] = {'status': 'skipped', 'reason': skip[name]}
                    continue
                previous = record.get(name, {})
                same = all(previous.get('context', {}).get(k) == context[k] for k in BASE_CONTEXT) and previous.get('inputs_key') == key
                if not force and name not in sentinel and same and previous.get('status') in ('complete', 'incomplete', 'timeout'):
                    continue
                override = overrides.get(f'{dll}:{name}', {}).get('seconds')
                entries.append({'id': f'{arch}-{dll}-{name}', 'dll': dll, 'arch': arch, 'name': name,
                                'exe': wt.test_exe(dll).name, 'timeout': override or timeout_for(host.get(name)),
                                'serial': dll in serial or f'{dll}:{name}' in serial, 'attempts': 0,
                                'inputs': found, 'inputs_key': key})
    return entries, records


def queue_text(entries, first_seq, trace=None, directory='C:\\wt'):
    """The queue file; with trace, each file runs with WINEDEBUG=err+all,TRACE (its output: the helper's log). Each file
    has a Temp folder of its own under DIRECTORY\\tmp, as each PC worker has a prefix of its own: files running together
    wrote one Temp\\test.avi (quartz, error 32, 2026-10-25)."""
    trace_env = f';WINEDEBUG=err+all,{trace}' if trace else ''
    lines = [f"{first_seq + i}\t{e['id']}\t{'s' if e['serial'] else 'p'}\t{e['timeout']}\t"
             f"{{TEMP={directory}\\tmp\\{e['id']};TMP={directory}\\tmp\\{e['id']}{trace_env}}}"
             f"{e['command'] if 'command' in e else 'C:' + chr(92) + e['exe'] + ' ' + e['name'] + ','}\n"
             for i, e in enumerate(entries)]
    lines.append(f'{first_seq + len(entries)}\tSTOP\n')
    return ''.join(lines)


def record_result(entries_by_id, records, run, directory, ident, code, ms, how, text):
    e = entries_by_id[ident]
    path, record, context = records[(e['dll'], e['arch'])]
    entry = wt.parse(text)
    entry.update(seconds=round(ms / 1000, 1), context=context, run=run, session=directory, exit_code=code,
                 timeout_seconds=e['timeout'], inputs=e['inputs'], inputs_key=e['inputs_key'])
    import re
    if how == 'killed' or re.search(rf"^winetest-batch: timeout {re.escape(e['name'])} (\d+)$", text, re.M):
        entry['status'] = 'timeout'
        entry['hung'] = f"still running after {e['timeout']} s" + ('; the session ended it' if how == 'killed' else '; it ended itself')
    elif how == 'unstarted':
        entry['status'] = 'no output'
    # As in the per-launch mode and on the PC: its summary makes a file complete, even when it then crashes on its way
    # out (ntdll info ends with 0xc0000005 on both); the exit code is kept beside it.
    record[e['name']] = entry
    wt.OUT = path.parent
    wt.save(path, record)
    e['done'] = True
    summary = entry.get('summary') or {}
    print(f"  {e['arch']} {e['dll']}:{e['name']}: {entry['status']}: {entry.get('failure_count', 0)} failure lines"
          f"{', ' + str(summary.get('tests')) + ' tests' if summary else ''} in {entry['seconds']} s", flush=True)


def idle_after(procs, seconds=60):
    """True once the console shows no app running, within seconds (a crashed title takes a moment to go)."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        try:
            if not procs():
                return True
        except Exception:
            pass
        time.sleep(5)
    return False


LAST_RUN = None   # the run of the last session started, as its records name it


def session(entries, records, jobs, name, extra_env, connect, settings, trace=None, attach=None, procs=None, on_result=None,
            run_name=None):
    """One title launch; the files it did not get to come back. attach: the run of a session still on the console, whose
    results are read until the title has ended (procs answers whether it still runs), without a launch of its own."""
    global LAST_RUN
    run = LAST_RUN = attach or run_name or f'{name}-{time.strftime("%H%M%S")}'
    directory = f'C:\\wt\\{run}'
    remote = f'{BASE}/wt/{run}'
    if attach:
        return attached(entries, records, run, directory, remote, connect, settings, procs)
    with connect(settings) as ftp:
        for folder in (f'{BASE}/wt', remote, remote + '/out', remote + '/tmp'):
            try:
                ftp.mkd(folder)
            except Exception:
                pass
        with SESSION_EXE.open('rb') as f:
            ftp.storbinary(f'STOR {BASE}/wt-session.exe', f)
        data = queue_text(entries, 1, trace, directory).encode()
        ftp.storbinary(f'STOR {remote}/queue.tmp', io.BytesIO(data))
        ftp.rename(f'{remote}/queue.tmp', f'{remote}/queue.txt')
    budget = int(min(8 * 3600, sum(e['timeout'] for e in entries) / max(1, jobs) + 600))
    command = [sys.executable, str(ROOT / 'tools/run-console.py'), '--exe', 'wt-session.exe', '--args', f'{jobs} 300 {directory}',
               '--marker', 'PS5_PROTON_WT_SESSION', '--seconds', str(budget), '--watchdog', str(budget + 300), '--name', run,
               '--env', 'PROTON_LAYOUT=launcher', '--env', 'WINEDEBUG=err+all'] + [x for kv in extra_env for x in ('--env', kv)]
    if os.environ.get('PW_STUB_CONSOLE'):
        command = stub_command(run, directory, jobs)
    holder = {}
    thread = threading.Thread(target=lambda: holder.setdefault('result', subprocess.run(command, capture_output=True, text=True)))
    started = time.monotonic()
    thread.start()
    by_id = {e['id']: e for e in entries}
    seen = set()
    print(f'session {run}: {len(entries)} files, {jobs} at a time, budget {budget} s', flush=True)
    while True:
        alive = thread.is_alive()
        exhausted = collect(by_id, seen, records, run, directory, remote, connect, settings, on_result)
        if exhausted:
            thread.join()
            raise SystemExit(f'{run}: stopped after {len(seen)}/{len(entries)} files: helpers no longer start this boot; '
                             'restart the console, then run again (the files not done are queued by their records)')
        if not alive:
            break
        time.sleep(5)
    result = holder.get('result')
    execution = wt.load(SESSIONS / (run + '.json'))
    print(f"session {run}: ended after {round(time.monotonic() - started)} s, {len(seen)}/{len(entries)} files, "
          f"result {execution.get('result')}, returned {execution.get('returned_to_launcher')}", flush=True)
    if not execution.get('returned_to_launcher') and execution.get('result') == 'crashed' and procs and idle_after(procs):
        # The title itself crashed (a GPU fault from one program takes the broker, and so the title, with it: B3c,
        # 2026-10-25) and the console is idle again: the files it was running are queued again, and run_sessions
        # records a file running at two such ends as a fault.
        print(f'{run}: the title crashed; the console is idle again, so the rest runs in a new session', flush=True)
        return [e for e in entries if not e.get('done')], round(time.monotonic() - started, 1)
    if not execution.get('returned_to_launcher'):
        raise SystemExit(f'{run}: no verified return to the launcher; no further session started'
                         + (f" ({(result.stdout + result.stderr).strip().splitlines()[-1][:150]})" if result and (result.stdout + result.stderr).strip() else ''))
    return [e for e in entries if not e.get('done')], round(time.monotonic() - started, 1)


def attached(entries, records, run, directory, remote, connect, settings, procs):
    """The results of a session the title still runs, until the console shows no app running."""
    by_id = {e['id']: e for e in entries}
    seen = set()
    started = time.monotonic()
    print(f'session {run}: attached, {len(entries)} files not done yet', flush=True)
    while True:
        alive = procs()
        if collect(by_id, seen, records, run, directory, remote, connect, settings):
            raise SystemExit(f'{run}: helpers no longer start this boot; restart the console, then run again')
        if not alive:
            break
        time.sleep(15)
    print(f'session {run}: the title ended after {round(time.monotonic() - started)} s attached, {len(seen)} files read', flush=True)
    return [e for e in entries if not e.get('done')], round(time.monotonic() - started, 1)


# Files in a row the session could not start: past this the console cannot spawn helpers any more (kern.ipc.maxpipekva
# and maxsockets exhausted, B3c sweep 2026-10-25: 317 files in a row, then the control payload lost), so the session is
# asked to stop and the run ends with that record instead of running the rest to no purpose.
UNSTARTED_LIMIT = 20
unstarted_streak = 0


def collect(by_id, seen, records, run, directory, remote, connect, settings, on_result=None):
    """Record every result the session has written since the last look; True once UNSTARTED_LIMIT files in a row could
    not start (the stop has then been asked for)."""
    global unstarted_streak
    try:
        with connect(settings) as ftp:
            text = io.BytesIO()
            try:
                ftp.retrbinary(f'RETR {remote}/results.txt', text.write)
            except Exception:
                pass
            for line in text.getvalue().decode(errors='replace').splitlines():
                fields = line.split('\t')
                if len(fields) != 5 or fields[1] in seen or fields[1] not in by_id:
                    continue
                out = io.BytesIO()
                try:
                    ftp.retrbinary(f'RETR {remote}/out/{fields[1]}.txt', out.write)
                except Exception:
                    pass
                seen.add(fields[1])
                unstarted_streak = unstarted_streak + 1 if fields[4] == 'unstarted' else 0
                (ROOT / 'klog').mkdir(exist_ok=True)
                (ROOT / 'klog' / f'{run}-{fields[1]}.txt').write_bytes(out.getvalue())
                text = out.getvalue().decode(errors='replace')
                if on_result:   # another runner's entries (the gate's probes, tools/run-battery.py --session)
                    by_id[fields[1]]['done'] = True
                    on_result(fields[1], int(fields[2]), int(fields[3]), fields[4], text)
                else:
                    record_result(by_id, records, run, directory, fields[1], int(fields[2]), int(fields[3]), fields[4], text)
    except Exception as error:
        print(f'  (results not read this time: {str(error)[:80]})', flush=True)
    if unstarted_streak >= UNSTARTED_LIMIT:
        print(f'{run}: {unstarted_streak} files in a row could not start: the console spawns no more helpers this boot '
              '(see the klog for kern.ipc.maxpipekva / maxsockets); asking the session to stop', flush=True)
        try:
            with connect(settings) as ftp:
                ftp.storbinary('STOR /data/ps5proton/stop-game', io.BytesIO(b'conformance: helpers no longer start\n'))
        except Exception as error:
            print(f'  (stop not sent: {str(error)[:80]})', flush=True)
        return True
    return False


def run_sessions(entries, records, jobs, name, extra, support, settings, trace=None, attach=None):
    """Sessions until every entry has a result; a file running when two sessions ended is a fault."""
    total = 0.0
    for _ in range(10):
        if not entries:
            break
        left, seconds = session(entries, records, jobs, name, extra, support['connect'], settings, trace, attach, support.get('procs'))
        attach = None
        total += seconds
        entries = left
        if not left:
            break
        # The session ended early: the files it was running are the first JOBS not done (they start in queue order).
        for e in left[:jobs]:
            e['attempts'] += 1
        for e in [e for e in left if e['attempts'] >= 2]:
            path, record, context = records[(e['dll'], e['arch'])]
            record[e['name']] = {'status': 'fault', 'context': context, 'failures': [], 'failure_count': 0, 'summary': None,
                                 'hung': 'running when two sessions ended'}
            wt.OUT = path.parent
            wt.save(path, record)
        entries = [e for e in left if e['attempts'] < 2]
        if entries:
            print(f'{len(entries)} files queued again in a new session', flush=True)
            time.sleep(60)
    return total, len(entries)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('dlls', nargs='+')
    parser.add_argument('--arch', action='append', choices=['x86_64', 'i386'])
    parser.add_argument('--files', nargs='*', help='only these test files (of every DLL named)')
    parser.add_argument('--jobs', type=int, default=6, help='files at once (6: 302 MiB peak at 4 of the guard\'s 990 MiB, ntdll 2026-10-25)')
    parser.add_argument('--force', action='store_true', help='run every file again')
    parser.add_argument('--sentinels', type=int, default=2, help='fastest complete files of each DLL and architecture rerun every time')
    parser.add_argument('--name', default=None)
    parser.add_argument('--env', action='append', default=[], help='NAME=VALUE for the whole session')
    parser.add_argument('--attach', metavar='RUN', help='read a session still on the console (its run, as sweep1-114050) '
                        'until its title ends, then go on with what it left; for a session whose watcher stopped early')
    parser.add_argument('--trace', help='Wine debug channels for each file (+seh,+bcrypt): a triage rerun; the trace is in the helpers\' logs')
    args = parser.parse_args()
    arches = args.arch or ['x86_64', 'i386']
    if not 1 <= args.jobs <= 8:
        parser.error('--jobs: 1 to 8')
    if not SESSION_EXE.is_file():
        raise SystemExit('build the session program first: tools/build-wine-probe.sh (tests/wt-session.c)')
    entries, records = plan(args.dlls, arches, args.files, args.force, args.sentinels)
    for (dll, arch), (path, record, _) in records.items():
        wt.OUT = path.parent
        wt.save(path, record)
    if os.environ.get('PW_STUB_CONSOLE'):
        support, settings = {'connect': lambda _settings: StubFtp(os.environ['PW_STUB_PREFIX'])}, None
    else:
        support = runpy.run_path(str(ROOT / 'tools/run-prefix-test.py'))
        settings = support['ps5_console'].load_settings()
        support['procs'] = lambda: ' count=0 ' not in support['ps5_console'].ps5vkctl_command(settings, 'procs', timeout=5) + ' '
    name = args.name or 'wts'
    total, left_total = 0.0, 0
    # DXVK's and VKD3D-Proton's overrides hold for a whole session: the Direct3D files get a session of their own.
    d3d = [e for e in entries if e['dll'] in wt.D3D_DLLS]
    for group, extra in (([e for e in entries if e['dll'] not in wt.D3D_DLLS], list(args.env)),
                         (d3d, list(args.env) + [f'WINEDLLOVERRIDES={wt.DXVK_OVERRIDES}'] + [f'{k}={v}' for k, v in wt.D3D_ENV.items()])):
        seconds, left = run_sessions(group, records, args.jobs, name, extra, support, settings, args.trace, args.attach)
        args.attach = None
        total += seconds
        left_total += left
    print(json.dumps({'sessions_seconds': round(total, 1), 'files_left': left_total}), flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
