/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_memory_guard.h"
#include <string.h>

enum { KEPT = sizeof(((struct pw_memory_guard *)0)->spans) / sizeof(uint32_t) };

void pw_memory_guard_init(struct pw_memory_guard *guard, unsigned limit_mib, unsigned end_mib)
{
    memset(guard, 0, sizeof(*guard));
    guard->limit = (uint64_t)limit_mib << 20;
    guard->end = (uint64_t)(end_mib ? end_mib : limit_mib + PW_MEMORY_GUARD_END_MARGIN_MIB) << 20;
}

enum pw_console_kind pw_console_kind_of(uint64_t direct_bytes)
{
    return !direct_bytes || (direct_bytes >> 20) > PW_CONSOLE_PRO_DIRECT_MIB ? PW_CONSOLE_PRO : PW_CONSOLE_STANDARD;
}

struct pw_memory_marks pw_memory_marks_for(enum pw_console_kind kind, unsigned firmware)
{
    if (kind == PW_CONSOLE_PRO) return (struct pw_memory_marks){850, 950};
    if ((firmware >> 24) >= PW_MEMORY_GUARD_FIRMWARE_MIN) return (struct pw_memory_marks){1750, 0};
    return (struct pw_memory_marks){1650, 0};
}

int pw_plateau_sample(struct pw_plateau *plateau, uint64_t resident_mib, uint64_t now_ms, uint64_t frame_gap_ms,
                      unsigned warn_mib)
{
    if (resident_mib < warn_mib) {
        memset(plateau, 0, sizeof(*plateau));
        return 0;
    }
    /* A new height, or a fall out of the band: the hold starts again from here. */
    if (!plateau->since_ms || resident_mib > plateau->peak_mib + PW_PLATEAU_BAND_MIB ||
        resident_mib + PW_PLATEAU_BAND_MIB < plateau->peak_mib) {
        plateau->peak_mib = resident_mib;
        plateau->since_ms = now_ms;
        plateau->gap_seen = 0;
    } else if (resident_mib > plateau->peak_mib) {
        plateau->peak_mib = resident_mib;
    }
    if (frame_gap_ms > PW_PLATEAU_GAP_MS) plateau->gap_seen = 1;
    return plateau->gap_seen && now_ms - plateau->since_ms >= PW_PLATEAU_HOLD_MS;
}

int pw_memory_start_slowed(uint64_t first_ms, uint64_t last_ms)
{
    return first_ms && last_ms >= 150 && last_ms > 3 * first_ms;
}

unsigned pw_memory_guard_default_end_mib(unsigned limit_mib)
{
    return limit_mib + (limit_mib < 1100 ? PW_MEMORY_GUARD_LAST_RESORT_END_MARGIN_MIB : PW_MEMORY_GUARD_END_MARGIN_MIB);
}

void pw_memory_guard_rearm(struct pw_memory_guard *guard)
{
    pw_memory_guard_init(guard, (unsigned)(guard->limit >> 20), (unsigned)(guard->end >> 20));
}

static void forget(struct pw_memory_guard *guard, unsigned drop)
{
    memmove(guard->spans, guard->spans + drop, (guard->count - drop) * sizeof(*guard->spans));
    memmove(guard->times, guard->times + drop, (guard->count - drop) * sizeof(*guard->times));
    guard->count -= drop;
}

uint64_t pw_memory_guard_pressure_ms(const struct pw_memory_guard *guard)
{
    uint64_t total = 0;
    for (unsigned i = 0; i < guard->count; ++i) total += guard->spans[i];
    return total;
}

enum pw_memory_verdict pw_memory_guard_sample(struct pw_memory_guard *guard, uint64_t system, uint64_t now_ms)
{
    uint64_t span;
    unsigned old = 0;

    if (!guard->limit || now_ms <= guard->last_ms) return PW_MEMORY_FINE;
    /* The time since the last census is counted at this one's level; a census that stopped coming in counts for one
     * interval, not for the whole gap. */
    span = guard->last_ms ? now_ms - guard->last_ms : 0;
    if (span > 1000) span = 1000;
    guard->last_ms = now_ms;
    while (old < guard->count && guard->times[old] + PW_MEMORY_GUARD_WINDOW_MS <= now_ms) ++old;
    if (old) forget(guard, old);
    if (guard->count == KEPT) forget(guard, 1);
    guard->spans[guard->count] = system >= guard->limit ? (uint32_t)span : 0;
    guard->times[guard->count++] = now_ms;

    /* The steep climb: the lowest census of the last 8 s against this one, at or near the limit. */
    {
        unsigned drop = 0, kept = guard->rise_count;
        uint64_t lowest = system;
        while (drop < kept && guard->rise_times[drop] + PW_MEMORY_GUARD_STEEP_WINDOW_MS <= now_ms) ++drop;
        if (drop) {
            memmove(guard->rise_times, guard->rise_times + drop, (kept - drop) * sizeof(*guard->rise_times));
            memmove(guard->rise_bytes, guard->rise_bytes + drop, (kept - drop) * sizeof(*guard->rise_bytes));
            guard->rise_count = kept -= drop;
        }
        for (unsigned i = 0; i < kept; ++i)
            if (guard->rise_bytes[i] < lowest) lowest = guard->rise_bytes[i];
        if (kept == sizeof(guard->rise_times) / sizeof(*guard->rise_times)) {
            memmove(guard->rise_times, guard->rise_times + 1, (kept - 1) * sizeof(*guard->rise_times));
            memmove(guard->rise_bytes, guard->rise_bytes + 1, (kept - 1) * sizeof(*guard->rise_bytes));
            guard->rise_count = --kept;
        }
        guard->rise_times[guard->rise_count] = now_ms;
        guard->rise_bytes[guard->rise_count++] = system;
        if (!guard->ended && guard->end <= (guard->limit + ((uint64_t)PW_MEMORY_GUARD_END_MARGIN_MIB << 20)) &&
            system + ((uint64_t)PW_MEMORY_GUARD_STEEP_NEAR_MIB << 20) >= guard->limit &&
            system - lowest >= ((uint64_t)PW_MEMORY_GUARD_STEEP_RISE_MIB << 20)) {
            guard->ended = guard->closed = guard->steep = 1;
            return PW_MEMORY_END;
        }
    }

    if (system < guard->end) guard->end_since_ms = 0;
    else if (!guard->end_since_ms) guard->end_since_ms = now_ms;
    if (!guard->ended && guard->end_since_ms && now_ms - guard->end_since_ms >= PW_MEMORY_GUARD_END_MS) {
        guard->ended = guard->closed = 1;
        return PW_MEMORY_END;
    }
    if (!guard->closed && pw_memory_guard_pressure_ms(guard) >= PW_MEMORY_GUARD_CLOSE_MS) {
        guard->closed = 1;
        return PW_MEMORY_CLOSE;
    }
    return PW_MEMORY_FINE;
}

enum pw_memory_verdict pw_page_table_sample(struct pw_page_table_guard *guard, uint32_t total, uint32_t free_tables,
                                            uint64_t now_ms)
{
    if (!pw_page_table_known(total, free_tables)) {
        memset(guard, 0, sizeof(*guard));
        return PW_MEMORY_FINE;
    }
    unsigned old = 0;
    while (old < guard->count && now_ms - guard->times[old] > PW_PAGE_TABLE_STEEP_MS) ++old;
    if (old || guard->count == 32) {
        if (!old) old = 1;
        memmove(guard->times, guard->times + old, (guard->count - old) * sizeof(*guard->times));
        memmove(guard->frees, guard->frees + old, (guard->count - old) * sizeof(*guard->frees));
        guard->count -= old;
    }
    uint32_t most = free_tables;
    for (unsigned i = 0; i < guard->count; ++i) if (guard->frees[i] > most) most = guard->frees[i];
    guard->times[guard->count] = now_ms;
    guard->frees[guard->count++] = free_tables;
    if (free_tables > PW_PAGE_TABLE_STEEP_FREE) {
        guard->closed = guard->ended = guard->steep = 0;
        return PW_MEMORY_FINE;
    }
    if (!guard->ended && free_tables <= PW_PAGE_TABLE_END_FREE + (most - free_tables)) {
        guard->ended = guard->closed = 1;
        guard->steep = free_tables > PW_PAGE_TABLE_END_FREE;
        return PW_MEMORY_END;
    }
    if (!guard->closed && free_tables <= PW_PAGE_TABLE_CLOSE_FREE) {
        guard->closed = 1;
        return PW_MEMORY_CLOSE;
    }
    return PW_MEMORY_FINE;
}
