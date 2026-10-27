#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Does the kernel act on a shared object's per-mapping charge or on its physical pages (tools/shared-charge-probe.c)?

shared: one payload maps one JIT object again at each step, so the per-mapping resident total (the census) grows by the
object while the physical pages do not. distinct, the control: a new object at each step, so both grow. The probe reads,
every second, every other process's resident flexible memory (as the title's census does) and its own, and at each step
the kernel's vm.swap_reserved. It stops itself, and the runner stops it, when the kernel takes memory: other processes
losing --others-stop MiB, its own pages evicted, a refusal, a fault, the console thrashing, Shell UI's missed heartbeat,
the console going silent.

SceShellCore prints its VM Stats and FMEM lines only on the running app's own process events (2026-10-21: none while a
title drew its menu for 120 s, nor for payloads started beside it), so the runner keeps the title on its launcher screen
for the run (test-run.txt "samples launcher", no Wine session, no memory guard): its start gives the baseline swap-out and
RSS, its exit, after the probe has released, the final ones.

Each step needs a grant: the runner writes the highest step allowed to /data/ps5proton/shared-charge.go only after the
probe's line for the step before passes every stop rule, so a runner that dies stops the ramp. A stop writes
/data/ps5proton/shared-charge.stop, retried until it lands; the probe then frees everything within a quarter second.

Order (docs/PLAN.md K18 and after): shared first. An action at a step >= 2 means the kernel acts on the per-mapping
total. Only if shared shows none, distinct up to the same total: an action there at a step <= maps - 2 means physical
pages; none means inconclusive at this total. The probe refuses any other firmware than --firmware names. Writes
evidence/<name>.json and klog/<name>.log.

--console pro (the PS5 Pro, installed 10.01): its VM Stats print a limit on RSS (about 1020 MiB, the second figure), the
standard PS5's print 0.0. Steam-client runs there went past it by up to 130 MiB for seconds with swap flat, and every
earlier run that went well past it for longer froze the console, so the ramp has a ceiling: no step is granted whose
projected total (the other processes' resident memory and the probe's own, as the probe counts them) would pass
limit + --over-limit MiB. Reaching it stops the probe cleanly (a planned end, not a kernel action). A klog line about
low or full memory, or the title ending under the ramp, is a stop and counts as the kernel acting."""
import argparse, ftplib, hashlib, io, json, os, re, socket, subprocess, sys, threading, time, urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console
sys.path.insert(0, str(ROOT / 'tools'))
import console_guard

DATA = '/data/ps5proton'
TITLE, BASE = 'PPSA99170', '/data/homebrew/PPSA99170'
STOP, GO, CHECK = DATA + '/shared-charge.stop', DATA + '/shared-charge.go', DATA + '/shared-charge.check'
VM = re.compile(r'VM Stats: RSS ([\d.]+) ([\d.]+), kernel ([\d.]+) [\d.]+, wire count ([\d.]+) [\d.]+, swap out ([\d.]+), '
                r'page table CPU (\d+)/(\d+)')
FMEM = re.compile(r'FMEM\s+([\d.]+)/\s*([\d.]+)\s+([\d.]+)/\s*([\d.]+)\s+(.+?)\s*$')
MARK = re.compile(r'\[shared-charge\] (.*)')
CENSUS = re.compile(r'step=(\d+) phase=(\w+) mappings=(\d+) resident_kib=(\d+).*? swap_reserved_kib=(-?\d+).*? '
                    r'others_kib=(\d+) others_min_kib=(\d+) processes=(\d+)')
ALARM = re.compile(r'No heart beat of Shell UI|maxpipekva|TTY\(STDIN\)')
# The system saying it is short of memory (the Pro, with --console pro): not our own lines, nor the VM Stats figures.
LOWMEM = re.compile(r'(?i)\b(?:not enough|insufficient|low|out of|exceed\w*|full)\W+(?:\w+\W+){0,3}memory\b|'
                    r'\bmemory\W+(?:\w+\W+){0,3}(?:low|full|exceed\w*|shortage)\b')
ADDRESS = re.compile(r'\b(?:\d{1,3}\.){3}\d{1,3}\b')


def mask(text):
    return ADDRESS.sub('<address>', str(text))


def signed(value):
    value = int(value)
    return value - (1 << 32) if value >= 1 << 31 else value


def send(settings, elf, words, results, timeout):
    boundary = 'SharedCharge'
    body = b''
    for name, data in (('args', ' '.join(words).encode()), ('pipe', b'1'), ('elf', elf.read_bytes())):
        header = f'--{boundary}\r\nContent-Disposition: form-data; name="{name}"'
        if name == 'elf':
            header += '; filename="shared-charge.elf"'
        body += (header + '\r\n\r\n').encode() + data + b'\r\n'
    body += f'--{boundary}--\r\n'.encode()
    request = urllib.request.Request(f'http://{settings["host"]}:8080/elfldr', data=body,
                                     headers={'Content-Type': 'multipart/form-data; boundary=' + boundary})
    try:
        with urllib.request.urlopen(request, timeout=timeout) as response:
            results['output'] = response.read().decode(errors='replace')
    except OSError as error:
        results['output'] = 'ERROR ' + mask(error)


def klog_stream(settings, stop, connected, lines):
    """klogsrv's lines as (time, live, text). Its replay buffer comes first: a line is live once a second of quiet has
    followed the replay, or 10 s after connecting, whichever comes first."""
    pending, live = b'', False
    try:
        with socket.create_connection((settings['host'], settings['klog_port']), timeout=10) as connection:
            connection.settimeout(1)
            connected.append(time.time())
            while not stop.is_set():
                try:
                    chunk = connection.recv(65536)
                except socket.timeout:
                    live = True
                    continue
                if not chunk:
                    break
                if time.time() - connected[0] > 10:
                    live = True
                pending += chunk
                *complete, pending = pending.split(b'\n')
                for raw in complete:
                    lines.append((time.time(), live, raw.decode(errors='replace')))
    except OSError as error:
        lines.append((time.time(), True, 'ERROR klog: ' + mask(error)))


def ftp_call(settings, action):
    with ftplib.FTP() as ftp:
        ftp.connect(settings['host'], settings['ftp_port'], timeout=15)
        ftp.login(settings['ftp_user'], settings['ftp_password'])
        return action(ftp)


def put(settings, path, text):
    ftp_call(settings, lambda ftp: ftp.storbinary('STOR ' + path, io.BytesIO(text.encode())))


def remove(settings, path):
    def delete(ftp):
        try:
            ftp.delete(path)
        except ftplib.all_errors:
            pass   # absent, or the console's DELE answering 226 (console FTP quirks)
    ftp_call(settings, delete)


def listed(settings, path):
    rows = []
    ftp_call(settings, lambda ftp: ftp.retrlines('LIST ' + DATA, rows.append))
    return any(row.split()[-1] == path.rsplit('/', 1)[1] for row in rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--name', required=True)
    parser.add_argument('--mode', choices=('shared', 'distinct'), required=True)
    parser.add_argument('--object-mib', type=int, default=64, choices=range(16, 257))
    parser.add_argument('--maps', type=int, default=24, choices=range(1, 25))
    parser.add_argument('--hold', type=int, default=30, choices=range(30, 61))
    parser.add_argument('--swap-stop', type=float, default=8.0, help='MiB of swap-out growth that stops the run (1-32)')
    parser.add_argument('--others-stop', type=float, default=32.0, help='MiB other processes may lose before a stop')
    parser.add_argument('--console', choices=('standard', 'pro'), default='standard')
    parser.add_argument('--over-limit', type=int, default=30, choices=range(-200, 121),
                        help='pro only: MiB past the printed RSS limit the ramp may reach (negative: stay below it)')
    parser.add_argument('--firmware', default=None, help="the installed firmware's top half (1340 = 13.40, 1001 = 10.01); "
                        "default by --console")
    parser.add_argument('--probe', help='use this built ELF instead of rebuilding the probe (the exact control binary)')
    args = parser.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]+', args.name):
        parser.error('use a plain run name')
    if not 1 <= args.swap_stop <= 32 or not 8 <= args.others_stop <= 64:
        parser.error('--swap-stop is 1 to 32 MiB, --others-stop 8 to 64 MiB')
    if args.object_mib * args.maps > 2048:
        parser.error('object-mib x maps is at most 2048')
    if args.firmware is None:
        args.firmware = '1001' if args.console == 'pro' else '1340'
    if not re.fullmatch(r'1[0-9]{3}', args.firmware):
        parser.error('--firmware is four digits, e.g. 1340')
    if 'PS5_HOST' not in os.environ:
        raise SystemExit('export PS5_HOST for the console this run is for (the shared tooling defaults to another)')
    evidence = ROOT / 'evidence' / (args.name + '.json')
    if evidence.exists():
        parser.error('use a new run name')
    settings = ps5_console.load_settings()
    # The title draws its launcher this long (60 frames a second, measured 2026-10-21): the ramp, then its own end.
    title_seconds = (args.maps + 2) * (args.hold + 5) + 150

    if args.probe:
        elf = Path(args.probe).resolve()
        if not elf.is_file():
            parser.error('--probe is not a file: ' + str(elf))
    else:
        elf = ROOT / 'build/shared-charge-probe.elf'
        elf.parent.mkdir(exist_ok=True)
        wrap = ['-Wl,--wrap=kernel_mprotect', '-Wl,--wrap=kernel_set_vmem_protection']
        subprocess.run([str(ROOT / '.deps/native/ps5-payload-sdk/bin/prospero-clang'), '-std=gnu11', '-O2', '-Wall',
                        '-Wextra', '-Werror', *wrap, '-o', str(elf), str(ROOT / 'tools/shared-charge-probe.c')],
                       check=True)
    listing = subprocess.run(['llvm-objdump', '-d', '--no-show-raw-insn', str(elf)], check=True, capture_output=True,
                             text=True).stdout
    imports = subprocess.run(['llvm-nm', '-D', '--undefined-only', str(elf)], capture_output=True, text=True).stdout
    if (re.search(r'(call|jmp)\S*\s.*<(kernel_mprotect|kernel_set_vmem_protection|fork|vfork|rfork)>', listing)
            or re.search(r'\b(fork|vfork|rfork|execve)\b', imports) or '__wrap_kernel_mprotect' not in listing):
        raise SystemExit('the probe can reach the SDK kernel write or fork: not sent')

    try:   # refusals, the control payload, idle, no helper left alive, pacing
        readiness = console_guard.before_launch(readiness=lambda: console_guard.wait_ready(settings))
    except (RuntimeError, SystemExit) as error:
        raise SystemExit(mask(error))
    # The runner must be able to write what stops the probe, before anything is sent.
    put(settings, CHECK, 'run-shared-charge: write check\n')
    if not listed(settings, CHECK):
        raise SystemExit('the stop path cannot be written on the console: not sent')
    remove(settings, CHECK)
    remove(settings, STOP)
    put(settings, GO, '0\n')

    ceiling_mib = None   # the Pro's, once the baseline is read; follows the lowest limit a live block shows
    stop_reader, connected, lines = threading.Event(), [], []
    reader = threading.Thread(target=klog_stream, args=(settings, stop_reader, connected, lines), daemon=True)
    reader.start()
    state = {'seen': 0, 'label': 'before', 'vm': [], 'block': None, 'marks': [], 'census': {}, 'steps': {},
             'reason': None, 'acted_step': None, 'stop_written': None, 'stop_errors': [], 'grant': 0, 'grant_errors': [],
             'probe_done': None, 'firmware_line': None, 'last_probe_t': None, 'title_ended': None, 'ceiling_step': None,
             'projected': {}, 'actual': {}, 'stop_pending': False, 'ramp_started': False, 'payload_returned': False}
    started = time.time()

    def stop_because(why, step=None):
        if state['reason'] is None:
            state['reason'] = why
            state['acted_step'] = step if step is not None else state['label']
            print('STOP:', why, flush=True)

    def drain(replay_check=False):
        batch = lines[state['seen']:]
        state['seen'] += len(batch)
        for at, live, line in batch:
            if not live:
                if replay_check and (ALARM.search(line) or console_guard.STORAGE_DAMAGE.search(line)):
                    state.setdefault('replay_alarms', []).append(mask(line)[:200])
                continue
            if ALARM.search(line):
                stop_because('klog: ' + mask(line)[:120])
            if console_guard.STORAGE_DAMAGE.search(line):
                stop_because('klog storage damage: ' + mask(line)[:120])
            if (args.console == 'pro' and 'shared-charge' not in line and 'VM Stats' not in line
                    and '[MihawkOS]' not in line and LOWMEM.search(line)):
                stop_because('klog: low-memory line: ' + mask(line)[:120])
            if re.search(r'\[MihawkOS\].*ends: status', line):
                state['title_ended'] = at
                if (args.console == 'pro' and state['probe_done'] is None and state['last_probe_t'] is not None
                        and not state['payload_returned']):
                    stop_because('title ended under the ramp')
            mark = MARK.search(line)
            if mark:
                text = mark.group(1)
                state['marks'].append({'t': round(at, 1), 'line': text})
                state['last_probe_t'] = at
                print('  probe:', text[:170], flush=True)
                found = CENSUS.search(text)
                if found:
                    step, phase = int(found[1]), found[2]
                    figures = {'phase': phase, 'mappings': int(found[3]), 'resident_kib': int(found[4]),
                               'swap_reserved_kib': int(found[5]), 'others_kib': int(found[6]),
                               'others_min_kib': int(found[7]), 'processes': int(found[8]), 't': round(at, 1)}
                    if phase in ('baseline', 'mapped', 'held'):
                        state['label'] = step
                        state['steps'].setdefault(step, {})[phase] = figures
                    elif phase == 'released':
                        state['label'] = 'released'
                        state['steps']['released'] = figures
                    if phase == 'mapped':
                        state['census'][step] = figures['resident_kib']
                        before = state['census'].get(step - 1)
                        if step >= 2 and before is not None and figures['resident_kib'] - before < 0.95 * args.object_mib * 1024:
                            stop_because(f"invalid: mapping {step} not charged ({before} -> {figures['resident_kib']} KiB)", step)
                    if phase == 'baseline':
                        state['census'][0] = figures['resident_kib']
                        state['base_others_kib'] = figures['others_kib']
                        if base is not None:
                            # The limit is on SceShellCore's RSS, which sums fewer processes than the probe's census
                            # (+59 to +73 MiB measured on both consoles): the gap at the start, signed, converts every
                            # projection to its unit. Live blocks are tested against the limit directly below.
                            state['offset_mib'] = base['rss_mib'] - (figures['others_kib'] + figures['resident_kib']) / 1024
                            print(f"  offset: SceShellCore's RSS {base['rss_mib']} against the probe's "
                                  f"{(figures['others_kib'] + figures['resident_kib']) / 1024:.1f} MiB: {state['offset_mib']:+.1f}", flush=True)
                    if ceiling_mib is not None and phase in ('mapped', 'held') and (
                            figures['processes'] == 0 or figures['others_kib'] < 1024):
                        stop_because(f"invalid: the probe's census of other processes is empty at step {step}", step)
                    if ceiling_mib is not None and phase == 'mapped' and state['reason'] is None:
                        live_ceiling = min(ceiling_mib, state.get('limit_min', ceiling_mib - args.over_limit) + args.over_limit)
                        # Not only before a step: what the processes hold once it is mapped (others grow too).
                        actual = (figures['others_kib'] + figures['resident_kib']) / 1024 + state.get('offset_mib', 0.0)
                        state['actual'][step] = round(actual, 1)
                        if actual > live_ceiling + 15:
                            state['ceiling_step'] = step
                            print(f'  ceiling: step {step} mapped at {actual:.0f} MiB > {live_ceiling:.0f} + 15: stop', flush=True)
                            state['stop_pending'] = True
                    base_others = state.get('base_others_kib')
                    if base_others and phase in ('mapped', 'held'):
                        low = min(figures['others_kib'], figures['others_min_kib'] or figures['others_kib'])
                        if base_others - low >= args.others_stop * 1024:
                            stop_because(f"other processes lost {(base_others - low) / 1024:.1f} MiB at step {step}", step)
                    # The grant: the step before passed (its held line), and nothing has stopped the run.
                    if phase == 'held' and state['reason'] is None and step == state['grant'] and step < args.maps:
                        # The probe's own count of the other processes and itself, the figure SceShellCore's RSS sums.
                        projected = ((figures['others_kib'] + figures['resident_kib']) / 1024 + args.object_mib
                                     + state.get('offset_mib', 0.0))
                        state['projected'][step + 1] = round(projected, 1)
                        live_ceiling = (min(ceiling_mib, state.get('limit_min', ceiling_mib - args.over_limit) + args.over_limit)
                                        if ceiling_mib is not None else None)
                        if live_ceiling is not None and projected > live_ceiling:
                            state['ceiling_step'] = step
                            print(f'  ceiling: step {step + 1} would project {projected:.0f} MiB > {live_ceiling:.0f}: stop',
                                  flush=True)
                            state['stop_pending'] = True
                        else:
                            state['want_grant'] = step + 1
                if text.startswith('start '):
                    state['firmware_line'] = text
                    state['ramp_started'] = True
                if text.startswith(('release', 'DONE')):
                    state['label'] = 'released'
                if text.startswith('DONE'):
                    state['probe_done'] = text
                if text.startswith(('evicted', 'others', 'refused', 'starved', 'mismatch', 'FAIL')) or 'ended signal=' in text:
                    at_step = re.search(r'step=(\d+)', text)
                    stop_because('probe: ' + text[:140], int(at_step[1]) if at_step else None)
            stats = VM.search(line)
            if stats:
                if ceiling_mib is not None and state['ramp_started']:
                    state['limit_min'] = min(state.get('limit_min', float(stats[2])), float(stats[2]))
                    if float(stats[1]) > state['limit_min'] + args.over_limit + 5:
                        stop_because(f"console RSS {float(stats[1]):.0f} MiB passed limit {state['limit_min']:.0f} "
                                     f"+ {args.over_limit} + 5", state['label'])
                if state['block'] is not None:
                    state['vm'].append(state['block'])
                state['block'] = {'t': round(at, 1), 'step': state['label'], 'rss_mib': float(stats[1]),
                                  'rss_limit_mib': float(stats[2]), 'kernel_mib': float(stats[3]),
                                  'wire_mib': float(stats[4]), 'swap_out_mib': float(stats[5]),
                                  'page_table_cpu': signed(stats[6]), 'others_mib': 0.0, 'shellui_mib': None,
                                  'probe_mib': None, 'fmem_lines': 0}
                continue
            fmem = FMEM.search(line)
            if fmem and state['block'] is not None:
                block, resident, name = state['block'], float(fmem[1]), fmem[5]
                block['fmem_lines'] += 1
                if name == 'SceSpZeroConf':
                    block['probe_mib'] = resident
                else:
                    block['others_mib'] = round(block['others_mib'] + resident, 1)
                    if name.endswith('SceShellUI'):
                        block['shellui_mib'] = resident

    def rows():
        return state['vm'] + ([state['block']] if state['block'] else [])

    def write_stop():
        if state['stop_written'] is not None:
            return
        try:
            put(settings, STOP, 'run-shared-charge: stop\n')
            state['stop_written'] = round(time.time() - started, 1)
        except ftplib.all_errors as error:
            state['stop_errors'].append(mask(error)[:120])

    def vm_swap_rules(base):
        for row in rows():
            if row is state['block'] or row.get('judged') or row['t'] < started:
                continue
            row['judged'] = True
            if row['swap_out_mib'] - base['swap_out_mib'] >= args.swap_stop:
                stop_because(f"swap-out grew {row['swap_out_mib'] - base['swap_out_mib']:.1f} MiB at step {row['step']}",
                             row['step'])

    # Live klog first, then the title on its launcher screen: its start-up VM Stats block is the baseline.
    refused, base, payload, results = None, None, None, {}
    try:
        deadline = time.time() + 30
        while time.time() < deadline and not (connected and time.time() - connected[0] > 11):
            time.sleep(0.5)
            drain(replay_check=True)
        drain(replay_check=True)
        if state.get('replay_alarms'):
            refused = 'the klog replay holds alarm lines; not sent:\n' + '\n'.join(state['replay_alarms'][:5])
        else:
            put(settings, BASE + '/test-run.txt', f'frames {60 * title_seconds}\nsamples launcher\n')
            reply = ps5_console.ps5vkctl_command(settings, f'launch {TITLE}', timeout=120)
            console_guard.after_launch()
            print('title:', mask(reply)[:100], f'(its launcher for {title_seconds} s)', flush=True)
            if 'ok launched' not in reply:
                refused = 'the title did not launch: ' + mask(reply)[:120]
            deadline = time.time() + 60
            while refused is None and time.time() < deadline:
                time.sleep(1)
                drain()
                done = [r for r in rows() if r is not state['block'] and r['fmem_lines'] >= 20 and r['t'] >= started]
                if done:
                    base = done[-1]
                    break
            if refused is None and state['reason']:
                refused = state['reason'] + ' (before the send): not sent'
            elif refused is None and base is None:
                refused = 'no VM Stats block from the title start in 60 s: not sent'
            elif refused is None and args.console == 'standard' and base['rss_limit_mib'] != 0.0:
                refused = f"this console prints an RSS limit ({base['rss_limit_mib']}): not the standard PS5; not sent"
            elif refused is None and args.console == 'pro' and base['rss_limit_mib'] <= 0.0:
                refused = 'this console prints no RSS limit: not the Pro; not sent'
            elif (refused is None and args.console == 'pro' and
                  base['rss_limit_mib'] + args.over_limit < base['rss_mib'] + 2 * args.object_mib):
                refused = 'the ceiling is below the baseline plus two objects: nothing to measure; not sent'
    except KeyboardInterrupt:
        refused = 'interrupted before the send: not sent'
    if refused:
        stop_reader.set()
        try:
            remove(settings, GO)
        except ftplib.all_errors:
            pass
        raise SystemExit(refused + ('\n(the title draws its launcher until its frames end, then closes itself)'
                                    if base is not None or 'title' not in refused else ''))
    print(f"baseline: rss={base['rss_mib']} limit={base['rss_limit_mib']} kernel={base['kernel_mib']} "
          f"swap_out={base['swap_out_mib']} others={base['others_mib']} shellui={base['shellui_mib']}", flush=True)
    ceiling_mib = base['rss_limit_mib'] + args.over_limit if args.console == 'pro' else None
    if ceiling_mib is not None:
        print(f'ceiling: {ceiling_mib:.1f} MiB projected (limit {base["rss_limit_mib"]} {args.over_limit:+d})', flush=True)

    words = ['shared-charge', args.mode, str(args.object_mib), str(args.maps), str(args.hold), args.firmware]
    budget = (args.maps + 2) * (args.hold + 95) + 120
    payload = threading.Thread(target=send, args=(settings, elf, words, results, budget), daemon=True)
    started = time.time()
    payload.start()
    next_ping, silent = started + 10, 0
    try:
        while payload.is_alive():
            time.sleep(1)
            now = time.time()
            drain()
            vm_swap_rules(base)
            if state['stop_pending'] and state['stop_written'] is None:
                write_stop()   # a failed write is retried at the next second
            want = state.pop('want_grant', None)
            if want and state['reason'] is None:
                for _ in range(10):
                    try:
                        put(settings, GO, f'{want}\n')
                        state['grant'] = want
                        print(f'  grant: step {want}', flush=True)
                        break
                    except ftplib.all_errors as error:
                        state['grant_errors'].append(mask(error)[:120])
                        time.sleep(1)
            if state['last_probe_t'] and now - state['last_probe_t'] > args.hold + 120:
                stop_because(f'no probe line for {args.hold + 120} s')
            if not reader.is_alive():
                stop_because('the klog stream ended')
            if now >= next_ping:
                next_ping = now + 10
                try:
                    ps5_console.ps5vkctl_command(settings, 'ping', timeout=5)
                    silent = 0
                except (Exception, SystemExit):
                    silent += 1
                    if silent >= 3:
                        stop_because('the console stopped answering')
            if now - started > budget:
                stop_because('the payload did not return in time')
            if state['reason']:
                write_stop()
                if state['reason'] == 'the payload did not return in time':
                    break
        state['payload_returned'] = True
        # The title's exit prints the final VM Stats block: swap-out after the release.
        print('payload returned; waiting for the title to end its launcher run', flush=True)
        until = started + title_seconds + 120
        while time.time() < until and state['title_ended'] is None:
            time.sleep(1)
            drain()
        after = time.time() + 15
        while time.time() < after:
            time.sleep(1)
            drain()
    except KeyboardInterrupt:
        stop_because('runner interrupted')
    finally:
        if payload.is_alive() or state['probe_done'] is None or (state['reason'] and state['stop_written'] is None):
            stop_because(state['reason'] or ('runner ended with the payload alive' if payload.is_alive()
                                             else 'the payload request ended before DONE'))
            for _ in range(10):
                write_stop()
                if state['stop_written'] is not None:
                    break
                time.sleep(1)
        payload.join(30)
        if state['probe_done'] is None:
            finished = re.findall(r'^DONE .*$', results.get('output', ''), re.M)
            if finished:
                state['probe_done'] = finished[-1]
        if state['stop_written'] is not None and state['probe_done'] is None:
            waited = time.time() + 15
            while time.time() < waited and state['probe_done'] is None and not any(
                    m['line'].startswith('release') for m in state['marks']):
                time.sleep(0.5)
                drain()
        stop_reader.set()
        drain()
        for path in ((GO,) if payload.is_alive() or state['probe_done'] is None else (GO, STOP)):
            try:
                remove(settings, path)
            except ftplib.all_errors:
                pass
        (ROOT / 'klog').mkdir(exist_ok=True)
        text = mask('\n'.join(line for _, _, line in lines))
        (ROOT / 'klog' / (args.name + '.log')).write_text(text + '\n')
        damage = console_guard.storage_damage([line for _, _, line in lines])
        if damage:
            console_guard.record_storage(args.name, damage[:5])
        if state['reason'] == 'the console stopped answering':
            console_guard.record_drop_unless_answering(
                state['reason'], lambda: ps5_console.ps5vkctl_command(settings, 'ping', timeout=5))
        all_rows = rows()
        final = [r for r in all_rows if r['t'] > started and r['fmem_lines'] >= 20]
        done = state['probe_done'] or ''
        writes = re.search(r'sdk_writes=(\d+)', done)
        reason = state['reason'] or ''
        actions = ('swap-out grew', 'other processes lost', 'probe: evicted', 'probe: others', 'probe: refused',
                   'probe: starved', 'klog: ', 'title ended')
        record = {'run': args.name, 'mode': args.mode, 'object_mib': args.object_mib, 'maps': args.maps,
                  'hold_s': args.hold, 'swap_stop_mib': args.swap_stop, 'others_stop_mib': args.others_stop,
                  'firmware': args.firmware, 'console': args.console, 'over_limit_mib': args.over_limit,
                  'ceiling_mib': ceiling_mib, 'ceiling_step': state['ceiling_step'], 'projected_mib': state['projected'],
                  'actual_mib': state['actual'], 'offset_mib': state.get('offset_mib'),
                  'title': 'launcher screen, no Wine session', 'title_seconds': title_seconds,
                  'readiness': readiness, 'elf_sha256': hashlib.sha256(elf.read_bytes()).hexdigest(),
                  'probe_start': state['firmware_line'], 'baseline_vm': base, 'final_vm': final[-1] if final else None,
                  'swap_out_growth_mib': round(final[-1]['swap_out_mib'] - base['swap_out_mib'], 1) if final else None,
                  'vm_stats': all_rows, 'steps': {str(k): v for k, v in state['steps'].items()},
                  'probe': state['marks'], 'granted_to': state['grant'], 'grant_errors': state['grant_errors'],
                  'stop_reason': state['reason'], 'stop_written_s': state['stop_written'],
                  'stop_errors': state['stop_errors'], 'probe_done': done,
                  'sdk_writes': int(writes[1]) if writes else None, 'title_ended': state['title_ended'] is not None,
                  'storage_damage': len(damage), 'payload_output': mask(results.get('output', ''))[-8000:]}
        record['kernel_acted'] = reason if reason.startswith(actions) or 'ended signal=' in reason else None
        record['acted_at_step'] = state['acted_step'] if record['kernel_acted'] else None
        record['completed_ramp'] = ((state['grant'] == args.maps and 'why=nogrant' not in done)
                                    or (state['ceiling_step'] is not None and state['ceiling_step'] >= 2
                                        and state['grant'] == state['ceiling_step'] and state['stop_written'] is not None))
        record['passed'] = (bool(done) and record['sdk_writes'] == 0
                            and (record['kernel_acted'] is not None or (not reason and record['completed_ramp'])))
        evidence.write_text(json.dumps(record, indent=1) + '\n')
        print(json.dumps({k: record[k] for k in ('run', 'mode', 'granted_to', 'stop_reason', 'kernel_acted',
                                                  'acted_at_step', 'swap_out_growth_mib', 'passed')}))
        for step, figures in sorted(record['steps'].items(), key=lambda kv: (len(kv[0]), kv[0])):
            held = figures.get('held') or figures.get('mapped') or figures
            print(f"  step {step}: mappings={held.get('mappings')} own={held.get('resident_kib', 0) // 1024} MiB "
                  f"others={held.get('others_kib', 0) // 1024} MiB (min {held.get('others_min_kib', 0) // 1024}) "
                  f"swap_reserved={held.get('swap_reserved_kib', 0) // 1024} MiB")
        if final:
            print(f"  title exit: rss={final[-1]['rss_mib']} swap_out={final[-1]['swap_out_mib']} "
                  f"(baseline {base['swap_out_mib']})")
        print('evidence:', evidence)
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    sys.exit(main())
