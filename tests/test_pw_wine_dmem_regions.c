/* SPDX-License-Identifier: GPL-3.0-or-later */
/* cc -std=c11 tests/test_pw_wine_dmem_regions.c wine/ps5/pw_wine_dmem.c -o /tmp/dmem-regions && /tmp/dmem-regions */
#include "../wine/ps5/pw_wine_dmem.h"
#include "../wine/ps5/pw_wine_perf.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* A kernel that grants everything but direct memory, for the last-failure record. */
static int fake_ok(void *context, uintptr_t address, size_t bytes) { (void)context; (void)address; (void)bytes; return 0; }
static int fake_no_memory(void *context, size_t bytes, int64_t *offset)
{
    (void)context; (void)bytes; (void)offset;
    return -1;
}
static int fake_map(void *context, uintptr_t address, size_t bytes, int64_t offset)
{
    (void)context; (void)address; (void)bytes; (void)offset;
    return 0;
}
static int fake_protect(void *context, uintptr_t address, size_t bytes, unsigned protection)
{
    (void)context; (void)address; (void)bytes; (void)protection;
    return 0;
}
static int fake_release(void *context, int64_t offset, size_t bytes) { (void)context; (void)offset; (void)bytes; return 0; }

/* A refused call is recorded with what it was and for which range (PwWineDmemStats.failed): a count alone did not
 * say where a 32-bit game's unbacked memory came from (2026-10-07; record removed from the tree). */
static void check_last_failure(void)
{
    static PwWineDmemRun runs[8];
    const PwWineDmemOps ops = { NULL, 16384, fake_ok, fake_no_memory, fake_map, fake_protect, fake_ok, fake_release,
                                0, NULL, NULL };
    PwWineDmem d;
    PwWineDmemStats stats;

    assert(!pw_wine_dmem_init(&d, &ops, runs, 8, 0));
    assert(!pw_wine_dmem_add_region(&d, 0x100000, 0x100000, 1));
    assert(pw_wine_dmem_protect(&d, 0x120000, 0x8000, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE) == -1);
    pw_wine_dmem_stats(&d, &stats);
    assert(stats.failures == 1 && stats.failed == PW_WINE_DMEM_FAILED_ALLOCATE);
    assert(stats.failed_address == 0x120000 && stats.failed_bytes == 0x8000);
    assert(pw_wine_dmem_protect(&d, 0x400000, 0x4000, PW_WINE_DMEM_READ) == -1);
    pw_wine_dmem_stats(&d, &stats);
    assert(stats.failures == 2 && stats.failed == PW_WINE_DMEM_FAILED_OUTSIDE && stats.failed_address == 0x400000);
}

/* Freeing a bare reservation to protection 0 asks the kernel for nothing: an emulator turned most of a 28 GiB
 * placeholder back into a placeholder and the console froze in that unmap (2026-10-14; record removed from the tree). */
static unsigned kernel_unmaps, kernel_reserves;
static int count_unmap(void *context, uintptr_t address, size_t bytes)
{
    (void)context; (void)address; (void)bytes;
    kernel_unmaps++;
    return 0;
}
static int count_reserve(void *context, uintptr_t address, size_t bytes)
{
    (void)context; (void)address; (void)bytes;
    kernel_reserves++;
    return 0;
}
static int fake_allocate(void *context, size_t bytes, int64_t *offset)
{
    (void)context; (void)bytes;
    *offset = 0;
    return 0;
}
static void check_bare_replace(void)
{
    static PwWineDmemRun runs[8];
    const PwWineDmemOps ops = { NULL, 16384, count_reserve, fake_allocate, fake_map, fake_protect, count_unmap,
                                fake_release, 0, NULL, NULL };
    PwWineDmem d;

    assert(!pw_wine_dmem_init(&d, &ops, runs, 8, 0));
    assert(!pw_wine_dmem_add_region(&d, 0x100000, 0x100000, 1));
    assert(!pw_wine_dmem_replace(&d, 0x100000, 0x100000, 0));
    assert(!kernel_unmaps && !kernel_reserves);
    /* A backed page inside the range: only its run is torn down and reserved again. */
    assert(!pw_wine_dmem_protect(&d, 0x140000, 0x4000, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE));
    assert(!pw_wine_dmem_replace(&d, 0x100000, 0x100000, 0));
    assert(kernel_unmaps == 1 && kernel_reserves == 1 && !d.run_count);
    /* A caller's mapping counts as held too. */
    assert(!pw_wine_dmem_adopt(&d, 0x1c0000, 0x4000));
    assert(!pw_wine_dmem_replace(&d, 0x180000, 0x80000, 0));
    assert(kernel_unmaps == 2 && !d.run_count);
}

/* A range the caller unmapped in the kernel stops being owned: a later replace must not take it for the reservation
 * it no longer is (a hole a mapping without an address lands in). Cutting the middle splits the region. */
static void check_remove_region(void)
{
    static PwWineDmemRun runs[8];
    const PwWineDmemOps ops = { NULL, 16384, count_reserve, fake_allocate, fake_map, fake_protect, count_unmap,
                                fake_release, 0, NULL, NULL };
    PwWineDmem d;
    size_t piece;

    assert(!pw_wine_dmem_init(&d, &ops, runs, 8, 0));
    assert(!pw_wine_dmem_add_region(&d, 0x100000, 0x100000, 1));
    assert(!pw_wine_dmem_protect(&d, 0x140000, 0x4000, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE));
    kernel_unmaps = 0;
    assert(!pw_wine_dmem_unmap(&d, 0x130000, 0x20000) && kernel_unmaps == 1);
    assert(d.regions == 2 && !d.run_count);
    assert(!pw_wine_dmem_split(&d, 0x130000, 0x20000, &piece) && piece == 0x20000);
    assert(pw_wine_dmem_replace(&d, 0x130000, 0x20000, 0) == -1);
    assert(pw_wine_dmem_owns(&d, 0x100000, 0x30000) && pw_wine_dmem_owns(&d, 0x150000, 0xb0000));
    /* The front and the back: the region shrinks, and a whole region goes. */
    assert(!pw_wine_dmem_unmap(&d, 0x100000, 0x10000));
    assert(!pw_wine_dmem_unmap(&d, 0x150000, 0xb0000));
    assert(d.regions == 1 && d.region_low[0] == 0x110000 && d.region_high[0] == 0x130000);
    /* Taken again later, it is owned again. */
    assert(!pw_wine_dmem_add_region(&d, 0x130000, 0x20000, 1));
    assert(pw_wine_dmem_owns(&d, 0x110000, 0x40000));
}

/* A payload's kernel keeps execute only on JIT memory (exec-kernel-2026-10-14): with exec_caller_only, direct runs
 * reach the kernel without execute, the caller's runs (JIT memory) with it, and a title passes execute through. */
static unsigned protections[8], protect_calls, release_calls;
static int64_t next_offset, released_offset;
static size_t released_bytes;
static int record_protect(void *context, uintptr_t address, size_t bytes, unsigned protection)
{
    (void)context; (void)address; (void)bytes;
    if (protect_calls < 8) protections[protect_calls] = protection;
    protect_calls++;
    return 0;
}
static int counting_allocate(void *context, size_t bytes, int64_t *offset)
{
    (void)context;
    *offset = next_offset;
    next_offset += (int64_t)bytes;
    return 0;
}
static int record_release(void *context, int64_t offset, size_t bytes)
{
    (void)context;
    released_offset = offset;
    released_bytes = bytes;
    release_calls++;
    return 0;
}
static void check_exec_caller_only(void)
{
    enum { R = PW_WINE_DMEM_READ, RW = R | PW_WINE_DMEM_WRITE, RX = R | PW_WINE_DMEM_EXEC,
           RWX = RW | PW_WINE_DMEM_EXEC };
    static PwWineDmemRun runs[8], small[2];
    PwWineDmemOps ops = { NULL, 16384, fake_ok, counting_allocate, fake_map, record_protect, fake_ok, record_release,
                          1, NULL, NULL };
    PwWineDmem d;
    PwWineDmemStats stats;
    uint64_t backed;

    assert(!pw_wine_dmem_init(&d, &ops, runs, 8, 0));
    assert(!pw_wine_dmem_add_region(&d, 0x100000, 0x100000, 1));
    /* Backed with RWX: mapped read-write, and the kernel is asked for nothing more. With RX: read. */
    assert(!pw_wine_dmem_protect(&d, 0x100000, 0x8000, RWX) && !protect_calls);
    assert(!pw_wine_dmem_replace(&d, 0x110000, 0x4000, RX) && protect_calls == 1 && protections[0] == R);
    /* RX on a direct run: read. */
    protect_calls = 0;
    assert(!pw_wine_dmem_protect(&d, 0x100000, 0x8000, RX) && protect_calls == 1 && protections[0] == R);
    /* The first page converted to JIT memory (the caller's): its direct memory released once, the rest kept; then RX
     * reaches it whole while the direct page beside it still gets read. */
    pw_wine_dmem_stats(&d, &stats);
    backed = stats.backed_bytes;
    assert(pw_wine_dmem_can_adopt(&d, 0x100000, 0x4000) && !pw_wine_dmem_adopt(&d, 0x100000, 0x4000));
    assert(release_calls == 1 && released_offset == 0 && released_bytes == 0x4000);
    pw_wine_dmem_stats(&d, &stats);
    assert(stats.backed_bytes == backed - 0x4000 && pw_wine_dmem_backed(&d, 0x100000, 0x8000) == 0x4000);
    protect_calls = 0;
    assert(!pw_wine_dmem_protect(&d, 0x100000, 0x8000, RX));
    assert(protect_calls == 2 && protections[0] == RX && protections[1] == R);
    assert(!pw_wine_dmem_protect(&d, 0x100000, 0x4000, RWX) && protect_calls == 3 && protections[2] == RWX);

    /* No room to record a caller's run in the middle of a run (a split and the run itself): refused, and nothing
     * changes. Outside every region: refused. */
    assert(!pw_wine_dmem_init(&d, &ops, small, 2, 0));
    assert(!pw_wine_dmem_add_region(&d, 0x100000, 0x100000, 1));
    assert(!pw_wine_dmem_protect(&d, 0x100000, 0x10000, RW) && d.run_count == 1);
    release_calls = 0;
    assert(!pw_wine_dmem_can_adopt(&d, 0x104000, 0x4000));
    assert(d.run_count == 1 && d.runs[0].bytes == 0x10000 && !release_calls);
    assert(pw_wine_dmem_can_adopt(&d, 0x100000, 0x4000) && !pw_wine_dmem_can_adopt(&d, 0x400000, 0x4000));

    /* A title: execute passes through, at backing and on a run. */
    ops.exec_caller_only = 0;
    assert(!pw_wine_dmem_init(&d, &ops, runs, 8, 0));
    assert(!pw_wine_dmem_add_region(&d, 0x100000, 0x100000, 1));
    protect_calls = 0;
    assert(!pw_wine_dmem_protect(&d, 0x100000, 0x4000, RWX) && protect_calls == 1 && protections[0] == RWX);
    assert(!pw_wine_dmem_protect(&d, 0x100000, 0x4000, RX) && protect_calls == 2 && protections[1] == RX);
}

/* The run under an address, for a second mapping of its memory (pw_wine_alias.h): the direct memory there and the
 * extent backed contiguously; a caller's run (JIT memory or a file), a bare page or no engine answers 0. */
static void check_run_at(void)
{
    static PwWineDmemRun runs[8];
    const PwWineDmemOps ops = { NULL, 16384, fake_ok, counting_allocate, fake_map, fake_protect, fake_ok, fake_release,
                                1, NULL, NULL };
    PwWineDmem d;
    PwWineDmemRun run;

    next_offset = 0x40000000;
    assert(!pw_wine_dmem_init(&d, &ops, runs, 8, 0));
    assert(!pw_wine_dmem_add_region(&d, 0x100000, 0x100000, 1));
    assert(!pw_wine_dmem_protect(&d, 0x100000, 0x10000, PW_WINE_DMEM_READ));
    assert(!pw_wine_dmem_protect(&d, 0x110000, 0x8000, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE));   /* joins it */
    assert(pw_wine_dmem_run_at(&d, 0x10c000, &run) == 1);
    assert(run.address == 0x100000 && run.bytes == 0x18000 && run.offset == 0x40000000);
    assert(pw_wine_dmem_run_at(&d, 0x117fff, &run) == 1 && run.address == 0x100000);
    assert(!pw_wine_dmem_run_at(&d, 0x118000, &run) && !pw_wine_dmem_run_at(&d, 0xfc000, &run));
    /* Cut by an adopt: two runs, each its own offset; the adopted page is the caller's. */
    assert(!pw_wine_dmem_adopt(&d, 0x108000, 0x4000));
    assert(!pw_wine_dmem_run_at(&d, 0x108000, &run));
    assert(pw_wine_dmem_run_at(&d, 0x10c000, &run) == 1);
    assert(run.address == 0x10c000 && run.bytes == 0xc000 && run.offset == 0x4000c000);
    assert(pw_wine_dmem_run_at(&d, 0x104000, &run) == 1 && run.address == 0x100000 && run.bytes == 0x8000);
    assert(!pw_wine_dmem_run_at(NULL, 0x104000, &run) && !pw_wine_dmem_run_at(&d, 0x104000, NULL));
}

/* The kernel as the guard sees it (late-library 2026-10-15): a region Wine reserved, and inside it, placed after Wine's
 * reservation, an entry that is not Wine's (F, a wired GPU mapping in the model). Every call reaching the kernel is
 * logged; the foreign op answers with F when the asked range overlaps it. With no lay op, a run goes back to a
 * reservation by unmap then reserve, and a reserve after an unmap of a hole finds an intruder there when asked to. */
enum { LOG = 64, K = 0x4000 };
typedef struct { char kind; uintptr_t address; size_t bytes; } Call;
static Call calls[LOG];
static unsigned call_count, queries;
static uintptr_t foreign_low, foreign_high;
static int query_fails, intruder_in_holes;
static void logged(char kind, uintptr_t address, size_t bytes)
{
    if (call_count < LOG) calls[call_count] = (Call){ kind, address, bytes };
    call_count++;
}
static unsigned count_of(char kind)
{
    unsigned n = 0;
    for (unsigned i = 0; i < call_count && i < LOG; i++) n += calls[i].kind == kind;
    return n;
}
static int overlaps_foreign(uintptr_t address, size_t bytes)
{
    return address < foreign_high && foreign_low < address + bytes;
}
static int k_reserve(void *c, uintptr_t a, size_t b)
{
    (void)c;
    logged('r', a, b);
    /* No-overwrite: a mapping without an address took the hole the unmap left (the window C3 closes). */
    if (intruder_in_holes) {
        foreign_low = a;
        foreign_high = a + b;
        return -1;
    }
    return 0;
}
static int k_map(void *c, uintptr_t a, size_t b, int64_t o) { (void)c; (void)o; logged('m', a, b); return 0; }
static int k_unmap(void *c, uintptr_t a, size_t b) { (void)c; logged('u', a, b); return 0; }
static int k_lay(void *c, uintptr_t a, size_t b) { (void)c; logged('l', a, b); return 0; }
static int k_foreign(void *c, uintptr_t a, size_t b, uintptr_t *start, uintptr_t *end, unsigned *protection)
{
    (void)c;
    queries++;
    if (query_fails) return -1;
    if (!overlaps_foreign(a, b)) return 0;
    *start = foreign_low;
    *end = foreign_high;
    *protection = 0x33;
    return 1;
}
static void guard_setup(PwWineDmem *d, PwWineDmemRun *runs, int laying)
{
    static PwWineDmemOps ops;
    ops = (PwWineDmemOps){ NULL, K, k_reserve, counting_allocate, k_map, fake_protect, k_unmap, fake_release, 0,
                           laying ? k_lay : NULL, k_foreign };
    assert(!pw_wine_dmem_init(d, &ops, runs, 16, 0));
    assert(!pw_wine_dmem_add_region(d, 0x1000000, 0x400000, 1));
    foreign_low = 0x1200000;
    foreign_high = foreign_low + 4 * K;
    query_fails = intruder_in_holes = 0;
    call_count = queries = 0;
}
static void check_foreign_guard(void)
{
    enum { RW = PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE };
    static PwWineDmemRun runs[16];
    PwWineDmem d;
    PwWineDmemStats stats;
    uintptr_t at;
    size_t bytes;

    guard_setup(&d, runs, 1);
    /* A fresh bare commit: one query, one map, no unmap or reserve. */
    assert(!pw_wine_dmem_replace(&d, 0x1000000, K, RW));
    assert(queries == 1 && count_of('m') == 1 && call_count == 1 && d.run_count == 1);
    /* G2: a commit, a protection change and the caller's own fixed mapping over F are refused before any change. */
    call_count = 0;
    errno = 0;
    assert(pw_wine_dmem_replace(&d, 0x11f0000, 0x20000, RW) == -1 && errno == EEXIST);
    errno = 0;
    assert(pw_wine_dmem_protect(&d, 0x11f0000, 0x20000, RW) == -1 && errno == EEXIST);
    errno = 0;
    assert(pw_wine_dmem_check(&d, 0x1200000, K) == -1 && errno == EEXIST);
    assert(!call_count && d.run_count == 1);
    pw_wine_dmem_stats(&d, &stats);
    assert(stats.maps_refused == 3 && stats.foreign_found == 1 && stats.failed == PW_WINE_DMEM_FAILED_FOREIGN);
    assert(d.hole_count == 1 && d.holes[0].address == foreign_low && d.holes[0].bytes == 4 * K &&
           d.holes[0].protection == 0x33 && d.holes[0].refusals == 3);
    /* Decommit over a run and F: only the run's span is touched, by one laid reservation. */
    assert(!pw_wine_dmem_protect(&d, 0x1100000, 2 * K, RW) && d.run_count == 2);
    call_count = queries = 0;
    assert(!pw_wine_dmem_replace(&d, 0x1100000, 0x200000, 0));
    assert(call_count == 1 && calls[0].kind == 'l' && calls[0].address == 0x1100000 && calls[0].bytes == 2 * K);
    assert(!queries && d.run_count == 1);
    /* G1: the release goes around F, which stays owned; the hole is offered once, and again after the next refusal. */
    assert(pw_wine_dmem_take_hole(&d, &at, &bytes) && at == foreign_low && bytes == 4 * K);
    assert(!pw_wine_dmem_take_hole(&d, &at, &bytes));
    call_count = 0;
    assert(!pw_wine_dmem_unmap(&d, 0x1100000, 0x200000));
    assert(call_count == 2 && calls[0].kind == 'u' && calls[0].address == 0x1100000 && calls[0].bytes == 0x100000);
    assert(calls[1].kind == 'u' && calls[1].address == foreign_high && calls[1].bytes == 0x1300000 - foreign_high);
    assert(pw_wine_dmem_owns(&d, foreign_low, 4 * K) && !pw_wine_dmem_owns(&d, 0x1100000, K));
    assert(pw_wine_dmem_take_hole(&d, &at, &bytes) && at == foreign_low);
    pw_wine_dmem_stats(&d, &stats);
    assert(stats.unmaps_skipped == 1 && d.holes[0].skips == 1);
    /* Kept instead of unmapped (patch 1128): asked about, recorded, no kernel call. */
    call_count = queries = 0;
    assert(!pw_wine_dmem_keep(&d, 0x1300000, 0x100000) && !call_count && queries == 1);
    assert(pw_wine_dmem_keep(&d, 0x2000000, K) == -1);
    /* A query the kernel does not answer: the release makes no kernel call, a commit is refused, both counted. */
    query_fails = 1;
    call_count = 0;
    assert(pw_wine_dmem_unmap(&d, 0x1300000, 0x100000) == -1 && !call_count);
    assert(pw_wine_dmem_replace(&d, 0x1300000, K, RW) == -1 && !call_count);
    pw_wine_dmem_stats(&d, &stats);
    assert(stats.query_failures == 2 && pw_wine_dmem_owns(&d, 0x1300000, 0x100000));
    query_fails = 0;
    /* The caller's adopted range, disowned, is bare again: protection 0 asks nothing, a release asks the kernel. */
    assert(!pw_wine_dmem_adopt(&d, 0x1300000, 0x10000) && !pw_wine_dmem_disown(&d, 0x1300000, 0x10000));
    call_count = 0;
    assert(!pw_wine_dmem_replace(&d, 0x1300000, 0x10000, 0) && !call_count && d.run_count == 1);
    assert(!pw_wine_dmem_protect(&d, 0x1300000, K, RW) && pw_wine_dmem_disown(&d, 0x1300000, K) == -1);
    /* A full hole table still refuses, and counts. */
    d.hole_count = PW_WINE_DMEM_HOLES;
    foreign_low = 0x1380000;
    foreign_high = foreign_low + K;
    assert(pw_wine_dmem_replace(&d, 0x1380000, K, RW) == -1 && d.hole_count == PW_WINE_DMEM_HOLES);
    pw_wine_dmem_stats(&d, &stats);
    assert(stats.foreign_found == 2);
}

/* The pager's guards (pw_wine_dmem_check_guards): a view adopted whole as the caller's run, which the engine never
 * asks about, and a range outside every region (the section arena) are asked about all the same: one query, no other
 * kernel call, refused over F and recorded. */
static void check_pager_guards(void)
{
    static PwWineDmemRun runs[16];
    PwWineDmem d;
    PwWineDmemStats stats;

    guard_setup(&d, runs, 1);
    assert(!pw_wine_dmem_adopt(&d, 0x1100000, 0x200000));
    assert(!pw_wine_dmem_check(&d, 0x1100000, 0x200000) && !queries);
    errno = 0;
    assert(pw_wine_dmem_check_guards(&d, 0x11f0000, 0x20000) == -1 && errno == EEXIST && queries == 1 && !call_count);
    assert(!pw_wine_dmem_check_guards(&d, 0x1100000, 0x10000) && queries == 2);
    foreign_low = (uintptr_t)0x8000100000;
    foreign_high = foreign_low + K;
    assert(pw_wine_dmem_check_guards(&d, (uintptr_t)0x8000000000, 0x200000) == -1);
    assert(!pw_wine_dmem_check_guards(&d, (uintptr_t)0x8000200000, 0x200000) && !call_count);
    pw_wine_dmem_stats(&d, &stats);
    assert(stats.maps_refused == 2 && stats.foreign_found == 2 && d.hole_count == 2 &&
           d.holes[1].address == foreign_low && d.holes[1].refusals == 1);
    assert(pw_wine_dmem_check_guards(&d, (uintptr_t)0x8000000001, K) == -1 && queries == 4);
}

/* C3: an intruder lands in any hole an unmap leaves before the reserve. Laying never opens one; without it, the hole is
 * taken and the commit that followed is refused instead of mapping over the intruder, now and later. */
static void check_hole_window(void)
{
    enum { RW = PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE };
    static PwWineDmemRun runs[16];
    PwWineDmem d;

    guard_setup(&d, runs, 1);
    foreign_low = foreign_high = 0;
    assert(!pw_wine_dmem_replace(&d, 0x1000000, 2 * K, RW));
    intruder_in_holes = 1;
    call_count = 0;
    assert(!pw_wine_dmem_replace(&d, 0x1000000, 2 * K, RW));
    assert(count_of('l') == 1 && !count_of('u') && !count_of('r') && count_of('m') == 1 && !foreign_high);

    guard_setup(&d, runs, 0);
    foreign_low = foreign_high = 0;
    assert(!pw_wine_dmem_replace(&d, 0x1000000, 2 * K, RW));
    intruder_in_holes = 1;
    call_count = 0;
    assert(pw_wine_dmem_replace(&d, 0x1000000, 2 * K, RW) == -1);
    assert(count_of('u') == 1 && count_of('r') == 1 && !count_of('m') && foreign_low == 0x1000000);
    intruder_in_holes = 0;
    call_count = 0;
    assert(pw_wine_dmem_replace(&d, 0x1000000, 2 * K, RW) == -1 && errno == EEXIST && !count_of('m'));
}

/* The engine lock names its holder while it is held (the helper's stall watch reads it, __wine_ps5_dmem_holder) and
 * nobody once it is given back: every kernel call the engine makes is under it. */
static uint64_t holder_seen, since_seen, doing_seen, doing_address_seen, doing_bytes_seen;
static int64_t doing_offset_seen;
static int holder_map(void *context, uintptr_t address, size_t bytes, int64_t offset)
{
    const PwWineDmem *d = context;
    (void)address; (void)bytes; (void)offset;
    holder_seen = __atomic_load_n(&d->holder, __ATOMIC_ACQUIRE);
    since_seen = __atomic_load_n(&d->held_since, __ATOMIC_RELAXED);
    doing_seen = __atomic_load_n(&d->doing, __ATOMIC_ACQUIRE);
    doing_address_seen = __atomic_load_n(&d->doing_address, __ATOMIC_RELAXED);
    doing_bytes_seen = __atomic_load_n(&d->doing_bytes, __ATOMIC_RELAXED);
    doing_offset_seen = __atomic_load_n(&d->doing_offset, __ATOMIC_RELAXED);
    return 0;
}
static void check_holder(void)
{
    static PwWineDmemRun runs[8];
    static PwWineDmem d;
    const PwWineDmemOps ops = { &d, 16384, fake_ok, fake_allocate, holder_map, fake_protect, fake_ok, fake_release,
                                0, NULL, NULL };

    assert(!pw_wine_dmem_init(&d, &ops, runs, 8, 0));
    assert(!pw_wine_dmem_add_region(&d, 0x100000, 0x100000, 1) && !d.holder);
    assert(!pw_wine_dmem_protect(&d, 0x140000, 0x4000, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE));
    assert(holder_seen == pw_wine_perf_tid() && since_seen && since_seen <= __builtin_ia32_rdtsc());
    /* Inside the map the record names it, its range and the offset the allocation gave; nothing once released. */
    assert(doing_seen == PW_WINE_DMEM_DOING_MAP && doing_address_seen == 0x140000 && doing_bytes_seen == 0x4000 &&
           doing_offset_seen >= 0);
    assert(!d.holder && d.doing == PW_WINE_DMEM_DOING_NONE);
}

/* The engine lock under contention (wine/ps5/pw_wine_spin.h): two threads still exclude each other through 4000
 * protects while one holds it 100 us in each of its kernel calls, past the pauses; and a thread behind a 200 ms hold
 * sleeps instead of spinning, its CPU time far below the hold (a pure spin spends all of it). */
enum { LOCK_LOOPS = 2000, LOCK_SHORT = 0x140000, LOCK_HELD = 0x180000 };
static PwWineDmem lock_engine;
static int lock_inside, lock_overlaps;
static long lock_hold_us;   /* how long a protect of LOCK_HELD keeps the lock */
static int lone_protect(void *context, uintptr_t address, size_t bytes, unsigned protection)
{
    (void)context; (void)bytes; (void)protection;
    if (__atomic_fetch_add(&lock_inside, 1, __ATOMIC_ACQ_REL)) __atomic_add_fetch(&lock_overlaps, 1, __ATOMIC_RELAXED);
    if (address == LOCK_HELD) {
        const struct timespec hold = {lock_hold_us / 1000000, lock_hold_us % 1000000 * 1000};
        nanosleep(&hold, NULL);
    }
    __atomic_sub_fetch(&lock_inside, 1, __ATOMIC_ACQ_REL);
    return 0;
}
static void *lock_worker(void *address)
{
    const unsigned loops = (uintptr_t)address == LOCK_HELD && lock_hold_us > 1000 ? 1 : LOCK_LOOPS;
    for (unsigned i = 0; i < loops; ++i)
        assert(!pw_wine_dmem_protect(&lock_engine, (uintptr_t)address, 0x4000, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE));
    return NULL;
}
static double seconds(clockid_t clock)
{
    struct timespec now;
    assert(!clock_gettime(clock, &now));
    return now.tv_sec + now.tv_nsec / 1e9;
}
static void check_lock_waits(void)
{
    static PwWineDmemRun runs[8];
    const PwWineDmemOps ops = { NULL, 16384, fake_ok, fake_allocate, fake_map, lone_protect, fake_ok, fake_release,
                                0, NULL, NULL };
    PwWineDmemStats before, after;
    pthread_t threads[2];

    assert(!pw_wine_dmem_init(&lock_engine, &ops, runs, 8, 0));
    assert(!pw_wine_dmem_add_region(&lock_engine, 0x100000, 0x100000, 1));
    lock_hold_us = 0;
    lock_worker((void *)LOCK_SHORT);   /* both backed: every protect from here is one kernel call under the lock */
    lock_worker((void *)LOCK_HELD);
    pw_wine_dmem_stats(&lock_engine, &before);
    lock_hold_us = 100;
    assert(!pthread_create(&threads[0], NULL, lock_worker, (void *)LOCK_SHORT));
    assert(!pthread_create(&threads[1], NULL, lock_worker, (void *)LOCK_HELD));
    assert(!pthread_join(threads[0], NULL) && !pthread_join(threads[1], NULL));
    pw_wine_dmem_stats(&lock_engine, &after);
    assert(!lock_overlaps && after.protects - before.protects == 2 * LOCK_LOOPS);

    lock_hold_us = 200000;
    assert(!pthread_create(&threads[1], NULL, lock_worker, (void *)LOCK_HELD));
    while (!__atomic_load_n(&lock_inside, __ATOMIC_ACQUIRE)) sched_yield();
    const double wall = seconds(CLOCK_MONOTONIC), cpu = seconds(CLOCK_THREAD_CPUTIME_ID);
    assert(!pw_wine_dmem_protect(&lock_engine, LOCK_SHORT, 0x4000, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE));
    const double waited = seconds(CLOCK_MONOTONIC) - wall, spent = seconds(CLOCK_THREAD_CPUTIME_ID) - cpu;
    assert(!pthread_join(threads[1], NULL));
    printf("engine lock: 4000 contended protects, no overlap; behind a 200 ms hold waited %.0f ms using %.1f ms of CPU\n",
           waited * 1e3, spent * 1e3);
    assert(!lock_overlaps && waited > 0.1 && spent < waited / 4);
}

/* Whole aligned 2 MiB windows are backed as separate pieces, so the console can map each with a page-directory entry
 * instead of a page table; a range the run table cannot hold falls back to one piece, and a failed piece unmaps and
 * releases the pieces before it. */
static unsigned piece_allocs, piece_maps, piece_unmaps, piece_releases, fail_alloc_at;
static size_t piece_alloc_bytes[16], piece_map_bytes[16];
static int piece_allocate(void *context, size_t bytes, int64_t *offset)
{
    (void)context;
    piece_allocs++;
    if (piece_allocs <= 16) piece_alloc_bytes[piece_allocs - 1] = bytes;
    if (fail_alloc_at && piece_allocs == fail_alloc_at) return -1;
    *offset = (int64_t)piece_allocs * 0x200000; /* aligned, never continues the previous run */
    return 0;
}
static int piece_map(void *context, uintptr_t address, size_t bytes, int64_t offset)
{
    (void)context; (void)address; (void)offset;
    piece_maps++;
    if (piece_maps <= 16) piece_map_bytes[piece_maps - 1] = bytes;
    return 0;
}
static int piece_unmap(void *context, uintptr_t address, size_t bytes)
{
    (void)context; (void)address; (void)bytes;
    piece_unmaps++;
    return 0;
}
static int piece_release(void *context, int64_t offset, size_t bytes)
{
    (void)context; (void)offset; (void)bytes;
    piece_releases++;
    return 0;
}
static void check_big_window_split(void)
{
    static PwWineDmemRun runs[64];
    const uintptr_t big = 0x200000;
    PwWineDmem d;
    PwWineDmemStats stats;
    PwWineDmemOps ops = { NULL, 4096, fake_ok, piece_allocate, piece_map, fake_protect, piece_unmap,
                          piece_release, 0, NULL, NULL };

    piece_allocs = piece_maps = piece_unmaps = piece_releases = 0;
    fail_alloc_at = 0;
    assert(!pw_wine_dmem_init(&d, &ops, runs, 64, 0));
    assert(!pw_wine_dmem_add_region(&d, 0x100000, 0x800000, 1));
    /* [0x100000, 0x900000): a 1 MiB head, three whole windows, a 1 MiB tail */
    assert(!pw_wine_dmem_protect(&d, 0x100000, 0x800000, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE));
    assert(piece_allocs == 5 && piece_maps == 5);
    assert(piece_alloc_bytes[0] == 0x100000 && piece_map_bytes[0] == 0x100000);
    assert(piece_alloc_bytes[1] == big && piece_alloc_bytes[2] == big && piece_alloc_bytes[3] == big);
    assert(piece_alloc_bytes[4] == 0x100000);
    assert(d.run_count == 2); /* contiguous pieces join their run when the direct memory continues */

    /* A run table too small for one entry per piece backs it in one piece. */
    piece_allocs = piece_maps = 0;
    assert(!pw_wine_dmem_init(&d, &ops, runs, 3, 0));
    assert(!pw_wine_dmem_add_region(&d, 0x100000, 0x800000, 1));
    assert(!pw_wine_dmem_protect(&d, 0x100000, 0x800000, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE));
    assert(piece_allocs == 1 && piece_alloc_bytes[0] == 0x800000);

    /* The third piece fails: the first two are unmapped and their direct memory released. */
    piece_allocs = piece_maps = piece_unmaps = piece_releases = 0;
    fail_alloc_at = 3;
    assert(!pw_wine_dmem_init(&d, &ops, runs, 64, 0));
    assert(!pw_wine_dmem_add_region(&d, 0x100000, 0x800000, 1));
    assert(pw_wine_dmem_protect(&d, 0x100000, 0x800000, PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE) == -1);
    assert(piece_allocs == 3 && piece_maps == 2 && piece_unmaps == 1 && piece_releases == 2);
    assert(d.run_count == 0);
    pw_wine_dmem_stats(&d, &stats);
    assert(stats.backed_bytes == 0);
    fail_alloc_at = 0;
}

/* A payload's converted code back to direct memory (pw_wine_dmem_restore, docs/FLEXIBLE_MEMORY.md): the bytes are filled
 * before anything is mapped at the address, execute is dropped, and every refusal says whether the bytes are still there. */
static char restore_log[64];
static unsigned restore_steps;
static int restore_fail_fill, restore_fail_map;
static int64_t restore_released = -1;
static void restore_note(char step) { if (restore_steps < sizeof(restore_log) - 1) restore_log[restore_steps++] = step; }
static int restore_lay(void *context, uintptr_t address, size_t bytes) { (void)context; (void)address; (void)bytes; restore_note('L'); return 0; }
static int restore_allocate(void *context, size_t bytes, int64_t *offset) { (void)context; (void)bytes; restore_note('A'); *offset = 0x7000000; return 0; }
static int restore_map(void *context, uintptr_t address, size_t bytes, int64_t offset)
{
    (void)context; (void)address; (void)bytes; (void)offset;
    restore_note('M');
    return restore_fail_map;
}
static unsigned restore_protection;
static int restore_protect(void *context, uintptr_t address, size_t bytes, unsigned protection)
{
    (void)context; (void)address; (void)bytes;
    restore_note('P');
    restore_protection = protection;
    return 0;
}
static int restore_release(void *context, int64_t offset, size_t bytes) { (void)context; (void)bytes; restore_note('R'); restore_released = offset; return 0; }
static int restore_fill(void *context, int64_t offset, uintptr_t from, size_t bytes)
{
    assert(context == (void *)restore_log && offset == 0x7000000 && from == 0x110000 && bytes == 0x8000);
    restore_note('F');
    return restore_fail_fill;
}
static void check_restore(void)
{
    static PwWineDmemRun runs[8];
    const PwWineDmemOps ops = { NULL, 16384, fake_ok, restore_allocate, restore_map, restore_protect, fake_ok,
                                restore_release, 1, restore_lay, NULL };
    PwWineDmem d;
    PwWineDmemRun run;

    assert(!pw_wine_dmem_init(&d, &ops, runs, 8, 0));
    assert(!pw_wine_dmem_add_region(&d, 0x100000, 0x100000, 1));
    /* Not wholly the caller's (a bare page in it): nothing is asked of the kernel. */
    assert(!pw_wine_dmem_adopt(&d, 0x110000, 0x4000));
    restore_steps = 0;
    assert(pw_wine_dmem_restore(&d, 0x110000, 0x8000, PW_WINE_DMEM_READ | PW_WINE_DMEM_EXEC, restore_fill, restore_log) == -1);
    assert(!restore_steps);
    /* Two touching runs of the caller's: allocated, filled, laid, mapped, protected read-only (execute dropped). */
    assert(!pw_wine_dmem_adopt(&d, 0x114000, 0x4000));
    restore_steps = 0; memset(restore_log, 0, sizeof(restore_log));
    assert(!pw_wine_dmem_restore(&d, 0x110000, 0x8000, PW_WINE_DMEM_READ | PW_WINE_DMEM_EXEC, restore_fill, restore_log));
    assert(!strcmp(restore_log, "AFLMP") && restore_protection == PW_WINE_DMEM_READ);
    assert(pw_wine_dmem_backed(&d, 0x110000, 0x8000) == 0x8000);
    assert(pw_wine_dmem_run_at(&d, 0x114000, &run) && run.offset == 0x7000000 && run.address == 0x110000);
    /* A fill that fails: released, and the caller's mapping stays as it was. */
    assert(!pw_wine_dmem_adopt(&d, 0x110000, 0x8000));
    restore_fail_fill = 1; restore_steps = 0; memset(restore_log, 0, sizeof(restore_log));
    assert(pw_wine_dmem_restore(&d, 0x110000, 0x8000, PW_WINE_DMEM_READ, restore_fill, restore_log) == -1);
    assert(!strcmp(restore_log, "AFR") && !pw_wine_dmem_backed(&d, 0x110000, 0x8000) && !pw_wine_dmem_run_at(&d, 0x110000, &run));
    restore_fail_fill = 0;
    /* A map refused after the lay: -2, released, the range bare. */
    restore_fail_map = 1; restore_steps = 0; memset(restore_log, 0, sizeof(restore_log));
    assert(pw_wine_dmem_restore(&d, 0x110000, 0x8000, PW_WINE_DMEM_READ, restore_fill, restore_log) == -2);
    assert(!strcmp(restore_log, "AFLMR") && !d.run_count);
    restore_fail_map = 0;
    /* Nothing of the caller's left there now. */
    assert(pw_wine_dmem_restore(&d, 0x110000, 0x8000, PW_WINE_DMEM_READ, restore_fill, restore_log) == -1);
}

int main(void)
{
    check_lock_waits();
    check_holder();
    check_last_failure();
    check_big_window_split();
    check_bare_replace();
    check_remove_region();
    check_exec_caller_only();
    check_run_at();
    check_foreign_guard();
    check_pager_guards();
    check_hole_window();
    check_restore();
    PwWineDmem d = {0};
    const uintptr_t page = 16384;
    d.ops.page = page;
    /* Fixed JIT/image reservations arrive out of order and leave gaps. */
    for (unsigned i = PW_WINE_DMEM_MAX_REGIONS; i; i--)
        assert(!pw_wine_dmem_add_region(&d, i * 3 * page, page, 1));
    assert(d.regions == PW_WINE_DMEM_MAX_REGIONS);
    assert(pw_wine_dmem_add_region(&d, page, page, 1) == -1);
    assert(d.regions == PW_WINE_DMEM_MAX_REGIONS);
    assert(!pw_wine_dmem_add_region(&d, 4 * page, page, 1));
    assert(d.regions == PW_WINE_DMEM_MAX_REGIONS);
    assert(!pw_wine_dmem_add_region(&d, 5 * page,
        (PW_WINE_DMEM_MAX_REGIONS * 3 - 5) * page, 1));
    assert(d.regions == 1);
    assert(pw_wine_dmem_owns(&d, 3 * page,
        (PW_WINE_DMEM_MAX_REGIONS * 3 - 2) * page));
    assert(pw_wine_dmem_add_region(&d, 1, page, 1) == -1);
    assert(pw_wine_dmem_add_region(&d, page, 4096, 1) == -1);
    puts("direct-memory regions: 4096 disjoint reservations, full-table extension and bridging, and the last refused "
         "call recorded, a bare reservation freed without kernel calls, an unmapped range no longer owned, execute "
         "only on the caller's runs in a payload and through in a title, adoption asked before it is made, a foreign "
         "entry never mapped over or unmapped and offered as a hole, runs laid back without a hole, the direct run "
         "under an address, the lock's holder named while held, waiters excluded and asleep behind a long hold, passed");
}
