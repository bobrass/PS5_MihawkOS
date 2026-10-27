#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Qualify the console-only failures of a paired Wine test run (tools/run-wine-tests.py) by their causes.

  tools/qualify-wine-tests.py DLL --arch ARCH [--write]

Each rule below is one cause in the platform layer, with the measurement that established it. A failure is qualified
only when its test file, line and text match a rule; whatever no rule covers stays a failure of the comparison, and so
does a rule whose reason starts with "OPEN:" (a defect to fix, not a limitation). A reason starting "AWAITING OWNER"
rests on a choice of ours (a patch, a service turned off, a feature not built): qualified, and counted apart
by tools/conformance-table.py until the owner accepts it. --write puts the qualified lines in
evidence/wine-tests/<arch>/<dll>-expected.json with the console run's context, as compare requires; without it the
qualification is only printed. Run compare again afterwards.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'evidence/wine-tests'

HOST_PAGE = ('OPEN: deferred bug (owner, 2026-10-25), to fix after the sweep: 16 KiB host pages: a 4 KiB page cannot be protected, unmapped or tracked alone inside a host page '
             '(wine/patches/0510; patches 1122 and 1134 track 4 KiB writes in every view but images; groups and options in docs/HOST_PAGES.md); fix: every 4 KiB protection, unmap and placeholder tracked in software')
WRITECOPY = ('AWAITING OWNER (rests on our own choice): patch 1004: Proton\'s simulate_writecopy is on for every process, so a copy-on-write page reads back as '
             'PAGE_READWRITE once protected, as Windows reports a page already copied (CEF renderers ended without it)')
NO_PTRACE = ('no debugger on the console: a process cannot ptrace, so hardware breakpoints never fire and DEBUG_PROCESS '
             'children cannot be created (ERROR_NOT_SUPPORTED)')
NO_XATTR = ('OPEN: deferred bug (owner, 2026-10-25), to fix after the sweep: the console\'s file system has no extended attributes, where Wine keeps DOS attributes (hidden, system), '
            'reparse points and set access times; security descriptors have their own store (patch 1108); fix: DOS attributes in a store of the prefix, as patch 1108 does for descriptors')
NO_LINK = 'the console refuses link() to a title (wine/ps5/pw_wine_cwd_libc.c __wrap_link: EPERM), so no hard links'
NO_NOTIFY = ('OPEN: deferred bug (owner, 2026-10-25): a change below a subtree watch\'s top directory is found only by polling (patch 0902), late and '
             'merged with others; the top directory is watched by kqueue (patch 2091); fix: a kqueue watch per subdirectory, within the descriptor budget')
SELECTORS = ('segment selectors: Wine reports its Linux constants for %fs and %gs (0 and the data selector); the '
             'console\'s FreeBSD kernel gives user code fs 0x13 and gs 0x1b. Only a program comparing the numbers sees it')
IOPL = ('the console runs a title with I/O privilege level 3: cli and sti do not fault in user mode, so no '
        'EXCEPTION_PRIV_INSTRUCTION is raised for them (exception.c\'s direction-flag test uses cli)')
WOW64_TEB = ('AWAITING OWNER (rests on our own choice): patch 0947 (docs/IA32_DESIGN.md, M4 step 1): a WoW64 thread\'s 32-bit TEB sits at the low half of its '
             'thread pointer, not 0x2000 above the 64-bit TEB, so 32-bit code needs no FS switch (system calls 2x cheaper)')
WOW64_BASE = ('AWAITING OWNER (rests on our own choice): a 32-bit image with a fixed base of 0x400000 is moved in a helper, where Sony\'s carrier eboot sits '
              '(memory note helper-0x400000-is-carrier-eboot); the image information keeps the preferred entry')
X87_STACK = ('the console\'s FreeBSD kernel reports an x87 stack fault as FPE_FLTSUB, which Wine\'s FreeBSD signal path '
             'raises as EXCEPTION_ARRAY_BOUNDS_EXCEEDED, not EXCEPTION_FLT_STACK_CHECK')
PROBE_PREFIX = ('AWAITING OWNER (rests on our own choice): the probe prefix has 32-bit drivers in syswow64\\drivers (tools/stage-wine.py); a Wine-made prefix has '
                'none, so the WoW64 redirection test finds ndis.sys there')
PLATFORM_THREADS = ('AWAITING OWNER (rests on our own choice): every console process runs the platform layer\'s own Windows threads (and their heaps and FLS '
                    'slots) beside the program\'s, so lists of threads, FLS slots and heaps hold more than the test made')
NO_CONHOST = ('no console host window on the console: a console-subsystem child gets no new console of its own')
FILE_TIMES = ('observed: a file\'s last write time read back unchanged after a write right after another; the console\'s '
              'file system keeps coarser times than the test needs (not measured further)')
IMAGE_TAIL = HOST_PAGE + ' (the rest of an image\'s last host page stays reserved after the image)'
WRITE_WATCH = ('AWAITING OWNER (rests on our own choice): a first write to a write-watched 4 KiB page clears the watch on its whole 16 KiB '
               'host page, so GetWriteWatch reports a superset (four pages for one store), which a garbage collector, the user of write '
               'watches, only scans more; the exact count (patch 2089) hung and was withdrawn (docs/HOST_PAGES.md, group B, option B2)')
DEP_32 = ('AWAITING OWNER (rests on our own choice): a host page holding an executable and a non-executable 4 KiB page is mapped '
          'executable, so 32-bit code beside data runs at full speed and DEP faults only at host-page granularity; faulting per 4 KiB '
          'would need every instruction on such a page emulated (docs/HOST_PAGES.md, group E, option E2)')
EVENTLOG = ('AWAITING OWNER (rests on our own choice): deliberate (docs/STEAM.md): the event log service and plug-and-play are off on the console, each Wine system '
            'process costing about 15 MiB of the 990 MiB budget; the EventLog service is disabled and stopped')
NO_NTLM = ('AWAITING OWNER (rests on our own choice): no NTLM on the console: Wine\'s NTLM and Negotiate run Samba\'s ntlm_auth helper (the PC has /usr/bin/ntlm_auth); '
           'a program asking for them gets SEC_E_SECPKG_NOT_FOUND, RPC_S_UNKNOWN_AUTHN_SERVICE or a 401')
XINPUT_STUB = ('Wine\'s own xinput answers a connected pad\'s battery and headset GUIDs with ERROR_NOT_SUPPORTED (a stub '
               'upstream too); the console has a DualSense connected, the PC\'s oracle no pad, so only the console reaches '
               'these checks')
NO_CAPTURE = ('AWAITING OWNER (rests on our own choice): feature not built: the console\'s audio driver (wine/wineps5) has one render endpoint and no capture '
              'endpoint, so a full-duplex device cannot initialise; the DualSense microphone is not offered to Windows programs')
NO_PNP = ('AWAITING OWNER (rests on our own choice): deliberate (docs/STEAM.md): plug-and-play (plugplay.exe, 19 MiB) is off on the console, so the virtual HID devices '
          'dinput\'s tests install through Wine\'s test driver never appear and their waits time out')
GUARD = ('AWAITING OWNER (rests on our own choice): patch 1120: a new thread\'s guard page sits below its committed top, as on Windows (an emulator pre-growing '
         'its stack died without it); the pages under the guard stay committed, so the bottom-up layout differs')

# (dll, test file, line ranges or None for any, text regex or None, reason)
RULES = [
    ('ntdll', 'virtual', [(1060, 1240)], r'guard_size|RegionSize|Protect|State', GUARD),
    ('ntdll', 'virtual', [(2500, 2600)], None, HOST_PAGE),
    ('ntdll', 'virtual', [(440, 450)], None, WRITE_WATCH),
    ('ntdll', 'virtual', [(150, 170)], r'zero_bits', HOST_PAGE),
    ('ntdll', 'file', [(1515, 1525)], r'access time', NO_XATTR),
    ('ntdll', 'file', [(1550, 1650), (4700, 4725)], r'attribute', NO_XATTR),
    ('ntdll', 'file', [(6908, 6950)], None, NO_LINK),   # test_reparse_points' hard links (CreateHardLinkW)
    ('ntdll', 'file', [(6250, 7300)], None, NO_XATTR + ' (reparse points)'),
    ('ntdll', 'file', [(2480, 3300)], r'c0000022|should exist|FindFirstFileW', NO_LINK),
    ('ntdll', 'directory', [(170, 180)], None, NO_XATTR),
    ('ntdll', 'directory', [(1440, 1630)], None, NO_XATTR + ' (reparse points)'),
    ('ntdll', 'change', None, None, NO_NOTIFY),
    ('ntdll', 'info', [(2100, 2830), (4860, 4905)], None, NO_PTRACE),
    ('ntdll', 'exception', [(9230, 9250), (1010, 1025)], r'debug register|expected .*got', 'OPEN: patch 2083 (debug registers read back) not yet on the console build'),
    ('ntdll', 'exception', [(3125, 3135), (310, 320), (1170, 1240), (3326, 3337)], None, NO_PTRACE),
    ('ntdll', 'exception', [(1290, 1300), (9800, 9815)], r'child process', NO_PTRACE),
    ('ntdll', 'exception', [(3370, 3400), (2010, 2016)], r'wrong (fs|gs|SegGs)', SELECTORS),
    ('ntdll', 'exception', [(3350, 3356)], r'exceptions, expected 1', IOPL),
    ('ntdll', 'wow64', [(935, 945)], None, WOW64_TEB),
    ('ntdll', 'info', [(680, 692)], r'TebBase', WOW64_TEB),
    ('ntdll', 'exception', [(1800, 1860)], r'disabled memory|Access violation type', DEP_32 + ' (patch 2075: a no-access 4 KiB page leaves its host page executable)'),
    ('ntdll', 'exception', [(1715, 1725)], r'FLT_STACK_CHECK', X87_STACK),
    ('ntdll', 'exception', [(10585, 10595), (10740, 10746)], None, 'OPEN: the AVX state set into a suspended 32-bit thread (NtSetContextThread with XSTATE) is lost on resume'),
    ('ntdll', 'directory', [(1340, 1360)], r'ndis.sys', PROBE_PREFIX),
    ('ntdll', 'wow64', [(290, 296)], r'entry', WOW64_BASE),
    ('kernelbase', 'process', [(150, 190)], None, HOST_PAGE + ' (a placeholder split at 4 KiB)'),
    ('advapi32', 'service', [(2905, 2970)], None, EVENTLOG),
    ('secur32', 'ntlm', None, None, NO_NTLM),
    ('xinput1_3', 'xinput', [(235, 255), (275, 282)], None, XINPUT_STUB),
    ('dinput', None, None, r'WaitForSingleObject returned 0x102|Wait for', NO_PNP),
    ('dsound', 'duplex', [(280, 290)], None, NO_CAPTURE),
    ('dinput', 'hotplug', [(665, 680)], r'failed to (create|register) (device|set hardware ID)', NO_PNP),
    ('hid', 'device', [(370, 380)], None, 'OPEN: a helper reading the console\'s DualSense HID device gets no input report in 5 s'),
    ('secur32', 'negotiate', None, None, NO_NTLM),
    ('rpcrt4', 'rpc', [(265, 290), (930, 950)], r'1746|1747|identity|principal|AUTHN', NO_NTLM),
    ('rpcrt4', 'server', [(2130, 2140)], r'1747', NO_NTLM),
    ('winhttp', 'winhttp', [(3185, 3210)], None, NO_NTLM),
    ('kernel32', 'virtual', [(4200, 4320)], r'0x4|got 0x4', WRITECOPY),
    ('kernel32', 'virtual', [(1790, 2400)], r'wrong count|wrong result', WRITE_WATCH),
    ('kernel32', 'virtual', [(150, 260), (4530, 4540)], None, HOST_PAGE + ' (no-access 4 KiB pages inside an accessible host page)'),
    ('kernel32', 'loader', [(1815, 1825)], None, WRITECOPY),
    ('kernel32', 'loader', [(1530, 1540)], None, NO_LINK),
    ('kernel32', 'loader', [(935, 950)], None, IMAGE_TAIL),
    ('kernel32', 'loader', [(4145, 4155)], None, HOST_PAGE + ' (a read across a no-access 4 KiB page succeeds)'),
    ('kernel32', 'loader', [(3380, 3390)], r'unexpected count', PLATFORM_THREADS),
    ('kernel32', 'heap', [(220, 230)], r'wrong heap', PLATFORM_THREADS),
    ('kernel32', 'console', [(5385, 5405)], r'Unexpected result', NO_CONHOST),
    ('kernel32', 'thread', [(315, 320), (2195, 2212)], r'context|expected', 'OPEN: patch 2084 (a new WoW64 thread gets its 32-bit TEB address) not yet on the console build'),
    ('kernel32', 'file', [(4775, 4780)], r'last write time', FILE_TIMES),
    ('kernel32', 'file', [(6355, 6380)], None, NO_LINK),
    ('kernel32', 'file', [(6740, 6870)], None, NO_XATTR + ' (reparse points)'),
    ('kernel32', 'file', [(5870, 5885)], r'access time', NO_XATTR),
    ('kernel32', 'file', [(3525, 3600)], None, HOST_PAGE + ' (I/O into a no-access 4 KiB page of an accessible host page)'),
    ('kernel32', 'change', None, None, NO_NOTIFY),
]


# Files whose run on the PC's oracle cannot complete (the comparison has nothing to compare): accepted only when the
# console completes them with no failure line at all (tools/run-wine-tests.py compare checks that every time).
# Files that cannot finish on the console, for a cause that is the console's on purpose: accepted unfinished.
# A test named FILE@ARCH holds for that architecture only.
UNFINISHED_OK = {
    ('ntdll', 'exception@i386'): ('the console\'s kernel numbers its user code selectors its own way: 32-bit code is 0x33 and 64-bit '
                                  '0x43 (docs/IA32_DESIGN.md), where Windows has 0x23 and 0x33. test_context_exception_request, the '
                                  'file\'s last test, enters 64-bit code with "push $0x33; lret" (call_func64), which on the console '
                                  'stays in 32-bit mode and runs the 64-bit bytes as 32-bit code; their access violation reaches the '
                                  'test\'s own vectored handler, which waits for a step the main thread never takes (trace '
                                  'b13-exc32-053728: AV at code_mem with cs 0x33, handler entered, no return until the 300 s timeout). '
                                  'A selector the kernel accepts raises nothing to catch: a platform limit for any 32-bit program '
                                  'that enters 64-bit code by a hard-coded selector'),
    ('dinput', 'hid'): NO_PNP,
    ('dsound', 'duplex'): NO_CAPTURE,
    ('dsound', 'capture'): ('the same on the PC: with no capture endpoint Wine\'s dsound answers DirectSoundCaptureCreate with '
                            'E_INVALIDARG, not DSERR_NODRIVER (capture.c:767), and the test goes on with a NULL object and '
                            'crashes; upstream Wine\'s, on both sides alike. ' + NO_CAPTURE),
}

ORACLE = {
    ('kernel32', 'volume'): 'the PC\'s Wine crashes in the mount point test (glibc: munmap_chunk(): invalid pointer, after '
                            'volume.c:1838, in a fresh prefix too; its mount manager has the PC\'s dbus, the console\'s none)',
}


def reason_for(dll, test, file, line, text):
    for rule_dll, rule_test, ranges, pattern, reason in RULES:
        if rule_dll != dll or rule_test not in (None, test):
            continue
        if ranges and not any(a <= line <= b for a, b in ranges):
            continue
        if pattern and not re.search(pattern, text):
            continue
        return reason
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('dll')
    parser.add_argument('--arch', choices=['x86_64', 'i386'], required=True)
    parser.add_argument('--write', action='store_true')
    args = parser.parse_args()
    # The console-only lines with their own texts: the comparison without any qualification (a qualified line's
    # comparison text is its reason, which the rules do not match).
    import importlib.util
    spec = importlib.util.spec_from_file_location('wine_tests', ROOT / 'tools/run-wine-tests.py')
    runner = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(runner)
    host_name = f'{args.dll}-host-dxvk.json' if args.dll in runner.D3D_DLLS else f'{args.dll}-host.json'
    host = json.loads((OUT / args.arch / host_name).read_text()) if (OUT / args.arch / host_name).is_file() else {}
    console = json.loads((OUT / args.arch / f'{args.dll}-console.json').read_text())
    if args.dll in runner.D3D_DLLS:
        host, console = runner.merge_slices(host, args.dll), runner.merge_slices(console, args.dll)
    comparison = runner.compare_records(host, console, {})
    expected, open_lines, unqualified = {}, [], []
    for entry in comparison['new_failures']:
        test, rest = entry.split(': ', 1)
        location = rest.split(': ', 1)[0]
        file, line = location.rsplit(':', 1)
        text = rest.split(': ', 1)[1] if ': ' in rest else ''
        reason = reason_for(args.dll, test, file, int(line), text)
        if not reason:
            unqualified.append(entry)
        elif reason.startswith('OPEN:'):
            open_lines.append(f'{entry} ({reason})')
        else:
            expected.setdefault(test, {})[location] = reason
    # one test build; the production build may differ between files of an incremental session (compare ignores it)
    contexts = {json.dumps({k: w for k, w in (v.get('context') or {}).items() if k != 'console_build_fingerprint'}, sort_keys=True)
                for v in console.values() if v.get('status') != 'skipped'}
    if len(contexts) != 1:
        raise SystemExit('the console entries come from more than one test build: rerun them before qualifying')
    oracle = {test: reason for (dll, test), reason in ORACLE.items() if dll == args.dll}
    unfinished_ok = {test.split('@')[0]: reason for (dll, test), reason in UNFINISHED_OK.items()
                     if dll == args.dll and test.split('@')[1:] in ([], [args.arch])}
    record = {'context': json.loads(contexts.pop()), 'tests': expected, 'oracle': oracle, 'unfinished_ok': unfinished_ok,
              'rules': 'tools/qualify-wine-tests.py', 'open': sorted(set(open_lines))}
    lines = sum(len(v) for v in expected.values())
    print(f'{args.dll} {args.arch}: {lines} lines qualified, {len(open_lines)} OPEN, {len(unqualified)} without a rule')
    for entry in unqualified[:60]:
        print('  unqualified:', entry[:170])
    if args.write:
        (OUT / args.arch / f'{args.dll}-expected.json').write_text(json.dumps(record, indent=1, sort_keys=True) + '\n')
    return 0


if __name__ == '__main__':
    sys.exit(main())
