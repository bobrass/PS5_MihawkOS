/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_dmem.h"
#include "pw_wine_perf.h"
#include "pw_wine_spin.h"
#include <errno.h>
#include <string.h>

enum { READ_WRITE = PW_WINE_DMEM_READ | PW_WINE_DMEM_WRITE };

static void lock(PwWineDmem *d)
{
    pw_wine_spin_lock(&d->lock);
    __atomic_store_n(&d->held_since, __builtin_ia32_rdtsc(), __ATOMIC_RELAXED);
    __atomic_store_n(&d->holder, pw_wine_perf_tid(), __ATOMIC_RELEASE);
}
static void unlock(PwWineDmem *d)
{
    __atomic_store_n(&d->doing, PW_WINE_DMEM_DOING_NONE, __ATOMIC_RELAXED);
    __atomic_store_n(&d->holder, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&d->lock, 0, __ATOMIC_RELEASE);
}
/* The kernel call the holder makes next (PwWineDmem.doing): diagnostics only. */
static void doing(PwWineDmem *d, uint32_t what, uintptr_t address, size_t bytes, unsigned protection, int64_t offset)
{
    __atomic_store_n(&d->doing_address, (uint64_t)address, __ATOMIC_RELAXED);
    __atomic_store_n(&d->doing_bytes, (uint64_t)bytes, __ATOMIC_RELAXED);
    __atomic_store_n(&d->doing_protection, protection, __ATOMIC_RELAXED);
    __atomic_store_n(&d->doing_offset, offset, __ATOMIC_RELAXED);
    __atomic_store_n(&d->doing, what, __ATOMIC_RELEASE);
}

static uintptr_t end_of(const PwWineDmemRun *run) { return run->address + run->bytes; }

/* Counts a failure and says which call it was, for which range (PwWineDmemStats.failed). */
static void failed_at(PwWineDmem *d, uint32_t what, uintptr_t address, size_t bytes)
{
    d->stats.failures++;
    d->stats.failed = what;
    d->stats.failed_address = address;
    d->stats.failed_bytes = bytes;
}

/* The first run that ends above address. */
static uint32_t first_after(const PwWineDmem *d, uintptr_t address)
{
    uint32_t low = 0, high = d->run_count;
    while (low < high) {
        uint32_t mid = (low + high) / 2;
        if (end_of(&d->runs[mid]) <= address) low = mid + 1; else high = mid;
    }
    return low;
}

static int valid(const PwWineDmem *d, uintptr_t address, size_t bytes)
{
    const size_t page = d->ops.page;
    if (!bytes || address % page || bytes % page || address + bytes < address) return 0;
    for (unsigned i = 0; i < d->regions; i++)
        if (address >= d->region_low[i] && address + bytes <= d->region_high[i]) return 1;
    return 0;
}

/* bytes at address became backed (sign 1) or stopped being (-1). */
static void note_backed(PwWineDmem *d, uintptr_t address, size_t bytes, int sign)
{
    const uint64_t four_gib = (uint64_t)1 << 32;
    uint64_t end = (uint64_t)address + bytes;
    uint64_t low = address >= four_gib ? 0 : (end < four_gib ? end : four_gib) - address;

    d->stats.backed_bytes += sign > 0 ? bytes : -(uint64_t)bytes;
    d->stats.low_backed_bytes += sign > 0 ? low : -low;
    if (d->stats.backed_bytes > d->stats.peak_backed_bytes)
        d->stats.peak_backed_bytes = d->stats.backed_bytes;
    if (d->stats.low_backed_bytes > d->stats.peak_low_backed_bytes)
        d->stats.peak_low_backed_bytes = d->stats.low_backed_bytes;
}

static void insert_at(PwWineDmem *d, uint32_t index, PwWineDmemRun run)
{
    memmove(&d->runs[index + 1], &d->runs[index], (d->run_count - index) * sizeof(run));
    d->runs[index] = run;
    if (++d->run_count > d->stats.peak_runs) d->stats.peak_runs = d->run_count;
}

static void remove_at(PwWineDmem *d, uint32_t index)
{
    memmove(&d->runs[index], &d->runs[index + 1], (d->run_count - index - 1) * sizeof(d->runs[0]));
    d->run_count--;
}

/* Drop the runs' share of [low, high), whose mappings are already gone, and
 * release its direct memory. A run cut in the middle becomes two, so the
 * caller keeps one entry free. */
static void forget(PwWineDmem *d, uintptr_t low, uintptr_t high)
{
    uint32_t i = first_after(d, low);

    while (i < d->run_count && d->runs[i].address < high) {
        PwWineDmemRun run = d->runs[i];
        uintptr_t from = run.address > low ? run.address : low;
        uintptr_t to = end_of(&run) < high ? end_of(&run) : high;
        int keep_left = from > run.address, keep_right = to < end_of(&run);

        if (run.offset != PW_WINE_DMEM_CALLER) {
            doing(d, PW_WINE_DMEM_DOING_RELEASE, from, to - from, 0, run.offset + (int64_t)(from - run.address));
            if (d->ops.release(d->ops.context, run.offset + (int64_t)(from - run.address), to - from))
                failed_at(d, PW_WINE_DMEM_FAILED_RELEASE, from, to - from);
            else
                d->stats.releases++;
            note_backed(d, from, to - from, -1);
        }
        if (keep_left) d->runs[i].bytes = from - run.address;
        if (keep_right) {
            PwWineDmemRun right = { to, end_of(&run) - to, run.offset };
            if (run.offset != PW_WINE_DMEM_CALLER) right.offset += (int64_t)(to - run.address);
            if (keep_left) insert_at(d, ++i, right); else d->runs[i] = right;
        }
        if (keep_left || keep_right) i++; else remove_at(d, i);
    }
}

/* Record a run whose range no run touches, joining the runs on either side
 * that continue it; one table entry is free. */
static void add_run(PwWineDmem *d, PwWineDmemRun run)
{
    const int caller = run.offset == PW_WINE_DMEM_CALLER;
    uint32_t i = first_after(d, run.address);
    PwWineDmemRun *before = i ? &d->runs[i - 1] : NULL;
    PwWineDmemRun *after = i < d->run_count ? &d->runs[i] : NULL;

    if (before && end_of(before) != run.address) before = NULL;
    if (before && (caller ? before->offset != PW_WINE_DMEM_CALLER
                          : before->offset == PW_WINE_DMEM_CALLER ||
                            before->offset + (int64_t)before->bytes != run.offset))
        before = NULL;
    if (after && after->address != end_of(&run)) after = NULL;
    if (after && (caller ? after->offset != PW_WINE_DMEM_CALLER
                         : after->offset != run.offset + (int64_t)run.bytes))
        after = NULL;
    if (before) {
        before->bytes += run.bytes;
        if (after) {
            before->bytes += after->bytes;
            remove_at(d, i);
        }
    } else if (after) {
        after->address = run.address;
        after->bytes += run.bytes;
        after->offset = run.offset;
    } else {
        insert_at(d, i, run);
    }
}

/* [address, +bytes), Wine's own mapping, back to a reservation: one laid over it, else unmapped and reserved again.
 * 0, 1 when it was unmapped but not reserved again (a hole), -1 when it could not be unmapped. */
static int lay(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    doing(d, PW_WINE_DMEM_DOING_LAY, address, bytes, 0, -1);
    if (d->ops.lay && !d->ops.lay(d->ops.context, address, bytes)) return 0;
    doing(d, PW_WINE_DMEM_DOING_UNMAP, address, bytes, 0, -1);
    if (d->ops.unmap(d->ops.context, address, bytes)) return -1;
    doing(d, PW_WINE_DMEM_DOING_RESERVE, address, bytes, 0, -1);
    return d->ops.reserve(d->ops.context, address, bytes) ? 1 : 0;
}

/* A page-directory entry covers 2 MiB: a commit that spans whole aligned 2 MiB windows is backed window by window, so
 * the kernel can map each with one page-directory entry instead of a page table (the pool holds ~8k of those). The
 * unaligned head and tail keep page-sized backing. */
#define BIG_PAGE ((size_t)0x200000)

static size_t back_piece_bytes(uintptr_t at, uintptr_t high)
{
    if (at % BIG_PAGE) {
        const size_t to_edge = BIG_PAGE - at % BIG_PAGE;
        return (size_t)(high - at) < to_edge ? (size_t)(high - at) : to_edge;
    }
    return (size_t)(high - at) >= BIG_PAGE ? BIG_PAGE : (size_t)(high - at);
}

/* Back a reserved range that no run touches, with one table entry free. */
static int back_piece(PwWineDmem *d, uintptr_t address, size_t bytes, unsigned protection, int64_t *offset_out)
{
    const PwWineDmemOps *ops = &d->ops;
    uint32_t what = PW_WINE_DMEM_FAILED_ALLOCATE;
    int64_t offset;

    if (ops->exec_caller_only) protection &= ~(unsigned)PW_WINE_DMEM_EXEC;
    doing(d, PW_WINE_DMEM_DOING_ALLOCATE, address, bytes, protection, -1);
    if (ops->allocate(ops->context, bytes, &offset)) goto failed;
    d->stats.allocations++;
    what = PW_WINE_DMEM_FAILED_MAP;
    doing(d, PW_WINE_DMEM_DOING_MAP, address, bytes, protection, offset);
    if (ops->map(ops->context, address, bytes, offset)) goto release;
    d->stats.maps++;
    if (d->zero_fill) memset((void *)address, 0, bytes);
    if (protection != READ_WRITE) {
        what = PW_WINE_DMEM_FAILED_PROTECT;
        doing(d, PW_WINE_DMEM_DOING_PROTECT, address, bytes, protection, offset);
        if (ops->protect(ops->context, address, bytes, protection)) {
            (void)lay(d, address, bytes);
            goto release;
        }
        d->stats.protects++;
    }
    note_backed(d, address, bytes, 1);
    d->stats.commits[bytes < 0x1000 ? 0 : bytes < 0x10000 ? 1 : bytes < 0x100000 ? 2 : bytes < BIG_PAGE ? 3 : 4]++;
    if (address % BIG_PAGE == 0 && bytes % BIG_PAGE == 0) d->stats.commits_2m_aligned++;
    /* Direct memory handed out in order often continues the run before. */
    add_run(d, (PwWineDmemRun){ address, bytes, offset });
    if (offset_out) *offset_out = offset;
    return 0;
release:
    doing(d, PW_WINE_DMEM_DOING_RELEASE, address, bytes, 0, offset);
    (void)ops->release(ops->context, offset, bytes);
failed:
    failed_at(d, what, address, bytes);
    return -1;
}

/* Back [address, +bytes) in 2 MiB windows where the range allows it. All or nothing: a piece that fails unmaps and
 * releases the pieces before it, so the caller's range stays as it was. */
static int back(PwWineDmem *d, uintptr_t address, size_t bytes, unsigned protection)
{
    const uintptr_t high = address + bytes;
    uint32_t pieces = 0;
    uintptr_t at;

    for (at = address; at < high; at += back_piece_bytes(at, high)) pieces++;
    if (pieces == 1 || d->run_count + pieces > d->run_capacity) {
        int64_t offset;
        return back_piece(d, address, bytes, protection, &offset);
    }
    for (at = address; at < high;) {
        const size_t piece = back_piece_bytes(at, high);
        if (back_piece(d, at, piece, protection, NULL)) {
            if (at > address) {
                doing(d, PW_WINE_DMEM_DOING_UNMAP, address, at - address, 0, -1);
                if (d->ops.unmap(d->ops.context, address, at - address))
                    failed_at(d, PW_WINE_DMEM_FAILED_UNMAP, address, at - address);
                forget(d, address, at);
            }
            return -1;
        }
        at += piece;
    }
    return 0;
}

/* A foreign entry's [at, to), found by a skip, a refusal or a scan (both 0): a new one is counted, and pending. */
static void note_hole(PwWineDmem *d, uintptr_t at, uintptr_t to, unsigned protection, int skip, int refusal)
{
    PwWineDmemHole *hole = NULL;

    for (unsigned i = 0; i < d->hole_count && !hole; i++)
        if (d->holes[i].address < to && at < d->holes[i].address + d->holes[i].bytes) hole = &d->holes[i];
    if (!hole) {
        d->stats.foreign_found++;
        if (d->hole_count == PW_WINE_DMEM_HOLES) return;
        hole = &d->holes[d->hole_count++];
        *hole = (PwWineDmemHole){ at, to - at, protection, 0, 0, 0 };
    } else if (at < hole->address || to > hole->address + hole->bytes) {
        const uintptr_t end = to > hole->address + hole->bytes ? to : hole->address + hole->bytes;
        if (at < hole->address) hole->address = at;
        hole->bytes = end - hole->address;
    }
    hole->skips += !!skip;
    hole->refusals += !!refusal;
    hole->pending = 1;
}

/* The first foreign entry in the bare parts of [low, high) (no run: runs are Wine's own mappings and are not asked
 * about; with all, the whole range is one part), on whole pages and clipped to the part: 1 with [*at, *to), recorded;
 * 0 when there is none or no guard; -1 when the kernel did not answer (counted). One query per bare part, and one per
 * entry in it. */
static int find_foreign(PwWineDmem *d, uintptr_t low, uintptr_t high, int skip, int refusal, int all, uintptr_t *at,
                        uintptr_t *to)
{
    const uintptr_t mask = d->ops.page - 1;
    uint32_t i = all ? d->run_count : first_after(d, low);

    if (!d->ops.foreign) return 0;
    for (uintptr_t cursor = low; cursor < high;) {
        uintptr_t start, end, part_end = high;
        unsigned protection;
        int status;

        if (i < d->run_count && d->runs[i].address <= cursor) {
            cursor = end_of(&d->runs[i++]);
            continue;
        }
        if (i < d->run_count && d->runs[i].address < high) part_end = d->runs[i].address;
        doing(d, PW_WINE_DMEM_DOING_FOREIGN, cursor, part_end - cursor, 0, -1);
        status = d->ops.foreign(d->ops.context, cursor, part_end - cursor, &start, &end, &protection);
        if (status < 0) {
            d->stats.query_failures++;
            return -1;
        }
        if (status > 0 && start < part_end && end > cursor) {
            *at = (start > cursor ? start : cursor) & ~mask;
            *to = ((end < part_end ? end : part_end) + mask) & ~mask;
            if (status == 1) note_hole(d, *at, *to, protection, skip, refusal);
            return 1;
        }
        cursor = part_end;
    }
    return 0;
}

/* Before a fixed mapping over [low, high): 1 when a foreign entry lies in its bare parts or the kernel did not answer,
 * counted, recorded, errno EEXIST; 0 when it may go ahead. */
static int refused(PwWineDmem *d, uintptr_t low, uintptr_t high, int all)
{
    uintptr_t at, to;

    if (!find_foreign(d, low, high, 0, 1, all, &at, &to)) return 0;
    d->stats.maps_refused++;
    failed_at(d, PW_WINE_DMEM_FAILED_FOREIGN, low, high - low);
    errno = EEXIST;
    return 1;
}

/* The runs' share of [low, high) back to reservations, stretch by stretch of touching runs: the bare parts between
 * them are reservations already and are never touched (one may hold a foreign entry). 0; -1 when an unmap was refused
 * (nothing more changed); *hole set when a stretch was unmapped but could not be reserved again. One table entry is
 * free (forget). */
static int clear_runs(PwWineDmem *d, uintptr_t low, uintptr_t high, int *hole)
{
    for (uint32_t i = first_after(d, low); i < d->run_count && d->runs[i].address < high; i = first_after(d, low)) {
        const uintptr_t from = d->runs[i].address > low ? d->runs[i].address : low;
        uintptr_t to = end_of(&d->runs[i]);
        int status;

        while (i + 1 < d->run_count && d->runs[i + 1].address == to) to = end_of(&d->runs[++i]);
        if (to > high) to = high;
        if ((status = lay(d, from, to - from)) < 0) {
            failed_at(d, PW_WINE_DMEM_FAILED_UNMAP, from, to - from);
            return -1;
        }
        if (status) {
            failed_at(d, PW_WINE_DMEM_FAILED_RESERVE, from, to - from);
            *hole = 1;
        }
        forget(d, from, to);
        low = to;
    }
    return 0;
}

int pw_wine_dmem_init(PwWineDmem *d, const PwWineDmemOps *ops, PwWineDmemRun *runs,
                      uint32_t capacity, unsigned zero_fill)
{
    if (!d || !ops || !runs || capacity < 2 || !ops->page || (ops->page & (ops->page - 1)) ||
        !ops->reserve || !ops->allocate || !ops->map || !ops->protect || !ops->unmap || !ops->release)
        return -1;
    memset(d, 0, sizeof(*d));
    d->ops = *ops;
    d->runs = runs;
    d->run_capacity = capacity;
    d->zero_fill = !!zero_fill;
    return 0;
}

int pw_wine_dmem_add_region(PwWineDmem *d, uintptr_t address, size_t bytes, int reserved)
{
    uintptr_t low = address, high = address + bytes;
    unsigned first = 0, last;
    int status = -1;

    lock(d);
    if (!bytes || address % d->ops.page || bytes % d->ops.page || high <= low)
        goto done;
    /* Keep regions sorted so a bridging reservation merges every neighbour.
     * A full table may still accept an extension of an existing region. */
    while (first < d->regions && d->region_high[first] < low) first++;
    last = first;
    while (last < d->regions && d->region_low[last] <= high) {
        if (d->region_low[last] < low) low = d->region_low[last];
        if (d->region_high[last] > high) high = d->region_high[last];
        last++;
    }
    if (first == last && d->regions == PW_WINE_DMEM_MAX_REGIONS) goto done;
    if (!reserved) doing(d, PW_WINE_DMEM_DOING_RESERVE, address, bytes, 0, -1);
    if (!reserved && d->ops.reserve(d->ops.context, address, bytes)) goto done;
    memmove(&d->region_low[first + 1], &d->region_low[last],
            (d->regions - last) * sizeof(d->region_low[0]));
    memmove(&d->region_high[first + 1], &d->region_high[last],
            (d->regions - last) * sizeof(d->region_high[0]));
    d->region_low[first] = low;
    d->region_high[first] = high;
    d->regions = d->regions - (last - first) + 1;
    status = 0;
done:
    unlock(d);
    return status;
}

/* [low, high) was unmapped in the kernel: its runs lost their mappings with it, and it is no longer owned. One table
 * entry is free. */
static int remove_locked(PwWineDmem *d, uintptr_t low, uintptr_t high)
{
    int status = 0;

    forget(d, low, high);
    for (unsigned i = 0; i < d->regions;) {
        const uintptr_t region_low = d->region_low[i], region_high = d->region_high[i];
        if (region_high <= low || region_low >= high) { i++; continue; }
        if (region_low < low && region_high > high) {
            if (d->regions == PW_WINE_DMEM_MAX_REGIONS) { status = -1; break; }
            memmove(&d->region_low[i + 2], &d->region_low[i + 1], (d->regions - i - 1) * sizeof(d->region_low[0]));
            memmove(&d->region_high[i + 2], &d->region_high[i + 1], (d->regions - i - 1) * sizeof(d->region_high[0]));
            d->region_high[i] = low;
            d->region_low[i + 1] = high;
            d->region_high[i + 1] = region_high;
            d->regions++;
            break;
        }
        if (region_low < low) { d->region_high[i++] = low; continue; }
        if (region_high > high) { d->region_low[i++] = high; continue; }
        memmove(&d->region_low[i], &d->region_low[i + 1], (d->regions - i - 1) * sizeof(d->region_low[0]));
        memmove(&d->region_high[i], &d->region_high[i + 1], (d->regions - i - 1) * sizeof(d->region_high[0]));
        d->regions--;
    }
    return status;
}

int pw_wine_dmem_unmap(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    const uintptr_t high = address + bytes;
    uintptr_t cursor = address, at, to;
    int status = 0;

    lock(d);
    if (!bytes || address % d->ops.page || bytes % d->ops.page || high <= address || d->run_count + 1 > d->run_capacity) {
        unlock(d);
        return -1;
    }
    while (cursor < high && !status) {
        const int found = find_foreign(d, cursor, high, 1, 0, 0, &at, &to);
        if (found < 0) {
            status = -1;
            break;
        }
        if (found) d->stats.unmaps_skipped++;
        else at = to = high;
        if (cursor < at) {
            doing(d, PW_WINE_DMEM_DOING_UNMAP, cursor, at - cursor, 0, -1);
            if (d->ops.unmap(d->ops.context, cursor, at - cursor)) {
                failed_at(d, PW_WINE_DMEM_FAILED_UNMAP, cursor, at - cursor);
                status = -1;
                break;
            }
            status = remove_locked(d, cursor, at);
        }
        cursor = to;
    }
    unlock(d);
    return status;
}

int pw_wine_dmem_keep(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    uintptr_t cursor = address, at, to;

    lock(d);
    if (!valid(d, address, bytes)) {
        unlock(d);
        return -1;
    }
    while (cursor < address + bytes && find_foreign(d, cursor, address + bytes, 0, 0, 0, &at, &to) > 0) cursor = to;
    unlock(d);
    return 0;
}

int pw_wine_dmem_split(PwWineDmem *d, uintptr_t address, size_t bytes, size_t *piece)
{
    uintptr_t end = address + bytes < address ? UINTPTR_MAX : address + bytes;
    int owned = 0;

    lock(d);
    for (unsigned i = 0; i < d->regions; i++) {
        if (address >= d->region_low[i] && address < d->region_high[i]) {
            owned = 1;
            if (d->region_high[i] < end) end = d->region_high[i];
        } else if (d->region_low[i] > address && d->region_low[i] < end) {
            end = d->region_low[i];
        }
    }
    unlock(d);
    *piece = end - address;
    return owned;
}

int pw_wine_dmem_owns(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    int owns;

    lock(d);
    owns = valid(d, address, bytes);
    unlock(d);
    return owns;
}

/* Table entries a replace may add: one when it cuts a run in two, one for
 * the new run. */
static uint32_t replace_needs(const PwWineDmem *d, uintptr_t address, size_t bytes, unsigned protection)
{
    uint32_t i = first_after(d, address);
    int split = i < d->run_count && d->runs[i].address < address && end_of(&d->runs[i]) > address + bytes;
    return (uint32_t)split + (protection != 0);
}

int pw_wine_dmem_replace(PwWineDmem *d, uintptr_t address, size_t bytes, unsigned protection)
{
    int status, hole = 0;

    protection &= 7;
    lock(d);
    if (!valid(d, address, bytes) ||
        d->run_count + replace_needs(d, address, bytes, protection) > d->run_capacity) {
        failed_at(d, PW_WINE_DMEM_FAILED_OUTSIDE, address, bytes);
        unlock(d);
        return -1;
    }
    if (protection && refused(d, address, address + bytes, 0)) {
        unlock(d);
        return -1;
    }
    /* Only the runs are torn down: a bare part is the reservation asked for already. An emulator build turns most of a
     * 28 GiB placeholder back into a placeholder at GPU start-up, and the console froze in that unmap with "unwire
     * timeout at 0xc80000000" (2026-10-14, five runs; record removed from the tree; in git history). */
    status = clear_runs(d, address, address + bytes, &hole);
    /* A stretch left a hole: whatever took it must not be mapped over. */
    if (!status && protection) status = hole ? -1 : back(d, address, bytes, protection);
    unlock(d);
    return status;
}

int pw_wine_dmem_restore(PwWineDmem *d, uintptr_t address, size_t bytes, unsigned protection,
                         int (*fill)(void *context, int64_t offset, uintptr_t from, size_t bytes), void *fill_context)
{
    const uintptr_t high = address + bytes;
    const PwWineDmemOps *ops = &d->ops;
    uintptr_t at = address;
    int64_t offset;
    int hole = 0, status = -1;
    uint32_t i;

    protection &= 7;
    if (ops->exec_caller_only) protection &= ~(unsigned)PW_WINE_DMEM_EXEC;
    lock(d);
    if (!protection || !valid(d, address, bytes) || d->run_count + 2 > d->run_capacity) {
        failed_at(d, PW_WINE_DMEM_FAILED_OUTSIDE, address, bytes);
        goto done;
    }
    /* Wholly the caller's: touching runs of its own and nothing bare, so no foreign entry can lie under it. */
    for (i = first_after(d, address); at < high; at = end_of(&d->runs[i++]))
        if (i == d->run_count || d->runs[i].address > at || d->runs[i].offset != PW_WINE_DMEM_CALLER) {
            failed_at(d, PW_WINE_DMEM_FAILED_OUTSIDE, address, bytes);
            goto done;
        }
    doing(d, PW_WINE_DMEM_DOING_ALLOCATE, address, bytes, protection, -1);
    if (ops->allocate(ops->context, bytes, &offset)) {
        failed_at(d, PW_WINE_DMEM_FAILED_ALLOCATE, address, bytes);
        goto done;
    }
    d->stats.allocations++;
    /* The bytes go in before anything is mapped at address: a reader there sees them or faults, never others. */
    if (fill(fill_context, offset, address, bytes)) {
        failed_at(d, PW_WINE_DMEM_FAILED_MAP, address, bytes);
        goto release;
    }
    if (clear_runs(d, address, high, &hole)) goto release;   /* the caller's mapping is still there: nothing changed */
    /* From here the range is bare. A failure loses it, and says so with -2: the caller must not count on the bytes. */
    status = -2;
    doing(d, PW_WINE_DMEM_DOING_MAP, address, bytes, protection, offset);
    if (ops->map(ops->context, address, bytes, offset)) {
        failed_at(d, PW_WINE_DMEM_FAILED_MAP, address, bytes);
        goto release;
    }
    d->stats.maps++;
    if (protection != READ_WRITE) {
        doing(d, PW_WINE_DMEM_DOING_PROTECT, address, bytes, protection, offset);
        if (ops->protect(ops->context, address, bytes, protection)) {
            failed_at(d, PW_WINE_DMEM_FAILED_PROTECT, address, bytes);
            (void)lay(d, address, bytes);
            goto release;
        }
        d->stats.protects++;
    }
    note_backed(d, address, bytes, 1);
    add_run(d, (PwWineDmemRun){ address, bytes, offset });
    unlock(d);
    return 0;
release:
    doing(d, PW_WINE_DMEM_DOING_RELEASE, address, bytes, 0, offset);
    (void)ops->release(ops->context, offset, bytes);
done:
    unlock(d);
    return status;
}

int pw_wine_dmem_check(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    int status;

    lock(d);
    status = !valid(d, address, bytes) ? -1 : refused(d, address, address + bytes, 0) ? -1 : 0;
    unlock(d);
    return status;
}

int pw_wine_dmem_check_guards(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    int status;

    lock(d);
    status = !bytes || address % d->ops.page || bytes % d->ops.page || address + bytes < address ? -1
             : refused(d, address, address + bytes, 1) ? -1 : 0;
    unlock(d);
    return status;
}

int pw_wine_dmem_disown(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    const uintptr_t high = address + bytes;
    int status = -1;

    lock(d);
    if (valid(d, address, bytes) && d->run_count + 1 <= d->run_capacity) {
        uint32_t i = first_after(d, address);
        while (i < d->run_count && d->runs[i].address < high && d->runs[i].offset == PW_WINE_DMEM_CALLER) i++;
        if (i == d->run_count || d->runs[i].address >= high) {
            forget(d, address, high);
            status = 0;
        }
    }
    unlock(d);
    return status;
}

int pw_wine_dmem_take_hole(PwWineDmem *d, uintptr_t *address, size_t *bytes)
{
    int taken = 0;

    lock(d);
    for (unsigned i = 0; i < d->hole_count && !taken; i++)
        if (d->holes[i].pending) {
            d->holes[i].pending = 0;
            *address = d->holes[i].address;
            *bytes = d->holes[i].bytes;
            taken = 1;
        }
    unlock(d);
    return taken;
}

int pw_wine_dmem_adopt(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    lock(d);
    if (!valid(d, address, bytes) ||
        d->run_count + replace_needs(d, address, bytes, 1) > d->run_capacity) {
        failed_at(d, PW_WINE_DMEM_FAILED_OUTSIDE, address, bytes);
        unlock(d);
        return -1;
    }
    forget(d, address, address + bytes);
    add_run(d, (PwWineDmemRun){ address, bytes, PW_WINE_DMEM_CALLER });
    unlock(d);
    return 0;
}

/* shortcut: the heap area (pw_wine_dmem_ps5.c) may take the last table entries between this answer and the adopt;
 * hold the lock across both if a conversion ever fails with PW_WINE_DMEM_FAILED_OUTSIDE. */
int pw_wine_dmem_can_adopt(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    int can;

    lock(d);
    can = valid(d, address, bytes) && d->run_count + replace_needs(d, address, bytes, 1) <= d->run_capacity;
    unlock(d);
    return can;
}

int pw_wine_dmem_protect(PwWineDmem *d, uintptr_t address, size_t bytes, unsigned protection)
{
    const uintptr_t high = address + bytes;
    uintptr_t at = address;
    uint32_t i;

    protection &= 7;
    lock(d);
    if (!valid(d, address, bytes)) { failed_at(d, PW_WINE_DMEM_FAILED_OUTSIDE, address, bytes); goto counted; }
    /* The bare parts are backed below with a fixed mapping: refused before any change if one holds a foreign entry. */
    if (protection && refused(d, address, high, 0)) goto counted;
    i = first_after(d, address);
    while (at < high) {
        if (i < d->run_count && d->runs[i].address <= at) {
            uintptr_t to = end_of(&d->runs[i]) < high ? end_of(&d->runs[i]) : high;
            const unsigned kept = d->ops.exec_caller_only && d->runs[i].offset != PW_WINE_DMEM_CALLER
                                      ? protection & ~(unsigned)PW_WINE_DMEM_EXEC : protection;
            doing(d, PW_WINE_DMEM_DOING_PROTECT, at, to - at, kept, d->runs[i].offset + (int64_t)(at - d->runs[i].address));
            if (d->ops.protect(d->ops.context, at, to - at, kept)) {
                failed_at(d, PW_WINE_DMEM_FAILED_PROTECT, at, to - at);
                goto counted;
            }
            d->stats.protects++;
            at = to;
            i++;
        } else {
            uintptr_t to = i < d->run_count && d->runs[i].address < high ? d->runs[i].address : high;
            if (protection) {
                if (d->run_count + 1 > d->run_capacity) {
                    failed_at(d, PW_WINE_DMEM_FAILED_OUTSIDE, at, to - at);
                    goto counted;
                }
                if (back(d, at, to - at, protection)) goto counted;
                i = first_after(d, to);
            }
            at = to;
        }
    }
    unlock(d);
    return 0;
counted:
    unlock(d);
    return -1;
}

int pw_wine_dmem_run_at(PwWineDmem *d, uintptr_t address, PwWineDmemRun *run)
{
    int found = 0;
    uint32_t i;

    if (!d || !run) return 0;
    lock(d);
    i = first_after(d, address);
    if (i < d->run_count && d->runs[i].address <= address && d->runs[i].offset != PW_WINE_DMEM_CALLER) {
        *run = d->runs[i];
        found = 1;
    }
    unlock(d);
    return found;
}

size_t pw_wine_dmem_backed(PwWineDmem *d, uintptr_t address, size_t bytes)
{
    const uintptr_t high = address + bytes;
    size_t total = 0;

    lock(d);
    for (uint32_t i = first_after(d, address); i < d->run_count && d->runs[i].address < high; i++) {
        if (d->runs[i].offset == PW_WINE_DMEM_CALLER) continue;
        uintptr_t from = d->runs[i].address > address ? d->runs[i].address : address;
        uintptr_t to = end_of(&d->runs[i]) < high ? end_of(&d->runs[i]) : high;
        total += to - from;
    }
    unlock(d);
    return total;
}

void pw_wine_dmem_stats(PwWineDmem *d, PwWineDmemStats *stats)
{
    lock(d);
    *stats = d->stats;
    stats->runs = d->run_count;
    unlock(d);
}

static void placement_fill(PwWineDmem *d, PwWineDmemPlacement *p)
{
    const uint64_t big = BIG_PAGE, mask = BIG_PAGE - 1;
    const uint64_t four = (uint64_t)4 << 20; /* a reservation this large implies the application chose the offsets */
    uint32_t i;

    memset(p, 0, sizeof *p);
    p->regions = d->regions;
    p->runs = d->run_count;
    p->backed_bytes = d->stats.backed_bytes;
    memcpy(p->commits, d->stats.commits, sizeof p->commits);
    p->commits_2m_aligned = d->stats.commits_2m_aligned;
    for (i = 0; i < d->run_count; i++) {
        const PwWineDmemRun *r = &d->runs[i];
        uintptr_t at, high;
        unsigned k;
        int big_region = 0;

        if (r->offset == PW_WINE_DMEM_CALLER) continue;
        if (r->address % big == 0 && r->bytes % big == 0 && r->bytes >= big) p->runs_2m++;
        for (k = 0; k < d->regions; k++)
            if (d->region_low[k] <= r->address && r->address < d->region_high[k]) {
                big_region = d->region_high[k] - d->region_low[k] >= four;
                break;
            }
        at = r->address & ~mask;
        high = (r->address + r->bytes + mask) & ~mask;
        for (; at < high; at += big) {
            const uintptr_t from = r->address > at ? r->address : at;
            const uintptr_t to = r->address + r->bytes < at + big ? r->address + r->bytes : at + big;
            const uint64_t bytes = to - from;

            p->windows++;
            if (big_region) p->win_big_region++; else p->win_small_region++;
            if (bytes >= big) p->full++;
            else if (bytes <= big / 8) p->part[0]++;
            else if (bytes <= big / 2) p->part[1]++;
            else p->part[2]++;
        }
    }
}
void pw_wine_dmem_placement_locked(PwWineDmem *d, PwWineDmemPlacement *p)
{
    if (d && p) placement_fill(d, p);
}
void pw_wine_dmem_placement(PwWineDmem *d, PwWineDmemPlacement *p)
{
    if (!d || !p) return;
    lock(d);
    placement_fill(d, p);
    unlock(d);
}
