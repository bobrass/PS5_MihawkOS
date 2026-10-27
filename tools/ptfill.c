/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Fill the kernel's page-table pool from a payload: map one 4 KiB page every
 * 2 MiB (one page-table page per mapping), touch it, and stop at the first
 * failure. Sent through websrv's launcher (POST /elfldr, pipe=1).
 *
 * Purpose (docs/HYPERVISOR.md, goal rounds 6-8): find the functional failure
 * point of the page-table pool and whether it fails gracefully (mmap ENOMEM)
 * or panics. Read-only in the sense that it only allocates and frees its own
 * address space; it does not patch anything.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BASE  0x100000000ULL     /* 4 GiB, below the title VA limit */
#define STEP  0x200000ULL        /* 2 MiB: one PTE page per mapping */
#define LIMIT 20000ULL

int main(int argc, char **argv)
{
    unsigned long long mapped = 0;
    unsigned long long limit = LIMIT;
    unsigned long long base = BASE;
    unsigned long long hold = 0;
    char line[256] = "";

    for (int i = 0; i < argc; i++) {
        if (argv[i]) {
            strncat(line, argv[i], sizeof line - strlen(line) - 2);
            strncat(line, " ", sizeof line - strlen(line) - 2);
        }
    }
    const char *p = strstr(line, "count=");
    if (p) limit = strtoull(p + 6, NULL, 0);
    p = strstr(line, "base=");
    if (p) base = strtoull(p + 5, NULL, 0);
    p = strstr(line, "hold=");
    if (p) hold = strtoull(p + 5, NULL, 0);
    printf("ptfill: base=0x%llx count=%llu step=0x%llx\n",
           base, limit, (unsigned long long)STEP);

    for (unsigned long long i = 0; i < limit; i++) {
        void *want = (void *)(uintptr_t)(base + i * STEP);
        void *got = mmap(want, 0x1000, PROT_READ | PROT_WRITE,
                         MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
        if (got == MAP_FAILED) {
            printf("mmap failed at i=%llu addr=0x%llx errno=%d\n",
                   i, (unsigned long long)(base + i * STEP), errno);
            break;
        }
        *(volatile unsigned char *)got = 1;
        mapped++;
    }
    printf("ptfill: mapped %llu pages at 2 MiB spacing\n", mapped);
    if (hold) {
        printf("ptfill: holding %llu s\n", hold);
        fflush(stdout);
        sleep((unsigned)hold);
    }
    return 0;
}
