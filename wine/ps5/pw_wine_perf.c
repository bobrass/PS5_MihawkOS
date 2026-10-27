/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_wine_perf.h"
#include <string.h>
#include <time.h>

static PwWinePerf perf;
static uint64_t tsc_hz;
static uint64_t calibration_tick, calibration_ns;

uint64_t pw_wine_perf_ticks(void)
{
    uint32_t low, high;
    __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static void add(uint64_t *field, uint64_t value)
{
    __atomic_fetch_add(field, value, __ATOMIC_RELAXED);
}

static void raise_to(uint64_t *field, uint64_t value)
{
    uint64_t seen = __atomic_load_n(field, __ATOMIC_RELAXED);
    while (value > seen &&
           !__atomic_compare_exchange_n(field, &seen, value, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

void pw_wine_perf_set_tsc_hz(uint64_t hz)
{
    __atomic_store_n(&tsc_hz, hz, __ATOMIC_RELAXED);
}

/* The set frequency, else one measured against CLOCK_MONOTONIC since the
 * first call (0 until 1 ms has passed). */
static uint64_t frequency(void)
{
    uint64_t hz = __atomic_load_n(&tsc_hz, __ATOMIC_RELAXED), tick, ns;
    if (hz) return hz;
    tick = pw_wine_perf_ticks();
    ns = monotonic_ns();
    if (!__atomic_load_n(&calibration_ns, __ATOMIC_ACQUIRE)) {
        uint64_t expected = 0;
        __atomic_store_n(&calibration_tick, tick, __ATOMIC_RELAXED);
        __atomic_compare_exchange_n(&calibration_ns, &expected, ns, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        return 0;
    }
    if (ns - calibration_ns < 1000000) return 0;
    return (uint64_t)((double)(tick - calibration_tick) * 1e9 / (double)(ns - calibration_ns));
}

uint64_t pw_wine_perf_ticks_to_us(uint64_t ticks, uint64_t hz)
{
    return hz ? (uint64_t)((double)ticks * 1e6 / (double)hz) : 0;
}

void pw_wine_perf_request(unsigned code, const char *name, uint64_t ticks)
{
    add(&perf.server_calls, 1);
    add(&perf.server_ticks, ticks);
    if (code >= PW_PERF_REQUESTS) return;
    add(&perf.request_calls[code], 1);
    add(&perf.request_ticks[code], ticks);
    /* The same static string every time: a racy store is harmless. A request without a real name (a WoW64 request
     * named "", patch 2079) leaves the label to one that has it. */
    if (name && *name && !__atomic_load_n(&perf.request_names[code], __ATOMIC_RELAXED))
        __atomic_store_n(&perf.request_names[code], name, __ATOMIC_RELAXED);
}

void pw_wine_perf_select(int blocked, uint64_t ticks, uint64_t block_ticks)
{
    add(&perf.selects, 1);
    add(&perf.select_ticks, ticks);
    if (blocked) {
        add(&perf.selects_blocked, 1);
        add(&perf.select_block_ticks, block_ticks);
    }
}

void pw_wine_perf_wait(unsigned handles, int wait_all, int polled)
{
    add(&perf.waits, 1);
    add(&perf.wait_handles, handles);
    if (wait_all) add(&perf.waits_all, 1);
    if (polled) add(&perf.waits_polled, 1);
}

void pw_wine_perf_delay(int64_t timeout_100ns)
{
    add(&perf.delays, 1);
    if (!timeout_100ns) {
        add(&perf.delay_yields, 1);
        return;
    }
    /* Relative timeouts are negative; an absolute one is counted, not timed. */
    if (timeout_100ns < 0) add(&perf.delay_requested_us, (uint64_t)(-timeout_100ns) / 10);
}

void pw_wine_perf_media(unsigned what, uint64_t value)
{
    if (what < PW_PERF_MEDIA_COUNTERS) add(&perf.media[what], value);
}

void pw_wine_perf_frame(unsigned api, uint64_t call_ticks)
{
    PwWinePerfFrames *frames;
    uint64_t now = pw_wine_perf_ticks(), last, hz;
    if (api >= PW_PERF_APIS) return;
    frames = &perf.frames[api];
    add(&frames->frames, 1);
    add(&frames->call_ticks, call_ticks);
    raise_to(&frames->call_max, call_ticks);
    last = __atomic_exchange_n(&frames->last, now, __ATOMIC_RELAXED);
    if (!last) {
        uint64_t none = 0;
        __atomic_compare_exchange_n(&frames->first, &none, now, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    }
    hz = frequency();
    if (last && hz && now > last) {
        const uint64_t us = pw_wine_perf_ticks_to_us(now - last, hz);
        const uint64_t bucket = us / PW_PERF_FRAME_BUCKET_US;
        add(&frames->interval[bucket < PW_PERF_FRAME_BUCKETS ? bucket : PW_PERF_FRAME_BUCKETS], 1);
        raise_to(&frames->interval_max, now - last);
    }
}

void pw_wine_perf_snapshot(PwWinePerf *out)
{
    const uint64_t *from = (const uint64_t *)&perf;
    uint64_t *to = (uint64_t *)out;
    /* Field by field so every 64-bit counter is read whole. */
    for (size_t i = 0; i < sizeof(perf) / sizeof(uint64_t); i++)
        to[i] = __atomic_load_n(&from[i], __ATOMIC_RELAXED);
    out->tsc_hz = frequency();
    out->now = pw_wine_perf_ticks();
}

uint32_t pw_wine_perf_frame_percentile(const PwWinePerfFrames *now, const PwWinePerfFrames *before,
                                       unsigned percent)
{
    uint64_t total = 0, seen = 0, target;
    for (unsigned i = 0; i <= PW_PERF_FRAME_BUCKETS; i++)
        total += now->interval[i] - (before ? before->interval[i] : 0);
    if (!total) return 0;
    /* The smallest bucket at or above the requested share (nearest rank). */
    target = (total * percent + 99) / 100;
    if (!target) target = 1;
    for (unsigned i = 0; i <= PW_PERF_FRAME_BUCKETS; i++) {
        seen += now->interval[i] - (before ? before->interval[i] : 0);
        if (seen >= target) return (i + 1) * PW_PERF_FRAME_BUCKET_US;
    }
    return (PW_PERF_FRAME_BUCKETS + 1) * PW_PERF_FRAME_BUCKET_US;
}

uint64_t pw_wine_perf_age_us(uint64_t since)
{
    const uint64_t hz = frequency(), now = pw_wine_perf_ticks();
    return since && now > since ? pw_wine_perf_ticks_to_us(now - since, hz) : 0;
}

pthread_mutex_t *pw_wine_perf_vlock_mutex;
uint64_t pw_wine_perf_vlock_waiters;
/* The record behind PwWinePerfVlock: ticks, not microseconds, and the holder's depth and the last owner. */
static struct {
    uint64_t owner, since, site, doing_since, depth, last_owner;
    const char *doing;
    uint64_t acquires, contended, changes;
    uint64_t longest, longest_owner, longest_site;
    const char *longest_doing;
} vlock;

void pw_wine_perf_vlock_lock(pthread_mutex_t *mutex, const void *site)
{
    uint64_t me;
    if (pthread_mutex_trylock(mutex)) {
        add(&pw_wine_perf_vlock_waiters, 1);
        pthread_mutex_lock(mutex);
        __atomic_fetch_sub(&pw_wine_perf_vlock_waiters, 1, __ATOMIC_RELAXED);
        add(&vlock.contended, 1);
    }
    /* One instruction, so a handler that locks and unlocks in between leaves the depth as it found it. */
    if (__atomic_add_fetch(&vlock.depth, 1, __ATOMIC_RELAXED) != 1) return;
    me = pw_wine_perf_tid();
    __atomic_store_n(&vlock.since, pw_wine_perf_ticks(), __ATOMIC_RELAXED);
    __atomic_store_n(&vlock.site, (uint64_t)(uintptr_t)site, __ATOMIC_RELAXED);
    __atomic_store_n(&vlock.doing, NULL, __ATOMIC_RELAXED);
    add(&vlock.acquires, 1);
    if (__atomic_exchange_n(&vlock.last_owner, me, __ATOMIC_RELAXED) != me) add(&vlock.changes, 1);
    /* Last: a reader that sees this owner reads its since or a newer one. */
    __atomic_store_n(&vlock.owner, me, __ATOMIC_RELEASE);
}

void pw_wine_perf_vlock_unlock(pthread_mutex_t *mutex)
{
    if (!__atomic_sub_fetch(&vlock.depth, 1, __ATOMIC_RELAXED)) {
        const uint64_t held = pw_wine_perf_ticks() - __atomic_load_n(&vlock.since, __ATOMIC_RELAXED);
        if (held > __atomic_load_n(&vlock.longest, __ATOMIC_RELAXED)) {
            __atomic_store_n(&vlock.longest_owner, __atomic_load_n(&vlock.owner, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
            __atomic_store_n(&vlock.longest_site, __atomic_load_n(&vlock.site, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
            __atomic_store_n(&vlock.longest_doing, __atomic_load_n(&vlock.doing, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
            __atomic_store_n(&vlock.longest, held, __ATOMIC_RELAXED);
        }
        __atomic_store_n(&vlock.owner, 0, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(mutex);
}

const char *pw_wine_perf_vlock_doing(const char *tag)
{
    const uint64_t owner = __atomic_load_n(&vlock.owner, __ATOMIC_RELAXED);
    const char *was;
    if (!owner || owner != pw_wine_perf_tid()) return NULL;
    was = __atomic_load_n(&vlock.doing, __ATOMIC_RELAXED);
    __atomic_store_n(&vlock.doing_since, pw_wine_perf_ticks(), __ATOMIC_RELAXED);
    __atomic_store_n(&vlock.doing, tag, __ATOMIC_RELAXED);
    return was;
}

void pw_wine_perf_vlock_read(PwWinePerfVlock *out)
{
    uint64_t owner, again, since, doing_since;
    const char *doing;
    unsigned tries = 0;
    do {
        owner = __atomic_load_n(&vlock.owner, __ATOMIC_ACQUIRE);
        since = __atomic_load_n(&vlock.since, __ATOMIC_RELAXED);
        out->site = __atomic_load_n(&vlock.site, __ATOMIC_RELAXED);
        doing = __atomic_load_n(&vlock.doing, __ATOMIC_RELAXED);
        doing_since = __atomic_load_n(&vlock.doing_since, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        again = __atomic_load_n(&vlock.owner, __ATOMIC_RELAXED);
    } while (owner != again && ++tries < 16);
    /* Changing hands that fast, it is held by nobody for long. */
    if (owner != again) owner = 0;
    out->owner = owner;
    out->held_us = owner ? pw_wine_perf_age_us(since) : 0;
    out->doing = owner ? doing : NULL;
    out->doing_us = owner && doing ? pw_wine_perf_age_us(doing_since) : 0;
    if (!owner) out->site = 0;
    out->waiters = __atomic_load_n(&pw_wine_perf_vlock_waiters, __ATOMIC_RELAXED);
    out->acquires = __atomic_load_n(&vlock.acquires, __ATOMIC_RELAXED);
    out->contended = __atomic_load_n(&vlock.contended, __ATOMIC_RELAXED);
    out->changes = __atomic_load_n(&vlock.changes, __ATOMIC_RELAXED);
    out->longest_us = pw_wine_perf_ticks_to_us(__atomic_load_n(&vlock.longest, __ATOMIC_RELAXED), frequency());
    out->longest_owner = __atomic_load_n(&vlock.longest_owner, __ATOMIC_RELAXED);
    out->longest_site = __atomic_load_n(&vlock.longest_site, __ATOMIC_RELAXED);
    out->longest_doing = __atomic_load_n(&vlock.longest_doing, __ATOMIC_RELAXED);
}
