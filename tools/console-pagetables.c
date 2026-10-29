/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The system's page tables over time, for the drift our title's ends leave in the kernel's count (the base PS5's used
 * count fell about 1,700 over each gate of 2026-10-27/28, from about 4,600 after a boot to a wrap; the Pro printed
 * "page table CPU 4294967019/9216"). A payload sent through websrv's launcher (POST /elfldr, pipe=1) like
 * tools/console-memory.c, whose output streams back to the sender. It only reads: sceKernelGetPageTableStats every
 * 250 ms for the given seconds, a line at each change and every 10 s, stamped with CLOCK_MONOTONIC in ms, the clock of
 * the kernel log's "EXEC ... <N msec>" stamps.
 *
 *   console-pagetables [SECONDS]      default 1200, at most 3600
 */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int sceKernelGetPageTableStats(int *cpu_total, int *cpu_free, int *gpu_total, int *gpu_free);

static long long monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int main(int argc, char **argv)
{
    long seconds = argc > 1 ? strtol(argv[1], NULL, 10) : 1200;
    if (seconds <= 0 || seconds > 3600) seconds = 1200;
    const long long end = monotonic_ms() + seconds * 1000;
    long long said = 0;
    int last[4] = {-1, -1, -1, -1};
    printf("pt start t=%lld seconds=%ld\n", monotonic_ms(), seconds);
    fflush(stdout);
    while (monotonic_ms() < end) {
        int now[4] = {0, 0, 0, 0};
        const int status = sceKernelGetPageTableStats(&now[0], &now[1], &now[2], &now[3]);
        const long long t = monotonic_ms();
        int changed = 0;
        for (int i = 0; i < 4; ++i) changed |= now[i] != last[i];
        if (status || changed || t - said >= 10000) {
            printf("pt t=%lld status=%d cpu_total=%d cpu_free=%d gpu_total=%d gpu_free=%d\n", t, status, now[0], now[1],
                   now[2], now[3]);
            fflush(stdout);
            said = t;
            for (int i = 0; i < 4; ++i) last[i] = now[i];
        }
        usleep(250000);
    }
    printf("pt end t=%lld\n", monotonic_ms());
    return 0;
}
