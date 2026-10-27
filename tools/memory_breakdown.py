#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""What the console's counted memory is made of, from a run's klog: python3 tools/memory_breakdown.py RUN [RUN...]

The memory guard and the console's own "VM Stats: RSS <sum> <limit>" count the resident flexible memory of every
process (sceKernelGetResidentFmemCount). Per helper the log has that figure (the "memory resident=" line), the code
it converted into its own JIT objects (converted_kib) and the code it mapped from another helper's object (shared_kib,
the copies beyond the first: counted in full per mapping, one set of physical pages). This prints, for each run:
  - the console's peak RSS against its printed limit, swap and the CPU page-table pool;
  - each helper's counted figure split into code it made, code it took, and the rest;
  - what the census would read if the copies beyond the first were counted once, against the limit.
The helpers' figures are the last each printed (every 10 s), so the sums are not one instant. Nothing here says what the
console's kernel does at the limit: that is tools/run-shared-charge.py's question."""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VM_STATS = re.compile(r'VM Stats: RSS ([\d.]+) ([\d.]+), kernel [\d.]+ [\d.]+, wire count [\d.]+ [\d.]+, '
                      r'swap out ([\d.]+), page table CPU (\d+)/(\d+)')
RESIDENT = re.compile(r'wine-child: memory resident=(\d+) MiB mapped=(\d+) MiB')
CONVERTED = re.compile(r'executable protection refused=\d+ conversions=\d+ converted_kib=(\d+)')
SHARED = re.compile(r'shared code copies shared=\d+ shared_kib=(\d+) offered=(\d+) mismatched=\d+ failed=\d+ '
                    r'private_writes=\d+ private_kib=(\d+)(?: mapped_kib=(\d+))?')
BEYOND_FIRST = re.compile(r'shared code copies beyond the first (\d+) MiB')
VM = re.compile(r'wine-child: vm resident=(\d+) MiB direct_resident=(\d+) reserved_resident=\d+ flexible=(\d+) '
                r'direct=(\d+) other=\d+ MiB')
KINDS = re.compile(r'wine-child: vm flexible-kinds (.*)')
KIND = re.compile(r'(r|rw|rx|o)\.([smlx])=(\d+)/(\d+)\.(\d)/(\d+)\.(\d)')
FLEXBIG = re.compile(r'wine-child: vm flexible-big (.*)')
SIZE_NAMES = {'s': '<64K', 'm': '<1M', 'l': '<8M', 'x': '>=8M'}
LOADED = re.compile(r'Loaded L"([^"]+\.exe)" at [0-9a-f]+: native')
TYPE = re.compile(r'--type=([a-z-]+)')
FIRMWARE = re.compile(r'firmware ([\d.]+) installed')


def last(pattern, text):
    found = pattern.findall(text)
    return found[-1] if found else None


def helper_row(text):
    """One helper's log: its counted figure and where it came from; None when it never printed one."""
    resident = last(RESIDENT, text)
    if not resident:
        return None
    converted, shared, vm = last(CONVERTED, text), last(SHARED, text), last(VM, text)
    exe = LOADED.search(text)
    kind = TYPE.search(text)
    kinds = {}
    for prot, size, n, m_i, m_f, r_i, r_f in KIND.findall(last(KINDS, text) or ''):
        kinds[(prot, size)] = (int(n), int(m_i) + int(m_f) / 10, int(r_i) + int(r_f) / 10)
    made = int(converted) / 1024 if converted else 0.0
    taken = int(shared[0]) / 1024 if shared else 0.0
    mapped = int(shared[3]) / 1024 if shared and shared[3] else None
    return {
        'mapped': mapped,
        'program': (exe.group(1).replace('\\\\', '\\').split('\\')[-1] if exe else '?') + (f'/{kind.group(1)}' if kind else ''),
        'counted': int(resident[0]), 'made': made, 'taken': taken,
        'rest': max(0.0, int(resident[0]) - made - taken),
        'flexible_mapped': int(vm[2]) if vm else None, 'direct_mapped': int(vm[3]) if vm else None,
        'kinds': kinds, 'flexbig': last(FLEXBIG, text) or '',
    }


def summarize(run_text, helper_texts):
    stats = [(float(a), float(b), float(s), int(p), int(n)) for a, b, s, p, n in VM_STATS.findall(run_text)]
    rows = [r for r in map(helper_row, helper_texts) if r]
    out = {'firmware': (FIRMWARE.search(run_text) or [None, '?'])[1], 'helpers': rows,
           'counted': sum(r['counted'] for r in rows), 'made': sum(r['made'] for r in rows),
           'taken': sum(r['taken'] for r in rows), 'rest': sum(r['rest'] for r in rows)}
    beyond = [int(m) for m in BEYOND_FIRST.findall(run_text)]
    out['beyond_first_last'] = beyond[-1] if beyond else None
    out['beyond_first_max'] = max(beyond) if beyond else None
    out['mapped'] = sum(r['mapped'] for r in rows if r['mapped'] is not None) if any(r['mapped'] is not None for r in rows) else None
    total = {}
    for r in rows:
        for key, (n, mapped, resident) in r['kinds'].items():
            cell = total.setdefault(key, [0, 0.0, 0.0])
            cell[0] += n; cell[1] += mapped; cell[2] += resident
    out['kinds'] = total
    if stats:
        peak = max(stats, key=lambda s: s[0])
        # What counting the copies beyond the first once would take off: the title's own figure when the log has it (a
        # stock, at most), else the helpers' running totals of taken clusters (events: an upper bound, unmapped ones too).
        dup = out['beyond_first_max'] if out['beyond_first_max'] is not None else out['taken']
        out.update(rss_peak=peak[0], limit=peak[1], swap_first=stats[0][2], swap_max=max(s[2] for s in stats),
                   pt_peak=max(s[3] for s in stats), pt_pool=stats[-1][4],
                   rss_once=peak[0] - dup, headroom_once=peak[1] - (peak[0] - dup))
    return out


def render(name, s):
    lines = [f"{name}: firmware {s['firmware']}, {len(s['helpers'])} helpers with a counted figure"]
    if 'rss_peak' in s:
        lines.append(f"  console RSS peak {s['rss_peak']:.0f} MiB, printed limit {s['limit']:.0f}, swap out "
                     f"{s['swap_first']:.1f} -> {s['swap_max']:.1f}, CPU page tables {s['pt_peak']}/{s['pt_pool']}")
    lines.append(f"  helpers counted {s['counted']} MiB = code made {s['made']:.0f} + code taken {s['taken']:.0f} "
                 f"(both running totals since each helper started) + rest {s['rest']:.0f}")
    if s['mapped'] is not None:
        lines.append(f"  registry objects the helpers map now: {s['mapped']:.0f} MiB")
    if s['beyond_first_max'] is not None:
        lines.append(f"  the title's count of copies beyond the first: {s['beyond_first_last']} MiB at the end, "
                     f"{s['beyond_first_max']} at most")
    if 'rss_peak' in s:
        lines.append(f"  copies counted once, at most: RSS peak {s['rss_peak']:.0f} -> {s['rss_once']:.0f} MiB, "
                     f"{s['headroom_once']:+.0f} MiB against the limit" if s['limit'] else
                     f"  copies counted once, at most: RSS peak {s['rss_peak']:.0f} -> {s['rss_once']:.0f} MiB (no limit printed)")
    if s['kinds']:
        lines.append('  flexible mappings in all helpers (protection, size: mappings, mapped MiB, resident MiB by the page walk, '
                     'which reads low):')
        for (prot, size), (n, mapped, resident) in sorted(s['kinds'].items(), key=lambda kv: -kv[1][1]):
            lines.append(f"    {prot:2} {SIZE_NAMES[size]:5} {n:6d} {mapped:8.1f} {resident:8.1f}")
    for r in sorted(s['helpers'], key=lambda r: -r['counted'])[:8]:
        lines.append(f"    {r['program'][:34]:34} counted {r['counted']:4d}  made {r['made']:5.1f}  taken {r['taken']:5.1f}"
                     f"  rest {r['rest']:5.1f}")
    return lines


def main(argv):
    klog = ROOT / 'klog'
    for name in argv or []:
        log = klog / f'{name}.log'
        if not log.is_file():
            print(f'{name}: no {log}', file=sys.stderr)
            return 1
        children = [p.read_text(errors='replace') for p in sorted(klog.glob(f'{name}-wine-child-*.log'))]
        print('\n'.join(render(name, summarize(log.read_text(errors='replace'), children))))
    return 0 if argv else 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
