/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../wine/ps5/pw_wine_perf.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static PwWinePerf before, after;

/* The virtual mutex's owner record (pw_wine_perf.h): Wine's is recursive, and only its holder writes the record. */
static pthread_mutex_t watched;
static int site_a, site_b;
static void pause_ms(long ms)
{
    struct timespec pause = {ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&pause, NULL);
}

static volatile int contender_in, contender_out;
static uint64_t contender_tid;
static void *contender(void *unused)
{
    PwWinePerfVlock seen;
    (void)unused;
    contender_tid = pw_wine_perf_tid();
    contender_in = 1;
    pw_wine_perf_vlock_lock(&watched, &site_b);
    pw_wine_perf_vlock_read(&seen);
    assert(seen.owner == contender_tid && seen.site == (uint64_t)(uintptr_t)&site_b && !seen.doing);
    while (!contender_out) pause_ms(1);
    pw_wine_perf_vlock_unlock(&watched);
    return NULL;
}
static void *stranger(void *unused)
{
    (void)unused;
    assert(!pw_wine_perf_vlock_doing("stranger"));   /* not the holder: nothing written */
    return NULL;
}

enum { STRESS_THREADS = 4, STRESS_ROUNDS = 100000 };
static uint64_t stress_tids[STRESS_THREADS];
static volatile int stress_ready, stress_done;
static void *stress_writer(void *slot)
{
    stress_tids[(uintptr_t)slot] = pw_wine_perf_tid();
    __atomic_add_fetch(&stress_ready, 1, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&stress_ready, __ATOMIC_SEQ_CST) < STRESS_THREADS + 1) {}
    for (int i = 0; i < STRESS_ROUNDS; i++) {
        pw_wine_perf_vlock_lock(&watched, &site_a);
        if (i % 7 == 0) {   /* recursion, as a fault inside a hold takes it again */
            pw_wine_perf_vlock_lock(&watched, &site_b);
            pw_wine_perf_vlock_doing("nested");
            pw_wine_perf_vlock_unlock(&watched);
        }
        pw_wine_perf_vlock_unlock(&watched);
    }
    return NULL;
}
static void *stress_reader(void *unused)
{
    struct timespec begin, now;
    PwWinePerfVlock seen;
    (void)unused;
    clock_gettime(CLOCK_MONOTONIC, &begin);
    __atomic_add_fetch(&stress_ready, 1, __ATOMIC_SEQ_CST);
    while (!__atomic_load_n(&stress_done, __ATOMIC_SEQ_CST)) {
        pw_wine_perf_vlock_read(&seen);
        clock_gettime(CLOCK_MONOTONIC, &now);
        const uint64_t elapsed_us = (uint64_t)(now.tv_sec - begin.tv_sec) * 1000000 + (uint64_t)(now.tv_nsec - begin.tv_nsec) / 1000;
        int known = !seen.owner;
        for (int i = 0; i < STRESS_THREADS; i++) known |= seen.owner == stress_tids[i];
        assert(known);
        assert(seen.owner ? 1 : !seen.held_us && !seen.doing && !seen.site);
        assert(seen.held_us <= elapsed_us + 1000);   /* the TSC's calibration is good to well under a millisecond here */
        assert(seen.waiters <= STRESS_THREADS);
    }
    return NULL;
}

/* Who takes the mutex when its holder lets go with a thread blocked on it and takes it again gap_us later, busy all
 * the while: the 4 KiB MMU's old yield relocked at once; a run that ends `yield` now comes back through the exit trap,
 * a native store and a fault (~80 us by pw_wine_mmu4k.h's estimates) to its fault handler's trylock. Printed, not
 * asserted: the mutex and the scheduler decide (pw_wine_mmu4k.h, Concurrency). */
enum { HANDOVER_ROUNDS = 50 };
static int handover_round, handover_taken;
static void *handover_waiter(void *unused)
{
    (void)unused;
    for (int round = 1; round <= HANDOVER_ROUNDS; round++) {
        while (__atomic_load_n(&handover_round, __ATOMIC_ACQUIRE) != round) {}
        pw_wine_perf_vlock_lock(&watched, &site_b);
        __atomic_store_n(&handover_taken, round, __ATOMIC_RELEASE);
        pw_wine_perf_vlock_unlock(&watched);
    }
    return NULL;
}
static unsigned handovers(unsigned gap_us)
{
    pthread_t thread;
    unsigned first = 0;
    handover_round = handover_taken = 0;
    pthread_create(&thread, NULL, handover_waiter, NULL);
    for (int round = 1; round <= HANDOVER_ROUNDS; round++) {
        pw_wine_perf_vlock_lock(&watched, &site_a);
        __atomic_store_n(&handover_round, round, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&pw_wine_perf_vlock_waiters, __ATOMIC_RELAXED)) {}
        pause_ms(1);   /* the waiter is asleep in the mutex, not between its count and its lock */
        pw_wine_perf_vlock_unlock(&watched);
        struct timespec from, now;
        clock_gettime(CLOCK_MONOTONIC, &from);
        do clock_gettime(CLOCK_MONOTONIC, &now);
        while ((uint64_t)(now.tv_sec - from.tv_sec) * 1000000 + (uint64_t)(now.tv_nsec - from.tv_nsec) / 1000 < gap_us);
        pw_wine_perf_vlock_lock(&watched, &site_a);
        first += __atomic_load_n(&handover_taken, __ATOMIC_ACQUIRE) == round;
        pw_wine_perf_vlock_unlock(&watched);
        while (__atomic_load_n(&handover_taken, __ATOMIC_ACQUIRE) != round) {}
    }
    pthread_join(thread, NULL);
    return first;
}

static void check_vlock(void)
{
    pthread_mutexattr_t recursive;
    PwWinePerfVlock seen, start;
    pthread_t thread, threads[STRESS_THREADS + 1];
    const uint64_t me = pw_wine_perf_tid();

    pthread_mutexattr_init(&recursive);
    pthread_mutexattr_settype(&recursive, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&watched, &recursive);
    pw_wine_perf_vlock_mutex = &watched;
    /* The TSC is calibrated against CLOCK_MONOTONIC from the first look on. */
    (void)pw_wine_perf_age_us(1);
    pause_ms(5);
    pw_wine_perf_vlock_read(&start);
    assert(!start.owner && !start.held_us && !start.waiters && !start.acquires);

    /* Recursion: the first acquisition stamps, the inner ones change nothing, the last release clears. */
    pw_wine_perf_vlock_lock(&watched, &site_a);
    pw_wine_perf_vlock_lock(&watched, &site_b);
    pw_wine_perf_vlock_read(&seen);
    assert(seen.owner == me && seen.site == (uint64_t)(uintptr_t)&site_a && seen.acquires == 1 && !seen.contended);
    pw_wine_perf_vlock_unlock(&watched);
    pw_wine_perf_vlock_read(&seen);
    assert(seen.owner == me);
    /* Tags: the holder's, the previous one back to restore, a stranger's ignored. */
    assert(!pw_wine_perf_vlock_doing("alias-map"));
    assert(!strcmp(pw_wine_perf_vlock_doing("pager-ask"), "alias-map"));
    pthread_create(&thread, NULL, stranger, NULL);
    pthread_join(thread, NULL);
    pw_wine_perf_vlock_read(&seen);
    assert(!strcmp(seen.doing, "pager-ask"));
    assert(!strcmp(pw_wine_perf_vlock_doing("alias-map"), "pager-ask"));
    /* Held 50 ms: its age, and on release the longest hold with its owner, site and tag. */
    pause_ms(50);
    pw_wine_perf_vlock_read(&seen);
    assert(seen.held_us >= 45000 && seen.held_us < 5000000 && seen.doing_us >= 45000);
    pw_wine_perf_vlock_unlock(&watched);
    pw_wine_perf_vlock_read(&seen);
    assert(!seen.owner && !seen.held_us && !seen.doing && !seen.site);
    assert(seen.longest_us >= 45000 && seen.longest_owner == me && seen.longest_site == (uint64_t)(uintptr_t)&site_a);
    assert(!strcmp(seen.longest_doing, "alias-map"));
    assert(!pw_wine_perf_vlock_doing("free"));   /* nobody holds it */

    /* Contention: a second thread waits (counted while it waits), then holds it; one more owner change. */
    pw_wine_perf_vlock_lock(&watched, &site_a);
    pthread_create(&thread, NULL, contender, NULL);
    while (!contender_in) pause_ms(1);
    for (int i = 0; i < 2000; i++) {
        pw_wine_perf_vlock_read(&seen);
        if (seen.waiters == 1) break;
        pause_ms(1);
    }
    assert(seen.waiters == 1 && seen.owner == me && seen.contended == 0);
    assert(__atomic_load_n(&pw_wine_perf_vlock_waiters, __ATOMIC_RELAXED) == 1);   /* what a 4 KiB MMU run reads */
    pw_wine_perf_vlock_unlock(&watched);
    for (int i = 0; i < 2000; i++) {
        pw_wine_perf_vlock_read(&seen);
        if (seen.owner == contender_tid) break;
        pause_ms(1);
    }
    assert(seen.owner == contender_tid && !seen.waiters && seen.contended == 1);
    assert(seen.acquires == 3 && seen.changes == 2);   /* me, me, the contender: the first ever counts as a change */
    contender_out = 1;
    pthread_join(thread, NULL);

    /* Stress: four writers, a reader that never sees an impossible record, and no acquisition lost. */
    pw_wine_perf_vlock_read(&start);
    for (uintptr_t i = 0; i < STRESS_THREADS; i++) pthread_create(&threads[i], NULL, stress_writer, (void *)i);
    pthread_create(&threads[STRESS_THREADS], NULL, stress_reader, NULL);
    for (int i = 0; i < STRESS_THREADS; i++) pthread_join(threads[i], NULL);
    __atomic_store_n(&stress_done, 1, __ATOMIC_SEQ_CST);
    pthread_join(threads[STRESS_THREADS], NULL);
    pw_wine_perf_vlock_read(&seen);
    assert(!seen.owner && !seen.waiters);
    assert(seen.acquires - start.acquires == STRESS_THREADS * STRESS_ROUNDS);
    assert(seen.contended - start.contended <= STRESS_THREADS * STRESS_ROUNDS);
    assert(seen.changes - start.changes <= seen.acquires - start.acquires);

    const unsigned at_once = handovers(0), after_80us = handovers(80);
    pw_wine_perf_vlock_read(&seen);
    assert(!seen.owner && !seen.waiters);
    printf("pw_wine_perf: a blocked thread took the mutex first %u times in %d with a relock at once, %u in %d with a "
           "relock 80 us later\n", at_once, HANDOVER_ROUNDS, after_80us, HANDOVER_ROUNDS);
    pw_wine_perf_vlock_mutex = NULL;
}

static void *requests(void *unused)
{
    (void)unused;
    for (int i = 0; i < 100000; i++) pw_wine_perf_request(7, "select", 3);
    return NULL;
}

int main(void)
{
    check_vlock();
    /* Concurrent recording loses nothing. */
    pthread_t threads[4];
    pw_wine_perf_snapshot(&before);
    for (int i = 0; i < 4; i++) pthread_create(&threads[i], NULL, requests, NULL);
    for (int i = 0; i < 4; i++) pthread_join(threads[i], NULL);
    pw_wine_perf_request(PW_PERF_REQUESTS + 3, "huge", 5);   /* counted in the total only */
    pw_wine_perf_snapshot(&after);
    assert(after.server_calls - before.server_calls == 400001);
    assert(after.server_ticks - before.server_ticks == 1200005);
    assert(after.request_calls[7] == 400000 && after.request_ticks[7] == 1200000);
    assert(!strcmp(after.request_names[7], "select"));
    /* A request without a name (a WoW64 one, patch 2079) leaves the label to the first that has one: a stack's bytes
     * kept as the label crashed a 32-bit game's census (isaac-load-fix1-2026-10-25-01). */
    pw_wine_perf_request(9, "", 1);
    pw_wine_perf_request(9, NULL, 1);
    pw_wine_perf_snapshot(&after);
    assert(!after.request_names[9]);
    pw_wine_perf_request(9, "get_handle_fd", 1);
    pw_wine_perf_snapshot(&after);
    assert(!strcmp(after.request_names[9], "get_handle_fd"));

    pw_wine_perf_select(0, 10, 0);
    pw_wine_perf_select(1, 30, 20);
    pw_wine_perf_wait(3, 1, 0);
    pw_wine_perf_wait(1, 0, 1);
    pw_wine_perf_delay(0);
    pw_wine_perf_delay(-10000);   /* 1 ms */
    pw_wine_perf_delay(5);        /* absolute: counted, not timed */
    pw_wine_perf_snapshot(&after);
    assert(after.selects == 2 && after.selects_blocked == 1 && after.select_ticks == 40 &&
           after.select_block_ticks == 20);
    assert(after.waits == 2 && after.wait_handles == 4 && after.waits_all == 1 && after.waits_polled == 1);
    assert(after.delays == 3 && after.delay_yields == 1 && after.delay_requested_us == 1000);

    /* Frame intervals: a fixed 1 GHz clock makes them exact. A 16.6 ms frame
     * lands in the 16.5..16.75 ms bucket, reported by its upper edge. */
    pw_wine_perf_set_tsc_hz(1000000000);
    PwWinePerfFrames none;
    memset(&none, 0, sizeof(none));
    assert(pw_wine_perf_frame_percentile(&none, NULL, 50) == 0);
    PwWinePerfFrames frames = none;
    frames.interval[66] = 98;     /* 16.5 ms .. 16.75 ms */
    frames.interval[133] = 2;     /* 33.25 ms .. 33.5 ms */
    assert(pw_wine_perf_frame_percentile(&frames, &none, 50) == 16750);
    assert(pw_wine_perf_frame_percentile(&frames, &none, 98) == 16750);
    assert(pw_wine_perf_frame_percentile(&frames, &none, 99) == 33500);
    assert(pw_wine_perf_frame_percentile(&frames, &none, 100) == 33500);
    frames.interval[PW_PERF_FRAME_BUCKETS] = 100;
    assert(pw_wine_perf_frame_percentile(&frames, &none, 99) == (PW_PERF_FRAME_BUCKETS + 1) * 250);

    /* Recorded presents: the first has no interval, later ones do. */
    pw_wine_perf_snapshot(&before);
    for (int i = 0; i < 5; i++) {
        struct timespec pause = {0, 2000000};
        pw_wine_perf_frame(PW_PERF_API_VULKAN, 100);
        nanosleep(&pause, NULL);
    }
    pw_wine_perf_frame(9, 1);     /* unknown API: ignored */
    pw_wine_perf_snapshot(&after);
    PwWinePerfFrames *vk = &after.frames[PW_PERF_API_VULKAN];
    assert(vk->frames == 5 && vk->call_ticks == 500 && vk->call_max == 100);
    assert(vk->first && vk->first < vk->last);
    uint64_t intervals = 0;
    for (unsigned i = 0; i <= PW_PERF_FRAME_BUCKETS; i++) intervals += vk->interval[i];
    assert(intervals == 4);
    assert(after.frames[PW_PERF_API_OPENGL].frames == 0);
    assert(pw_wine_perf_ticks_to_us(1500, 1000000000) == 1);

    /* Media counters add up between snapshots; an unknown counter is ignored. */
    pw_wine_perf_snapshot(&before);
    pw_wine_perf_media(PW_PERF_MEDIA_OPENS, 1);
    pw_wine_perf_media(PW_PERF_MEDIA_READS, 1);
    pw_wine_perf_media(PW_PERF_MEDIA_BYTES, 65536);
    pw_wine_perf_media(PW_PERF_MEDIA_READS, 1);
    pw_wine_perf_media(PW_PERF_MEDIA_BYTES, 4096);
    pw_wine_perf_media(PW_PERF_MEDIA_COUNTERS, 7);
    pw_wine_perf_snapshot(&after);
    assert(after.media[PW_PERF_MEDIA_OPENS] - before.media[PW_PERF_MEDIA_OPENS] == 1);
    assert(after.media[PW_PERF_MEDIA_READS] - before.media[PW_PERF_MEDIA_READS] == 2);
    assert(after.media[PW_PERF_MEDIA_BYTES] - before.media[PW_PERF_MEDIA_BYTES] == 69632);
    assert(after.media[PW_PERF_MEDIA_SEEKS] == before.media[PW_PERF_MEDIA_SEEKS]);
    puts("pw_wine_perf: ok");
    return 0;
}
