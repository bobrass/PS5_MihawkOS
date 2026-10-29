#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The guard console launches, deploys and the probe tools' uploads go through.

The console has dropped off the network after title launches several times (a run of experimental code, and after
about 13 launches since a restart). A launch is refused if the previous run left the console unreachable,
until the console has been restarted and `new-boot` is run. Older probe tools
retain conservative pacing. Production runners may replace it with verified
idle/process readiness; time passing alone never proves cleanup.
The launch count is diagnostic only; there is no per-boot launch limit.

What refuses, in order (refusal()); each lasts until its fix:
  dropped   the console dropped off the network, or ps5vkctl answered this boot and is now gone or has another pid
            (rest mode unloads payloads while kstuff stays): restart by hand, then new-boot.
  storage   a klog line showed storage damage (STORAGE_DAMAGE): no launch and no write until a person has looked and
            run clear-storage. new-boot keeps it: the SSD errors of 2026-10-13/14 came back on the next boots.
  deploys   the title or the runtime on the console is not verified: a deploy did not finish, or (the title) the console
            restarted since its read-back. tools/verify-title.py reads the title back.
Each console has its own state (STATE: .deps/console-guard-<key>.json, the key a hash of the console's address from
PS5_HOST or PS5_Vulkan/.env, so no address is written into a name or a log), so a run on the standard PS5 and one on
the Pro go at once from checkouts that share .deps (docs/CONSOLE_TESTING.md). A console's first state starts with the
blocks of the single state file of before (console-guard.json): they may have been this console's. With no console
set, that single file is used. The storage and deploy blocks live in <state>-sticky.json beside the state file. The
sibling checkout (../PS5_Proton) shares the state through a symlink, and its older guard honours only `dropped`: a
storage block is therefore also recorded as a drop, and that guard ignores the deploy records until it carries this
guard.
Every deploy (ps5/tools/deploy.sh, deploy-wine, deploy-folder, console-setup) and the probe tools that
upload (prefix-forget-dualsense, run-process-probe, restore-process-probe, deploy-d3d-probe,
recover-child-probe, run-payload-start-test, run-mesa-zink-probe) calls
begin_write() first (it also refuses while a title runs) and end_write() last (writing()), which asks ps5vkctl to
sync; a launch waits WRITE_SETTLE s after the last such write. Test runners' setup writes are not bracketed or synced:
run-console.py's pre-launch control files, the prefix-fresh and launcher-tls fixtures (run-prefix-baseline.py,
run-tls-fixture.py) and the other run-* runners.

`new-boot` is run only once the console has really been restarted by hand; nothing in the tools runs it. It saves
klogsrv's replay to klog/boot-<UTC stamp>.log and scans it. It is refused while ps5vkctl still answers with the pid it
had this boot, unless the replay's uptime stamps show a boot newer than the last recorded launch (restarted_by_uptime:
ps5vkctl got pid 97 again after a real restart on 2026-10-17).

  tools/console_guard.py status
  tools/console_guard.py new-boot
  tools/console_guard.py drop "why"           record that the console dropped off the network (no retry: by hand)
  tools/console_guard.py clear-storage "why"  after a person checked the storage damage: what they checked
  tools/console_guard.py begin-write WHAT     before a write to the console (title: ps5/tools/deploy.sh)
  tools/console_guard.py end-write            after it
  tools/console_guard.py mode lapy|legacy     record which elevation the title uses
"""
import contextlib
import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path

from console_log import LineLog, redact

ROOT = Path(__file__).resolve().parents[1]
LEGACY_STATE = 'console-guard.json'   # the one state every console shared before 2026-10-28
KLOG = ROOT / 'klog'
MIN_GAP = 60.0
SETTLE = 8.0
# The SSD's read errors before format #1 (a klog of 2026-10-13) and reinstall #2 (a klog of 2026-10-14); none in
# the 34,424 klogs before them. Routine lines stay out: '[fserr] ClearSlotInfoCache()' and a repaired fsck (two
# klogs of 2026-10-14).
STORAGE_DAMAGE = re.compile(r'\[IOD\] process_completion: error|bfs_device_\w+ \[[^\]]*\] -> errno=5|'
                            r'\[fserr\] 0x[0-9a-f]+,0x[0-9a-f]+,ssd0|ReportCorruptPackageError|'
                            r'Decrypt error in SELF block')
# Twice UFS's 30 s syncer delay: an assumption, not proof the data reached the disk (only a read-back after a restart
# shows that).
WRITE_SETTLE = 60.0
BOOT_QUIET = 2.0
BOOT_LIMIT = 30.0
PING = re.compile(r'\bok ps5vkctl \d+ pid=(\d+)')
VERIFY = {'title': 'python3 tools/verify-title.py, or deploy again: bash ps5/tools/deploy.sh',
          'runtime': 'deploy again: python3 tools/deploy-wine.py'}


def ps5_console_module():
    tools = str(ROOT.parent / 'PS5_Vulkan/tools')
    if tools not in sys.path:
        sys.path.insert(0, tools)
    import ps5_console
    return ps5_console


def console_key(settings=None):
    """The console PS5_HOST (or PS5_Vulkan/.env) names, as 12 hex digits of its address's sha256: the name its guard
    state and gate records are kept under. None when no console is set."""
    try:
        host = (settings or ps5_console_module().load_settings())['host']
    except (SystemExit, ImportError, OSError, KeyError):
        return None
    return hashlib.sha256(host.encode()).hexdigest()[:12]


KEY = console_key()


def state_path(root=ROOT):
    return root / '.deps' / (f'console-guard-{KEY}.json' if KEY else LEGACY_STATE)


STATE = state_path()
LAST_RUN = ROOT / '.deps' / (f'last-console-run-{KEY}' if KEY else 'last-console-run')


def fresh():
    return {'launches': 0, 'dropped': None, 'last_launch': 0.0, 'boot_started': time.time()}


# The blocks that outlive a restart live beside the shared file, in a file the sibling checkout's older guard never
# rewrites: its new-boot saves only its own keys, which erased them (it does not know them).
STICKY_KEYS = ('storage', 'storage_cleared', 'deploys')


def sticky_path(path=None):
    path = (path or STATE).resolve()
    return path.with_name(path.stem + '-sticky.json')


def read_state(path, missing):
    try:
        return json.loads(path.read_text())
    except FileNotFoundError:
        return missing
    except ValueError:
        raise SystemExit(f'guard state unreadable ({path}): restore it or delete it by hand; '
                         'nothing launches or writes until then') from None


# What a console's first state takes from the single state of before: the blocks, never the ps5vkctl pid or the
# launch count of whichever console that was (another console's pid would read as a drop).
INHERITED_KEYS = ('dropped', 'storage', 'storage_cleared')


def load(path=None):
    """A missing file is a fresh state, with the single state's blocks for a console's first; an unreadable one
    refuses everything (it may have held a block)."""
    path = path or STATE
    state = read_state(path, None)
    if state is None:
        state = fresh()
        legacy = path.with_name(LEGACY_STATE)
        if path.name != LEGACY_STATE and legacy.exists():
            old = load(legacy)
            state.update({key: old[key] for key in INHERITED_KEYS if old.get(key)})
    state.update(read_state(sticky_path(path), {}))   # an older state's own sticky keys stay until the next save
    return state


def write_state(target, data):
    # Replaced whole: never half written; a symlink (../PS5_Proton's guard) is kept.
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = target.with_name(f'.{target.name}.{os.getpid()}')
    temporary.write_text(json.dumps(data, indent=1) + '\n')
    os.replace(temporary, target)


def save(state):
    # The sticky keys first, so a crash between the two writes never loses them.
    if any(key in state for key in STICKY_KEYS):
        write_state(sticky_path(), {key: state[key] for key in STICKY_KEYS if key in state})
    write_state(STATE.resolve(), {key: value for key, value in state.items() if key not in STICKY_KEYS})


def refusal(state, deploys=True):
    """Why nothing may launch now (with deploys=False: why nothing may be written); None when allowed."""
    if state.get('dropped'):
        return (f"no launch: the console dropped off the network ({state['dropped']['why']}, "
                f"{time.strftime('%F %T', time.localtime(state['dropped']['at']))}). It must be restarted by hand, "
                f"then: tools/console_guard.py new-boot")
    storage = state.get('storage')
    if storage:
        return (f"no launch and no write: the klog of {storage['run']} showed storage damage ({storage['lines'][0]}); "
                "a person must look first (the console's Safe Mode screen, python3 tools/verify-title.py), "
                "then: tools/console_guard.py clear-storage 'what was checked'")
    for what, record in sorted(state.get('deploys', {}).items()) if deploys else ():
        if not record.get('verified'):
            return (f"no launch: the {what} on the console is not verified by hash ({record.get('why')}); "
                    + VERIFY.get(what, 'deploy it again'))
    return None


def check_launch_allowed():
    """The refusals are unconditional, including the readiness path."""
    state = load()
    message = refusal(state)
    if message:
        raise SystemExit(message)
    return state


def before_launch(what='', readiness=None):
    """Use real readiness when supplied; retain legacy pacing otherwise."""
    state = check_launch_allowed()
    settle = WRITE_SETTLE - (time.time() - state.get('last_write', 0.0))
    if settle > 0:
        print(f"settling {settle:.0f} s after the last write to the console (sync: {state.get('sync')})", flush=True)
        time.sleep(settle)
    if readiness is not None:
        result = readiness()
        check_launch_allowed()
        if not result or result.get('ready') is not True:
            raise RuntimeError('Console readiness was not verified; launch refused')
        return result
    gap = time.time() - state.get('last_launch', 0.0)
    if gap < MIN_GAP:
        time.sleep(MIN_GAP - gap)  # paced, not refused: the batch runners already wait longer


def leftover_helpers():
    """Read-only census: None means unknown, never an empty process list."""
    listing = subprocess.run([sys.executable, str(ROOT / 'tools/console-processes.py')],
                             capture_output=True, text=True, timeout=120)
    fields = []
    for line in listing.stdout.splitlines():
        entry = dict(item.split('=', 1) for item in line.split() if '=' in item)
        if 'pid' in entry and 'name' in entry:
            fields.append((entry, line))
    launcher = [entry['pid'] for entry, _ in fields if entry['name'] == 'websrv.elf']
    if listing.returncode or len(launcher) != 1:
        return None
    return [line for entry, line in fields
            if entry['name'] == 'SceSpZeroConf' and entry.get('ppid') == launcher[0]]


def control_payload(settings):
    """ps5vkctl's pid, recorded the first time it answers this boot. Rest mode unloads payloads while kstuff stays
    (a manual run of 2026-10-14), and a run on such a console is not on the boot it seems: one that answered this boot and
    is gone, or answers with another pid, is recorded as a drop. One that never answered this boot only needs loading."""
    ps5_console = ps5_console_module()
    pids = []

    def ping():
        reply = ps5_console.ps5vkctl_command(settings, 'ping', timeout=5)
        match = PING.search(reply)
        if not match:
            raise RuntimeError(f'ps5vkctl answered ping without its pid ({redact(reply)[:80]})')
        pids.append(int(match.group(1)))

    known = load().get('ps5vkctl_pid')
    try:
        ping()
    except (SystemExit, Exception) as error:
        if known is None:
            raise RuntimeError(f'ps5vkctl does not answer ({redact(error)[:120]}): load it, then run again; '
                               'run refused') from None
        if record_drop_unless_answering(f'ps5vkctl (pid {known}) answered this boot and is gone: rest mode or a '
                                        'dead payload', ping):
            check_launch_allowed()
    if known is not None and pids[-1] != known:
        record_drop(f'ps5vkctl was pid {known} this boot and is now pid {pids[-1]}: it was gone and was loaded again')
        check_launch_allowed()
    if known is None:
        state = load()
        state['ps5vkctl_pid'] = pids[-1]
        save(state)
    return pids[-1]


def wait_ready(settings):
    """Verify idle on both sides of the existing native helper census.

    A completed run's cleanup proof permits immediate checking. Unknown or
    failed cleanup keeps the historical eight-second display settle. Neither
    path skips the census, accepts a running app, or clears the outage guard.
    """
    ps5_console = ps5_console_module()
    state = check_launch_allowed()
    control_payload(settings)
    started = time.monotonic()
    wait = 0.0
    if not state.get('cleanup', {}).get('clean'):
        try:
            wait = max(0.0, SETTLE - (time.time() - LAST_RUN.stat().st_mtime))
        except FileNotFoundError:
            pass
        if wait:
            print(f'settling {wait:.1f} s after unverified cleanup', flush=True)
            time.sleep(wait)
    def idle():
        return ' count=0 ' in ps5_console.ps5vkctl_command(settings, 'procs', timeout=5) + ' '
    deadline = time.monotonic() + 60
    while not idle():
        if time.monotonic() >= deadline:
            raise RuntimeError('The console is not idle; run refused')
        time.sleep(0.25)
    census_started = time.monotonic()
    leftovers = leftover_helpers()
    census_seconds = time.monotonic() - census_started
    if leftovers is None:
        raise RuntimeError('Cannot list console helpers; run refused')
    if leftovers:
        safe_lines = [re.sub(r'\b(?:\d{1,3}\.){3}\d{1,3}\b', '[console]', line) for line in leftovers]
        print('Helpers an earlier run left are still alive:\n  ' + '\n  '.join(safe_lines), flush=True)
        raise RuntimeError(f'{len(leftovers)} helper process(es) outlived their title; '
                           'restart the console before the next run; run refused')
    if not idle():
        raise RuntimeError('The console became occupied during the census; run refused')
    return {'ready': True, 'seconds': round(time.monotonic() - started, 3),
            'settle_seconds': round(wait, 3),
            'census_seconds': round(census_seconds, 3),
            'previous_cleanup_verified': bool(state.get('cleanup', {}).get('clean'))}


def finished(clean=False):
    """Only runners proving normal completion and hook cleanup set clean."""
    state = load()
    state['cleanup'] = {'clean': bool(clean), 'at': time.time()}
    save(state)
    LAST_RUN.touch()


def after_launch():
    state = load()
    state['launches'] += 1
    state['last_launch'] = time.time()
    state['cleanup'] = {'clean': False, 'at': state['last_launch']}
    save(state)


def record_drop(why):
    state = load()
    state['dropped'] = {'why': why[:200], 'at': time.time()}
    save(state)
    print(f'console guard: the console is recorded as dropped ({why[:120]}); no launch until it is restarted by hand', flush=True)


DROP_RETRIES = 12
DROP_INTERVAL = 5.0


def record_drop_unless_answering(why, ping, retries=None, interval=None):
    """Record a drop only if the console stays silent. A game launcher run's end-of-run check timed out twice on 2026-10-11
    (the control payload was busy for a few seconds) and the console answered everything right after, each time
    costing a restart by hand. `ping` is a call that raises when the console does not answer; it is tried for about
    a minute. A console that has really dropped stays silent that long and is recorded exactly as before.
    True when the drop was recorded."""
    retries = DROP_RETRIES if retries is None else retries
    interval = DROP_INTERVAL if interval is None else interval
    for attempt in range(retries):
        try:
            ping()
        except (SystemExit, Exception):
            time.sleep(interval)
            continue
        print(f'console guard: the console answered again after {attempt + 1} tries ({why[:100]}); not recorded as dropped',
              flush=True)
        return False
    record_drop(why)
    return True


def storage_damage(lines):
    """The lines that show storage damage; any one stops console work until a person clears it."""
    return [line for line in lines if STORAGE_DAMAGE.search(line)]


def record_storage(run, lines):
    state = load()
    if not state.get('storage'):   # the first evidence is the one kept
        state['storage'] = {'run': run, 'at': time.time(), 'lines': [redact(line)[:200] for line in lines[:5]]}
        # Also a drop, the one block the sibling checkout's older guard honours; clear-storage lifts it.
        state['dropped'] = state.get('dropped') or {'why': f'storage damage in the klog of {run}', 'at': time.time(),
                                                    'storage': True}
        save(state)
    print(f'console guard: storage damage in the klog of {run} ({redact(lines[0])[:200]}); '
          'no launch and no write until a person clears it', flush=True)


def clear_storage(why):
    state = load()
    state['storage_cleared'] = {'why': why[:200], 'at': time.time(), 'was': state.get('storage')}
    state['storage'] = None
    if (state.get('dropped') or {}).get('storage'):
        state['dropped'] = None
    save(state)


def begin_write(what, settings=None):
    """The first step of a deploy (deploy.sh, deploy-wine, deploy-folder, console-setup) and of the
    probe tools' uploads (writing(), the module docstring lists them). Refused on a drop, storage damage or a running title, before anything is marked; the title or the
    runtime then stays unverified until its read-back. end_write() is the last step."""
    message = refusal(load(), deploys=False)
    if message:
        raise SystemExit(message)
    ps5_console = ps5_console_module()
    try:
        procs = ps5_console.ps5vkctl_command(settings or ps5_console.load_settings(), 'procs', timeout=20)
    except SystemExit as error:
        raise SystemExit(redact(error)) from None
    if ' count=0 ' not in procs + ' ':
        raise SystemExit(f'the console is running an application ({redact(procs)}): close it first; nothing written')
    state = load()
    state['last_write'] = time.time()
    if what in VERIFY:
        state.setdefault('deploys', {})[what] = {
            'verified': False, 'why': f"a deploy began at {time.strftime('%F %T')} and was not read back"}
    save(state)


def end_write(settings=None):
    """The last step of every write: ps5vkctl's sync (tools/patches/console-control-sync.patch; an older ps5vkctl's
    refusal is recorded, not raised) and the time the next launch's settle counts from. Returns the sync reply."""
    ps5_console = ps5_console_module()
    try:
        reply = ps5_console.ps5vkctl_command(settings or ps5_console.load_settings(), 'sync', timeout=60)
    except (SystemExit, OSError) as error:
        reply = 'err ' + redact(error)
    if reply.startswith('err usage: sync'):   # PS5_Vulkan's own ps5vkctl reads the word as a title id (2026-10-27)
        reply += '; this ps5vkctl has no sync: tools/console-setup.py loads the pinned one that has it'
    state = load()
    state['last_write'] = time.time()
    state['sync'] = reply[:80]
    save(state)
    return reply


@contextlib.contextmanager
def writing(what='probe', settings=None):
    """begin_write() and end_write() around a block of console writes, however the block ends. A probe tool's writes use
    'probe', which only stamps the write: the tool's own launch is not refused."""
    begin_write(what, settings)
    try:
        yield
    finally:
        end_write(settings)


def record_unverified(what, why):
    """The title or the runtime on the console is no longer the verified copy (a probe's restore did not finish)."""
    state = load()
    state.setdefault('deploys', {})[what] = {'verified': False, 'why': why[:200], 'at': time.time()}
    save(state)


def record_verified(what, digest, how):
    state = load()
    state.setdefault('deploys', {})[what] = {'verified': True, 'at': time.time(), 'digest': digest, 'how': how}
    save(state)


def capture_boot_klog(settings, path):
    """klogsrv's replay buffer, saved redacted to path and returned as lines. It starts minutes after boot (146 and
    314 s measured), so boot-time fsck lines may already be gone from it."""
    lines = []
    with socket.create_connection((settings['host'], settings['klog_port']), timeout=10) as connection, \
            path.open('w') as output:
        log = LineLog(output, lines.append)
        connection.settimeout(BOOT_QUIET)
        end = time.monotonic() + BOOT_LIMIT
        while time.monotonic() < end:
            try:
                chunk = connection.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            log.write(chunk)
        if log.pending:
            log.write(b'\n')
    return lines


def new_boot():
    """After a real restart. The storage block and the deploy records survive it, but not the title's read-back:
    libc.prx, read back before, did not load after the dmem-01 power cut (klog/vmp-tracker-2026-10-14-01.log:150-163)."""
    old = load()
    state = fresh()
    state.update({key: old[key] for key in ('mode', 'storage', 'storage_cleared', 'deploys') if key in old})
    state.setdefault('mode', 'legacy')
    if 'title' in state.get('deploys', {}):
        state['deploys']['title'] = dict(state['deploys']['title'], verified=False,
                                         why='the console restarted since its read-back')
    save(state)


EXEC_UPTIME = re.compile(r'<(\d+) msec>')


def restarted_by_uptime(state, lines):
    """For a reused ps5vkctl pid: the replay's process starts are stamped with the console's uptime. The last launch
    recorded since the last boot started processes at an uptime of at least last_launch - boot_started, so a replay
    whose newest stamp is below that is a newer boot. No launch or no stamp proves nothing: not restarted."""
    stamps = [int(match.group(1)) for line in lines for match in [EXEC_UPTIME.search(line)] if match]
    since = state.get('last_launch', 0.0) - state.get('boot_started', 0.0) if state.get('launches') else 0.0
    return bool(stamps) and max(stamps) / 1000 < since


def boot():
    """The new-boot command: refused on a console that was not restarted, then the replay saved and scanned."""
    ps5_console = ps5_console_module()
    settings = ps5_console.load_settings()
    known = load().get('ps5vkctl_pid')
    same_pid = False
    if known is not None:
        try:
            match = PING.search(ps5_console.ps5vkctl_command(settings, 'ping', timeout=5))
        except SystemExit:
            match = None
        same_pid = bool(match and int(match.group(1)) == known)
    KLOG.mkdir(exist_ok=True)
    path = KLOG / f"boot-{time.strftime('%Y-%m-%d-%H%M%S', time.gmtime())}.log"
    try:
        lines = capture_boot_klog(settings, path)
    except OSError as error:
        path.unlink(missing_ok=True)
        print(f'new-boot not recorded: klog did not answer ({redact(error)}); load klogsrv, then run it again',
              file=sys.stderr)
        return 2
    if same_pid and not restarted_by_uptime(load(), lines):
        path.unlink(missing_ok=True)
        print(f'new-boot refused: ps5vkctl still answers as pid {known}, the pid it had this boot, and the klog replay '
              'shows no boot newer than the last launch, so the console was not restarted (rest mode is not a '
              'restart). Restart it by hand, then run new-boot.', file=sys.stderr)
        return 1
    new_boot()
    print(f'recorded: a fresh boot, 0 launches; klogsrv replay saved to {path} ({len(lines)} lines; it starts '
          'minutes after boot, so boot-time lines may be missing)')
    found = storage_damage(lines)
    if found:
        record_storage(path.stem, found)
        print('\n'.join(found))
        return 1
    return 0


def set_mode(mode):
    state = load()
    state['mode'] = mode
    save(state)


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else 'status'
    words = ' '.join(sys.argv[2:])
    if command == 'new-boot':
        return boot()
    if command == 'mode':
        set_mode(words or 'legacy')
        print('mode:', load()['mode'])
    elif command == 'drop':
        record_drop(words or 'recorded by hand')
    elif command == 'clear-storage':
        if not words:
            sys.exit('clear-storage needs the reason: what a person checked')
        clear_storage(words)
        print('storage block cleared:', words)
    elif command == 'begin-write':
        if not words:
            sys.exit('begin-write needs what is written: title, runtime, ...')
        begin_write(words)
    elif command == 'end-write':
        print('sync:', end_write())
    else:
        print(f'console {KEY or "not set"} ({STATE.name})')
        print(json.dumps(load(), indent=1))
    return 0


if __name__ == '__main__':
    sys.exit(main())
