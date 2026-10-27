#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Capture one isolated probe and restore the signed launcher; direct spawn stays blocked."""
import argparse
import hashlib
import io
import json
import re
import socket
import struct
import sys
import time
import uuid
from ftplib import FTP, error_perm, error_reply
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT.parent / 'PS5_Vulkan/tools'))
import ps5_console
import console_guard
from console_log import LineLog, redact

BASE = '/data/homebrew/PPSA99170/'
FAULT = re.compile(r'A user thread receives a fatal signal|mDBG: Sending signal|GPU_FAULT|gpu fault', re.I)
ACTIVE_EVIDENCE = None


def sha(data):
    return hashlib.sha256(data).hexdigest()


def executable_sha(data):
    # ftpsrv's reconstructed ELF has unstable bytes in unmapped version records
    # and the final unmapped note. Check the complete headers and every other
    # file-backed segment, including code, data, imports, dynamic data and notes.
    if data[:4] != b'\x7fELF' or data[4:6] != b'\x02\x01':
        raise RuntimeError('Expected a little-endian ELF64 readback')
    offset = struct.unpack_from('<Q', data, 32)[0]
    size, count = struct.unpack_from('<HH', data, 54)
    if size != 56 or offset + size * count > len(data):
        raise RuntimeError('Invalid ELF program headers')
    digest = hashlib.sha256(data[:offset + size * count])
    for index in range(count):
        kind, flags, start, address, physical, length, memory, alignment = struct.unpack_from(
            '<IIQQQQQQ', data, offset + index * size)
        if start + length > len(data):
            raise RuntimeError('ELF segment exceeds readback')
        if address == 0 and kind in (0x6fffff00, 0x6fffff01, 4):
            continue
        digest.update(data[start:start + length])
    return digest.hexdigest()


def helper_preload(data):
    """Read our helper's process-parameter relocation, before console access."""
    offset = struct.unpack_from('<Q', data, 32)[0]
    stride, count = struct.unpack_from('<HH', data, 54)
    if stride != 56:
        raise RuntimeError('Invalid helper program headers')
    segments = [struct.unpack_from('<IIQQQQQQ', data, offset + i * stride) for i in range(count)]

    def file_offset(address, length):
        for kind, _, start, base, _, size, _, _ in segments:
            if kind == 1 and base <= address and address + length <= base + size:
                return start + address - base
        raise RuntimeError('Helper metadata lies outside its load segments')

    process = next(p for p in segments if p[0] == 0x61000001)
    dynamic = next(p for p in segments if p[0] == 2)
    tags = dict(struct.unpack_from('<qQ', data, i)
                for i in range(dynamic[2], dynamic[2] + dynamic[5], 16))
    start = file_offset(tags[7], tags[8])
    relocations = [struct.unpack_from('<QQq', data, start + i) for i in range(0, tags[8], 24)]
    matches = [r for r in relocations if r[0] == process[3] + 0x50]
    if len(matches) != 1 or matches[0][1] != 8:
        raise RuntimeError('Helper lacks its unique relative preload pointer')
    return struct.unpack_from('<Q', data, file_offset(matches[0][2], 8))[0]


def main():
    global ACTIVE_EVIDENCE
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=Path)
    parser.add_argument('--name', required=True)
    parser.add_argument('--kind', choices=('direct', 'service-preflight', 'service-native', 'install-probe'), default='direct')
    parser.add_argument('--plan', type=Path, help='install-probe only: its steps, one a line (native/proton_install_probe.cpp)')
    parser.add_argument('--production-root', type=Path,
                        help='the checkout whose production build is installed (its dist/ and build/ps5/), restored after; '
                             'without it the installed eboot.bin is read back raw (SELF off) and exactly those bytes restored')
    parser.add_argument('--missing-image', action='store_true', help='service-native only: expect a missing image rejection')
    parser.add_argument('--gpu-bytes',type=int,default=16384,help='shared GPU probe bytes per client, up to 32 MiB')
    parser.add_argument('--gpu-window32',action='store_true',help='place shared allocations in RADV shader-address window')
    parser.add_argument('--memory-owner-touch',type=int,choices=(0,1),default=1,help='gpu-memory: the owner writes every shared page itself')
    parser.add_argument('--memory-direct-arm',type=int,choices=(0,1),default=1,help='gpu-memory: also fill device-local direct memory')
    parser.add_argument('--param-kernel',action='append',default=[],metavar='KEY=VALUE',
                        help='experiment: for this run only, the title\'s param.json gets "kernel": {KEY: VALUE} (e.g. flexibleMemorySize=1073741824); the original is put back and checked')
    parser.add_argument('--gpu-local',action='store_true',help='gpu-vulkan/draw/present: the broker offers device-local memory from its direct pool')
    parser.add_argument('--memory-alias-arm',type=int,choices=(0,1,2),default=0,help='gpu-memory: 1 only direct-memory aliases in both directions, 2 only what sharing costs')
    parser.add_argument('--mode', choices=('normal', 'repeat', 'parent-close','gpu-broker','gpu-broker-shm','gpu-vulkan','gpu-draw','gpu-present','gpu-memory','fd-budget','channel','payload-start','flex'), default='normal',
                        help='service-native lifecycle experiment')
    args = parser.parse_args()
    if args.gpu_bytes<16384 or args.gpu_bytes>33554432 or args.gpu_bytes%16384:
        parser.error('--gpu-bytes must be a multiple of 16 KiB, up to 32 MiB')
    if args.gpu_local and args.mode not in ('gpu-vulkan','gpu-draw','gpu-present'):parser.error('--gpu-local is for the Vulkan client modes')
    if (args.gpu_bytes!=16384 or args.gpu_window32) and args.mode!='gpu-broker-shm':
        parser.error('shared GPU settings require --mode gpu-broker-shm')
    if args.mode != 'normal' and (args.kind != 'service-native' or args.missing_image):
        parser.error('--mode requires service-native without --missing-image')
    if args.missing_image and args.kind != 'service-native':
        parser.error('--missing-image requires --kind service-native')
    if (args.kind == 'install-probe') != bool(args.plan):
        parser.error('--plan is for, and required by, --kind install-probe')
    plan = args.plan.read_text().strip() + '\n' if args.plan else ''
    if not re.fullmatch(r'[a-zA-Z0-9_-]+', args.name):
        parser.error('use a plain run name')
    if args.kind == 'direct':
        for prior in (ROOT / 'evidence').glob('child-native-*.json'):
            if json.loads(prior.read_text()).get('console_kernel_panic_reported_by_user'):
                raise RuntimeError('Direct title spawning caused a kernel panic. Investigate the native spawn lifecycle before running this probe again.')
    build = args.build.resolve()
    manifest = json.loads((build / 'manifest.json').read_text())
    if manifest.get('kind', 'direct') != args.kind:
        raise RuntimeError('Requested probe kind does not match its build manifest')
    probe_name = 'child-probe' if args.kind == 'direct' else args.kind
    executable = (build / 'source/dist/PPSA99170/eboot.bin').read_bytes()
    expected_elf = (build / 'build/eboot.elf').read_bytes()
    if sha(executable) != manifest['eboot_sha256']:
        raise RuntimeError('Probe executable does not match the isolated build manifest')
    helper_image = helper_elf = client_image = client_elf = None
    if args.kind == 'service-native':
        if args.mode in ('gpu-vulkan','gpu-draw','gpu-present'):
            if bool(manifest.get('gpu_draw_helper'))!=(args.mode in ('gpu-draw','gpu-present')):raise RuntimeError('Requested draw mode differs from client build')
            if bool(manifest.get('gpu_present_helper'))!=(args.mode=='gpu-present'):raise RuntimeError('Requested presentation mode differs from client build')
            if not manifest.get('vulkan_gpu_helper'):raise RuntimeError('Vulkan broker mode requires its isolated client build')
            client_image=(build/'gpu-client/gpu-client.prx').read_bytes()
            client_elf=(build/'gpu-client/client.elf').read_bytes()
            if sha(client_image)!=manifest['gpu_client_sha256']:raise RuntimeError('Client module differs from its manifest')
        helper_image = (build / 'helper/helper.bin').read_bytes()
        helper_elf = (build / 'helper/helper.elf').read_bytes()
        if args.mode=='gpu-memory' and not manifest.get('elevate_gpu_helper'):
            raise RuntimeError('The memory-ceiling probe uses the elevated GPU helper build (--elevate-gpu-helper)')
        needed = ['libkernel.sprx'] + (['libSceLibcInternal.sprx'] if manifest.get('elevate_gpu_helper') else [])
        if sha(helper_image) != manifest['helper_sha256'] or sorted(manifest['helper_needed']) != sorted(needed):
            raise RuntimeError('Headless helper does not match its build manifest')
        if helper_preload(helper_elf) != int(manifest['helper_preload_mask'], 16):
            raise RuntimeError('Headless helper preload metadata differs from its manifest')
    root = args.production_root or ROOT
    production = (root / 'dist/PPSA99170/eboot.bin').read_bytes() if args.production_root else None
    production_elf = (root / 'build/ps5/eboot.elf').read_bytes() if args.production_root else None
    if production and production[:4] != bytes.fromhex('4f153d1d'):
        raise RuntimeError('Local production backup is not a signed SELF')
    console_guard.before_launch()
    settings = ps5_console.load_settings()

    def idle():
        return ' count=0 ' in ps5_console.ps5vkctl_command(settings, 'procs', timeout=15) + ' '

    def connect():
        ftp = FTP()
        ftp.connect(settings['host'], settings['ftp_port'], timeout=30)
        ftp.login(settings['ftp_user'], settings['ftp_password'] or 'codex')
        return ftp

    def raw(path):
        """A file's own bytes: the FTP server's SELF mode off (with it on, a signed eboot reads back decrypted)."""
        with connect() as ftp:
            for _ in range(2):
                if 'disabled' in ftp.sendcmd('SELF').lower():
                    return download(ftp, path)
        raise RuntimeError("The FTP server's SELF mode did not turn off; no replacement performed")

    def download(ftp, path, optional=False):
        record['transfer_stage']='read '+path.rsplit('/',1)[-1];save()
        sink = io.BytesIO()
        try:
            ftp.retrbinary('RETR ' + path, sink.write)
        except error_perm as error:
            if not optional or not str(error).startswith('550'):
                raise
        return sink.getvalue()

    def remove(ftp, path):
        try:
            ftp.delete(path)
        except error_perm as error:
            if not str(error).startswith('550'):
                raise
        except error_reply as error:
            if not str(error).startswith('226'):
                raise

    if not idle():
        raise RuntimeError('Console is running an application; no deployment performed')
    if production is None:
        record = {}   # download() reports its stage here until the run's record exists
        save = lambda: None  # noqa: E731
        production = raw(BASE + 'eboot.bin')
        if production[:4] != bytes.fromhex('4f153d1d'):
            raise RuntimeError('The installed eboot.bin does not read back as a signed SELF; no replacement performed')
    evidence_path = ROOT / 'evidence' / (args.name + '.json')
    if evidence_path.exists():
        raise RuntimeError('Evidence already exists; use a new run name')
    run = ROOT / 'klog' / args.name
    run.mkdir(parents=True, exist_ok=False)
    (run / 'production-eboot.bin').write_bytes(production)
    identity = uuid.uuid4().hex
    record = {'title': 'PPSA99170', 'identity': identity, 'kind': args.kind, 'build': manifest,
              'capture': str(run.relative_to(ROOT)), 'expected_missing_image': args.missing_image, 'mode': args.mode, 'passed': False, 'status': 'incomplete',
                'production_sha256': sha(production), 'production_restored': False,
                'production_backup': 'local build' if args.production_root else 'the installed eboot.bin, read back raw',
              'root_reference_balance_verified': False, 'forced_title_close': False}
    ACTIVE_EVIDENCE = evidence_path

    def save():
        evidence_path.write_text(json.dumps(record, indent=2) + '\n')

    save()
    deployed = False
    helper_uploaded = client_uploaded = False
    param_deployed = False;original_param = b''
    try:
        with connect() as ftp:
            if args.missing_image and download(ftp, BASE + 'service-native-never-exists.bin', optional=True):
                raise RuntimeError('Synthetic missing-image pathname exists; no request submitted')
            if args.kind == 'service-native' and download(ftp, BASE + 'service-native-helper.bin', optional=True):
                raise RuntimeError('Helper pathname already exists; no replacement performed')
            if client_image and download(ftp,BASE+'gpu-client.prx',optional=True):
                raise RuntimeError('Client module pathname already exists; no replacement performed')
            if args.mode=='gpu-present':
                for client in range(2):
                    for cycle in range(2):
                        if download(ftp,BASE+f'gpu-present-{client}-{cycle}.ppm',optional=True):
                            raise RuntimeError('Presentation capture pathname already exists; no replacement performed')
            prior = download(ftp, BASE + 'eboot.bin')
            record['installed_readback_sha256'] = sha(prior)
            if production_elf:
                record['production_executable_sha256'] = executable_sha(production_elf)
                if executable_sha(prior) != executable_sha(production_elf):
                    raise RuntimeError('Installed executable differs from local production ELF; no replacement performed')
            for hook in ('test-run.txt', 'wine-run.txt', 'child-probe.txt', 'service-preflight.txt', 'service-native.txt', 'gpu-broker-settings.txt', 'gpu-memory-settings.txt',
                         'install-probe.txt', 'install-probe.phase2'):
                old = download(ftp, BASE + hook, optional=True)
                if old:
                    (run / ('prior-' + hook)).write_bytes(old)
                    remove(ftp, BASE + hook)
            (run / 'daemon-before.log').write_text(redact(download(
                ftp, '/data/ps5proton/logs/lapy-service.log', optional=True).decode(errors='replace')))
        with socket.create_connection((settings['host'], settings['klog_port']), timeout=10) as sock:
            sock.settimeout(0.5)
            replay = bytearray()
            deadline = time.monotonic() + 20
            while True:
                try:
                    chunk = sock.recv(65536)
                    if not chunk:
                        raise RuntimeError('Klog closed before launch')
                    replay.extend(chunk)
                except socket.timeout:
                    break
                if time.monotonic() > deadline:
                    raise RuntimeError('Klog replay never became quiet; no deployment performed')
            (run / 'replay.log').write_text(redact(replay.decode(errors='replace')))
            if not idle():
                raise RuntimeError('Console became busy; no deployment performed')
            with connect() as ftp:
                deployed = True # Restore even after an uncertain upload.
                ftp.storbinary('STOR ' + BASE + 'eboot.bin', io.BytesIO(executable))
                uploaded = download(ftp, BASE + 'eboot.bin')
                record['probe_readback_sha256'] = sha(uploaded)
                record['probe_executable_sha256'] = executable_sha(expected_elf)
                if executable_sha(uploaded) != executable_sha(expected_elf):
                    raise RuntimeError('Uploaded probe ELF does not match the isolated build')
                if args.kind == 'service-native':
                    helper_uploaded = True
                    record['helper_uploaded'] = True
                    save() # Recovery knows the synthetic pathname even if upload disconnects.
                    ftp.storbinary('STOR ' + BASE + 'service-native-helper.bin', io.BytesIO(helper_image))
                    helper = download(ftp, BASE + 'service-native-helper.bin')
                    record['helper_executable_verified'] = executable_sha(helper) == executable_sha(helper_elf)
                    if not record['helper_executable_verified']:
                        raise RuntimeError('Uploaded helper ELF does not match its isolated build')
                if client_image:
                    client_uploaded=True;record['gpu_client_uploaded']=True;save()
                    ftp.storbinary('STOR '+BASE+'gpu-client.prx',io.BytesIO(client_image))
                    record['gpu_client_executable_verified']=executable_sha(download(ftp,BASE+'gpu-client.prx'))==executable_sha(client_elf)
                    if not record['gpu_client_executable_verified']:raise RuntimeError('Client module readback differs from its build')
                if args.param_kernel:
                    # Only over the production param.json, byte for byte, so the original is known.
                    original_param=download(ftp,BASE+'sce_sys/param.json')
                    if original_param!=(ROOT/'dist/PPSA99170/sce_sys/param.json').read_bytes():
                        raise RuntimeError('Deployed param.json differs from the production build; not changed')
                    (run/'param-original.json').write_bytes(original_param)
                    param=json.loads(original_param)
                    for item in args.param_kernel:
                        key,value=item.split('=',1);param.setdefault('kernel',{})[key]=int(value,0)
                    changed=(json.dumps(param,indent=2)+'\n').encode()
                    param_deployed=True;record['param_kernel']=param['kernel'];save()
                    ftp.storbinary('STOR '+BASE+'sce_sys/param.json',io.BytesIO(changed))
                    if download(ftp,BASE+'sce_sys/param.json')!=changed:raise RuntimeError('Changed param.json readback differs')
                remove(ftp, BASE + probe_name + '-results.txt')
                if args.mode=='gpu-memory':
                    ftp.storbinary('STOR '+BASE+'gpu-memory-settings.txt',io.BytesIO(f'{args.memory_owner_touch} {args.memory_direct_arm} {args.memory_alias_arm}\n'.encode()))
                if args.mode=='gpu-broker-shm':
                    ftp.storbinary('STOR '+BASE+'gpu-broker-settings.txt',io.BytesIO(f'{args.gpu_bytes} {int(args.gpu_window32)}\n'.encode()))
                if args.gpu_local:
                    ftp.storbinary('STOR '+BASE+'gpu-broker-settings.txt',io.BytesIO(b'16384 0 1\n'))
                ftp.storbinary('STOR ' + BASE + probe_name + '.txt', io.BytesIO((identity + (' missing-image' if args.missing_image else (' ' + args.mode if args.mode != 'normal' else '')) + '\n' + plan).encode()))
                if plan:
                    record.update(plan=plan.splitlines(), host_clock_at_launch=int(time.time()))
            save() # Persist deployment and verification before any spawn can run.
            console_guard.after_launch()
            ps5_console.ps5vkctl_command(settings, 'launch PPSA99170', timeout=120)
            print(args.kind + ' probe launched; capturing the native result.', flush=True)
            started = time.monotonic()
            next_poll = started
            idle_since = None
            captured = []
            with (run / 'capture.log').open('w') as output:
                stream = LineLog(output, captured.append)
                while time.monotonic() - started < (600 if plan else 180 if args.mode=='gpu-memory' else 160 if args.mode in ('gpu-vulkan','gpu-draw','gpu-present') else 90 if args.mode=='flex' else 40):
                    try:
                        chunk = sock.recv(65536)
                        if not chunk:
                            console_guard.record_drop('Klog disconnected during native child probe')
                            raise RuntimeError('Klog disconnected; no further launches')
                        stream.write(chunk)
                    except socket.timeout:
                        pass
                    now = time.monotonic()
                    if now >= next_poll:
                        next_poll = now + 1
                        if idle():
                            idle_since = idle_since or now
                        else:
                            idle_since = None
                    # An install probe replaces itself once (its loadexec step): the shell may be empty between.
                    if idle_since and now - idle_since >= (10 if plan else 3):
                        break
                stream.finish()
            record['shell_idle_after_run'] = bool(idle_since)
            record['post_launch_fault_records'] = sum(bool(FAULT.search(line)) for line in captured)
            record['kernel_fmem']=sorted({m for line in captured for m in re.findall(r'FMEM size: (0x[0-9a-f]+)',line)})
            record['kernel_dmem']=sorted({m for line in captured for m in re.findall(r'DMEM size: (0x[0-9a-f]+)',line)})
            record['parent_shell_exit_seen'] = any(re.search(
                r'\[MihawkOS\] exit status \d+: asking the shell to close the title', line) for line in captured)
            if not idle_since:
                raise RuntimeError('Probe did not return to the shell; no forced close performed')
            with connect() as ftp:
                observations = redact(download(ftp, BASE + probe_name + '-results.txt').decode(errors='replace'))
                (run / 'daemon-after.log').write_text(redact(download(
                    ftp, '/data/ps5proton/logs/lapy-service.log', optional=True).decode(errors='replace')))
            (run / 'results.txt').write_text(observations)
            record['observations'] = observations.splitlines()
            record['fresh_identity_seen'] = (probe_name + ' identity=' + identity + ' ') in observations
            if plan:
                # The console's clock against each server's Date header (the install probe's https lines).
                from email.utils import parsedate_to_datetime
                record['clock_skew_s'] = []
                for url, clock, date in re.findall(r'https url=(\S+) .*console_clock=(\d+) date_header=(\S+)', observations):
                    try:
                        record['clock_skew_s'].append({'url': url, 'console_minus_server': int(clock) - int(
                            parsedate_to_datetime(date.replace('_', ' ')).timestamp())})
                    except (TypeError, ValueError):
                        record['clock_skew_s'].append({'url': url, 'date_header': date})
            record['passed'] = (record['fresh_identity_seen'] and 'result=PASS\n' in observations and
                                record['parent_shell_exit_seen'] and not record['post_launch_fault_records'] and
                                (not args.missing_image or 'expected_missing_image=1\n' in observations))
            if args.mode == 'parent-close':
                child = re.search(r'parent_close live_service_id=(\d+) live_native_pid=(\d+) list_count=(\d+)', observations)
                pid_hex = hex(int(child[2])) if child else None
                record['live_child_native_pid'] = int(child[2]) if child else None
                record['child_exit_seen_after_parent_close'] = bool(pid_hex and any(
                    'NOTE_EXIT received : pid=' + pid_hex in line for line in captured))
                record['child_deleted_after_parent_close'] = bool(pid_hex and any(
                    'process delete event : pid=' + pid_hex in line for line in captured))
                record['passed'] = record['passed'] and record['child_exit_seen_after_parent_close'] and record['child_deleted_after_parent_close']
            if args.mode == 'repeat':
                record['completed_cycles'] = observations.count('service_close status=0\n')
                record['passed'] = record['passed'] and record['completed_cycles'] == 4
            if args.mode in ('gpu-broker','gpu-broker-shm'):
                record['gpu_shared_bytes']=args.gpu_bytes
                record['gpu_window32']=args.gpu_window32
                configured=re.search(r'gpu-broker shared_bytes=(\d+) window32=(\d+) last_page_checks=2',observations)
                record['gpu_configuration_verified']=bool(configured and int(configured[1])==args.gpu_bytes and bool(int(configured[2]))==args.gpu_window32)
                record['shared_gpu_clients']=[{'client':int(a),'service_id':int(b),'pid':int(c),'cpu_alias':int(d),'map_status':int(e,16)}
                    for a,b,c,d,e in re.findall(r'gpu-broker client=(\d+) service=(\d+) pid=(\d+) cpu_alias=(\d+) map_status=(0x[0-9a-f]+|0)',observations)]
                record['gpu_completion_checks']=[{'client':int(a),'pid':int(b),'value':int(c),'invalid':int(d),'packet_valid':int(e),'submit':int(f),'gpu_value':int(g),'child_observed':int(h),'verdict':int(i)}
                    for a,b,c,d,e,f,g,h,i in re.findall(r'gpu-broker client=(\d+) pid=(\d+) value=(\d+) invalid=(\d+) packet_valid=(\d+) submit=(-?\d+) gpu_value=(\d+) child_observed=(\d+) verdict=(\d+)',observations)]
                record['gpu_mapping_credentials']=[{'client':int(a),'elevation':int(b),'euid':int(c),'map_stage':int(d)}
                    for a,b,c,d in re.findall(r'gpu-broker client=(\d+).*map_status=(?:0x[0-9a-f]+|0) elevation=(-?\d+) euid=(\d+) map_stage=(\d+)',observations)]
                checks=record['gpu_completion_checks']
                record['passed'] &= len(record['shared_gpu_clients'])==2 and len(checks)==7 and all(q['verdict']==1 for q in checks) and \
                    'gpu-broker live_clients=2 separate_native_processes=1' in observations and observations.count('closed=0 absent=1')==2
                record['passed'] &= record['gpu_configuration_verified']
            if args.mode in ('gpu-vulkan','gpu-draw','gpu-present'):
                record['vulkan_child_results']=[{'client':int(a),'pid':int(b),'images':int(c),'hash':d,'live_bytes':int(e),'verdict':int(f)}
                    for a,b,c,d,e,f in re.findall(r'gpu-vulkan client=(\d+) pid=(\d+) images=(\d+) hash=(0x[0-9a-f]+) live_bytes=(\d+) verdict=(\d+)',observations)]
                joined=observations
                record['vulkan_pixel_checks']=[{'client':int(a),'cycle':int(b),'pixels':int(c),'draw':int(d or 0),'verdict':int(e)}
                    for a,b,c,d,e in re.findall(r'gpu-vulkan client=(\d+) cycle=(\d+) pixels=(\d+) hash=0x[0-9a-f]+ device_created=1(?: draw=(\d+))? verdict=(\d+)',joined)]
                record['vulkan_device_creations']=len(re.findall(r'gpu-vulkan client=\d+ cycle=\d+ pixels=4096 hash=0x[0-9a-f]+ device_created=1(?: draw=[01])?',joined))
                record['vulkan_coherence']=[{'client':int(a),'cycle':int(b),'kind':int(c),'words':int(d),'verdict':int(e)} for a,b,c,d,e in
                    re.findall(r'gpu-vulkan client=(\d+) coherence slot=\d+ cycle=(\d+) kind=(\d+) words=(\d+) hash=0x[0-9a-f]+ verdict=(\d)',observations)]
                record['passed'] &= len(record['vulkan_coherence'])==8 and all(c['verdict']==1 for c in record['vulkan_coherence'])
                record['vulkan_timings']=[{'client':int(a),'cycle':int(b),'phase':('serial','pipelined','submit_call','fence_wait','sleep_1ms','noop_round_trip','cpus','submit_round_trip')[int(c)],'count':int(d),'ns':int(e),'per_item_us':int(f)} for a,b,c,d,e,f in
                    re.findall(r'gpu-vulkan client=(\d+) timing cycle=(\d+) phase=(\d) submissions=(\d+) ns=(\d+) per_submission_us=(\d+)',observations)]
                record['passed'] &= len(record['vulkan_timings'])==28
                record['vulkan_cpus']=[{'client':int(a),'cycle':int(b),'allowed':c,'seen':d} for a,b,c,d in
                    re.findall(r'gpu-vulkan client=(\d+) cpus cycle=(\d+) allowed=(0x[0-9a-f]+) seen=(0x[0-9a-f]+)',observations)]
                record['vulkan_submit_time']=[dict(zip(('client','submissions','waits','stage_us','submit_us','wait_us','reply_us','recv_ticks','recv_samples','poll_missed'),map(int,m))) for m in
                    re.findall(r'gpu-vulkan client=(\d+) submit_time submissions=(\d+) waits=(\d+) stage_us=(\d+) submit_us=(\d+) wait_us=(\d+) release_us=(\d+) recv_ticks=(\d+) recv_samples=(\d+) poll_missed=(\d+)',observations)]
                tsc=re.search(r'gpu-vulkan tsc ticks=(\d+) ns=(\d+)',observations)
                record['tsc_ticks_per_us']=int(tsc[1])*1000/int(tsc[2]) if tsc else None
                record['vulkan_rpc_split']=[{'client':int(a),'cycle':int(b),'part':c,'submissions':int(d),'ns':int(e),'per_submission_us':int(f)} for a,b,c,d,e,f in
                    re.findall(r'gpu-vulkan client=(\d+) rpc_split cycle=(\d+) part=(\w+) submissions=(\d+) ns=(\d+) per_submission_us=(\d+)',observations)]
                record['vulkan_reply_transit']=[{'client':int(a),'cycle':int(b),'submissions':int(c),'ns':int(d),'per_submission_us':int(e)} for a,b,c,d,e in
                    re.findall(r'gpu-vulkan client=(\d+) reply_transit cycle=(\d+) submissions=(\d+) ns=(\d+) per_submission_us=(\d+)',observations)]
                record['gpu_local']=args.gpu_local
                record['vulkan_local_offers']=[int(v) for v in re.findall(r'gpu-vulkan client=\d+ pid=\d+ initialized=1 local_offer=(\d+)',observations)]
                record['vulkan_local_peaks']=[int(v) for v in re.findall(r'gpu-vulkan client=\d+ pid=\d+ images=\d+ .* local_peak=(\d+)',observations)]
                if args.gpu_local:
                    record['passed'] &= len(record['vulkan_local_offers'])==2 and all(v>0 for v in record['vulkan_local_offers']) and \
                        len(record['vulkan_local_peaks'])==2 and all(v>0 for v in record['vulkan_local_peaks'])
                results=record['vulkan_child_results'];checks=record['vulkan_pixel_checks']
                record['passed'] &= len(results)==2 and len({r['pid'] for r in results})==2 and all(r['images']==2 and r['verdict']==1 for r in results) and \
                    len(checks)==4 and all(r['pixels']==4096 and r['verdict']==1 and r['draw']==int(args.mode in ('gpu-draw','gpu-present')) for r in checks) and record['vulkan_device_creations']==4 and observations.count('absent=1')==2
            if args.mode=='gpu-memory':
                def number(text): return int(text, 0)
                record['memory_baseline']=[dict(zip(('flexible','direct','pool','descriptors'),map(number,m)))
                    for m in re.findall(r'gpu-memory baseline owner=\d+ flexible=(\d+) direct=(\d+) pool=(\d+) descriptors=(\d+)',observations)]
                record['memory_settings']={'owner_touch':args.memory_owner_touch,'direct_arm':args.memory_direct_arm,'alias_arm':args.memory_alias_arm}
                record['memory_alias_steps']=[{'direction':a,'variant':b,'result':c,'mapped':int(d)} for a,b,c,d in
                    re.findall(r'gpu-memory alias (helper_maps_owner|owner_maps_helper) variant=(\S+) .*?result=(0x[0-9a-f]+) mapped=(\d)',observations)]
                record['memory_alias_reads']=re.findall(r'gpu-memory alias (?:helper_reads_owner marks|owner_maps_helper variant=\S+ .* marks)=(\d+/\d+)',observations)
                record['memory_alias_helper_dmem']=re.findall(r'gpu-memory alias helper_dmem=(\d) stage=(\d+) result=(0x[0-9a-f]+) physical=(\S+) direct_drop=(-?\d+)',observations)
                record['memory_alias_gpu_checks']=[int(v) for v in re.findall(r'gpu-memory alias gpu .* verdict=(\d)',observations)]
                alias=re.search(r'gpu-memory alias summary steps=(\d+) mapped=(\d+) gpu_checks=(\d+)/(\d+) complete=(\d)',observations)
                record['memory_alias_summary']=dict(zip(('steps','mapped','gpu_passed','gpu_checks','complete'),map(int,alias.groups()))) if alias else None
                record['memory_charge']=[dict(zip(('variant','sized','mapped','mprotect','helper','helper_mib','gpu','f0','after_object','after_owner_map','after_protect','after_helper_map','after_helper_release','after_owner_release'),
                    (m[0],)+tuple(int(x,0) for x in m[1:]))) for m in re.findall(r'gpu-memory charge variant=(\S+) sized=(\d) mapped=(\d) mprotect=(0x[0-9a-f]+|0) helper=(\d) helper_mib=(\d+) gpu=(-?\d) f0=(\d+) after_object=(\d+) after_owner_map=(\d+) after_protect=(\d+) after_helper_map=(\d+) after_helper_release=(\d+) after_owner_release=(\d+)',observations)]
                charge=re.search(r'gpu-memory charge summary variants=(\d+) gpu_checks=(\d+)/(\d+)',observations)
                record['memory_charge_summary']=dict(zip(('variants','gpu_passed','gpu_checks'),map(int,charge.groups()))) if charge else None
                record['memory_shared_chunks']=[dict(zip(('chunk','address','owner_flexible_before','owner_flexible_touched','owner_flexible_after','helper_map','helper_stage','helper_flexible_mib','total'),map(number,m)))
                    for m in re.findall(r'gpu-memory shared chunk=(\d+) address=(0x[0-9a-f]+) bytes=\d+ owner_flexible_before=(\d+) owner_flexible_touched=(\d+) owner_flexible_after=(\d+) helper_map=(\d+) helper_stage=(\d+) helper_flexible_mib=(\d+) total=(\d+)',observations)]
                record['memory_direct_chunks']=[dict(zip(('chunk','address','bytes','helper_reserve','helper_stage','direct_available','owner_flexible','helper_flexible_mib','total'),map(number,m)))
                    for m in re.findall(r'gpu-memory direct chunk=(\d+) address=(0x[0-9a-f]+) bytes=(\d+) helper_reserve=(\d+) helper_stage=(\d+) direct_available=(\d+) owner_flexible=(\d+) helper_flexible_mib=(\d+) total=(\d+)',observations)]
                record['memory_stops']=re.findall(r'gpu-memory (shared|direct) stop=(\S+) chunks=(\d+) total=(\d+)',observations)
                record['memory_gpu_checks']=[{'arm':a,'chunk':int(b),'verdict':int(c)} for a,b,c in re.findall(r'gpu-memory (shared|direct) gpu chunk=(\d+) .*verdict=(\d+)',observations)]
                summary=re.search(r'gpu-memory summary shared_bytes=(\d+) shared_gpu_checks=(\d+)/(\d+) direct_bytes=(\d+) direct_gpu_checks=(\d+)/(\d+) .* restored=(\d)',observations)
                record['memory_summary']=dict(zip(('shared_bytes','shared_passed','shared_checks','direct_bytes','direct_passed','direct_checks','restored'),map(int,summary.groups()))) if summary else None
                record['passed'] &= bool(summary) and record['memory_summary']['restored']==1 and \
                    (record['memory_summary']['shared_checks']>0 or args.memory_alias_arm) and record['memory_summary']['shared_passed']==record['memory_summary']['shared_checks'] and \
                    (args.memory_alias_arm!=1 or (record['memory_alias_summary'] or {}).get('complete')==1) and \
                    (args.memory_alias_arm!=2 or (record['memory_charge_summary'] or {}).get('variants')==5) and \
                    (record['memory_summary']['direct_checks']>0 or not args.memory_direct_arm or args.memory_alias_arm) and record['memory_summary']['direct_passed']==record['memory_summary']['direct_checks'] and \
                    'gpu-memory helper closed=0 absent=1' in observations
            if args.mode=='flex':
                # docs/FLEXIBLE_MEMORY.md: which pool a Sony local process draws on, and whether it runs code from direct memory.
                fields=lambda line:{k:(int(v,0) if re.fullmatch(r'-?(0x[0-9a-f]+|\d+)',v) else v) for k,v in re.findall(r'(\w+)=(\S+)',line)}
                record['flex']={'title':[fields(l) for l in observations.splitlines() if l.startswith('flex title ')],
                                'helper':[fields(l) for l in observations.splitlines() if l.startswith('flex helper ')],
                                'points':[fields(l) for l in observations.splitlines() if l.startswith('flex point=')],
                                'exec':[fields(l) for l in observations.splitlines() if l.startswith('flex exec ')],
                                'close':[fields(l) for l in observations.splitlines() if l.startswith('flex close ')]}
                record['vm_stats']=[l.split('] ',1)[-1] for l in captured if 'VM Stats:' in l]
                record['fmem_lines']=[l.split('] ',1)[-1] for l in captured if '] FMEM ' in l]
            if args.mode=='fd-budget':
                record['budget_alone']=[dict(zip(('owner','errno','owner_files'),map(int,m))) for m in
                    re.findall(r'fd-budget alone owner=(\d+) errno=(\d+) owner_files=(\d+)',observations)]
                record['budget_steps']=[dict(step=m[0],**dict(zip(('owner','owner_errno','owner_files','helper','helper_errno','helper_files'),map(int,m[1:]))))
                    for m in re.findall(r'fd-budget step=(\S+) owner=(\d+) owner_errno=(\d+) owner_files=(\d+) helper=(\d+) helper_errno=(-?\d+) helper_files=(-?\d+)',observations)]
                summary=re.search(r'fd-budget summary alone=(\d+) after=(\d+) steps=(\d+) measured=(\d+) absent=(\d)',observations)
                record['budget_summary']=dict(zip(('alone','after','steps','measured','absent'),map(int,summary.groups()))) if summary else None
            if args.mode=='gpu-present':
                record['present_us']=[{'present_us':int(a),'copy_us':int(b)} for a,b in re.findall(r'gpu-present client=\d+ present_us=(\d+) copy_us=(\d+)',observations)]
                record['presented_images']=[{'client':int(a),'cycle':int(b),'flip':int(c),'verdict':int(d)}
                    for a,b,c,d in re.findall(r'gpu-present client=(\d+) cycle=(\d+) hash=0x[0-9a-f]+ flip=(\d+) verdict=(\d+)',observations)]
                captures=[]
                with connect() as ftp:
                    for client in range(2):
                        for cycle in range(2):
                            raw=download(ftp,BASE+f'gpu-present-{client}-{cycle}.ppm')
                            header=b'P6\n1920 1080\n255\n'
                            rgb=bytes((255 if client==0 else 0,255 if client==1 else 0,255 if cycle==1 else 0))
                            correct=raw.startswith(header) and raw[len(header):]==rgb*(1920*1080)
                            path=run/f'gpu-present-{client}-{cycle}.ppm';path.write_bytes(raw)
                            captures.append({'client':client,'cycle':cycle,'scanout_pixels':1920*1080,'correct':correct,
                                'capture':str(path.relative_to(ROOT)),'sha256':sha(raw)})
                record['display_captures']=captures
                record['passed'] &= len(record['presented_images'])==4 and all(x['verdict']==1 for x in record['presented_images']) and \
                    {x['flip'] for x in record['presented_images']}=={1,2,3,4} and all(x['correct'] for x in captures) and \
                    'gpu-display closed=1 flips=4 blank=0 unregister=0 close=0 release=0' in observations
            record['status'] = 'complete'
    finally:
        if deployed:
            try:
                if idle():
                    with connect() as ftp:
                        if param_deployed:
                            ftp.storbinary('STOR '+BASE+'sce_sys/param.json',io.BytesIO(original_param))
                            record['param_restored']=download(ftp,BASE+'sce_sys/param.json')==original_param
                        ftp.storbinary('STOR ' + BASE + 'eboot.bin', io.BytesIO(production))
                        restored = download(ftp, BASE + 'eboot.bin')
                        record['restored_readback_sha256'] = sha(restored)
                        record['production_restored'] = (executable_sha(restored) == executable_sha(production_elf) if production_elf
                                                         else raw(BASE + 'eboot.bin') == production)
                        remove(ftp, BASE + probe_name + '.txt')
                        remove(ftp, BASE + 'install-probe.phase2')
                        remove(ftp, BASE + 'gpu-broker-settings.txt')
                        remove(ftp, BASE + 'gpu-memory-settings.txt')
                        if args.mode=='gpu-present':
                            for client in range(2):
                                for cycle in range(2):
                                    remove(ftp,BASE+f'gpu-present-{client}-{cycle}.ppm')
                        if helper_uploaded:
                            remove(ftp, BASE + 'service-native-helper.bin')
                            record['helper_removed'] = True
                        if client_uploaded:
                            remove(ftp,BASE+'gpu-client.prx');record['gpu_client_removed']=True
                    if not record['production_restored'] or (param_deployed and not record.get('param_restored')):
                        record['passed'] = False
                else:
                    record.update(passed=False, restoration_error='Console is still busy; signed backup kept in capture folder')
            except BaseException as error:
                record.update(passed=False, restoration_error=redact(error),
                              restoration_pending=True)
            state = console_guard.load()
            state['last_launch'] = time.time()
            console_guard.save(state)
        save()
    print(json.dumps(record, indent=2), flush=True)
    return 0 if record['passed'] else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BaseException as error:
        if isinstance(error, SystemExit) and isinstance(error.code, int):
            raise
        message = redact(error)
        if ACTIVE_EVIDENCE is not None:
            record = json.loads(ACTIVE_EVIDENCE.read_text())
            record.update(passed=False, error=message)
            ACTIVE_EVIDENCE.write_text(json.dumps(record, indent=2) + '\n')
        print('Child probe failed: ' + message, file=sys.stderr)
        sys.exit(1)
