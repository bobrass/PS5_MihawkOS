# 16 KiB host pages: what is left, the options, and a first step

Status, 2026-10-26: a design note for the owner. Nothing here is built. Counts come from the console records of
builds B9-B13 (`evidence/wine-tests/<arch>/*-console.json`, qualified by `tools/qualify-wine-tests.py`). They were
split by test function with the same method as the extended-attribute split in [CONFORMANCE.md](CONFORMANCE.md).

## The constraint

The console maps memory in 16 KiB host pages. Windows programs see 4 KiB pages. Wine already keeps 4 KiB pages over
a larger host page (patch 0510, the scheme Wine uses on 16 KiB aarch64 hosts), with these rules:

- a view starts on a host-page boundary;
- each 4 KiB page has its own protection byte (`pages_vprot`);
- the host page is mapped with what its pages allow.

Our patches added 4 KiB behaviour on top:

| Patch | What it adds |
| --- | --- |
| 1122 | A host page whose committed pages disagree on write is held read-only, and a store a page allows runs as one stepped instruction. Every view except image views (`get_host_page_unix_prot`, `!(view->protect & SEC_IMAGE)`); write-watched pages count as not writable. |
| 1134 | The same stores run through a 4 KiB MMU in 64-bit helpers. |
| 2075 | A no-access page no longer holds its host page read-only. |
| 2088 | A buffer check (ReadFile, WriteFile, NtRead/WriteVirtualMemory in the same process) honours each 4 KiB page. |

## What is still open: 247 lines in five groups

| Group | Lines | Tests | What Windows does that we do not |
| --- | --- | --- | --- |
| A. The space after an image | 112 | kernel32 loader test_Loader 942-946, 14 images × 4 checks × 2 builds | VirtualQuery just past an image reports MEM_FREE |
| B. Write watch per 4 KiB page | 88 | kernel32 virtual test_write_watch 1797-2350 (84); ntdll virtual 445-446 (4) | one store marks one page; we mark the four pages of its host page |
| C. Partial frees and placeholder splits | 30 | ntdll virtual test_NtFreeVirtualMemory 2547-2593 (22); kernelbase process test_MapViewOfFile3 156-185 (8) | releasing or splitting a reservation at a 4 KiB boundary |
| D. Another process's no-access pages | 13 | kernel32 virtual test_ReadProcessMemory 4536-4538 (6), test_VirtualAllocEx 244, 256 (4); loader test_ExitProcess 4149-4152 (3, x86_64) | ReadProcessMemory and WriteProcessMemory stop at a no-access 4 KiB page of another process |
| E. Executing a no-access 4 KiB page, 32-bit | 4 | ntdll exception test_dpe_exceptions 1811-1854 (i386) | DEP faults on a non-executable 4 KiB page beside executable ones |

### A. The space after an image: not really a host-page problem

**Measured.** The record shows a VirtualQuery just past the image (`hlib + SizeOfImage` rounded to 4 KiB) returning:

- AllocationBase = that address;
- State MEM_RESERVE;
- Type MEM_PRIVATE.

Windows returns MEM_FREE.

**Cause, from the source.** The image's last host page is mapped up to its 16 KiB end. In
`get_memory_region_size` (virtual.c, the `__PROSPERO__` block), `__wine_ps5_vm_gap` therefore finds a native mapping
there and sets `fake_reserved`. That flag was added so that address-space scanners are never told unverified space is
free.

**Option A1: report that space as free.** Report [view end, end of the view's last host page) as MEM_FREE when a view
ends inside a host page.

- Nothing can ever be allocated there, but the same is true on Windows: allocations are 64 KiB-aligned, and this
  space always lies below the next 64 KiB boundary.
- Cost: a view lookup in a query that already does one.
- Risk: low. It is a reporting change on a path that allocates nothing.

**Option A2: unmap the tail.** Impossible: a host page cannot be partly unmapped.

**Recommendation: A1**, and A1 is the first step overall. It is the largest group, about 30 lines of code in one
function, and it touches no mapping. The check is kernel32 loader in both builds plus the address-space probes of the
gate (vm-truth32/64, virtual-query32/64).

### B. Write watch per 4 KiB page

**What happens now.** The first write to a watched page clears the watch on all four pages of its host page, so
GetWriteWatch reports four pages for one store. That is a superset, which is safe for a garbage collector: it only
scans more.

**Why patch 2089 hung: not found.** 2089 cleared the watch on the written 4 KiB page only, and left the host page
read-only while its neighbours stayed watched, for 1122's step or 1134's MMU to complete the store. 1122 does cover
private views, write-watched ones included, so the store does have a 4 KiB path. The hang is therefore not a missing
path. The measured facts:

- kernel32 virtual in both builds and ntdll virtual PE32 hit their 120 s timeout instead of finishing in 9 s and 4 s
  (b10-virtual1-021001);
- the patch was withdrawn (docs/withdrawn-patches/2089).

Candidates to tell apart, each with a counter that exists:

- a fault loop: the watch-clearing fault returns, and the retry finds the page in a state no branch handles.
  `VM_STAT_HOST_PAGE_OPENS` and the fault counts would climb without bound.
- 32-bit stores: WoW64 code has no MMU runs, and PE32 hung too, so each store is stepped. A loop over the step slots
  would show as `stepfall` in the MMU census.
- a lock order problem: the watch is cleared under virtual_mutex inside the fault, and the step opens the page from
  the same path. A hang without counters climbing would point here.

The next run is 2089 rebuilt in a private build, with kernel32 virtual alone and the helper census logged every
second; it ends in a verdict for one of the three.

The earlier note in CONFORMANCE.md that 1122 and 1134 cover "image and section views only" was wrong, and is
corrected there.

**Options:**

- **B1: find 2089's hang and fix it.** Each first store to a 4 KiB page then costs one
  fault and one stepped or MMU-run instruction (86 µs stepped, measured in vmp-tracker-2026-10-14-03), instead of one
  fault per host page. Every path that completes such a store must also treat a still-watched neighbour as protected:
  - the MMU's alias stores;
  - the step's open window, where other threads' stores to the neighbours are not seen;
  - system calls writing into the page (check_write_access).

  Risk: medium-high. This is the fault path every program runs, and the cause of 2089's hang is not known yet.
- **B2: keep the superset and qualify it.** No cost, no risk. Write watches serve garbage collectors (Boehm's
  GetWriteWatch mode in Unity's Mono, for example), and they only need a superset.

**Recommendation: B2.** Ask the owner to accept the superset as a choice of ours ("awaiting owner"). B1 stays the
route if a program is ever found that needs the exact pages.

### C. Partial frees and placeholder splits

**What happens now.** `NtFreeVirtualMemory` of 0x1000 bytes at 0x1000 inside a reservation, and
`VirtualFree(MEM_PRESERVE_PLACEHOLDER)` of one 4 KiB page, both need a view that starts mid-host-page. Wine refuses
that (virtual.c:3032, STATUS_CONFLICTING_ADDRESSES, and the alignment asserts in create_view and map_view).

**Options:**

- **C1: views of reserved or anonymous memory may share a host page.** Two views share the host mapping, and
  `pages_vprot` already gives each 4 KiB page its own state. Every helper that unmaps, reprotects, or finds free
  space by view would have to treat a shared host page as one unit:
  - unmap the host page only when no view is left in it;
  - protect it with what all its pages allow (1122's rule extended across views);
  - zero a 4 KiB page that is committed again, since Windows gives zeroed pages.

  Cost: a large change to the address-space engine (create_view, delete_view, find_free_area, unmap_area, and about
  60 host-page roundings). Risk: high. This is the code whose mistakes hung the console before ("unwire timeout",
  memory note foreign-gpu-mapping-in-wine-reservation).
- **C2: map a file or section into a 4 KiB-aligned placeholder** (MapViewOfFile3 after the split). On top of C1 this
  needs a host page whose contents come from two sources, which a 16 KiB MMU cannot map. Either:
  - emulate every access, which in practice means 1134's MMU for all instructions; or
  - make a private copy, which breaks a shared writable section between processes.

  Neither is acceptable for games.

**Recommendation: none yet.** C1 is possible but large. No game is known to split placeholders at 4 KiB: emulators
and V8 split at 64 KiB. Keep these lines open until a program needs it, then decide C1 with its measurements.

### D. Another process's no-access pages

**What happens now.** A read across another process's no-access 4 KiB page succeeds, and the record shows the
whole range copied (test_ReadProcessMemory 4537: `copied = 8192`, where Windows copies nothing and returns
ERROR_PARTIAL_COPY). Patch 2088 fixed the same-process case. Which copy path a cross-process read takes on the
console (the server's, or the helper's memory service) was not traced for this note; D1 starts with that trace.

**Option D1: check the target's protections first.** Before copying, ask the target process for its pages'
protections over the range, through the APC_VIRTUAL_QUERY call Wine already has, and stop at the first page that
does not allow the access.

- Cost: one round trip to the target per call. That is tens of microseconds; ReadProcessMemory is rare in games
  (launchers, anti-cheat, crash reporters).
- Risk: low to medium. A new failure path in a call that used to succeed.

**Recommendation: D1**, the second step: small, contained, and it completes what 2088 began.

### E. Executing a no-access 4 KiB page from 32-bit code

**What happens now.** A host page holding both an executable page and a non-executable page is mapped executable
(1122 keeps upstream's rule for executable pages, so code beside data stays fast). DEP then cannot fault on the
non-executable page.

**Options:**

- **E1: map such host pages non-executable and run their code through an MMU.** 1134's engine runs only 64-bit code;
  a 32-bit engine is the M4 interpreter work (docs/IA32_DESIGN.md). Every instruction fetched from such a page would
  run emulated.
- **E2: keep the rule and qualify it.**

**Recommendation: E2**, as a platform limit. It costs only DEP tests, and E1 would slow any 32-bit program whose code
shares a host page with data.

## Summary and order

| Step | Group | Lines | Cost | Risk |
| --- | --- | --- | --- | --- |
| 1 | A1: report an image's tail as free | 112 | small, one function | low |
| 2 | D1: check the target's 4 KiB protections | 13 | small, one copy path | low-medium |
| (owner) | B2: accept the write-watch superset | 88 | none | none |
| (owner) | E2: accept DEP at host-page granularity | 4 | none | none |
| later | C1: views sharing a host page | 30 | large | high |

Steps 1 and 2 would close 125 of the 247 lines in our own layer. B2 and E2 would qualify another 92 if the owner
accepts them. C stays open: 30 lines, and no game known to need it.

The owner asked for the 2D game's load time to be measured before and after a host-page change. Steps 1 and 2 touch only a
query and a cross-process copy, so neither is on a game's load path. They should still be measured once with
`tools/start-timeline.py`, as patch 2088 was.
