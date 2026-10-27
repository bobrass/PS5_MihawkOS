/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Does the console's kernel act on a shared object's per-mapping charge, or on its physical pages?
 *
 * K14 and K18 (docs/PLAN.md, "Shared code copies") showed that a JIT object mapped by several processes, or twice in
 * one, counts in full in each mapping's resident flexible memory (sceKernelGetResidentFmemCount, what the title's
 * census sums). SceShellCore's VM Stats "RSS" on 13.40 is its own sum of those per-process figures (its FMEM lines add
 * up to it to 0.1 MiB), so it rising per mapping is given, not evidence. What is not known is what the kernel does when
 * that per-mapping total grows while the physical pages do not: swap, evict or refuse, as for distinct memory, or
 * nothing. That decides whether shared code copies could ever give helpers room.
 *
 * shared: one payload makes one JIT shared-memory object of OBJECT MiB and writes every page (physical memory: one
 * object), then maps the same object again, read-only, at each step, reading every page of each mapping, so the
 * resident per-mapping total grows by OBJECT at each step while the physical pages stay the same.
 * distinct, the control: each step makes a new object of OBJECT MiB and writes every page (the same memory type and
 * mapping; only the physical sharing differs), so the per-mapping total and the physical pages grow together.
 * Shared acting at a step >= 2: the kernel acts on the per-mapping total. Shared flat through all steps while distinct
 * acts at a step <= MAPS - 2: it acts on physical pages. Both flat: inconclusive at this total. Holds for 13.x with no
 * title running; the Pro (10.01) prints an RSS limit and may differ.
 *
 *   shared-charge tick                                   start, print one line, end (a reading for the runner)
 *   shared-charge shared|distinct OBJECT_MIB MAPS HOLD_S FIRMWARE
 *     16 <= OBJECT_MIB <= 256, 1 <= MAPS <= 24, OBJECT_MIB * MAPS <= 2048, 5 <= HOLD_S <= 60, FIRMWARE the installed
 *     firmware's top half as hex digits (1340 for 13.40): the probe refuses any other console before it maps anything.
 *
 * Every step needs the runner's grant (tools/run-shared-charge.py writes the highest step allowed to GO after a VM Stats
 * line shows the last step passed its stop rules); without it the probe releases everything and ends, so a runner that
 * dies or loses the console cannot leave the ramp climbing. The runner's stop file ends it within a quarter of a second,
 * and so does an access error on it. Each step is held HOLD s and marked in the klog ("[shared-charge] step=").
 * Also bounded by a deadline re-armed at each step, and by its own resident count: a drop below what its mappings hold
 * means the kernel is evicting its pages, and it releases at once. Whatever ends the process frees every mapping.
 * Only libkernel's own calls: no SDK kernel write (wrapped to a counted refusal), no fork, no /dev/dmem (both crashed the
 * console, 2026-10-12), no direct memory. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <time.h>
#include <unistd.h>

extern int sceKernelJitCreateSharedMemory(const char *, size_t, int, int *);
extern int sceKernelMmap(void *, size_t, int, int, int, off_t, void **);
extern int sceKernelMunmap(void *, size_t);
extern int sceKernelGetResidentFmemCount(int, uint64_t *, uint64_t *);
extern int sceKernelDebugOutText(int, const char *);

#ifndef STOP   /* overridable for a host test */
#define STOP "/data/ps5proton/shared-charge.stop"
#endif
#ifndef GO
#define GO "/data/ps5proton/shared-charge.go"
#endif
#ifndef GRANT_WAIT_S
#define GRANT_WAIT_S 90
#endif
enum { PAGE = 16384, MAX_MAPS = 24, MAX_TOTAL_MIB = 2048 };

/* The SDK's kernel write (its loader's so_open calls it), wrapped at link time (-Wl,--wrap=..., as
 * tools/run-dmem-share.py does): any call is refused and counted in the DONE line, which the runner checks. */
static unsigned sdk_writes;
int __wrap_kernel_mprotect(int pid, unsigned long addr, unsigned long len, int prot)
{
    (void)pid; (void)addr; (void)len; (void)prot;
    ++sdk_writes;
    errno = EPERM;
    return -1;
}
int __wrap_kernel_set_vmem_protection(int pid, unsigned long addr, unsigned long len, int prot)
{
    (void)pid; (void)addr; (void)len; (void)prot;
    ++sdk_writes;
    errno = EPERM;
    return -1;
}

/* One line to the klog first (beside SceShellCore's VM Stats lines), then stdout (the runner's record). */
static void say(const char *format, ...)
{
    char text[512], klog[544];
    va_list args;
    va_start(args, format);
    const int length = vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (length < 0) return;
    snprintf(klog, sizeof(klog), "[shared-charge] %s\n", text);
    sceKernelDebugOutText(0, klog);
    printf("%s\n", text);
    fflush(stdout);
}

/* A fault while touching a mapping (the kernel refusing at fault time), or the deadline: a line naming the step, then
 * _exit (no core of a process holding gigabytes of mappings; the kernel frees them with it). */
static volatile sig_atomic_t current_step;
static void ended(int signal_number)
{
    char text[64] = "[shared-charge] ended signal=";
    size_t at = strlen(text);
    const int values[2] = {signal_number, (int)current_step};
    for (int v = 0; v < 2; ++v) {
        char digits[12];
        int n = 0, value = values[v] < 0 ? 0 : values[v];
        do digits[n++] = (char)('0' + value % 10); while ((value /= 10) && n < 11);
        while (n && at < sizeof(text) - 8) text[at++] = digits[--n];
        if (!v) {
            const char *more = " step=";
            while (*more && at < sizeof(text) - 3) text[at++] = *more++;
        }
    }
    text[at++] = '\n';
    text[at] = 0;
    sceKernelDebugOutText(0, text);
    (void)!write(1, text + 16, at - 16);
    _exit(signal_number == SIGALRM ? 4 : 3);
}

/* Every other process's resident flexible memory, as the title's census reads it (src/pw_memory_helper.c): the
 * kernel taking memory from them shows here within a second, with no wait for SceShellCore, which prints its VM Stats
 * only on the app's own process events. 0 with *kib and *count, or -1. */
/* Each process's figure at the baseline, to name the ones that changed most since (pid, name, KiB). */
enum { TRACKED = 512 };
static struct { int pid; char name[20]; uint64_t kib; } baseline_of[TRACKED], latest_of[TRACKED];
static unsigned baseline_count, latest_count;
static int census_others(uint64_t *kib, unsigned *count)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    static char raw[512 * 1200];
    size_t bytes = sizeof(raw);
    const int self = getpid();
    *kib = 0;
    *count = 0;
    latest_count = 0;
    if (sysctl(mib, 4, raw, &bytes, NULL, 0)) return -1;
    for (const char *cursor = raw; cursor + sizeof(int) <= raw + bytes;) {
        struct kinfo_proc record;
        uint64_t pages = 0, mapped = 0;
        int size;
        memcpy(&size, cursor, sizeof(size));
        if (size <= 0 || cursor + size > raw + bytes) break;
        memset(&record, 0, sizeof(record));
        memcpy(&record, cursor, (size_t)size < sizeof(record) ? (size_t)size : sizeof(record));
        cursor += size;
        /* pid 0 answers with the caller's own figure (2026-10-21: "kernel" moved exactly as this probe did), so it is
         * this process again, not another one. */
        if (record.ki_pid <= 0 || record.ki_pid == self ||
            sceKernelGetResidentFmemCount(record.ki_pid, &pages, &mapped)) continue;
        *kib += pages * 16;
        if (latest_count < TRACKED) {
            latest_of[latest_count].pid = record.ki_pid;
            snprintf(latest_of[latest_count].name, sizeof(latest_of[0].name), "%s", record.ki_comm);
            latest_of[latest_count++].kib = pages * 16;
        }
        ++*count;
    }
    return 0;
}

/* The three processes whose figure moved most since the baseline, as "pid:name:+KiB". */
static void movers(char *text, size_t size)
{
    struct { int pid; const char *name; long long delta; } top[3] = {{0, "", 0}, {0, "", 0}, {0, "", 0}};
    for (unsigned i = 0; i < latest_count; ++i) {
        long long delta = (long long)latest_of[i].kib;
        for (unsigned j = 0; j < baseline_count; ++j)
            if (baseline_of[j].pid == latest_of[i].pid) { delta -= (long long)baseline_of[j].kib; break; }
        const long long size_of = delta < 0 ? -delta : delta;
        for (int k = 0; k < 3; ++k) {
            const long long held = top[k].delta < 0 ? -top[k].delta : top[k].delta;
            if (size_of > held) {
                for (int m = 2; m > k; --m) top[m] = top[m - 1];
                top[k].pid = latest_of[i].pid; top[k].name = latest_of[i].name; top[k].delta = delta;
                break;
            }
        }
    }
    snprintf(text, size, "%d:%s:%+lld,%d:%s:%+lld,%d:%s:%+lld", top[0].pid, top[0].name, top[0].delta, top[1].pid,
             top[1].name, top[1].delta, top[2].pid, top[2].name, top[2].delta);
}
static uint64_t base_others_kib, step_others_min_kib;
#ifndef OTHERS_STOP_KIB
#define OTHERS_STOP_KIB (32 * 1024)   /* other processes losing this much: the kernel is taking memory */
#endif

/* This process's resident and mapped flexible memory, the figure the title's census sums, and the pool's free size. */
static uint64_t resident_kib(void)   /* UINT64_MAX when the kernel does not answer */
{
    uint64_t resident = 0, mapped = 0;
    return sceKernelGetResidentFmemCount(getpid(), &resident, &mapped) ? UINT64_MAX : resident * 16;
}
/* vm.swap_reserved, the kernel's own count of swap-backed memory it has promised: it should count the object once in
 * shared mode and once per step in distinct mode. -1 when it cannot be read (errno in *error). */
static long long swap_reserved_kib(int *error)
{
    uint64_t value = 0;
    size_t size = sizeof(value);
    *error = 0;
    if (sysctlbyname("vm.swap_reserved", &value, &size, NULL, 0)) { *error = errno; return -1; }
    return size == sizeof(uint64_t) ? (long long)(value >> 10) : size == sizeof(uint32_t) ? (long long)((uint32_t)value >> 10) : -1;
}
static void census(const char *phase, int step, int mappings)
{
    uint64_t resident = 0, mapped = 0;
    int error = 0;
    const int status = sceKernelGetResidentFmemCount(getpid(), &resident, &mapped);
    const long long reserved = swap_reserved_kib(&error);
    uint64_t others = 0;
    unsigned count = 0;
    const int listed = census_others(&others, &count);
    char moved[160];
    movers(moved, sizeof(moved));
    say("step=%d phase=%s mappings=%d resident_kib=%" PRIu64 " mapped_kib=%" PRIu64 " status=%#x swap_reserved_kib=%lld "
        "sysctl_errno=%d others_kib=%" PRIu64 " others_min_kib=%" PRIu64 " processes=%u census_status=%d movers=%s", step,
        phase, mappings, resident * 16, mapped * 16, (unsigned)status, reserved, error, others, step_others_min_kib, count,
        listed, moved);
}

/* The stop file, or any error but "not there" on it: a stop. */
static int stop_requested(void)
{
    if (access(STOP, F_OK) == 0) return 1;
    return errno != ENOENT;
}

/* The highest step the runner allows, or -1 when GO cannot be read. */
static long granted(void)
{
    char text[32];
    const int fd = open(GO, O_RDONLY);
    if (fd < 0) return -1;
    const ssize_t got = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (got <= 0) return -1;
    text[got] = 0;
    char *end = NULL;
    const long value = strtol(text, &end, 10);
    return end == text ? -1 : value;
}

/* Wait for the runner's grant of step: 1 granted; 0 with *why "stop" or "nogrant". */
static int wait_grant(long step, const char **why)
{
    for (int i = 0; i < GRANT_WAIT_S * 4; ++i) {
        if (stop_requested()) { *why = "stop"; return 0; }
        if (granted() >= step) return 1;
        usleep(250000);
    }
    *why = "nogrant";
    return 0;
}

/* Hold seconds in quarter seconds: 1 with *why when the runner stops the run, when this process's resident count has
 * dropped margin_kib below expected_kib (the kernel evicting its pages) or cannot be read, or when a quarter second took
 * over 2 s (the console thrashing); 0 when the hold ran out. */
static uint64_t margin_kib;
static int hold_step(int seconds, uint64_t expected_kib, const char **why)
{
    for (int i = 0; i < seconds * 4; ++i) {
        struct timespec before, after;
        clock_gettime(CLOCK_MONOTONIC, &before);
        if (stop_requested()) { *why = "stop"; return 1; }
        if (expected_kib && i % 4 == 3) {
            const uint64_t now = resident_kib();
            if (now == UINT64_MAX) {
                say("FAIL resident count step=%d", (int)current_step);
                *why = "fmem-error";
                return 1;
            }
            if (now + margin_kib < expected_kib) {
                say("evicted step=%d resident_kib=%" PRIu64 " expected_kib=%" PRIu64, (int)current_step, now, expected_kib);
                *why = "evicted";
                return 1;
            }
        }
        if (base_others_kib && i % 4 == 1) {
            uint64_t others = 0;
            unsigned count = 0;
            if (!census_others(&others, &count)) {
                if (others < step_others_min_kib) step_others_min_kib = others;
                if (others + OTHERS_STOP_KIB < base_others_kib) {
                    say("others step=%d others_kib=%" PRIu64 " base_kib=%" PRIu64 " processes=%u", (int)current_step,
                        others, base_others_kib, count);
                    *why = "others";
                    return 1;
                }
            }
        }
        usleep(250000);
        clock_gettime(CLOCK_MONOTONIC, &after);
        if ((after.tv_sec - before.tv_sec) * 1000 + (after.tv_nsec - before.tv_nsec) / 1000000 > 2000) {
            say("starved step=%d", (int)current_step);   /* a quarter second took over 2 s: the console is thrashing */
            *why = "starved";
            return 1;
        }
    }
    return 0;
}

static void *kmap(size_t bytes, int protection, int fd)
{
    void *out = NULL;
    const int rc = sceKernelMmap(NULL, bytes, protection, MAP_SHARED, fd, 0, &out);
    if (rc) errno = (int)((unsigned)rc & 0xffff);
    return rc ? MAP_FAILED : out;
}

static unsigned char pattern(size_t at) { return (unsigned char)(((at / PAGE) * 131u + 7u) & 0xff); }

/* A whole number in base, or -1. */
static long number(const char *text, int base)
{
    char *end = NULL;
    errno = 0;
    const long value = strtol(text, &end, base);
    return errno || end == text || *end || value < 0 ? -1 : value;
}

int main(int argc, char **argv)
{
    int first = 1;
    if (argc > 1 && !strcmp(argv[1], "shared-charge")) first = 2;
    /* tick: a process that starts and ends, nothing else. SceShellCore prints its VM Stats and FMEM lines on process
     * events, not on a timer (a title drawing its menu for 120 s printed none, 2026-10-21), so the runner sends one of
     * these whenever it needs a fresh reading. */
    if (argc > first && !strcmp(argv[first], "tick")) {
        say("tick pid=%d", getpid());
        return 0;
    }
    if (argc < first + 5 || (strcmp(argv[first], "shared") && strcmp(argv[first], "distinct"))) {
        say("FAIL usage: shared-charge shared|distinct OBJECT_MIB MAPS HOLD_S FIRMWARE");
        return 2;
    }
    const int private_mode = !strcmp(argv[first], "distinct");
    const long object_mib = number(argv[first + 1], 10), maps = number(argv[first + 2], 10),
               hold = number(argv[first + 3], 10), expected = number(argv[first + 4], 16);
    if (expected < 0 || object_mib < 16 || object_mib > 256 || maps < 1 || maps > MAX_MAPS || object_mib * maps > MAX_TOTAL_MIB ||
        hold < 5 || hold > 60) {
        say("FAIL arguments out of bounds: object_mib=%ld maps=%ld hold_s=%ld", object_mib, maps, hold);
        return 2;
    }
    unsigned installed = 0;
    size_t size = sizeof(installed);
    if (sysctlbyname("kern.sdk_version", &installed, &size, NULL, 0) || size != sizeof(installed) ||
        (installed >> 16) != (unsigned long)expected) {
        say("FAIL firmware: installed %#x, the run is for %lx: nothing done", installed, (unsigned long)expected);
        return 2;
    }
    if (stop_requested()) {
        say("FAIL the stop file is there before the start (or unreadable): nothing done");
        return 2;
    }
    if (granted() < 0) {
        say("FAIL no grant file: nothing done");
        return 2;
    }
    signal(SIGSEGV, ended);
    signal(SIGBUS, ended);
    signal(SIGILL, ended);
    signal(SIGALRM, ended);
    alarm((unsigned)(GRANT_WAIT_S + hold + 60));   /* re-armed at each step */

    const size_t bytes = (size_t)object_mib << 20;
    void *mapping[MAX_MAPS];
    int objects[MAX_MAPS];
    int held = 0, made = 0, fd = -1, stopped = 0, step = 0;
    const char *why = "";
    uint64_t checksum = 0;

    say("start mode=%s object_mib=%ld maps=%ld hold_s=%ld firmware=%#x pid=%d", private_mode ? "distinct" : "shared",
        object_mib, maps, hold, installed, getpid());
    {
        unsigned count = 0;
        if (census_others(&base_others_kib, &count)) base_others_kib = 0;   /* no listing: that check is off */
        step_others_min_kib = base_others_kib;
        for (baseline_count = 0; baseline_count < latest_count; ++baseline_count) baseline_of[baseline_count] = latest_of[baseline_count];
    }
    census("baseline", 0, 0);
    const uint64_t base_kib = resident_kib();   /* the payload's own memory, before any object */
    margin_kib = ((uint64_t)object_mib << 10) / 4 < 16384 ? ((uint64_t)object_mib << 10) / 4 : 16384;
    if (base_kib == UINT64_MAX) {
        say("FAIL resident count at the start");
        why = "fmem-error";
        stopped = 1;
        goto release;
    }
    if ((stopped = hold_step((int)hold, 0, &why))) goto release;
    census("held", 0, 0);

    for (step = 1; step <= maps; ++step) {
        step_others_min_kib = base_others_kib;
        current_step = step;
        if (!wait_grant(step, &why)) { stopped = 1; break; }
        alarm((unsigned)(GRANT_WAIT_S + hold + 60));
        /* shared: one object, made at the first step; private: a new one at every step. */
        if (step == 1 || private_mode) {
            const int created = sceKernelJitCreateSharedMemory("pw-shared-charge", bytes, 7, &fd);
            if (created) {
                say("refused step=%d create status=%#x mappings=%d", step, (unsigned)created, held);
                why = "refused";
                stopped = 1;
                break;
            }
            objects[made++] = fd;
        }
        void *at = kmap(bytes, step > 1 && !private_mode ? PROT_READ : PROT_READ | PROT_WRITE, fd);
        if (at == MAP_FAILED) {
            say("refused step=%d map errno=%d mappings=%d", step, errno, held);
            why = "refused";
            stopped = 1;
            break;
        }
        mapping[held++] = at;
        volatile unsigned char *p = at;
        if (step == 1 || private_mode) {
            for (size_t i = 0; i < bytes; i += PAGE) {   /* new physical pages */
                if (!(i & ((1u << 20) - 1)) && stop_requested()) { why = "stop"; stopped = 1; goto release; }
                p[i] = pattern(i);
            }
        } else {
            unsigned wrong = 0;
            for (size_t i = 0; i < bytes; i += PAGE) {   /* every page resident in this mapping too */
                if (!(i & ((1u << 20) - 1)) && stop_requested()) { why = "stop"; stopped = 1; goto release; }
                const unsigned char value = p[i];
                checksum += value;
                wrong += value != pattern(i);
            }
            if (wrong) say("mismatch step=%d pages_wrong=%u", step, wrong);
        }
        census("mapped", step, held);
        if ((stopped = hold_step((int)hold, base_kib + (uint64_t)held * ((uint64_t)object_mib << 10), &why))) break;
        census("held", step, held);
    }

release:
    /* Free first, then report: a report that blocks must not keep the mappings. */
    for (int i = 0; i < held; ++i) (void)sceKernelMunmap(mapping[i], bytes);
    for (int i = 0; i < made; ++i) close(objects[i]);
    current_step = 0;
    say("release mappings=%d objects=%d stopped=%d why=%s checksum=%" PRIu64, held, made, stopped, why, checksum);
    census("released", 0, 0);
    /* The kernel's figures after the release, unless the runner stopped the run (then it reads them itself). */
    const char *late = "";
    if (!stopped) (void)hold_step((int)hold, 0, &late);
    alarm(0);
    say("DONE mappings=%d stopped=%d why=%s sdk_writes=%u", held, stopped, why, sdk_writes);
    return 0;
}
