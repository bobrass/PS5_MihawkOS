# Console testing: a gate per console, and quick runs between gates

Two changes of 2026-10-28 (owner's request, Design session) that make console testing faster without weakening the
commit gate. Neither has been run on a console yet: host tests only (`tests/test_gate_quick.py`,
`tests/test_console_guard.py`, `tests/test_battery_runner.py`).

## 1. Each console keeps its own records

**Why.** A gate is 104 cases and takes 35-60 minutes. With the standard PS5 (13.60) and the Pro (10.01) both
available, two gates could not run at once from checkouts that share `.deps` (the `PS5_Proton-*` worktrees link it),
because both wrote the same files: `.deps/gate/passed.json`, `.deps/gate/build.json` and the console guard's
`.deps/console-guard.json` (launch count, ps5vkctl pid, drop and storage blocks). The second run read the first
console's ps5vkctl pid as "gone and loaded again", which is a drop.

**What changed.**

| Record | Before | Now |
| --- | --- | --- |
| Guard state | `.deps/console-guard.json` | `.deps/console-guard-<key>.json` |
| Guard blocks (storage, deploys) | `.deps/console-guard-sticky.json` | `.deps/console-guard-<key>-sticky.json` |
| Last run marker | `.deps/last-console-run` | `.deps/last-console-run-<key>` |
| Gate pass | `.deps/gate/passed.json` | `.deps/gate/passed-<key>.json` (with `console`) |
| Build record | `.deps/gate/build.json` (the last build) | the same, and `.deps/gate/builds/<fingerprint>.json` for every build |

`<key>` (`console_guard.KEY`) is the first 12 hex digits of the sha256 of the console's address. The address comes
from where every tool already reads it: `PS5_HOST` in the environment, else `PS5_Vulkan/.env`. So picking a console
is the same as before: `PS5_HOST=<address> python3 tools/gate.py run --name ...`. No address is written into a file
name or a log line. `tools/console_guard.py status` shows the key it is using.

- **The pre-commit hook** (`gate.py check`) accepts a pass from any console whose record has exactly the
  fingerprint of the commit's tree, and names the battery and console. The rule is unchanged: commit exactly what
  passed. A `passed.json` written before this change still counts.
- **`gate.py run`** looks up the build record of the tree's own fingerprint. Two checkouts that share `.deps` can each
  have a build recorded, and a build made in one no longer turns the other's gate into "the code changed since the
  last build". A build recorded only in `build.json`, from before this change, is still found.
- **A console's first state** starts with the drop and storage blocks of the old single `console-guard.json`, since
  they may have been this console's. It does not take the ps5vkctl pid or launch count, which belonged to whichever
  console that was. So after this change, a block recorded before it must be cleared on each console
  (`new-boot` after a restart, or `clear-storage`).
- With no console set (no `PS5_HOST`, no `.env`), the guard uses the old single file.

**Limits.**
- The runtime both runs check against is `.deps/staged/manifest.json`, and `.deps` is shared. Two gates run at once
  when both consoles hold that staged runtime: one build gated on both, or checkouts whose Wine runtime is the same.
  If one checkout stages a different runtime, the other's `gate.py run` refuses on the read-back ("the console does
  not hold this build"), so this is safe but serial. Giving each checkout its own staged runtime is a build change
  (owned by the faster-builds session).
- The key comes from the address. If a console's address changes (DHCP), it gets a new key and a fresh state, and
  the blocks recorded under the old key no longer stop it. After an address change, run
  `tools/console_guard.py status` and copy any block over by hand.
- A checkout without this change still reads and writes the single `console-guard.json`. Every checkout that shares
  a `.deps` and runs on a console should carry this change, or two guards will watch one console with different
  records.

## 2. Quick runs: only the cases a change touches

**Why.** Between "it builds" and the full gate, an agent usually needs to know only whether the cases its change can
affect still pass. Picking `--only` cases by hand is slow and can be wrong.

**What.** `python3 tools/gate.py quick --name <run> [--since REV]` makes the same checks as `gate.py run` (a build
of exactly this tree, the console read back as holding it). Then it runs
`run-battery.py --grouped --only <cases>` with the gate cases the change touches. It **never writes a pass record**,
so the hook never accepts a quick run, and `gate.py run` stays the only way code gets committed. `--since` counts
from the merge base with HEAD, so `--since main` means "this branch's changes". To see the choice without a console:
`python3 tools/gate_quick.py [--since REV]`.

**A case that holds GiBs of memory never shares a session** in a quick run. Those cases are listed in
`gate_quick.HOLDS_GIB`: `payload-memory`, whose two helpers commit and touch 2 GiB each, and `large-shared-section`.
`gate.py quick` passes them to `run-battery.py --alone`, which takes a case out of its group for a launch of its own.
The memory session found that on a fresh boot, `payload-memory` in the shared session pushed the page-table pool to
the guard. Taking it out of the full gate's session is that session's change to `GROUPS`.

**How cases are chosen** (`tools/gate_quick.py`). The first rule that matches a path decides its cases:

1. Notes and evidence (`gate.notes`: `docs/`, `evidence/`, `prototypes/`, `*.md`) and host-only files (host tests,
   git hooks, the privacy and publication checks, CI): none.
2. `RULES`, a table checked into `tools/gate_quick.py`. It covers probes whose `.exe` name differs from their
   source, the grouped-launch and session runners (every grouped case), and the title's feature modules that only
   some cases reach (Steam guide, guided setup, game folders, detailed logs, controls, Game Mode, MihawkBrowser,
   memory guard, UMIP).
3. `tests/<x>-probe.c`: the gate cases that run `<x>-probe.exe`, `<x>32-probe.exe` or `<x>64-probe.exe`, or are
   named `<x>`, `<x>32` or `<x>64`. Derived from `run-battery.py`'s `CASES`.
4. `tools/run-<x>.py`: the gate cases whose own launch runs that script. Derived from `run-battery.py`'s `runner_of`
   (the runner choice moved out of `main()` into that function, unchanged).
5. Anything else: **every case**. `gate.py quick` then refuses and points to the full gate.

So `wine/`, `native/`, `ps5/src/`, `platform/`, the build scripts, `run-console.py`, `run-battery.py`,
`gate.py` and the console guard always take every case. Quick mode can under-select only where a rule in step 2, or
a probe or runner shared outside what steps 3 and 4 see, is wrong. If so, the full gate before the commit catches it.
A row is added only for a file that only those cases reach. `tests/test_gate_quick.py` fails on a row that matches
no file in the repository or names a case outside `GATE_CASES`.

**Not done.** No smoke set is added to every quick run, and nothing narrows a Wine patch to the cases of the module
it changes. Both would be new rules in `RULES`, each justified by what the cases measure.
