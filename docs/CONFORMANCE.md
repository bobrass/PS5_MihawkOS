# Conformance and regression validation

The validation loop promoted from `win-32bit` to local `main` compares identical PE32 and PE64 Wine tests against a host baseline, then
checks the PS5-specific payload and GPU-broker contracts. Upstream suites find API defects; small probes
isolate a defect; game acceptance proves the complete workload. None substitutes for the others.

## Start here

| Question | Command or source |
| --- | --- |
| Does Windows memory, exception or process behavior differ? | `tools/build-wine-tests.sh`, `tools/run-wine-tests.py` |
| Did a runtime change break production? | `tools/gate.py run`; required cases are defined in `tools/run-battery.py` |
| Can Zink upload large textures and reuse memory? | battery cases `gl-upload32`, `gl-upload64`; `tests/gl-upload-probe.c` |
| Does the full game work? | `tools/run-console.py` with displayed-frame evidence, input/audio checks, exit and relaunch |
| What did older broad suites show? | [historical report](history/2026-10-07/docs/CONFORMANCE.md), `evidence/wine-tests/` legacy records |
| Does the whole tree pass on the console? | `tools/run-wine-session.py` (one persistent session), `tools/run-wine-baselines.py`, `tools/qualify-wine-tests.py`, `tools/conformance-table.py`; section below |
| Do real installers work? | `tools/run-installer-slice.py` (host, console, compare) |

## Suite-driven compatibility (2026-10-25)

Per-game debugging found one fault at a time. This round runs Wine's own conformance tests broadly on the PS5 Pro and
pairs every file with the same executable on the PC, the way the Vulkan CTS qualified RADV: a console-only failure is a
difference in our platform layer, reduced to one cause, fixed in the shared layer or qualified with its reason.

### Order: what games touch most

| Group | DLLs |
| --- | --- |
| 1 | ntdll, kernelbase, kernel32 (virtual, file, path, process, sync, thread, exception, loader) |
| 2 | user32, gdi32 |
| 3 | msvcrt, ucrtbase, advapi32 |
| 4 | ws2_32, winhttp, ole32, rpcrt4, crypt32, bcrypt, secur32 |
| 5 | dinput, xinput1_3, hid, mmdevapi, dsound, xaudio2_7 |
| 6 | d3d11, dxgi, d3d9, d3d12, d3d10core (DXVK/VKD3D; sliced, `count` then `--slices`) |

```sh
bash tools/build-wine-tests.sh ntdll kernel32 kernelbase user32 gdi32 msvcrt ucrtbase advapi32 ws2_32 winhttp ole32 \
    rpcrt4 crypt32 bcrypt secur32 dinput xinput1_3 hid mmdevapi dsound xaudio2_7
bash tools/build-wine-host-display.sh          # the oracle, once
python3 tools/run-wine-tests.py host DLL --arch ARCH --host-wine .deps/wine-host-display/build/wine
python3 tools/run-wine-tests.py console DLL --arch ARCH --per-launch 6 --file-timeout 90
python3 tools/qualify-wine-tests.py DLL --arch ARCH [--write]   # causes -> <arch>/<dll>-expected.json
python3 tools/run-wine-tests.py compare DLL --arch ARCH
```

### One persistent session instead of a launch per few files

The per-launch mode paid a title start, a Wine session start and a cleanup (20-30 s) for every few files: ntdll in both
architectures took 21 min 9 s. `tests/wt-session.c` runs in one title launch: it takes test files from a queue the host
writes over FTP (`C:\wt\RUN\queue.txt`, replaced whole), runs each as its own process from C:\ with its output in
`C:\wt\RUN\out\ID.txt`, several at a time, and appends each verdict to `C:\wt\RUN\results.txt`; a crashed or hung file
is one result and the session goes on. `tools/run-wine-session.py` launches it through `tools/run-console.py`, feeds it,
reads each result as it lands and records it exactly as the per-launch mode does (same parser, statuses, context and
evidence file), so `compare` and `tools/qualify-wine-tests.py` read either.

- **Parallel:** 6 files at a time by default (4 at a time peaked at 302 MiB of the guard's 990 MiB). Files that share
  the session's state run alone (`tests/wine-tests-serial-ps5.json`: windows, focus, clipboard, devices, audio, the
  display, services, registry-wide settings). Two builds of the same test file never overlap: they use the same pipe,
  mutex and file names (ntdll pipe failed 146 times when its PE32 and PE64 runs overlapped in a dry run).
- **Timeouts from the PC:** 10 times the file's time on the PC plus 30 s, 90 s at least and 900 s at most; 300 s when
  the PC did not complete it. A timeout fails.
- **Incremental:** each record keeps its inputs: the test program's import closure in the staged runtime, the module
  under test, their Unix modules, the core (ntdll and win32u Unix sides, the helper loader, the title; WoW64's 64-bit
  side for PE32), by digest. A file runs again only when its test program, the Wine source or an input changed, or its
  last result was not final, plus the two fastest files of each DLL and architecture every time (sentinels for what a
  test loads by name). `--force` runs everything. Records of one DLL may then come from several production builds;
  compare and the qualification leave the build fingerprint out (each record still keeps it).
- **Deploy:** `tools/deploy-wine.py` already uploads only files whose digest changed since the last deploy, and now
  prints which.
- **A stand-in for the console** (`PW_STUB_CONSOLE=1 PW_STUB_PREFIX=... PW_EVIDENCE_DIR=...`) runs the same session on
  the PC's Wine, so the runner itself is debugged without console time.

**Proof, ntdll in both architectures, one production build** ([evidence](../evidence/wine-tests/session-vs-perlaunch-ntdll-2026-10-25.json)):
106.5 s in the session against 21 min 9 s per launch, 12x. 48 of 52 files give the same status and the same failure
lines. Of the four: x86_64 pipe had 30 lines per launch that are state left by an earlier file of the same batch process
(only pipe.c:654 stays when the file has its process to itself); i386 exception hangs after exception.c:1992 in both
(the per-launch record said complete because a child's summary line was taken for the file's, fixed); change.c:280
(x86_64, per launch only) and pipe.c:654/1363 (i386, session only) are timing races, the second an open defect: a
cancelled read's I/O status is written after NtCancelIoFile returns.

### What the method needed (found on the way)

- **Direct3D's users need the oracle on DXVK too.** The title starts every program with DXVK's graphics DLLs
  (native/proton_wine.cpp default_overrides), so a d3dx9, d3dx10, d3d8 or Media Foundation test draws through DXVK on
  the console, but only the graphics DLLs themselves were compared with the PC on DXVK. The rest were compared with
  the PC's wined3d: d3dx9_36 shader's 75,097 console-only lines (a boolean shader constant reads back as 1 on DXVK,
  the raw value on Windows and wined3d) are on the PC on DXVK too, line for line (2026-10-26). tools/run-wine-tests.py
  D3D_DLLS now lists every module whose tests create a Direct3D device, 38 of them, and tests/test_dll_overrides.py
  checks the list against the test sources. That DXVK prefix on the PC had none of the console's fonts, so d3dx9
  core.c:377's D3DXCreateFont failed there and the test crashed; it now gets them as the display prefix does
  (console_fonts). Rerun on both sides (console d3dora, 2026-10-27, no pipe exhaustion; PC 66 runs): of these 33
  modules' 228 files, 158 match against 129 before (d3dx9 shader, d3dx10_33-43, d3dx11, d2d1, d3d10_1, win32u and
  d3dcompiler_47 blob); 8 that matched the PC's wined3d are open now, d3dx9 core in all four versions and both builds:
  on the PC on DXVK, D3DXCreateFont still fails (0x88760b59) with the fonts in place, so the oracle does not
  finish, while the console completes it with the 5 lines the PC's wined3d has too. The PC's DXVK there is the open
  question, not the console.
- **Files sharing a registry key run alone.** Every reg.exe test file adds, copies and deletes
  HKCU\Software\Wine\reg_test, and regedit's twins share HKCU\Software\Wine\regedit_test; beside each other their
  failure counts differed between the builds (reg add: 64 and 9). Both now run alone
  (tests/wine-tests-serial-ps5.json).
- **The oracle has to have a display.** The pinned host build is headless (`--without-x`): user32 files ran into the
  290 s timeout one after another, and Inno's and Burn's silent installers could not make their windows.
  `tools/build-wine-host-display.sh` builds the same source and revision (1c78596) with its X11 driver; the runner puts
  every window of it into one Wine virtual desktop and runs it in its own prefix. With it the host completes user32
  24/24 and gdi32 14/14 in both architectures.
- **A child of a batched test re-ran the batch.** In batch mode argv[1] was the whole file list, and a test starts its
  children as `argv[0] argv[1] child-args`: every debuggee and dummy child ran the list again (ntdll info: two complete
  batches in one stdout). The batch header now gives each file its own name as argv[1]; `split_batch` marks a file whose
  begin line repeats `duplicated` (tests/test_wine_conformance.py checks it). All results before this fix are stale.
- **Run from C:\ on both sides.** The console starts `C:\<dll>_test.exe`; the host ran it by its Unix path from .deps,
  so argv[0], the DLL search path and the current directory differed. The host now copies the program into its
  prefix's C:\ and starts it from there.
- **Batches share a process.** The console runs six files per launch, the host one per process: a file's leftover
  process state reaches the next (kernel32 module's DLL directories made path's `LdrGetDllPath` fail). A console-only
  line without a platform cause is rerun alone (`--per-launch 1 --force`) before it is called a defect.
- **A helper's crash is not the title's.** run-title ends a watch at any `mDBG: Sending signal` line; the second sweep
  session lost its watcher after 394 files when the kernel signalled one payload helper (i386 quartz avisplit), while the
  title went on running the queue. `tools/run-console.py` now ends a run only for the title's own process (the kernel's
  banner waits for its `# proc ID`; tests/test_run_console_verdicts.py), and `run-wine-session.py --attach RUN` reads a
  session still on the console until its title ends.

### Results by group (whole scope, the fmem merge: commit 6837d44, gate-merge-2026-10-26-03)

Counted per test file and architecture by `tools/conformance-table.py` against the PC's display-capable Wine
(evidence/wine-tests/conformance-table-merge-6837d44-2026-10-26.json). Matching: complete on both sides, no console-only
line. Fixed: matching now, and named in evidence/wine-tests/fixes.json with the change that made it so. Qualified: every
console-only line has an accepted reason. Awaiting owner: qualified only by a choice of ours, until the owner accepts
it. Open: anything else, including a skipped, unfinished, timed-out or faulted file. The sweep ran on 2026-10-26 in
parts, with a console restart before each, because the program tests (cmd, reg, regedit, ...) start helpers by the
thousand and the kernel's pipe memory runs out within a boot: ntdll, kernelbase and kernel32 (sweepm1), the rest of
the first half from user32 (sweepm1b: 750 results), the second half's first nine modules (sweepm2), its 218 library
modules (sweepm2c: 680 results, no pipe exhaustion, no title crash, 890 helpers) and its 18 program modules
(sweepm2d). That last run ran out of pipe memory at 20:26:16, 13 minutes in, with cmd, reg, regedit, conhost, explorer
and net done; the twelve modules after it completed and match B3c except tasklist (below). No title crash in any part.

reg.exe and regedit.exe, which the last part left unrun, ran alone on the Pro after the integration-1027 gate
(serial-reg, 2026-10-27): all 14 files complete with no failure line in both builds (reg add 685, copy 502, delete
142, export 305, import 3,787, query 148, regedit 3,084 checks) and match the PC. Pipe memory ran out half-way
through that run (klog line 23,799 of 51,940); every file still completed, and the console work of that boot ended
there. winscard and wpcap reran without tracing (clean-crash) as before (below).

Against B3c (1708 files, the same scope): 22 more matching, 27 fewer open, six more awaiting owner. 29 files got
better and 2 worse, after the four files the sweep had found worse were run again one at a time on a fresh boot
(serial-tasklist, -extract, -policy, -filtermapper, -fdi; 2026-10-26, all complete without a failure line):
- kernel32 volume, both builds (qualified in B3c): now the same as the PC. Patch 2092 made `CreateSymbolicLinkA`
  work, so volume.c:1845 creates `C:\winetest_link`, and the first lookup through that directory link ends the process
  with EXCEPTION_WINE_ASSERTION (0x80000101, a SIGABRT on the Unix side) right after volume.c:1838, where the PC's own
  Wine stops too (glibc: "double free or corruption (out)"; tools/qualify-wine-tests.py ORACLE). Before 2092 the link
  could not be made and the console ran on to the end, which the oracle rule accepts; a stop where the PC stops it
  counts open, by design. The x86_64 run left `C:\winetest_mnt` and the link behind, which is the i386 run's error
  183 at 1675 and 1845. Not a console-only difference: an upstream Wine abort in the link lookup, both sides alike.
- tasklist.exe, both builds (sweep): an empty `GetModuleBaseNameA` name through an `OpenProcess` handle of the test
  itself, after pipe memory had run out. Matching run again on a fresh boot: exhaustion was the cause.
- cabinet extract (i386), hnetcfg policy (x86_64) and quartz filtermapper (x86_64) (sweep): one build only, the other
  build matching. cabinet and hnetcfg's two builds finished within 50 ms of each other, both running from `C:\`; extract
  and fdi both make `C:\extract.cab` (fdi was open in B3c for it), hnetcfg changes the UPnP port-mapping list,
  filtermapper reads a filter category other quartz files register into. Matching run alone; cabinet,
  hnetcfg:policy and quartz:filtermapper now run alone (tests/wine-tests-serial-ps5.json).
Better: ntdll change and directory (x86_64) and kernel32 pipe, matching (patches 2091, 2092, 2095); ntdll exception
PE32, qualified; kernel32 heap, virtual and loader and ntdll directory PE32, awaiting owner (the platform layer's own
threads, patch 1004's simulate_writecopy, the write-watch superset, the 32-bit drivers in syswow64); and ddraw4/7,
msvcp110, comctl32 tooltips, d2d1, msi action, wtsapi32, advpack install, d3dcompiler_42, explorerframe nstc,
odbccp32 and cabinet fdi, matching.

The B3c sweep (commit 4bfc242, gate-b3c-2026-10-25-02) took eleven title launches on 2026-10-25: the first ran out of
the kernel's pipe and socket budgets (a console restart); seven ended in a title crash that the runner recovered from
(four GPU page faults, "unmapped page access", with evr and explorerframe nstc running; three divide faults in the
server's scale_dpi, patch 2086); one I stopped to drop the sentinels; two finished.

| Group | files | matching | fixed | qualified | awaiting owner | open |
| --- | --- | --- | --- | --- | --- | --- |
| Core | 130 | 91 | 1 | 3 | 9 | 26 |
| Windows, text and drawing | 218 | 182 | 0 | 0 | 0 | 36 |
| C and C++ runtimes | 94 | 90 | 0 | 0 | 0 | 4 |
| Security, registry, RPC and COM | 152 | 131 | 0 | 0 | 6 | 15 |
| Network | 78 | 60 | 0 | 0 | 2 | 16 |
| Input | 18 | 9 | 0 | 2 | 5 | 2 |
| Audio and media | 164 | 88 | 2 | 0 | 4 | 70 |
| Graphics | 224 | 140 | 0 | 0 | 0 | 84 |
| Everything else (shell, installers, scripting, programs, the rest) | 630 | 546 | 0 | 0 | 0 | 84 |
| **all in scope** | **1708** | **1337** | **3** | **5** | **26** | **337** |

Of the 337 open files: 261 complete on both sides with console-only lines still to triage or qualify, 53 unfinished, 17 timed out, 6 skipped.

**Core**

| DLL | files (both builds) | matching | fixed | qualified | awaiting owner | open |
| --- | --- | --- | --- | --- | --- | --- |
| ntdll | 52 | 36 | 0 | 1 | 4 | 11 |
| kernelbase | 12 | 10 | 0 | 0 | 0 | 2 |
| kernel32 | 66 | 45 | 1 | 2 | 5 | 13 |
| **all** | **130** | **91** | **1** | **3** | **9** | **26** |

**Windows, text and drawing**

| DLL | files (both builds) | matching | fixed | qualified | awaiting owner | open |
| --- | --- | --- | --- | --- | --- | --- |
| user32 | 48 | 32 | 0 | 0 | 0 | 16 |
| gdi32 | 28 | 24 | 0 | 0 | 0 | 4 |
| comctl32 | 58 | 52 | 0 | 0 | 0 | 6 |
| comdlg32 | 10 | 10 | 0 | 0 | 0 | 0 |
| imm32 | 2 | 2 | 0 | 0 | 0 | 0 |
| msctf | 2 | 2 | 0 | 0 | 0 | 0 |
| dwrite | 6 | 0 | 0 | 0 | 0 | 6 |
| d2d1 | 2 | 1 | 0 | 0 | 0 | 1 |
| windowscodecs | 30 | 29 | 0 | 0 | 0 | 1 |
| gdiplus | 24 | 24 | 0 | 0 | 0 | 0 |
| usp10 | 2 | 2 | 0 | 0 | 0 | 0 |
| dwmapi | 2 | 2 | 0 | 0 | 0 | 0 |
| t2embed | 2 | 2 | 0 | 0 | 0 | 0 |
| uxtheme | 2 | 0 | 0 | 0 | 0 | 2 |
| **all** | **218** | **182** | **0** | **0** | **0** | **36** |

**C and C++ runtimes**

| DLL | files (both builds) | matching | fixed | qualified | awaiting owner | open |
| --- | --- | --- | --- | --- | --- | --- |
| msvcrt | 26 | 26 | 0 | 0 | 0 | 0 |
| ucrtbase | 16 | 16 | 0 | 0 | 0 | 0 |
| msvcp140 | 2 | 0 | 0 | 0 | 0 | 2 |
| msvcp120 | 2 | 0 | 0 | 0 | 0 | 2 |
| msvcp110 | 2 | 2 | 0 | 0 | 0 | 0 |
| msvcp100 | 4 | 4 | 0 | 0 | 0 | 0 |
| msvcp90 | 6 | 6 | 0 | 0 | 0 | 0 |
| msvcr120 | 2 | 2 | 0 | 0 | 0 | 0 |
| msvcr100 | 2 | 2 | 0 | 0 | 0 | 0 |
| msvcr90 | 2 | 2 | 0 | 0 | 0 | 0 |
| concrt140 | 2 | 2 | 0 | 0 | 0 | 0 |
| vcomp | 2 | 2 | 0 | 0 | 0 | 0 |
| msvcirt | 2 | 2 | 0 | 0 | 0 | 0 |
| msvcp140_1 | 2 | 2 | 0 | 0 | 0 | 0 |
| msvcp140_2 | 2 | 2 | 0 | 0 | 0 | 0 |
| msvcp140_atomic_wait | 2 | 2 | 0 | 0 | 0 | 0 |
| msvcp60 | 4 | 4 | 0 | 0 | 0 | 0 |
| msvcr110 | 2 | 2 | 0 | 0 | 0 | 0 |
| msvcr70 | 2 | 2 | 0 | 0 | 0 | 0 |
| msvcr71 | 2 | 2 | 0 | 0 | 0 | 0 |
| msvcr80 | 2 | 2 | 0 | 0 | 0 | 0 |
| msvcrtd | 2 | 2 | 0 | 0 | 0 | 0 |
| vccorlib140 | 2 | 2 | 0 | 0 | 0 | 0 |
| vcomp110 | 2 | 2 | 0 | 0 | 0 | 0 |
| **all** | **94** | **90** | **0** | **0** | **0** | **4** |

**Security, registry, RPC and COM**

| DLL | files (both builds) | matching | fixed | qualified | awaiting owner | open |
| --- | --- | --- | --- | --- | --- | --- |
| advapi32 | 24 | 18 | 0 | 0 | 2 | 4 |
| ole32 | 28 | 26 | 0 | 0 | 0 | 2 |
| rpcrt4 | 12 | 8 | 0 | 0 | 2 | 2 |
| crypt32 | 30 | 30 | 0 | 0 | 0 | 0 |
| bcrypt | 2 | 0 | 0 | 0 | 0 | 2 |
| secur32 | 10 | 4 | 0 | 0 | 2 | 4 |
| oleaut32 | 20 | 19 | 0 | 0 | 0 | 1 |
| combase | 4 | 4 | 0 | 0 | 0 | 0 |
| propsys | 2 | 2 | 0 | 0 | 0 | 0 |
| wintrust | 8 | 8 | 0 | 0 | 0 | 0 |
| cryptnet | 2 | 2 | 0 | 0 | 0 | 0 |
| ncrypt | 2 | 2 | 0 | 0 | 0 | 0 |
| schannel | 2 | 2 | 0 | 0 | 0 | 0 |
| userenv | 2 | 2 | 0 | 0 | 0 | 0 |
| dssenh | 2 | 2 | 0 | 0 | 0 | 0 |
| rsaenh | 2 | 2 | 0 | 0 | 0 | 0 |
| **all** | **152** | **131** | **0** | **0** | **6** | **15** |

**Network**

| DLL | files (both builds) | matching | fixed | qualified | awaiting owner | open |
| --- | --- | --- | --- | --- | --- | --- |
| ws2_32 | 6 | 0 | 0 | 0 | 0 | 6 |
| winhttp | 6 | 4 | 0 | 0 | 2 | 0 |
| iphlpapi | 2 | 0 | 0 | 0 | 0 | 2 |
| dnsapi | 8 | 8 | 0 | 0 | 0 | 0 |
| wininet | 12 | 10 | 0 | 0 | 0 | 2 |
| urlmon | 14 | 13 | 0 | 0 | 0 | 1 |
| netapi32 | 8 | 8 | 0 | 0 | 0 | 0 |
| hnetcfg | 2 | 2 | 0 | 0 | 0 | 0 |
| httpapi | 2 | 1 | 0 | 0 | 0 | 1 |
| nsi | 2 | 0 | 0 | 0 | 0 | 2 |
| webservices | 12 | 10 | 0 | 0 | 0 | 2 |
| wldap32 | 4 | 4 | 0 | 0 | 0 | 0 |
| **all** | **78** | **60** | **0** | **0** | **2** | **16** |

**Input**

| DLL | files (both builds) | matching | fixed | qualified | awaiting owner | open |
| --- | --- | --- | --- | --- | --- | --- |
| dinput | 12 | 7 | 0 | 0 | 5 | 0 |
| xinput1_3 | 2 | 0 | 0 | 2 | 0 | 0 |
| hid | 2 | 0 | 0 | 0 | 0 | 2 |
| windows.gaming.input | 2 | 2 | 0 | 0 | 0 | 0 |
| **all** | **18** | **9** | **0** | **2** | **5** | **2** |

**Audio and media**

| DLL | files (both builds) | matching | fixed | qualified | awaiting owner | open |
| --- | --- | --- | --- | --- | --- | --- |
| mmdevapi | 12 | 8 | 2 | 0 | 0 | 2 |
| dsound | 14 | 10 | 0 | 0 | 4 | 0 |
| xaudio2_7 | 2 | 0 | 0 | 0 | 0 | 2 |
| dxva2 | 2 | 2 | 0 | 0 | 0 | 0 |
| winmm | 20 | 18 | 0 | 0 | 0 | 2 |
| quartz | 36 | 14 | 0 | 0 | 0 | 22 |
| mf | 6 | 0 | 0 | 0 | 0 | 6 |
| mfplat | 2 | 0 | 0 | 0 | 0 | 2 |
| mfreadwrite | 2 | 0 | 0 | 0 | 0 | 2 |
| xaudio2_8 | 2 | 0 | 0 | 0 | 0 | 2 |
| xactengine3_7 | 2 | 0 | 0 | 0 | 0 | 2 |
| amstream | 2 | 0 | 0 | 0 | 0 | 2 |
| devenum | 2 | 0 | 0 | 0 | 0 | 2 |
| dmband | 2 | 2 | 0 | 0 | 0 | 0 |
| dmcompos | 2 | 2 | 0 | 0 | 0 | 0 |
| dmime | 2 | 0 | 0 | 0 | 0 | 2 |
| dmloader | 2 | 0 | 0 | 0 | 0 | 2 |
| dmscript | 2 | 2 | 0 | 0 | 0 | 0 |
| dmstyle | 2 | 2 | 0 | 0 | 0 | 0 |
| dmsynth | 2 | 0 | 0 | 0 | 0 | 2 |
| dmusic | 2 | 0 | 0 | 0 | 0 | 2 |
| dsdmo | 2 | 2 | 0 | 0 | 0 | 0 |
| evr | 2 | 0 | 0 | 0 | 0 | 2 |
| mfmediaengine | 2 | 0 | 0 | 0 | 0 | 2 |
| mfplay | 2 | 0 | 0 | 0 | 0 | 2 |
| mfsrcsnk | 2 | 0 | 0 | 0 | 0 | 2 |
| mp3dmod | 2 | 2 | 0 | 0 | 0 | 0 |
| qcap | 18 | 16 | 0 | 0 | 0 | 2 |
| qedit | 8 | 4 | 0 | 0 | 0 | 4 |
| wmp | 4 | 4 | 0 | 0 | 0 | 0 |
| wmvcore | 2 | 0 | 0 | 0 | 0 | 2 |
| **all** | **164** | **88** | **2** | **0** | **4** | **70** |

**Graphics**

| DLL | files (both builds) | matching | fixed | qualified | awaiting owner | open |
| --- | --- | --- | --- | --- | --- | --- |
| d3d11 | 2 | 0 | 0 | 0 | 0 | 2 |
| dxgi | 2 | 0 | 0 | 0 | 0 | 2 |
| d3d9 | 8 | 0 | 0 | 0 | 0 | 8 |
| d3d12 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3d10core | 2 | 0 | 0 | 0 | 0 | 2 |
| d3d10 | 6 | 6 | 0 | 0 | 0 | 0 |
| d3d10_1 | 4 | 2 | 0 | 0 | 0 | 2 |
| d3d8 | 6 | 0 | 0 | 0 | 0 | 6 |
| ddraw | 18 | 14 | 0 | 0 | 0 | 4 |
| opengl32 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dcompiler_43 | 10 | 10 | 0 | 0 | 0 | 0 |
| d3dcompiler_47 | 10 | 10 | 0 | 0 | 0 | 0 |
| d3dx9_36 | 22 | 16 | 0 | 0 | 0 | 6 |
| d3dx10_43 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dx11_43 | 2 | 0 | 0 | 0 | 0 | 2 |
| vulkan-1 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dcompiler_42 | 10 | 10 | 0 | 0 | 0 | 0 |
| d3dcompiler_46 | 10 | 10 | 0 | 0 | 0 | 0 |
| d3drm | 4 | 2 | 0 | 0 | 0 | 2 |
| d3dx10_33 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dx10_34 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dx10_35 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dx10_36 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dx10_37 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dx10_38 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dx10_39 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dx10_40 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dx10_41 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dx10_42 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dx11_42 | 2 | 0 | 0 | 0 | 0 | 2 |
| d3dx9_35 | 22 | 16 | 0 | 0 | 0 | 6 |
| d3dx9_42 | 22 | 16 | 0 | 0 | 0 | 6 |
| d3dx9_43 | 22 | 16 | 0 | 0 | 0 | 6 |
| d3dxof | 2 | 2 | 0 | 0 | 0 | 0 |
| ddrawex | 4 | 4 | 0 | 0 | 0 | 0 |
| dxcore | 2 | 2 | 0 | 0 | 0 | 0 |
| glu32 | 2 | 2 | 0 | 0 | 0 | 0 |
| wing32 | 2 | 2 | 0 | 0 | 0 | 0 |
| **all** | **224** | **140** | **0** | **0** | **0** | **84** |

**Everything else** (182 modules): the most open are shell32 (14), reg.exe (12), dbghelp (4), msi (4), bluetoothapis (4), qasf (4), win32u (4), sapi (3), shlwapi (2), psapi (2), cfgmgr32 (2), ieframe (2), wbemprox (2), cmd.exe (2), regedit.exe (2). Every module: [CONFORMANCE_TABLE.md](CONFORMANCE_TABLE.md).

Open: the files holding a reopened cause (below), ntdll exception PE64 (an access violation in test_continue, below; PE32's
timeout is qualified: the console's 64-bit code selector), the completion races, threadpool (skipped: froze the whole title once; to be reduced
first), ntdll info and wow64 (PE32); process and thread (timeouts:
longer budgets in tests/wine-tests-timeouts-ps5.json, patch 2084 for thread PE32), pipe and fiber (skipped),
and volume and ntdll wow64, which crash on the PC's oracle too.

### Causes

Every console-only line is assigned to one cause by `tools/qualify-wine-tests.py`, whose rules name the test lines
and the measurement; what no rule covers stays a failure, and so do an `OPEN:` rule and a deferred bug. Lines are those
of the expected files of every module paired so far, both builds.

**Reopened by the owner as deferred bugs (counted open; fixed after the sweep, in this order):**

| Cause | Lines | Where it shows | The fix |
| --- | --- | --- | --- |
| 16 KiB host pages: a 4 KiB page cannot be protected, unmapped or tracked alone (247 open since patch 2088; groups, options and order in [HOST_PAGES.md](HOST_PAGES.md)) | 283 | ntdll virtual 2547-2593, 445; kernel32 virtual (write watch, Read/WriteProcessMemory across no-access pages), loader (an image's last host page), file (I/O into a no-access page); kernelbase placeholder split; executing a no-access 4 KiB page (PE32, patch 2075) | every 4 KiB protection (NOACCESS, GUARD, READONLY, EXECUTE), unmap and placeholder tracked in software, as patches 1122 and 1134 do for writes in every view but images (docs/HOST_PAGES.md); a game's load measured before and after |
| A change below a subtree watch's top directory found only by polling | 10 | kernel32 change 621-628 (both builds), 81 (a notification thread's 100 ms wait; the record does not name the step) | a kqueue watch per subdirectory of a subtree watch, within the descriptor budget (patch 2091 watches the top directory) |
| A reparse tag set on a directory that holds a file | 2 | ntdll file 6694 (both builds) | fixed by patch 2098: the console's server found the directory not empty (STATUS_DIRECTORY_NOT_EMPTY); the PC's Wine passed only because is_dir_empty's dup() shares the descriptor's offset, so its second look at the directory starts at the end |
| No extended attributes: a set access time | 6 | ntdll file 1520; kernel32 file 5873, 5883 (both builds) | the console's file system ignores a set access time (set 0x200deadcafebeef, read back the current time); emulable in the same store, no program known to need it |

**Awaiting the owner: qualified by a choice of ours** (why the choice is needed; the lines it costs):

| Choice | Lines | Where | Why it is needed |
| --- | --- | --- | --- |
| Patch 1004, simulate_writecopy for every process | 2,208 | kernel32 loader 1821, virtual 4218-4315 | CEF renderers ended without it; a PAGE_WRITECOPY page reads back PAGE_READWRITE once protected, as Windows reports a page already copied |
| Patch 1120, a new thread's guard below its committed top | 126 | ntdll virtual 1078-1234 | an emulator that pre-grows its stack (as on Windows) |
| No NTLM (Samba's ntlm_auth helper not on the console) | 44 | rpcrt4 rpc and server, secur32 negotiate and ntlm, winhttp | feature not built: a program asking for NTLM or Negotiate gets SEC_E_SECPKG_NOT_FOUND or a 401 |
| Plug-and-play off (docs/STEAM.md) | 22 | dinput device8, force_feedback, hid (unfinished), hotplug, joystick8 | plugplay.exe costs 19 MiB of the 990 MiB budget |
| Event log service off (docs/STEAM.md) | 10 | advapi32 service | about 15 MiB per Wine system process |
| Patch 0947, a WoW64 thread's 32-bit TEB in the low half of its thread pointer | 9 | ntdll info 688, wow64 939-942 | 32-bit code needs no FS switch: system calls 2x cheaper |
| The platform's own Windows threads, heaps and FLS slots in every process | 6 | kernel32 heap 224, loader 3386 | the platform layer's threads run in every process |
| The probe prefix's 32-bit drivers in syswow64\drivers (stage-wine.py) | 4 | ntdll directory 1350-1356 (PE32) | the test prefix, not a Wine-made one |
| No capture endpoint in the audio driver | 4 | dsound duplex 284-288, capture (crashes the same way on the PC) | feature not built: the DualSense microphone is not offered |
| A fixed-base 32-bit image moved off 0x400000 | 3 | ntdll wow64 292 | Sony's carrier eboot sits at 0x400000 in a helper |

**Qualified (platform or environment; shown for the owner to see):**

| Cause | Kind | Where it shows |
| --- | --- | --- |
| No ptrace: hardware breakpoints never fire, no DEBUG_PROCESS children | platform limit | ntdll info 2135-2824, 4869-4900; exception single-step and breakpoint counts, debuggee children |
| No hard links (`link()` refused to a title) | platform limit | ntdll file 2489-3287; kernel32 loader 1534, file 6360-6378 |
| I/O privilege level 3 in titles: `cli` does not fault | environment | ntdll exception 3354 |
| FreeBSD selectors (fs 0x13, gs 0x1b) differ from Wine's reported constants | cosmetic | ntdll exception 3377-3399, 2014 |
| FreeBSD reports an x87 stack fault as FPE_FLTSUB | cosmetic | ntdll exception 1721 (PE32) |
| No console host window: a console-subsystem child gets no console of its own | platform limit | kernel32 console 5391-5402 |
| File times coarser than two quick writes (observed) | platform limit | kernel32 file 4778 |
| Creating a name costs the console's file system ~20 ms (measured natively) | platform limit | ole32 storage32 needs a 900 s budget |
| Wine's xinput answers a connected pad's battery and headset queries with ERROR_NOT_SUPPORTED; only the console has a pad | environment | xinput1_3 235-282 |

### Fixed in the shared layer

- **The mount manager on the console** (tools/build-wine-ps5.sh): mountmgr.sys's Unix side was never built as a PRX,
  so winedevice could not start it, and no drive had its `\??\C:` device: QueryDosDevice, NT and GUID paths and volume
  names failed in every program. It is now linked through ntdll's path and descriptor wraps (its dosdevices links are
  virtual on the console). Console: ntdll file.c:350/359 pass in both builds.
- **Debug registers read back** (patch 2083): a thread's NtGetContextThread with CONTEXT_DEBUG_REGISTERS returned the
  caller's buffer unchanged (0xcc) whenever Dr7 was set, in 64-bit and 32-bit code: the console's server cannot read
  them. Anti-debugging code reads that as a debugger. Console: exception.c:9241-9246 pass (PE64).
- **A new 32-bit thread's activation context** (patch 2084, built, not yet on the console): kernelbase activated the
  creator's context in the new thread's stale 32-bit TEB (wow64 assumed teb+0x2000; patch 0947 moves it).
- **Audio events after Stop** (wine/wineps5/unix.c, built, not yet on the console): an event-driven stream that has been
  started is signalled every period until it is released, stopped or not, as Windows does (mmdevapi render.c:947-961,
  "Still receiving events!"); the driver stopped signalling, and a program waiting on the event after Stop would hang.
- **Spatial audio** (src/pw_audio_mix.h, built, not yet on the console): the mixer takes every speaker position (18
  channels), so ISpatialAudioClient's 7.1.4 bed (12 channels) activates (spatialaudio.c:184, 0x88890008); host-tested.
- **A block cipher without an IV starts from zeros** (patch 2100, gate B): bcrypt starts a GnuTLS cipher with no IV
  when the program gives none, and emulates ECB as CBC with an empty IV, one block per call. The console's GnuTLS
  3.8.9 leaves such a cipher with no IV size, its CBC passes that size to Nettle 3.10.1 as the block size, and
  cbc_decrypt's assert divides by zero: a SIGFPE in the Unix side (libgnutls.so+0x10e9a7, nettle cbc.c:72; traced
  with +seh, bcrypt-seh), which ended bcrypt's test (x86_64) or hung it (i386). The same pins built on the PC
  reproduce it (an AES-128-CBC cipher started without an IV: SIGFPE on the first decrypt; with a zero IV of one
  block: 0); the PC's GnuTLS 3.8.13 does not. A CBC, ECB or CFB key with no IV now starts from a zero IV of one
  block, Windows' state for a fresh key (bcrypt.c:1795 continues the chain from there). On the console (b-bcrypt)
  x86_64 completes (23,274 checks) and i386 runs on to test_DH, where both builds meet the next gap (Open).
- **The descriptor-table instructions run, as on Windows and Linux** (patch 2102, src/pw_umip.h): the console's CPU
  runs with CR4.UMIP and its kernel leaves the #GP(0) to the program, so SGDT, SIDT, SLDT, SMSW and STR ended any
  program that ran them with an access violation reading 0xFFFFFFFFFFFFFFFF; a store game's anti-tamper `sgdt [rsp]`
  ended it about 40 s in on the base PS5 (2026-10-27). ntdll's fault handler now has the instruction decoded (the ISA
  table gained the five rows) and stores what Linux stores for SGDT, SIDT and SMSW and what Windows stores for SLDT and
  STR, then goes on after it; anything else faults as before. Probe tests/umip-probe.c (gate cases umip32/umip64): on
  the Pro both builds pass every check, the read-only destination raising its write fault (umip64-probe-02,
  umip32-probe-02). The game itself is the proof still to come.
- **A live process without a Unix loader address is still live** (patch 2101, gate B): dbghelp's check_live_target
  refused a process whose PEB carries no Unix loader address, and on the console it is always 0 (no getauxval, no
  ELF loader). So SymInitialize with fInvadeProcess failed for every live process ("failed to initialize process",
  minidump-trace) and with it every MiniDumpWriteDump, which a game's crash handler calls; the test then read the
  empty dump and crashed. With no address the process is live with its Windows modules only, which dbghelp already
  does when the address cannot be read. On the console (b-minidump) dbghelp minidump completes with no failure line
  in both builds (1,101 checks), where both crashed. A clean rerun without tracing (clean-minidump, 2026-10-27)
  is the same; the two files are counted fixed (evidence/wine-tests/fixes.json).
- **A directory reparse tag on a directory that holds files** (patch 2098, build B21): FSCTL_SET_REPARSE_POINT
  refused every directory that held a file (STATUS_DIRECTORY_NOT_EMPTY); Windows 10 1607 and later accept a tag with
  the directory bit there (IsReparseTagDirectory) and refuse the others, which ntdll file.c:6694 and 6655 check. The
  PC's Wine passes 6694 by accident: server/fd.c is_dir_empty() reads a dup() of the directory's descriptor, which
  shares its offset, so after 6655's look a second one on the same handle starts at the end and finds nothing (a host
  test of the function: 0, then 1, the file still there); the console's libc reads it from the start. The tag with the
  bit is now accepted, as on Windows. On the console (b21-file) ntdll file went from 44 lines to 43 in both builds,
  with 7 more checks run (2,929 and 2,939: the tag's read-back after 6694), and gate-2098-2026-10-26-01 passed 102 of 102.
- **A client the server cannot reach does not end the title** (patch 2099, build B20): get_handle_fd sends the
  descriptor with send_client_fd, which kills a client it cannot reach (EPIPE: a process that is ending) and its
  requesting thread, so `current` is NULL afterwards; patch 2076's lease then read `current->process`, and the title's
  in-process server faulted at 0x88 (req_get_handle_fd+0x12f, from main_loop), ending every program with it. Seen once,
  in the milestone sweep on 2026-10-26 during i386 ntdll file; reachable since 2094 lets debugged children start
  beside 2076's lease, not through the fmem merge. The lease is now taken only while the thread is alive. A race, not
  reproduced on demand: on the console (b20-file) ntdll file completes in both builds (2,922 and 2,932 checks, 44
  qualified lines each), with no fault, and gate-2099-2026-10-26-01 passed 100 of 100.
- **A cancel runs its own results before it returns** (patch 2095, build B19): the server ends a thread's own asyncs
  at once when they are cancelled and queues their results to the thread as system APCs, and the signal that runs them
  reaches a helper's thread through its agent, after the server's reply. So NtCancelIoFileEx returned with the
  cancelled read's status block unwritten (ntdll pipe.c:654, 0x55555555). After a cancel that ended its asyncs at
  once, the thread now runs its pending system APCs (the zero-timeout select NtTerminateThread uses). On the console
  (b19-pipe) ntdll pipe went from 1 and 2 lines to 0 and 0, and kernel32 pipe from 4 and 2 to 0 and 0 (2,428 and
  38,459 checks). Two of the lines gone were not targeted: i386's pipe.c:387 (a listen completed by the client's
  connect) and kernel32's WaitForSingleObject timeouts (pipe.c:2621, 3085, 3119); one run, so they are observed,
  not proven, gone.
- **A child to be debugged starts** (patch 2094, build B18): NtCreateUserProcess refused a child with a debug object
  (DEBUG_PROCESS) with STATUS_NOT_SUPPORTED, a refusal from the first experimental child loader that nothing had
  measured since. Wine's debug events come from the server and the debuggee's own ntdll, not from ptrace. kernel32
  process created such a child and waited in WaitForDebugEvent(INFINITE), so the file ended at its 900 s budget in
  both builds. On the console (b18-debug):
  - kernel32 process completes in about 71 s: 3,425 checks, 12 lines (3,410 and 12 in i386);
  - kernel32 debugger, skipped until now, completes in about 60 s with 29 and 27 lines, of which the PC's Wine has 59
    and 37 of its own, so it is back in the runs;
  - ntdll exception i386 went from 37 lines to 27 (the debuggee children's error 50).

  Left open in kernel32 process: process.c:1551-1564, the startup information a debugged child reports (cb 0, no
  desktop or title, flags and window counts wrong), both builds.
- **A copy into or out of a process honours each 4 KiB page** (patch 2097, build B17, HOST_PAGES.md group D): the
  helpers' memory service copies whole 16 KiB host pages, so a no-access or read-only 4 KiB page beside an accessible
  one was read or written through. A read of the process itself now checks its source page by page (patch 2088's
  check); a copy through the server asks the target which pages allow it. A read that reaches a page it may not read
  copies nothing; a write writes the part it may and returns STATUS_PARTIAL_COPY, as on Windows. A first version
  refused execute-only pages to reads, and kernel32 loader.c:2041 (an image's PAGE_EXECUTE section) failed 3 times in
  each build; reads now take any committed page that is not no-access or a guard. On the console (b17b-copy) kernel32
  virtual went from 1055 to 1050 lines in both builds (virtual.c:244, 256, 4536-4538), kernel32 loader kept 102 and
  99, ntdll virtual kept 85 and 69, and remote-memory-probe passes (remote-memory-b17). Left of group D:
  loader.c:4149-4152 (x86_64), a read of a process that has called ExitProcess, which is about a dying process, not
  host pages.
- **An image's host-page tail is free** (patch 2096, build B16, HOST_PAGES.md group A): VirtualQuery just past an
  image reported the rest of its last 16 KiB host page as reserved, because the native query finds the image's own
  mapping there; Windows reports it free, and nothing can be allocated there on Windows either (below the next 64 KiB
  boundary). On the console after a restart (b16-tail-073802, pipe memory fresh), kernel32 loader went from 158 to
  102 lines (x86_64) and from 155 to 99 (i386): the 112 lines of loader.c:942-946, and no other line moved; ntdll
  virtual (85 and 69) and kernel32 virtual (1055) kept their B11 counts.
- **Written, not yet gated** (the console needs a restart, above):
  - tests/continue-probe.c round E (test_debug_service's `int $0x2d` breakpoints, then NtContinue), for the PE64
    exception crash; it passes on the PC.

  Each gets its files rerun and a gate of its own after the restart. Console use is on hold meanwhile: the owner is
  copying games to both consoles (2026-10-26).
- **An exception at the bottom of a deep stack is dispatched** (patch 2093, build B14): kernel32 heap ended in both
  builds at GlobalSize(0xdeadbee0). The read faults on the PC as well, and kernelbase catches it there; on the console
  the dispatcher refused: "Exception frame is not in stack limits", frame 0x100018f840 against StackLimit
  0x10001f8000. Patch 1120 puts a thread's guard page below its committed top, as Windows does, and leaves the pages
  below committed. ntdll's `__chkstk` does not probe, so test_GlobalAlloc's 512 KiB of locals ran past the guard with
  no fault, and StackLimit stayed above it. An exception whose frame lies below StackLimit on a committed page now
  moves the guard and the limit down to it, as a probe would have. On the console:
  - tests/stack-limit-probe.c failed before the patch in its "skipped" case (one 512 KiB frame entered with no probe,
    stack-limit64-2026-10-26-02) and passes after it (-03); it is now gate case stack-limit64;
  - kernel32 heap completes in both builds (278,899 and 278,915 checks, b14-heap). The one line left is 224, the
    platform layer's own heaps; i386's three memory-status lines match the PC.
- **Reparse points and symbolic links work** (patch 2092, build B13): Wine keeps a reparse point as the file renamed
  to `name?` with its data in user.WINEREPARSE, set by the server; the server's attribute calls now use the store
  patch 2090 gave ntdll, and ntdll reads the reparse data from it. On the console (b13-reparse) ntdll file, ntdll
  directory and kernel32 file went from 404 failure lines to 110 over both builds (ntdll file 159/160 to 44/44,
  directory 6/10 to 0/4, kernel32 file 36/33 to 9/9); the extended-attribute cause went from 327 lines to 34, and no
  other line was added. Of the reparse test's lines left, 26 are its hard links (no link() for a title) and 2 the
  non-empty directory above.
- **Directory changes are seen at once** (patch 2091, build B12): patch 0902 polled a watched directory at best
  every 25 ms, so a change was late, or lost when two came between lists. The server makes every name change itself
  (create_file, a delete on close, a rename) and its own loop already runs on kqueue, so a watched directory's
  descriptor is also put in one kqueue (EVFILT_VNODE), and after each request the server lists again each directory
  it says changed, before the reply. On the console (b12-kqueue-033549; the server logged "directory changes by
  kqueue and polling") ntdll change went from 5 lines to 0 in both builds and kernel32 change from 14 to 5, with 333
  checks run instead of 331 (38 lines to 10). What is left is below a subtree watch's top directory, still polled.
- **DOS attributes are kept** (patch 2090, wine/ps5/pw_wine_xattr.h, build B11): the console's file system has no
  extended attributes, where Wine keeps a file's hidden and system attributes, so they were lost at once. ntdll's
  xattr calls now keep them in a store of the prefix (config_dir/.pw-xattr), by device and inode, so they follow a file
  through a reopen and a rename; a newly created file starts with none, so a reused inode brings nothing. A file with
  no attribute costs no file call while the store is quiet. Reparse points stay unsupported for now: Wine makes one
  by renaming the file to `name?` and keeping its data in user.WINEREPARSE, set by the server (server/fd.c
  set_reparse_point), whose calls do not reach the store yet; ntdll reading the store alone resolved to garbage
  (kernel32 file.c:6773), so patch 2090 leaves that name out. The 327 lines left split as 321 reparse points and
  symbolic links (fixable in the server) and 6 set access times (2026-10-26, by test function). On the console (b11b-xattr-024602) the extended-attribute lines still open went
  from 355 to 327; host test test_pw_wine_xattr (gcc and clang, ASan and UBSan).
- **A buffer check honours each 4 KiB page's protection** (patch 2088, build B9): virtual_check_buffer_for_read and
  _for_write touched a buffer to probe it, which the console's kernel judges by its 16 KiB host page, so ReadFile into
  a no-access 4 KiB page, WriteFile from one and Read/WriteProcessMemory across one succeeded. Where a host page's four
  pages differ, the check now goes page by page under the virtual mutex; where they agree, nothing more is done. On
  the console kernel32 file.c:3520-3600 and virtual.c:150-175 no longer fail in either build (b9-k32-004658), group 1
  kept its 87 matching files (b9-core-004806), and the 16 KiB lines still open went from 283 to 247. The 2D game's start
  (`tools/start-timeline.py`, frames steady from): 82 s on B8 (the first run after a restart, cold), 46 s and 27 s on
  B9 (warm): no slowdown seen, but the runs measure the warm-up, not the check.
- **A monitor DPI of 0 no longer ends the title** (patch 2086, build B4, commit 3dd565f; on the console the 16 Direct3D
  files that had ended the title three times ran in one session to its normal end): the server kept the 0 a
  client sends when it cannot find a window's monitor, and the next DPI conversion divided by zero in scale_dpi; the
  server runs in the title, so every program ended (SIGFPE in scale_dpi from call_req_handler, B3c's Direct3D session
  with dxgi and d3d9ex running, 2026-10-25).
- **Boolean socket options** (patch 2085) and **service names** (wine/ps5/pw_ws2_32_libc.c: http, https and 20 more,
  the console has no services database), built, not yet on the console.

### Installer slice

`tests/installer-slice-probe.c` runs one Tier 1 installer kind end to end in one launch, in an owned throwaway game entry
and prefix (`tools/run-installer-slice.py console`, removed afterwards; the owner's prefixes are never used), and the
same program on the PC's display-capable Wine is the oracle (`host`). Per kind, each step timed: a snapshot of what an
install can touch (both Program Files, the common Start menu and desktop, the top of system32 and syswow64,
Windows\Installer, uninstall/fixture/runtime registry keys in both views); a silent install (a run past its budget is a
hung prompt and is ended); files, registry, shortcuts and, for redistributables, DLLs that are no longer Wine's own;
the installed program (exit 77); a reinstall or repair with the same checks; the uninstall; a clean prefix against the
snapshot. Fixtures: `tests/installers/` (pwapp, fixture.nsi/.iss/.wxs, a Steam app with installscript.vdf),
`tools/build-installer-fixtures.sh` (makensis, wixl, Inno's ISCC on the PC's Wine), `tools/fetch-installers.py`
(Inno Setup 7.1.0, VC++ 2015-2022 x64/x86, DirectX June 2010, pinned by URL, size and SHA-256, never committed).
The Steam kind has no Steam: the slice's own installscript runner (Registry, Run Process with HasRunKey, Run Process On
Uninstall) stands in for SteamService.exe /installscript, which the console replaces with hostname.exe today.

| Kind | Console | PC | Console-only | Install / reinstall / uninstall, console vs PC |
| --- | --- | --- | --- | --- |
| Inno Setup 7.1.0 fixture | 25/25 | 25/25 | none | 2.7 / 2.6 / 1.9 s vs 0.36 / 0.34 / 0.18 s |
| NSIS 3.13 fixture | (pending) | 25/25 | | |
| MSI (wixl) | (fixture needs msitools) | | | |
| Steam install script | 23/23 | 23/23 | none | 0.6 s vs 0.01 s |
| VC++ 2015-2022 x64+x86 (Burn) | 21/22 | 21/22 | none: the uninstall removes the 24 runtime DLLs Wine had placed, on both | 11.3 / 8.8 / 9.5 s vs 1.4 / 1.2 / 1.2 s |
| DirectX June 2010 (DXSETUP) | 18/18 | 18/18 | none (no uninstaller) | 102 / 96 s vs 17 / 14 s |

Evidence: `evidence/installer-slice/`. DXSETUP's 6-7x is a speed difference to probe once correctness is done.
`installers-fast` (Inno and Steam) is a battery case.

### Platform stress and cost slice

`tests/handles-paths-probe.c`: 3,000 files held open (the console allows a title about 256; the platform parks the
rest and reopens them on use) and 6,000 events together; each file read back through its handle, every 4th renamed
while open and used through the old handle, every other 7th deleted on close; a 20-level path past MAX_PATH made,
opened, listed and removed in the other case. PASS on the PC and the console, both builds. `tests/pipe-cancel-probe.c`:
pending overlapped pipe reads cancelled (CancelIoEx, two on one OVERLAPPED, with APCs) leave the pipe working: PASS on
both, both builds. `tests/path-limit-probe.c`: the console refuses the 34th Unix path component (27 levels under C:\,
305 Windows characters; the PC made 89, 997 characters); not located (not pw_wine_cwd, not PATH_MAX, not the name
caches): OPEN. Descriptors past 4096 and memory near the guard keep their own cases (relative-dir32/64, memory-guard).

Costs per call, console against the PC's pinned Wine ([evidence](../evidence/wine-tests/platform-stress-2026-10-25.json)):

| Operation (64-bit) | PC | console | x |
| --- | --- | --- | --- |
| ReadFile 4 KiB | 0.54 us | 6.30 us | 11.7 |
| SetFilePointer + ReadFile 512 B | 0.63 | 6.76 | 10.7 |
| CreateFile + ReadFile 4 KiB + CloseHandle | 33.3 | 126.8 | 3.8 |
| GetFileAttributes, a file there | 3.79 | 17.94 | 4.7 |
| GetFileAttributes, missing / other case | 137 / 167 | 52 / 64 | 0.4 |
| NtYieldExecution | 0.68 | 21.11 | 31 |
| CreateEvent + CloseHandle | 10.6 | 40.7 | 3.8 |
| SetEvent + Wait + ResetEvent | 0.93 | 1.37 | 1.5 |

Names are what cost (`tests/lock-probe.c`, 2026-10-25, per call; 64-bit, 32-bit alike):

| Operation | PC | console | x |
| --- | --- | --- | --- |
| CreateFile, a new file | 41 us | 10,881 us | 265 |
| MoveFileEx | 71 | 1,319-7,071 | 19-100 |
| DeleteFile | 30 | 1,428-1,860 | 48-62 |
| CreateDirectory | 49 | 14,212-15,923 | 290-325 |
| RemoveDirectory | 39 | 5,637-10,730 | 145-275 |
| GetTempFileName (create, close, delete) | 50-100 | 12,000-16,000 | ~200 |
| A transacted storage open (StgOpenStorage) | 0-1,000 | 47,000-76,000 | ~50+ |
| LockFileEx + UnlockFileEx; 512 B write + read; FlushFileBuffers | 10; 0; 0 | 40-160; 700-800; 700-950 | |

Byte-range locks behave as on the PC (taken, refused, freed with their handle, shared). ole32's storage32 runs past its
budget on the console (2 s on the PC) because every transacted open makes a scratch file. The file system is the cost:
natively (`tools/console-iobench.py --only names`, no title running) open(O_CREAT|O_EXCL) takes 19-20 ms, mkdir 23 ms,
rmdir 4.4 ms and unlink 1.2 ms in /data, /user/data and the prefix's Temp alike, so Wine adds nothing
([evidence](../evidence/wine-tests/file-names-cost-2026-10-25.json)). A platform limit: storage32 gets a 900 s budget.

### Gate

`tools/gate.py run --session` (run-battery.py `--grouped --session`) runs the gate's 54 grouped probes in the
persistent test session, 4 at a time, instead of one launch per group running them one after another. A probe that
touches the display, the desktop, the theme, the pointer, fonts, input or the shell, or that measures or fills memory,
runs alone, and so does every GPU broker client (`SESSION_SERIAL`); the 32-bit and 64-bit builds of one probe never
run together (they share fixed paths). Each case is judged by its exit status and its marker's verdict in its own
output, or in its own Windows process's OutputDebugString records in the helper logs (wt-session appends the process
id), and by everything run-console checks of the session's launch. Every case runs on every gate, on exactly the build
being committed.

Measured on one build (B6, fingerprint c05dc890, 2026-10-25), same verdict for every one of the 98 cases:

| Mode | Gate (wall) | The grouped probes |
| --- | --- | --- |
| one launch per group (`gate-b6-grouped-2026-10-25-01`) | 1,869 s | 192 s (two launches) |
| persistent session, 4 at a time (`gate-b6-session-2026-10-25-01`) | 1,752 s | 126 s (one launch) |

The other 44 cases keep a launch each (about 1,620 s): each tests the launch, the teardown, a fresh prefix (186 s
each), the desktop, game mode, or an environment the title itself reads (locale, native signals, stdout), which a
session cannot give one case. No Wine test group matches the PC yet, so none is a gate case yet.

### Open

- Diffie-Hellman and ECDH in bcrypt: not built ("Compiled without DH support"). Wine's bcrypt does DH through the
  system GMP (HAVE_GMP_H, SONAME_LIBGMP), and the console build has only Nettle's mini-gmp; ECDH answers
  STATUS_NOT_IMPLEMENTED (bcrypt.c:3286-3301, 4391-4444, 4888-4893, both builds) and i386 ends in test_DH
  (3638-3651). A program that agrees a key this way (some TLS and launcher handshakes) fails. Next: a GMP for the
  console's Unix side, pinned like Nettle and GnuTLS, or DH over mini-gmp.
- A Traditional Chinese font request falls back to Arial (user32 and comctl32 edit, "Tahoma for charset 136 isn't
  available", both builds). Proton's msyhbd.ttf (Source Han Sans Bold) names itself "Microsoft YaHei", style
  Regular, weight 700, version 2.005, and Wine keeps one face per full name, the newer: it replaces msyh.ttf (0.2) as
  YaHei's regular face. So every YaHei text drew bold, and the only font declaring Big5 (code page 950) was gone.
  Reproduced on the PC with the console's fonts and no system fonts (FONTCONFIG_FILE empty, a probe printing the face
  and tmCharSet): "Tahoma", "Arial", "MS Shell Dlg", "PMingLiU" with charset 136 gave Arial with charset 0; without
  msyhbd.ttf all gave Microsoft Yahei with charset 136, and user32 edit's two skips go. On the console (a-edit,
2026-10-27, gate A's build) user32 and comctl32 edit have no skip left in either build (263,266 and 267,342 checks).
Fixed:
  tools/stage-wine.py leaves msyhbd.ttf out (LEFT_OUT_FONTS) and writes Fonts/.pw-retired; wine/ps5/pw_wine_prefix.c
  removes the retired names from every existing prefix at its next update (tests/test_pw_wine_prefix.c);
  tools/deploy-wine.py removes runtime files the last deploy wrote and this build no longer ships. Players' runtimes
  updated by the title's installer keep the file until that path removes it too (self-installer).
- AddFontResourceEx failed on the console for every font file a program writes itself (gdi32 font.c:2118, 5807-6131,
  6300-6353, 7080-7419, both builds: 86 console-only lines), while on the PC with the console's fonts only every one
  loads. Cause: win32u opens a font file itself by its Unix name (freetype.c unix_face_create, then FreeType gets the
  bytes), which Wine gives through dosdevices/c:, a link that exists only in the PS5 path layer
  (wine/ps5/pw_wine_cwd_libc.c: a title cannot make a symbolic link), and win32u was linked without that layer. The
  prefix's own Fonts folder failed the same way; its fonts loaded only from the runtime's share/wine/fonts copies,
  a real path. A game adding the fonts it ships, from its own folder on a drive letter, failed too. Fixed in
  tools/build-wine-ps5.sh: win32u is linked with ntdll's path wraps, as mountmgr is, including the OpenGL link
  branch, which had dropped link_options. On the console (a3-font, 2026-10-27, gate A's build) gdi32 font went from
  188 failure lines to 97 in both builds (90,907 checks to 92,607) and no font addition fails; user32 and comctl32
  edit stay clean.
- user32 menu's owner-drawn items are never measured (menu.c:717-744, both builds); the PC with the console's fonts
  only does the same, so it rests on the font set, not on the console's Wine. cursoricon.c:758 likewise.
- msi is slow, not hung: install and action take 4-6 s on the PC and 171-227 s on the console (x86_64 install and
  action and i386 install at their budgets, i386 action complete at 227 s; sweepm2, 2026-10-26). Every install starts
  a custom-action server per architecture (`msiexec.exe -Embedding <pid>`), as Windows does: 96 of them for the 36 msi
  test processes, each one a helper start on the console. A game's redistributable installers (MSI packages in
  a store client's first-run scripts) pay the same. Next: measure one helper start's cost with the helper logs kept
  (docs/CONSOLE_QUEUE.md), then make the start cheaper for every program, not msi alone.
- OpenGL: opengl32 runs to its end on the console in both builds since Windows Zink at PS5_Mesa a8800d9 (graphics
  patch 0034; zink-proof, 2026-10-27: 10,525 checks each, 1,126 and 1,129 failure lines against the PC's 1,028 and
  1,031 of 11,442). 50 lines per build are the console's alone and not yet sorted. The two stops before it are fixed:
  SwapBuffers with no context (test_window_dc; PS5_Mesa e7c317df9c11, patch 0033) and test_make_current_read
  (a swapchain source left to blit_native, and a context current on another thread refused with ERROR_BUSY).
- Vulkan: vulkan-1's test ended on the console in test_win32_surface, both builds (clean-crash, 2026-10-27). Traced
  (vulkan-trace-111334, +vulkan): the helper's broker WSI printed "gpu-wsi rejected extent=201x160 count=2 format=44
  mode=0": broker_create (PS5_Mesa ps5/broker/radv-gpu-broker-wsi.patch, applied by the graphics foundation) refused
  every present mode but FIFO, and the test names IMMEDIATE, as a Windows driver allows; it then used the swapchain
  it did not get. Graphics patch 0035 accepts any mode and presents it as FIFO, as the chain already did. On the Pro
  (vulkan-present-mode, 2026-10-27) both builds now complete, 496 checks each. The present rectangle was the
  display's (vulkan.c:767: every surface is a display-plane surface, patch 0460); patch 2150 reports the window's
  client area, as the surface capabilities do. On the Pro (vulkan-present-rect, 2026-10-27) both builds have no
  failure line: i386 matches the PC; x86_64 differs only in a skip. The console's device has no external-memory
  extensions (vulkan.c:1333 and 1545, "Failed to create device with external memory extensions, VkResult -7"), which
  is also why it runs 496 checks against the PC's 1,010; the PC skips present timing instead (vulkan.c:510).
- Direct3D 9 focus loss: d3d9's tests take the focus from a fullscreen device with
  SetForegroundWindow(GetDesktopWindow()) (device.c:5234 and 12142, d3d9ex.c:3629), which failed on the console only.
  A probe (fgprobe-pro-01, 2026-10-28) showed why: no explorer owns the PS5 desktop window (the server creates it
  detached, patch 0410), and the server refuses a window without a thread as the foreground. Patch 2151 makes the
  thread-less desktop the foreground for the caller. That exposed a second difference: with no clip the cursor clip
  is the whole physical screen, and inside an emulated 640x480 mode it read (-107,0)-(747,480) (device.c:14950);
  patch 2152 keeps GetClipCursor inside the virtual screen. On the Pro (2026-10-28, build 2bf8276e) the console-only
  lines went from 63 to 51 (x86_64) and 50 (i386) in device and from 42 to 32 in d3d9ex in both builds. One new line
  in x86_64 (device.c:6727, an occlusion-query count) belongs to the query test whose neighbouring line 6780 already
  reads different values from run to run. test_device_window_reset crashed with 0xc0000005 on the console only:
  a Reset from fullscreen to fullscreen onto a new device window built DXVK's D3D9WindowMessageFilter for a window
  DXVK never hooked, and the filter dereferenced its map's end() (d3d9-device-seh-pro-01: d3d9.dll+0x8968f, a read
  at 0x19). The PC never reaches that Reset: its fullscreen device fails to create at 3440x1440 (device.c:5946,
  skipped). DXVK patch 0006 skips the filter for an unhooked window; on the Pro (build 5105ced8) the function runs
  to its end with no failure line in both builds. The crash also left its window class registered, so a batch that
  ran d3d9ex after device in one process failed d3d9ex's RegisterClass. i386 device needs more than the 60 s default
  file limit (80-120 s): run it with --file-timeout 240. With the PC's display awake (2026-10-28 morning, same
  counts as the evening before) d3d9 has 136 console-only lines in each build: device 49, d3d9ex 32, visual 55.
- Direct3D 11: 233 console-only lines in each build (2026-10-27) were mostly the oracle's. Wine's d3dcompiler_47
  loads wined3d, and d3d11's tests read wined3d's Vulkan renderer as "damavand", which turns 228 todo checks of
  test_clear_buffer_unordered_access_view (d3d11.c:16937-16949) into plain checks. Every console program gets that
  renderer (WINE_D3D_CONFIG=renderer=vulkan, native/proton_wine.cpp); the PC's runs had wined3d's default, OpenGL.
  tools/run-wine-tests.py now gives the PC's Direct3D runs the console's setting. DXVK still draws on both sides (a
  probe, d3d11which-pro-01: DXVK's d3d11 and dxgi load, wined3d does not, until d3dcompiler_47 is imported). Three
  more lines were a real crash: a texture with D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX (test_create_texture1d, 2d,
  3d) called address 0, because DxvkFence asked vkGetSemaphoreWin32HandleKHR, null without
  VK_KHR_external_semaphore_win32, for the handle of a semaphore it could not export. DXVK patch 0005 asks for no
  handle then, as 0003 does for images. On the Pro (2026-10-28, build 7970d8a5) d3d11 leaves 2 console-only lines
  in each build: VSInvocations reads 5 for a 4-vertex quad (d3d11.c:6392 and 6415), the console's AMD GPU against
  the PC's NVIDIA one. evr stays skipped on the console (its GPU page fault, tests/wine-tests-skip-ps5.json). The
  PC's dxgi oracle needs the owner's word (its tests switch the real display's modes). While the PC's display was
  asleep (Xwayland reported a 0x0 screen) its d3d9 visual run stalled at test_texture_blending and its device and
  d3d9ex results moved by 21-27 window and focus lines; with the display awake they match the evening's again. Take
  the PC's Direct3D oracles with its display awake.
- A module whose Unix side the console does not ship crashes on its first call when its Windows side ignores the
  failure: winscard (no pcsclite; "no pcsclite support, expect problems") and wpcap (no libpcap) both builds. The
  call faults in __wine_unix_call_dispatcher (ntdll.prx+0x39aae) and returns STATUS_ACCESS_VIOLATION, which the
  module then uses anyway (crash-seh). Upstream Wine without those libraries does the same. 18 modules have no Unix
  side on the console; amd_ags_x64, which games load, guards every call behind a successful __wine_init_unix_call
  and is not affected.
- Cancelled or closed asyncs complete after the call returns: a cancelled pipe read's I/O status (pipe.c:387, 654,
  1363) and a closed pipe's completion packet (ntdll file.c:1265). Hypothesis: a system APC waits for the thread's
  next wait on the console instead of interrupting it.
- user32 raw input: GetRawInputDeviceInfo refuses the handles GetRawInputDeviceList returned (input.c:1997-2064);
  SDL and other controller code enumerate devices this way.
- The 34th Unix path component, the AVX state of a suspended 32-bit thread (exception.c:10744, 11786).
- ntdll exception PE64 ends in test_continue's first call, not after test_debug_registers. Traced on B13
  (b13-exc64seh-052017, +seh), the test generates an unhandled access violation at its own code's `pop %r14`
  (code_mem+0xf3), reported as a write at r8, which holds NtContinue's address, with the direction flag set
  (eflags 0x10602), right after the SIMD test's exceptions. The same with the 4 KiB MMU off (WINE_PS5_MMU4K=0), and
  with no fault, protection change or code conversion between the SIMD test and the crash (+virtual). The test's
  bytes alone pass on the console (tests/continue-probe.c, continue64-2026-10-26-01 to -04) in four rounds:
  - as they are;
  - after the debug registers test_debug_registers leaves (Dr7 0x400, Dr0-Dr3 and Dr6 0xffffffff);
  - copied over other code that ran from the same memory;
  - after a SIMD invalid operation continued past its instruction.

  So the state comes from another earlier test (test_debug_registers_wow64's 32-bit child, test_debug_service's
  `int 2d`), or from the session's process. Still open. The next run adds those two to the probe.
- ntdll exception PE32's timeout is the file's last test, test_context_exception_request: its thread enters 64-bit
  code with `push $0x33; lret`, which on the console stays in 32-bit mode, because the kernel's 32-bit code selector is
  0x33 (64-bit is 0x43). The access violation from running 64-bit bytes as 32-bit code reaches the test's vectored
  handler, which waits for a step the main thread never takes (b13-exc32-053728). Qualified as unfinished for i386
  only: every earlier test of the file has its result. Any 32-bit program that enters 64-bit code by a hard-coded
  0x33 ("heaven's gate") does the same on the console, with nothing to catch: the selector is valid.
- File creation costs 10-20 ms on the console's file system itself (above): a platform limit, measured natively.
- Write watches per 4 KiB page: a first write to a watched page clears the watch on its whole 16 KiB host page, so
  GetWriteWatch reports four pages for one store (kernel32 virtual.c:1750-2350). A patch clearing only the written
  4 KiB page and leaving the store to the 4 KiB path (MMU or step) hung kernel32 virtual in both builds and ntdll
  virtual PE32 (build B10, b10-virtual1-021001, 120 s timeouts against 9 s and 4 s), and was withdrawn. Every path
  that completes such a store (the MMU's alias stores, the single step's open host page) has to treat a still-watched
  neighbour as protected first. The other 16 KiB lines (partial frees and unmaps, placeholder splits, the space after
  an image) need memory below one host page to belong to different views, which upstream Wine on 16 KiB hosts does
  not do either: a design of its own, not a fix.
- A GPU page fault from one program ends the whole title (B3c sweep after the restart, 2026-10-25): the title
  (eboot.bin, the GPU broker's process) took `GPU_FAULT_PAGE_FAULT_ASYNC` while i386 d3dx9_43 shader, evr and
  explorerframe were running (the first two make Direct3D 9 devices through wined3d and Zink). A game's GPU fault
  should end that game, not every program. The session runner now queues the rest again after such a crash and marks
  a file running at two of them a fault. Four crashes in a row (B3c continuation sessions): the only files running at all four were
  evr (a Direct3D 9 device and DXVA2 surfaces through wined3d and Zink) and explorerframe nstc. Alone on B4, evr
  crashes the title within 20 s in both builds, four times of four, at an unmapped GPU address near
  0x750a2c220000; explorerframe nstc completes with no failure line. evr is the reproducer, and stays skipped until
  the fault is fixed.
- One title launch exhausts the kernel's pipe and socket budgets (B3c sweep, 2026-10-25): `kern.ipc.maxpipekva exceeded`
  first at klog line 62,744 of 249,798, as rpcrt4's `server ncalrpc_autolisten` started, then 1,305 times;
  `kern.ipc.maxsockets limit reached` near the end, which took the control payload (ps5vkctl) down with it. From about
  the 1,090th file on, helpers could not be spawned (317 files "no output"), while the title's own descriptors stayed
  low (about 840 of 12,555 open). The pipe budget leaks per boot (memory note tty-slot-leak-budget): only a restart
  clears it. The session runner should stop with a clear record after a run of unstarted files.
  It ran out again on 2026-10-26, after builds B11-B15 and about 40 launches since the last restart. A gate that ran
  just before passed (B14), but afterwards every copy through the helpers' memory service failed:
  - tests/remote-memory-probe.c failed every Read/WriteProcessMemory step (remote-memory-2026-10-26-01);
  - ntdll virtual 1480-1809 and kernel32 loader 2023-2036 read nothing from a child.

  It was the same with and without patch 2096 (b15b-tail, b14c-virtual), so it is the boot, not a patch. The battery now
  fails any case whose klog shows `kern.ipc.maxpipekva exceeded` and says the console needs a restart, and
  remote-memory64 is a gate case.
- A descriptor race in executable-memory conversion (gate-b3c-2026-10-25-01, mihawk-browser): in MihawkBrowser.exe's
  helper, "executable memory at ..., 16384 bytes, protection 5 not made: scratch failed (errno 9)". The JIT object's
  descriptor was closed by another thread between sceKernelJitCreateSharedMemory and its scratch mapping in jit_over
  (wine/ps5/pw_wine_dmem_ps5.c): some path closes a descriptor number it no longer owns. Pre-existing (patch 1127's
  path), one sighting; the rerun on the same build passed 98 of 98. To be found with a threaded host test.
- i386 quartz avisplit, filtergraph, mpegsplit and waveparser: each helper took a native crash (avisplit's: the system
  signalled it, reason 0xa0020328, just after winedmo.prx and ws2_32.prx loaded) and was left **stopped** (state 4):
  the title could not end them, and the console's guard refused the next launch until a restart. The x86_64 builds
  complete; mf-video32 (an MP4 through winedmo) passes in the gate. They ran while parallel files rewrote one
  Temp\test.avi (now a Temp folder per file). Alone on B4 with a Temp folder of its own, i386 avisplit does the same
  (lone-avisplit32-193035): its helper set up wined3d's Vulkan renderer, the DualSense and winedmo.prx (FFmpeg), loaded
  ws2_32.prx, and was signalled by the system (reason 0xa0020328) and left stopped; the file timed out at 150 s and the
  title returned, but the next launch needs a console restart. The collision was not the cause. The x86_64 build
  completes. Unverified hypothesis: a native stack overflow in FFmpeg on a WoW64 thread's Unix stack. Why it stays:
  the title's close of a helper waited 10 s for it to leave and then gave up, and sent SIGKILL only while a game was
  ending; it now sends SIGKILL after any close that times out, and SIGCONT 2 s later (wine/ps5/pw_wine_process.c
  close_until_gone). On B7 both were accepted (errno 0) and the process stayed in state 4 (reap-avisplit32-223121): the
  kernel's debug subsystem holds a natively crashed helper in a stop that neither signal ends, and nothing releases it
  (a helper is not an app, so no App Crash handling follows). Ending it needs the kernel's help, which the owner does not want, or no native crash.
  A helper cannot catch it and leave either: no signal reaches the process (the trace shows no SEH or signal handler
  running); the kernel's debug subsystem stops it directly with Sony's reason 0xa0020328. The crash itself: a
  trace (trace-avisplit32-224423) ends at winedmo_demuxer_destroy, and wow64_demuxer_destroy handed demuxer_destroy a
  struct demuxer_create_params, whose first member (the url, never set) is where demuxer_destroy reads its demuxer:
  the 64-bit side destroyed a stale handle from the stack and read through it into execute-only memory. Patch 2087
  (upstream's code, not ours) passes a struct demuxer_destroy_params. On build B8 (2026-10-25) i386 avisplit
  completes alone in 3.1 s with no kernel signal and no helper left (b8-avisplit32-232340), and all 36 quartz files of
  both builds ran in one session with no signal and an empty census (b8-quartz-232416): avisplit, mpegsplit and
  waveparser i386 complete with the same lines as x86_64; filtergraph times out in both builds alike (about 420 s),
  a hang still open. Un-skipped; a native crash that leaves a helper stopped is a platform defect in its own right (a crashing game
  would hold the console). ole32 clipboard: the second WM_DRAWCLIPBOARD (clipboard.c:1134) does not arrive.

## Paired Wine tests

The builder compiles the same source for both architectures: `<dll>_test.exe` and `<dll>_test32.exe`.
It strips debug information before generating `.deps/wine-tests/manifest.json`, so the baseline and
staged console executable have the same digest. Both use the same generated batching/watchdog header.
No test source is hand-edited; the existing build-time Direct3D guards remain diagnostic harness changes.

```sh
bash tools/build-wine-tests.sh ntdll kernel32 kernelbase
# Stage/deploy as part of the production loop, after the build and before console runs.
bash tools/build-production.sh
bash ps5/tools/deploy.sh
python3 tools/deploy-wine.py

# Cheap first slice. Repeat these commands with --arch x86_64.
python3 tools/run-wine-tests.py host ntdll virtual thread --arch i386 --timeout 120
python3 tools/run-wine-tests.py console ntdll virtual thread --arch i386 --per-launch 2 --file-timeout 60
python3 tools/run-wine-tests.py compare ntdll virtual thread --arch i386

# Then processes/pipes/loading, exceptions/WoW64/synchronization, filesystem/registry,
# CRT, networking, audio/input and graphics. Run a failure alone before assigning its cause.
python3 tools/run-wine-tests.py host kernel32 process pipe loader --arch i386
python3 tools/run-wine-tests.py console kernel32 process pipe loader --arch i386 --per-launch 2
python3 tools/run-wine-tests.py compare kernel32 process pipe loader --arch i386
```

Results live in `evidence/wine-tests/<arch>/<dll>-<where>.json`. Each entry carries the test digest,
Wine revision, batching-header digest and architecture. Host entries also record the loader digest and
an ignored raw-log path. The initial payload receives the run's stdout destination; descendants keep their inherited/redirected
Windows handles without reopening that file. `WINETEST_` controls reach payloads too. Console batches record their unique run names; `run-console.py` writes their
individual lifecycle and kernel records. Resuming never reuses another test build's results.

`compare` writes `<dll>-comparison.json`. Missing baselines, mismatched provenance, unfinished/skipped
files and extra failures make its verdict fail. Failures at the same line are compared with multiplicity.
An explicit list after the DLL limits the comparison to that list; its verdict records that scope.
A matched Wine failure is not proof of Windows correctness: Windows results provide an additional
reference where available. A complete test with failures differs from an unfinished test.

Historical `--noproc` records and old line-number allowances are not modern baselines. The child-dependent
`process`, `pipe` and `toolhelp` skips were reopened. Debugger, fiber and threadpool cases retain isolated-
qualification requirements; consult `tests/wine-tests-skip-ps5.json`. Those skips do not count as passes.
A known `OPEN:` defect never becomes a passing limitation. If a genuine platform limitation is requalified,
record it in `<arch>/<dll>-expected.json` with `context` equal to the console entry's context and `tests`
containing the reviewed per-test `file:line` reasons. Mismatched qualifications are refused.

Direct3D tests use native DXVK/VKD3D DLL overrides. The host loader defaults to the pinned runtime;
its headless configuration cannot qualify graphics. Supply `--host-wine` with a display-capable build of
the same revision, and record how it was built. The runner stages production's architecture-matched
DirectX DLLs into its separate graphics prefix and records the loader hash. It no longer silently chooses
system Wine. DXGI host display-mode tests still require the explicit existing opt-in. Console Direct3D
slicing remains available (`count`, then `--slices`); run those exact ranges on the host too.
The comparison refuses different slice ranges. Slicing is a diagnostic subset, not a full-suite pass.

## Texture-upload check

`gl-upload32` and `gl-upload64` use the same source through the production WGL/Zink path. Six
4096×4096 RGBA8 textures remain alive together (384 MiB); uploads are issued without a GPU wait between
them. Two rounds verify five pixels per texture, change and verify a subimage, issue 60 window swaps per
round, then release and recreate all textures. Each upload logs progress and time before/after driver
calls. A missing final verdict remains unfinished; a PASS following a watchdog stop is rejected.
The initial paired run exposed a 32-bit message-processing stall after successful uploads; see
[the analysis](../evidence/conformance-upload-analysis-2026-10-07-01.json). It is not a playable-game pass.

```sh
python3 tools/run-battery.py --name conformance-upload-RUN --only gl-upload32 gl-upload64
```

A probe PASS means 120 pixel checks, 120 swap calls and two release/recreate rounds completed. Compare
its maximum upload/presentation times and the owner's actual displayed-frame counters separately;
API completion is not proof of smooth or complete game rendering. No game configuration or GPU-budget
override is installed by the probe. `gl-upload32` and the focused `input32` case are required in
`GATE_CASES` after the message-stall fix below; their PE64 counterparts remain paired diagnostic cases.

## Keep the loop short

1. Run `python3 tests/test_wine_conformance.py` for parser/verdict changes; `python3 tests/test_wow64_input.py`
   compiles Wine's actual patched input converter on the host. Use host CPU oracles for decoder/interpreter changes.
2. Run the selected Wine groups on the host and console, in both architectures. Preserve skips, crashes and timeouts.
3. Reduce each new difference to one small probe with an explicit oracle.
4. Build production, deploy and pass the entire mandatory gate on exactly the code being committed. Never bypass the hook.
5. At milestones, widen the API suites and validate real games with pixels, sustained presentation, input, audio, closure and relaunch.

Console conditions and pacing remain in [NEXT_CONSOLE_SESSION.md](NEXT_CONSOLE_SESSION.md). `run-console.py`
owns stopping and cleanup; this runner never kills a later app or clears a disconnect guard. Sibling
repositories remain read-only. Commits are pushed only as AGENTS.md rule 10 allows. Full upstream-suite perfection is not a prerequisite
for measured game acceptance, and a successful game does not imply full API conformance.

## First paired foundation measurements (2026-10-07)

| Test | PE32 PS5 | PE64 PS5 | Comparison |
| --- | --- | --- | --- |
| ntdll `thread` | complete, 49 checks, no failure lines | complete, 49 checks, no failure lines | matches each architecture's host result |
| ntdll `virtual` | watchdog timeout, 23 failure lines | complete, 2,498 checks, 47 failure lines (summary counts 49 failures) | PE32 has 21 lines absent from its host baseline and is unfinished; PE64 has 47 lines absent from its host baseline |

Evidence: [PE32 comparison](../evidence/wine-tests/i386/ntdll-comparison.json),
[PE64 comparison](../evidence/wine-tests/x86_64/ntdll-comparison.json). The exact cause of the virtual-memory
failures and PE32 timeout is unqualified. Do not automatically explain all of them by the 16 KiB host pages.
The host PE32 `exception` and `wow64` files also ended incomplete and cannot yet serve as completed baselines.
Host process/pipe/loader files completed in both architectures; their current console qualification is still pending.

The upload probe's
observed readback and timing separated working large uploads from a presentation-loop problem. It did not
establish that a real game is playable.

The repeated upload run on the corrected runner confirmed the distinction:
[PE32](../evidence/conformance-upload-2026-10-07-02-gl-upload32.json) failed after
221.3 seconds with watchdog intervention; [PE64](../evidence/conformance-upload-2026-10-07-02-gl-upload64.json)
passed autonomously in 9.9 seconds. Both eventually verified 120 pixels and issued 120 swaps.
PE32's maximum whole-frame time was 35,803 ms, versus 27 ms for PE64; each architecture's maximum
`SwapBuffers` call was 27 ms. PE32 logged 182,077 unknown-message-type-zero errors. Neither run
recorded a broker allocation refusal. These observations locate the slowdown outside the upload
and swap calls; they do not yet identify the exact faulty message/input crossing.
See [the paired analysis](../evidence/conformance-upload-analysis-2026-10-07-02.json).

Host validation: all 26 sanitizer-enabled C tests and the Python conformance-verdict checks passed.
The C suite required execution outside the sandbox because LeakSanitizer could not inspect processes inside it.

The final production build passed all **13 mandatory gate cases**:
[gate-conformance-2026-10-07-01](../evidence/gate-conformance-2026-10-07-01.json).
Its exact source fingerprint is `da8343761da748b2991f67bb972b397be7e63f65388617d22a878c5a907a12f3`.
Every case recorded return to the launcher, with zero recorded CPU/GPU faults or process-protocol errors.
This covers the working tree, including its pre-existing memory/foundation changes, stdout/control forwarding,
paired-test tooling and stricter probe verdict. The failing upload probe is an optional diagnostic case;
the mandatory gate passing does not erase that failure or qualify the unfinished API files.

## Message-stall fix (2026-10-07)

The focused `input32`/`input64` probe injects the HID driver's internal hardware request without graphics:
20 HID packets with verified bytes and 40 device-change messages with verified parameters.
On the unpatched runtime, [PE32](../evidence/input-message-before-2026-10-07-01-input32.json)
failed at its first reply; [PE64](../evidence/input-message-before-2026-10-07-01-input64.json) passed all 60 checks.

WoW64 converted the outer `send_hardware_input_params` but passed its inner `INPUT32` unchanged.
The union begins at offset 4 in PE32 and offset 8 in PE64: the native reader interpreted the report's
zero `wParam` as its message number. Repeated real HID reports became unknown message zero, filling
the hardware queue. Patch 0955 uses one `input_32to64` helper for the internal call and public `SendInput`,
including mouse, keyboard and hardware fields. This is a shared conversion fix, without game configuration changes.

The patched production build passed all 60 checks in
[PE32](../evidence/input-message-after-2026-10-07-01-input32.json) and
[PE64](../evidence/input-message-after-2026-10-07-01-input64.json).
The [PE32 upload probe](../evidence/input-message-after-2026-10-07-01-gl-upload32.json) then completed naturally
in 11.4 seconds, with 120 pixel checks, 120 swaps, maximum whole-frame time 44 ms and zero unknown-message errors
(previously 221.3 seconds with watchdog intervention and a 35,803 ms maximum frame).
The [PE64 upload probe](../evidence/input-message-after-2026-10-07-01-gl-upload64.json) passed in 10.0 seconds,
maximum frame 27 ms. All 26 host regression tests and the actual converter's host check passed.

The virtual-memory differences remain open. Rerun the unfinished file alone with progress instrumentation,
and qualify process/pipe/loader on the current runtime after game validation and the expanded regression gate.

## Game validation after the fix

The 2026-10-07 fix/game comparison (record removed from the tree; in git history) records
both automated observations and human confirmation:

- A native game's repeat run displayed 4,317 frames. Inspected captures show active gameplay;
  the person confirmed picture, controls and sound. The scripted stop returned to
  the launcher. Its roughly 39-second startup gap remains a separate issue.
- An FNA game's OpenGL repeat run displayed 3,470 frames and progressed into a level.
  The title recorded non-silent stereo submission and normal closure after 80 seconds.
- The same FNA game's DirectX first run finished prefix setup and reached the display stage;
  its captured frame was black. The relaunch failed with status 1 during loading,
  with a native read fault in `libmono-2.0-x86+0x25ec80` at `0xffffffef`.
  This is a failing automated relaunch, not an accepted gameplay test.
- A .NET (MonoGame) game's PE64 control run displayed 4,112 frames, loaded a world and changed the
  character's position. Non-silent audio was submitted and the normal stop returned
  to the launcher, with application status 1 as in previous stop tests.

The person subsequently confirmed that all games run with input, sound and video,
and asked to make the accepted work the local `main` baseline. That report is
recorded alongside the earlier DirectX failure; it does not establish repeatability,
full operation of a game launcher, or completion of a Java game's launcher chain.

The exact production build passed all **15 mandatory gate cases** in
[gate-message-fix-2026-10-07-01](../evidence/gate-message-fix-2026-10-07-01.json),
source fingerprint `f27ba1ac28864daa5a4cc96ba863d16ea334f00df72ee562a52e17fc087d3ed5`. Every case returned to the launcher; the battery
recorded zero CPU/GPU fault records and zero process-protocol errors. The gate
includes the focused PE32 input and large-upload regressions. All 26 sanitizer host
tests, the actual Wine converter check and the conformance-verdict check passed.
The API differences and prior DirectX crash remain open independently of this pass.

### Owned TLS gate without public internet

`launcher-tls` uses `tools/run-tls-fixture.py`: two ephemeral host LAN TLS
endpoints and one marked throwaway prefix. The PE program commits a unique root
to that prefix, checks an HTTPS answer and both certificate chains, and commits
removal of the root; the runner removes its owned prefix and test entry. It
never writes certificates into game or Mihawk prefixes. `tls-probe.exe` without
fixture arguments remains the separate public-network diagnostic.

Hosts with an inbound firewall can select unused permitted ports through
`--trusted-port` / `--untrusted-port`, or `PROTON_TLS_TRUSTED_PORT` /
`PROTON_TLS_UNTRUSTED_PORT` for a whole battery. Default 0 chooses free ephemeral
ports. The runner does not modify firewall policy or disable TLS identity/root
checks. Host IP identities also carry a matching DNS SAN because the pinned
Wine SSL policy checks DNS SAN entries for numeric hosts; this fixture does not
qualify IP-only SAN support. Test validity covers 2000–2040; the existing clock
diagnostic/date-only retry remains explicit in its verdict.
