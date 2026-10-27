#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gate orchestration with fake case runners: completeness, failures and safe stop."""
import contextlib
import importlib.util
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('battery', Path(__file__).resolve().parents[1] / 'tools/run-battery.py')
battery = importlib.util.module_from_spec(spec)
spec.loader.exec_module(battery)


class BatteryTests(unittest.TestCase):
    def run_fake(self, *, fail=None, runner_fail=None, stop=False, extra=(), gate=True):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for folder in ('evidence', 'klog', '.deps'): (root / folder).mkdir()
            calls = []
            def run(command, **kwargs):
                name = command[command.index('--name') + 1]
                case = name.removeprefix('host-test-')
                calls.append(case)
                if '--sequence' in command:
                    fake_sequence(root, name, Path(command[command.index('--sequence') + 1]), fail)
                okay = case != fail
                record = {'passed': okay, 'result': 'finished', 'returned_to_launcher': True,
                          'seconds': 1, 'launch_readiness': {'ready': True, 'seconds': 0.2}}
                (root / 'evidence' / (name + '.json')).write_text(json.dumps(record))
                if stop: (root / '.deps/host-test.stop').touch()
                return SimpleNamespace(returncode=0 if okay and case != runner_fail else 1)
            output = io.StringIO()
            with patch.object(battery, 'ROOT', root), \
                 patch.object(sys, 'argv', ['run-battery.py', '--name', 'host-test', *(['--gate'] if gate else []), *extra]), \
                 patch.object(battery.subprocess, 'check_output', return_value='host-revision'), \
                 patch.object(battery.subprocess, 'run', side_effect=run), \
                 contextlib.redirect_stdout(output), contextlib.redirect_stderr(io.StringIO()):
                status = battery.main()
            self.output = output.getvalue()
            return status, calls, json.loads((root / 'evidence/host-test.json').read_text())

    def test_gate_runs_every_required_case_and_keeps_timing(self):
        status, calls, record = self.run_fake()
        self.assertEqual(status, 0)
        self.assertEqual(len(calls), len(battery.GATE_CASES))
        self.assertEqual(set(calls), set(battery.GATE_CASES))
        self.assertTrue(record['complete'])
        self.assertTrue(record['passed'])
        self.assertTrue(all(c['launch_readiness']['ready'] for c in record['cases']))
        self.assertTrue(all('runner_overhead_seconds' in c for c in record['cases']))
        count = len(battery.GATE_CASES)
        self.assertIn(f'Test 1/{count}: {calls[0]}', self.output)
        self.assertIn(f'Test {count}/{count}:', self.output)
        self.assertIn(f'{count}/{count} completed, {count} passed, 0 failed', self.output)

    def test_runner_failure_overrides_partial_passing_evidence(self):
        status, calls, record = self.run_fake(runner_fail='shell32')
        self.assertNotEqual(status, 0)
        self.assertTrue(record['complete'])
        self.assertFalse(record['passed'])
        case = next(case for case in record['cases'] if case['case'] == 'shell32')
        self.assertEqual(case['runner_status'], 1)
        self.assertFalse(case['passed'])

    def test_subset_progress_uses_selected_count(self):
        status, calls, record = self.run_fake(gate=False, extra=('--only', 'proton-ui32', 'proton-ui64'))
        self.assertEqual(status, 0)
        self.assertEqual(calls, ['proton-ui32', 'proton-ui64'])
        self.assertIn('Test 1/2: proton-ui32', self.output)
        self.assertIn('Test 2/2: proton-ui64 PASS', self.output)
        self.assertIn('2/2 completed, 2 passed, 0 failed', self.output)

    def test_one_failure_cannot_pass_but_other_idle_cases_still_run(self):
        status, calls, record = self.run_fake(fail='memory-classes')
        self.assertEqual(status, 1)
        self.assertEqual(set(calls), set(battery.GATE_CASES))
        self.assertTrue(record['complete'])
        self.assertFalse(record['passed'])
        failed = [c['case'] for c in record['cases'] if not c['passed']]
        self.assertEqual(failed, ['memory-classes'])

    def test_stop_finishes_current_case_and_never_qualifies_partial_gate(self):
        status, calls, record = self.run_fake(stop=True)
        self.assertEqual(status, 1)
        self.assertEqual(calls, [battery.GATE_CASES[0]])
        self.assertTrue(record['cases'][0]['passed'])
        self.assertFalse(record['complete'])
        self.assertFalse(record['passed'])
        self.assertEqual(record['stop_reason'], 'requested at a case boundary')
        self.assertIn(f'1/{len(battery.GATE_CASES)} completed, 1 passed, 0 failed', self.output)

    def test_plain_run_leaves_out_on_request_cases(self):
        status, calls, record = self.run_fake(gate=False)
        self.assertEqual(status, 0)
        self.assertTrue(calls)
        self.assertFalse(battery.ON_REQUEST.intersection(calls))
        self.assertFalse(battery.ON_REQUEST.intersection(battery.GATE_CASES))
        status, calls, record = self.run_fake(gate=False, extra=('--only', 'memory-stall'))
        self.assertEqual(calls, ['memory-stall'])

    def test_one_on_request_case_per_run(self):
        with self.assertRaises(SystemExit) as caught:
            self.run_fake(gate=False, extra=('--only', 'memory-stall', 'breaker64'))
        self.assertEqual(caught.exception.code, 2)

    def test_groups_hold_only_cases_that_can_share_a_launch(self):
        cases = dict(battery.CASES)
        grouped = [name for names in battery.GROUPS.values() for name in names]
        self.assertEqual(len(grouped), len(set(grouped)))
        self.assertFalse(set(battery.GROUPS) & set(cases))
        self.assertTrue(set(grouped) <= set(battery.GATE_CASES))
        for name in grouped:
            command, marker, budget = battery.sequence_entry(name, cases[name])
            self.assertTrue(command.startswith('C:\\') and marker.startswith('PS5_PROTON_') and budget >= 90, name)
        self.assertEqual(battery.sequence_entry('descendants', cases['descendants']),
                         ('C:\\process-probe.exe --descendants', 'PS5_PROTON_PROCESS_PROBE', 125))
        self.assertEqual(battery.sequence_entry('payload-eight', cases['payload-eight'])[2], 275)
        for name in ('initial-env32', 'manual-stdout', 'native-signals', 'large-shared-section', 'memory-guard',
                     'doom-backstop', 'telemetry-teardown', 'shell32', 'prefix-fresh64'):
            with self.assertRaises(ValueError, msg=name):
                battery.sequence_entry(name, cases[name])

    def test_grouped_gate_runs_each_group_in_one_launch_with_its_own_verdicts(self):
        status, calls, record = self.run_fake(extra=('--grouped-launches',), fail='dnsapi32')
        grouped = {name for names in battery.GROUPS.values() for name in names}
        self.assertEqual(sorted(calls), sorted(set(battery.GATE_CASES) - grouped | set(battery.GROUPS)))
        self.assertEqual(status, 1)
        self.assertTrue(record['complete'])
        self.assertEqual(sorted(c['case'] for c in record['cases']), sorted(battery.GATE_CASES))
        self.assertEqual([c['case'] for c in record['cases'] if not c['passed']], ['dnsapi32'])
        dns = next(c for c in record['cases'] if c['case'] == 'dnsapi32')
        self.assertEqual((dns['group'], dns['run'], dns['status'], dns['probe_verdict']),
                         ('probes', 'host-test-probes', '1', 'FAIL'))
        self.assertTrue(dns['launch_passed'])
        owner = next(c for c in record['cases'] if c['case'] == 'payload-vm')
        self.assertEqual((owner['probe_verdict'], owner['passed'], len(owner['helpers'])), ('PASS', True, 1))
        self.assertIn('== Test 9/', self.output)
        self.assertIn('probes, one launch (host-test-probes)', self.output)

    def test_grouped_gate_runs_in_one_session_by_default(self):
        seen = {}
        def fake_session(run, members, entries):
            seen['run'], seen['members'] = run, [n for n, _ in members]
            (battery.ROOT / 'evidence' / (run + '.json')).write_text(json.dumps(
                {'passed': True, 'result': 'finished', 'returned_to_launcher': True, 'seconds': 1}))
            return {n: battery.session_verdict(entries[n][1], 1 if n == 'dnsapi32' else 0, 1000, 'exited',
                                               '' if n == 'dnsapi32' else f'{entries[n][1]} PASS\n') for n, _ in members}
        with patch.object(battery, 'session_outcomes', side_effect=fake_session):
            status, calls, record = self.run_fake(extra=('--grouped',), fail='dnsapi32')
        grouped = {name for names in battery.GROUPS.values() for name in names}
        self.assertEqual(sorted(calls), sorted(set(battery.GATE_CASES) - grouped))   # the groups made no launch of their own
        self.assertEqual(sorted(seen['members']), sorted(grouped & set(battery.GATE_CASES)))
        self.assertEqual(seen['run'], 'host-test-session')
        self.assertEqual(status, 1)
        self.assertTrue(record['complete'])
        self.assertEqual([c['case'] for c in record['cases'] if not c['passed']], ['dnsapi32'])
        self.assertIn('one session, 4 at a time (host-test-session)', self.output)

    def test_a_launch_on_exhausted_pipe_memory_fails(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for folder in ('evidence', 'klog'): (root / folder).mkdir()
            (root / 'evidence/run.json').write_text(json.dumps({'passed': True, 'result': 'finished'}))
            (root / 'klog/run.log').write_text('kern.ipc.maxpipekva exceeded; see tuning(7)\n')
            with patch.object(battery, 'ROOT', root):
                outcome = battery.launch_outcome('run', SimpleNamespace(returncode=0), 0)
                self.assertEqual(outcome['pipe_memory_exhausted'], 1)
                self.assertFalse(outcome['passed'])
                (root / 'klog/run.log').write_text('[MihawkOS] Wine: alive\n')
                self.assertTrue(battery.launch_outcome('run', SimpleNamespace(returncode=0), 0)['passed'])

    def test_sequence_outcomes_read_each_window(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'klog').mkdir()
            sequencer = ''.join(f'{t:.3f}:0020:0024:warn:debugstr:OutputDebugStringA "PS5_PROTON_GATE_SEQUENCE {line}\\n"\n'
                                for t, line in ((10, 'begin case=a'), (11, 'end case=a status=0'),
                                                (11, 'begin case=b'), (40, 'end case=b status=0'),
                                                (40, 'begin case=c'), (41, 'end case=c status=3221225477'),
                                                (41, 'begin case=d')))
            probe = '{:.3f}:0030:0034:warn:debugstr:OutputDebugStringA "PS5_PROTON_X {}\\n"\n'
            for pid, text in ((5, sequencer), (6, 'wine-child: entry\n' + probe.format(10.5, 'PASS')),
                              (7, probe.format(12, 'PASS: early') + probe.format(13, 'FAIL: later')),
                              (8, probe.format(40.5, 'PASS'))):
                (root / 'klog' / f'run-wine-child-{pid}.log').write_text(text)
            with patch.object(battery, 'ROOT', root):
                own = battery.sequence_outcomes('run', [('a', 'PS5_PROTON_X'), ('b', 'PS5_PROTON_X'),
                                                        ('c', 'PS5_PROTON_X'), ('d', 'PS5_PROTON_X'),
                                                        ('e', battery.OWNER)])
        self.assertEqual(own['a'], {'status': '0', 'probe_verdict': 'PASS', 'exit_stall': False, 'seconds': 1.0,
                                    'helpers': [5, 6], 'probe_passed': True})
        # b's verdict is its first line; it took 28 s more to end: an exit stall.
        self.assertEqual((own['b']['probe_verdict'], own['b']['exit_stall'], own['b']['probe_passed']), ('PASS', True, False))
        # c printed PASS and crashed: its status decides.
        self.assertEqual((own['c']['probe_verdict'], own['c']['probe_passed']), ('PASS', False))
        # d never ended and e never began.
        for name in 'de':
            self.assertEqual((own[name]['status'], own[name]['probe_verdict'], own[name]['probe_passed']), (None, None, False))

    def test_sequence_outcomes_read_records_spliced_behind_native_text(self):
        # gate-release-2026-10-19-01-probes-wine-child-297.log, its address masked: kill-convert's PASS landed behind
        # an unbuffered fprintf's pieces ("wine-ps5: kill thread 18bee", ": ", "0", "\n") and the battery missed it.
        release = ('2340.733:05d4:05e0:trace:plugplay:get_device_instance_id Returning ID L"HID\\\\VID_054C&PID_0CE6".\n'
                   'wine-ps5: kill thread 18f242340.735:05d4:05e0:trace:plugplay:get_device_instance_id Returning ID '
                   'L"USB\\\\VID_054C&PID_0CE6".\n'
                   ': 0\n'
                   'wine-ps5: thread 0358 killed, 42 left\n'
                   'wine-ps5: kill thread 18bee2342.839:05d4:05d8:warn:debugstr:OutputDebugStringA '
                   '"PS5_PROTON_KILL_CONVERT PASS: kills=300 stuck=0 worst_ms=26\\n"\n'
                   ': 2342.839:05d4:05d8:warn:seh:dispatch_exception "PS5_PROTON_KILL_CONVERT PASS: kills=300 stuck=0 '
                   'worst_ms=26\\n"\n'
                   '02342.839:05d4:05d8:trace:seh:dispatch_exception code=40010006 (DBG_PRINTEXCEPTION_C) flags=0 '
                   'addr=<masked>\n'
                   '\n')
        # The tid's last digits ran into the seconds: "18f24" + "2340.735", and "0" + "2342.839".
        self.assertEqual([at for at, _ in battery.wine_records(release)], [2340.733, 2340.735, 2342.839, 2342.839, 2342.839])
        sequence = '{:.3f}:0020:0024:warn:debugstr:OutputDebugStringA "PS5_PROTON_GATE_SEQUENCE {}\\n"\n'
        sequencer = ('2300.000:0020:0024:trace:process:start\n' + sequence.format(2339.811, 'begin case=kill-convert') +
                     'wine-ps5: kill thread 18f2' + sequence.format(2342.881, 'end case=kill-convert status=0') +
                     ''.join(sequence.format(at, line) for at, line in (
                         (2343, 'begin case=fail'), (2344, 'end case=fail status=1'),
                         (2345, 'begin case=clean'), (2346, 'end case=clean status=0'),
                         (2347, 'begin case=prefix'), (2348, 'end case=prefix status=0'))))
        probe = '{:.3f}:0030:0034:warn:debugstr:OutputDebugStringA "{}\\n"\n'
        logs = {247: sequencer, 297: release,
                298: 'wine-ps5: kill thread 18bef' + probe.format(2343.5, 'PS5_PROTON_FAIL FAIL: stuck=1'),
                299: probe.format(2345.5, 'PS5_PROTON_CLEAN PASS'),
                # A marker in the native text before a record is not the record's.
                300: 'PS5_PROTON_PREFIX PASS ' + probe.format(2347.5, 'something else')}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'klog').mkdir()
            for pid, text in logs.items():
                (root / 'klog' / f'run-wine-child-{pid}.log').write_text(text)
            with patch.object(battery, 'ROOT', root):
                own = battery.sequence_outcomes('run', [('kill-convert', 'PS5_PROTON_KILL_CONVERT'),
                                                        ('fail', 'PS5_PROTON_FAIL'), ('clean', 'PS5_PROTON_CLEAN'),
                                                        ('prefix', 'PS5_PROTON_PREFIX')])
        self.assertEqual(own['kill-convert'], {'status': '0', 'probe_verdict': 'PASS', 'exit_stall': False,
                                               'seconds': 3.1, 'helpers': [297], 'probe_passed': True})
        self.assertEqual((own['fail']['probe_verdict'], own['fail']['probe_passed']), ('FAIL', False))
        self.assertEqual((own['clean']['probe_verdict'], own['clean']['probe_passed']), ('PASS', True))
        self.assertEqual((own['prefix']['probe_verdict'], own['prefix']['probe_passed']), (None, False))

    def test_gate_cannot_skip_cases(self):
        with self.assertRaises(SystemExit) as caught:
            self.run_fake(extra=('--skip', 'memory-classes'))
        self.assertEqual(caught.exception.code, 2)


def fake_sequence(root, name, sequence, fail):
    """What a grouped launch's helpers would log: the sequence's lines, and each probe's verdict 0.5 s into its 1 s
    window in a helper of its own; the failing case prints FAIL and exits 1."""
    cases = dict(battery.CASES)
    lines, at = [], 100.0
    for pid, row in enumerate(sequence.read_text().splitlines(), 101):
        case = row.split('\t')[0]
        marker = battery.sequence_entry(case, cases[case])[1]
        ok = case != fail
        lines += [(at, f'begin case={case}'), (at + 1, f'end case={case} status={0 if ok else 1}')]
        (root / 'klog' / f'{name}-wine-child-{pid}.log').write_text(
            f'{at + 0.5:.3f}:0030:0034:warn:debugstr:OutputDebugStringA "{marker} {"PASS" if ok else "FAIL"}\\n"\n')
        at += 1
    (root / 'klog' / f'{name}-wine-child-100.log').write_text(''.join(
        f'{t:.3f}:0020:0024:warn:debugstr:OutputDebugStringA "PS5_PROTON_GATE_SEQUENCE {line}\\n"\n' for t, line in lines))


class SessionVerdict(unittest.TestCase):
    """--session judges each case by its own output: its marker's first verdict line, or the owner's exit status."""

    def test_marker_and_exit(self):
        v = battery.session_verdict('PS5_PROTON_DNSAPI', 0, 1200, 'exited', 'x\nPS5_PROTON_DNSAPI PASS: 12 checks\n')
        self.assertTrue(v['probe_passed']) ; self.assertEqual(v['seconds'], 1.2)
        self.assertFalse(battery.session_verdict('PS5_PROTON_DNSAPI', 0, 1, 'exited', 'PS5_PROTON_DNSAPI FAIL: x\n')['probe_passed'])
        self.assertFalse(battery.session_verdict('PS5_PROTON_DNSAPI', 0, 1, 'exited', 'no verdict\n')['probe_passed'])
        self.assertFalse(battery.session_verdict('PS5_PROTON_DNSAPI', 1, 1, 'exited', 'PS5_PROTON_DNSAPI PASS\n')['probe_passed'])
        self.assertFalse(battery.session_verdict('PS5_PROTON_DNSAPI', 0, 1, 'killed', 'PS5_PROTON_DNSAPI PASS\n')['probe_passed'])
        self.assertTrue(battery.session_verdict(battery.OWNER, 0, 1, 'exited', '')['probe_passed'])
        self.assertFalse(battery.session_verdict(battery.OWNER, 3, 1, 'exited', '')['probe_passed'])
        self.assertFalse(battery.session_verdict('M', None, None, 'no result', '')['probe_passed'])

    def test_twins_share_a_group(self):
        self.assertEqual(battery.session_id('file-write32'), '32-file-write')
        self.assertEqual(battery.session_id('file-write64'), '64-file-write')
        self.assertEqual(battery.session_id('payload-vm'), 'x-payload-vm')
        group = lambda ident: ident.split('-', 1)[1]   # tests/wt-session.c group_of
        self.assertEqual(group(battery.session_id('dnsapi32')), group(battery.session_id('dnsapi64')))
        ids = [battery.session_id(n) for names in battery.GROUPS.values() for n in names]
        self.assertEqual(len(ids), len(set(ids)))

    def test_serial_cases_are_group_members(self):
        members = {n for names in battery.GROUPS.values() for n in names}
        self.assertLessEqual(battery.SESSION_SERIAL, members)


if __name__ == '__main__': unittest.main()
