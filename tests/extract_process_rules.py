# SPDX-License-Identifier: GPL-3.0-or-later
# Pull the rule code out of patch 2070 (the lines between PW_PROCESS_RULES_BEGIN and _END) so
# tests/test_process_rules.c tests the code the runtime is built with, not a copy.
#   python3 tests/extract_process_rules.py <the patch> <output .inc>
import sys
lines = open(sys.argv[1]).read().splitlines()
begin = next(i for i, l in enumerate(lines) if l.startswith('+/* PW_PROCESS_RULES_BEGIN */'))
end = next(i for i, l in enumerate(lines) if l.startswith('+/* PW_PROCESS_RULES_END */'))
open(sys.argv[2], 'w').write('\n'.join(l[1:] for l in lines[begin:end + 1]) + '\n')
