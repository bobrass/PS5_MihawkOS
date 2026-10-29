# Build speed: what `tools/build-production.sh` skips, and why its output stays the same bytes

2026-10-29, branch `fast-build` from `integration-1028` (fe0334e). Host only; no console run.

## What it does

- **Stages skip by content.** `tools/build-stamp.py run NAME ... -- COMMAND` runs a stage only when the content of
  what it reads or writes changed since its last good run: the repository (git blob ids, less notes and the prefixes a
  stage provably does not read), each outside input (`--input`: SDKs, RADV, the host Wine build, the theme tools), the
  host compiler versions and the build environment, and its outputs (`--output`). File digests are cached by size,
  mtime, ctime and inode, as git's index caches them, with git's two-second "racy" rule. A failed or interrupted run
  removes the stamp, so it never counts. Each stamp in `.deps/build-stamps/NAME.json` keeps one digest per part, and a
  stage that runs says which parts changed (`build-stamp: staged runs: input .deps/wine-ps5, repository`).
  `BUILD_STAMP_FORCE=1` runs every stage.
- **The stamped stages:** the two Wine builds, the probes, DXVK's extraction, staging and the runtime pack. Cheap
  stages keep their own idempotence (Ninja, Meson, the scripts' stamps). The GPU client (4 s) is not stamped, because
  it reads `../PS5_Mesa` through the foundation's script.
- **The two Wine builds run side by side.** Most of one is single-threaded. They share only the theme generators
  and the copies into each tree, which take turns under `.deps/theme.lock`. Each one's log is
  `.deps/build-stamps/wine-ps5.log` or `wine-gpu-broker.log`.
- **ccache** goes in front of every PS5 compile in `tools/build-wine-ps5.sh` when the host has it: the shims, and
  Wine's `CC`, `x86_64_CC` and `i386_CC` overridden on make's command line, so adding it needs no reconfigure. It keys
  on the compiler's `--version`, since `prospero-clang` is a script over the host clang. `PROSPERO_NO_CCACHE=1` turns
  it off.
- **Staging strips in batches:** one `llvm-strip` per 500 images, not one per image (staging 41 s → 18.5 s, the
  same bytes in all of `.deps/staged`).

## Wine: reconfigure only when configure's inputs change, and the 2065 trap still caught

Configure runs again only when its arguments, compiler or flags change, or `configure` itself does (unchanged).
makedep (`config.status`) reruns when the patch series changes, or now also when `tools/build-wine-ps5.sh` changes,
since its copy lines decide which `pw_*` headers are in the tree. Every untracked `pw_*` file in the tree that this
run did not copy is removed (change time older than this run's mark).

Checked on the PC: with the `pw_wine_perf.h` copy line deleted, the incremental build removed the stale copy and
stopped at makedep with the error a fresh configure gave for patch 2065:
`dlls/winedmo/unix_private.h:48: error: pw_wine_perf.h: No such file or directory`. The old script built on the
stale copy and passed.

## The title's link knows the SDK's libraries

`ps5/tools/link-title.sh` writes `build/ps5/eboot.elf.d`, which lists everything the link read: about 50 files,
`libps5platform.a`, libc++, libunwind, the RADV archive, the stubs, the recipe and the tools among them. CMake's
`DEPFILE` hands it to Ninja. Before, a changed `libps5platform.a` (an SDK patch) kept the old title linked until
`build/ps5/eboot.elf` was deleted by hand. Checked: after `touch` on the SDK's `libps5platform.a`, `ninja -d explain`
relinks `eboot.elf` "older than most recent input .../libps5platform.a". `ps5/tools/build.sh` now stops when
`ninja -t deps eboot.elf` does not name `libps5platform.a` and `libvulkan_radeon.ps5.a`.

## Builds that were not reproducible

A clean build has to give an incremental build's bytes, and a stage can only skip on content when its inputs are
the same bytes for the same source. These were not; each one was found by hashing outputs across a rebuild or by
comparing a clean build with an incremental one:
- `build/proton-payload-child.elf`, shipped as `run/proton-payload-child.elf`: the SDK driver's `--build-id=uuid`.
  It now links with `--build-id=md5`, as the forwarder does.
- Every Wine PE module this build makes from the patched tree (about 520 x86_64 and i386 modules, `explorer.exe`
  and `wineboot.exe` among them): lld-link's link time, four bytes in the COFF header and the debug directory. A
  clean build relinks them all and an incremental one only what changed, so the two always differed. `wmake` now
  passes `-Wl,/timestamp:0` in `x86_64_LDFLAGS` and `i386_LDFLAGS`. The first build with it removes every PE image
  already linked in the tree once (`<work>/build/.pe-link`), so nothing linked before keeps a time.
- Mesa's `opengl32.dll` and `libgallium_wgl.dll` (both architectures): the mingw link's time stamp, and with it the
  hashes in `manifest.json` and the runtime's `mesa/*/version`. Both now link with `--no-insert-timestamp`: the
  i686 one in `tools/build-mesa-windows-zink-i686.sh`, the x86_64 one in the graphics foundation's script through
  `platform/graphics/patches/0050` (0037 and 0039 are used on other branches).
- The patched DXVK (`d3d8`, `d3d9`, `d3d10core`, `d3d11`, `dxgi`, both architectures): the same mingw time stamp.
  `tools/build-dxvk.py` now passes `--no-insert-timestamp` too.
- `MihawkBrowser.exe`, `mihawk-appearance.exe` (shipped in system32) and every Windows probe: lld-link's link time.
  They now link with `/timestamp:0`, as the theme and icon DLLs already did.

The PRX modules, the title (`eboot.bin` and the rest of `dist/PPSA99170`) and the release DXVK and VKD3D-Proton
(extracted from the pinned archive) were already the same bytes in a clean and an incremental build.

## Timings on this PC (12 threads)

| Case | Before (fe0334e's scripts) | After |
| --- | --- | --- |
| (a) no change | 400 s (load ~7–11 from other sessions' builds) | 20 s (load ~2) |
| (b) one title `.cpp` line (`ps5/src/proton_launch.cpp`) | 270 s (load ~1–3) | 25 s, twice (edit and revert) |
| (c) one Wine patch line (a comment in 0891) | 274 s (load ~1–3) | 137 s, twice (edit and revert, load ~2) |
| a clean build from nothing, ccache off | not measured (the first attempt was killed) | 1,238 s and 1,334 s |

Under contention (c) varied: 196 s and 283 s while another session built FFmpeg. In (c) after, the two Wine builds
side by side take ~95 s, staging ~27 s; everything else skips.

Before, a no-change build spent 234 s in the two Wine builds, 69 s on the probes, 44 s staging, 18 s extracting
DXVK and 10 s packing. One Wine run with nothing to do was 83 s. Of that: 20 s its report (disassembling every
PRX), 18 s `tools/wine-patched-modules.py`, ~13 s the theme generators and 6 s of relinks. For (c), what remains is
those, twice, in parallel: they are the next things to make cheaper. Not reproduced here: the "~25 minutes"
figure. On this PC the slowest typical case measured before was 6.7 min, under load.

Logs and traces are in `cleanroom-5c5728e/fastbuild-logs/` (outside the repository); the per-stage times come from a
`bash -x` trace with `$EPOCHREALTIME`.

## Byte-identical to a clean build

Proven on the PC, 2026-10-29: an incremental build (after the edits above, then one no-change build) and a clean
build of the same tree (every build output in the worktree deleted, the pinned inputs kept, `PROSPERO_NO_CCACHE=1`)
give the same bytes in all 5,040 files of `dist/PPSA99170`, `dist/runtime` and `.deps/staged`: 0 differ (sha256 of
each file, `fastbuild-logs/fp-proof-*.json`). The first such comparison, before the time stamp fixes, differed in
1,295 files; with Wine and Mesa fixed, in 46 (DXVK).

## Limits

- A stage's key names what it reads; a stage that reads something not named would skip when it should run. The
  repository part excludes only what a grep of each stage's scripts shows they never read (the title's sources;
  `wine/` for the probes, DXVK and the pack), and staging takes all of `.deps` but its own outputs. The GPU client
  is not stamped for this reason. A new input to a stamped stage must be added to its `--input` list.
- A killed Wine make can leave a half-written generated file that make then trusts. Seen on 2026-10-28: widl's
  `mmc.h` without its interface, and `mmcndmgr.c` failed to compile. Delete `<work>/build` after a killed build; the
  stamp alone does not repair it.
- ccache is in place but measured only as a no-op here. The typical changes above recompile a few files, and the
  clean build ran with `PROSPERO_NO_CCACHE=1`, so its own gain (a reconfigured or wiped tree in the same folder) is
  not yet measured.
