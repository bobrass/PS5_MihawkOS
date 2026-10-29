# SPDX-License-Identifier: GPL-3.0-or-later
# Pull the rule code out of patch 2070, the hold code out of patch 2104 and the own DualSense link name out of patch
# 2401 (the lines between PW_PROCESS_<NAME>_BEGIN and _END) so tests/test_process_rules.c and
# tests/test_process_hid_link.c test the code the runtime is built with, not a copy.
#   python3 tests/extract_process_rules.py <patch 2070> <output .inc> [<more patches>...]
import re
import sys
out = []
for patch in [sys.argv[1]] + sys.argv[3:]:
    lines = open(patch).read().splitlines()
    for begin, line in enumerate(lines):
        m = re.match(r'\+/\* PW_PROCESS_(\w+)_BEGIN \*/', line)
        if m:
            end = next(i for i in range(begin, len(lines)) if lines[i].startswith(f'+/* PW_PROCESS_{m.group(1)}_END */'))
            out += [l[1:] for l in lines[begin:end + 1]]
open(sys.argv[2], 'w').write('\n'.join(out) + '\n')
