/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the console's kernel says about memory, for the title's memory safety net and for where a helper's memory
 * goes: a payload sent through websrv's launcher (POST /elfldr, pipe=1) like tools/console-processes.c, whose output
 * comes back to the sender. It only reads. tools/console-memory.py builds and sends it.
 *
 *   console-memory            every number the kernel publishes under vm. and hw.'s memory sizes (and any other
 *                             name that speaks of memory, swap or budgets), then every process's resident pages
 *   console-memory map=PID    last, PID's mappings, summed by kind, with the files that have pages in memory
 *   console-memory census     last, the memory helper's census (src/pw_memory_helper.c, built in), served here by
 *                             a thread as the memory helper serves the title, and read back from its page
 *
 * Helpers draw on the memory the console's system services share (about 1 GB; SceShellCore's "VM Stats" klog lines
 * report it every half minute or so), not on the title's.
 *
 * The kernel library's code cannot be read to see which system call a function makes: its text is execute-only, and
 * reading it ended the probe (console-memory-2026-10-07-01, a fault in libkernel_sys.sprx's "xotext"). A game launcher's helpers filled it on 2026-10-07 and the
 * console swapped until it stopped answering (that run's klog). The title needs a figure it can watch. */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#include <sys/user.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../src/pw_memory_helper.h"

/* Each writes two counts in 16 KiB pages, and refuses (EFAULT, writing neither) without the second pointer
 * (console-memory-2026-10-07-07): resident and mapped, SceShellCore's FMEM pair for the second call. */
extern int sceKernelGetResidentCount(int, uint64_t *, uint64_t *);
extern int sceKernelGetResidentFmemCount(int, uint64_t *, uint64_t *);
extern int sceKernelAvailableFlexibleMemorySize(size_t *);
extern int sceKernelConfiguredFlexibleMemorySize(uint64_t *);

static void line(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    putchar('\n');
    fflush(stdout);
}

/* vm. is the VM's own counters. hw. and dev. have handlers that read devices, and debug. has ones that act, so of
 * those only hw.'s memory sizes are read. */
static int wanted(const char *name)
{
    static const char *const words[] = {"mem", "swap", "budget", "page", "rss", "wire"};
    static const char *const sizes[] = {"hw.physmem", "hw.usermem", "hw.realmem", "hw.pagesize", "hw.availpages"};
    if (!strncmp(name, "vm.", 3))
        return 1;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); i++)
        if (!strcmp(name, sizes[i]))
            return 1;
    if (!strncmp(name, "hw.", 3) || !strncmp(name, "dev.", 4) || !strncmp(name, "debug.", 6))
        return 0;
    for (size_t i = 0; i < sizeof(words) / sizeof(*words); i++)
        if (strstr(name, words[i]))
            return 1;
    return 0;
}

static void print_value(const char *name, const int *oid, size_t length, unsigned kind)
{
    unsigned char value[512];
    size_t bytes = sizeof(value);
    char text[600];
    size_t used = 0;
    size_t width;
    int is_signed = 0;

    switch (kind & CTLTYPE) {
    case CTLTYPE_INT: width = 4; is_signed = 1; break;
    case CTLTYPE_UINT: width = 4; break;
    case CTLTYPE_LONG: width = 8; is_signed = 1; break;
    case CTLTYPE_ULONG: case CTLTYPE_U64: width = 8; break;
    case CTLTYPE_S64: width = 8; is_signed = 1; break;
    default: return;   /* strings, structures and nodes say nothing a number would not */
    }
    if (sysctl(oid, (u_int)length, value, &bytes, NULL, 0)) {
        line("sysctl %s: errno %d", name, errno);
        return;
    }
    for (size_t at = 0; at + width <= bytes && used < sizeof(text) - 32; at += width) {
        long long number;
        if (width == 4) {
            int32_t v;
            memcpy(&v, value + at, 4);
            number = is_signed ? (long long)v : (long long)(uint32_t)v;
        } else {
            int64_t v;
            memcpy(&v, value + at, 8);
            number = v;
        }
        used += (size_t)snprintf(text + used, sizeof(text) - used, is_signed ? "%s%lld" : "%s%llu", at ? " " : "",
                                 number);
    }
    line("sysctl %s = %s", name, text);
}

/* The kernel's names, walked with its own introspection (sysctl 0.2 "next", 0.1 "name", 0.4 "kind"). */
static void kernel_numbers(void)
{
    int oid[CTL_MAXNAME] = {1}, next[CTL_MAXNAME], query[CTL_MAXNAME + 2];
    size_t length = 1, count = 0;

    for (;;) {
        size_t bytes = sizeof(next);
        query[0] = 0;
        query[1] = 2;
        memcpy(query + 2, oid, length * sizeof(int));
        if (sysctl(query, (u_int)(length + 2), next, &bytes, NULL, 0)) {
            if (errno != ENOENT)
                line("sysctl walk: errno %d after %zu names", errno, count);
            break;
        }
        length = bytes / sizeof(int);
        memcpy(oid, next, bytes);

        char name[256];
        bytes = sizeof(name);
        query[1] = 1;
        memcpy(query + 2, oid, length * sizeof(int));
        if (sysctl(query, (u_int)(length + 2), name, &bytes, NULL, 0))
            continue;
        if (!wanted(name))
            continue;
        unsigned char kind[64];
        bytes = sizeof(kind);
        query[1] = 4;
        if (sysctl(query, (u_int)(length + 2), kind, &bytes, NULL, 0) || bytes < sizeof(unsigned))
            continue;
        unsigned flags;
        memcpy(&flags, kind, sizeof(flags));
        print_value(name, oid, length, flags);
        count++;
    }
    line("sysctl walk done: %zu names", count);
}

/* The VM's counters by name, for when the walk finds none. */
static void named_numbers(void)
{
    static const char *const names[] = {
        "vm.stats.vm.v_page_count", "vm.stats.vm.v_free_count", "vm.stats.vm.v_wire_count",
        "vm.stats.vm.v_active_count", "vm.stats.vm.v_inactive_count", "vm.stats.vm.v_cache_count",
        "vm.stats.vm.v_laundry_count", "vm.stats.vm.v_free_target", "vm.stats.vm.v_free_min",
        "vm.stats.vm.v_swappgsout", "vm.stats.vm.v_swappgsin", "vm.stats.vm.v_swapout", "vm.stats.vm.v_swapin",
        "vm.swap_total", "vm.swap_reserved", "vm.overcommit", "vm.kmem_size", "hw.physmem", "hw.usermem",
        "hw.realmem", "hw.pagesize", "vm.stats.vm.v_page_size",
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(*names); i++) {
        unsigned char value[16] = {0};
        size_t bytes = sizeof(value);
        if (sysctlbyname(names[i], value, &bytes, NULL, 0)) {
            line("named %s: errno %d", names[i], errno);
            continue;
        }
        uint64_t number = 0;
        memcpy(&number, value, bytes < sizeof(number) ? bytes : sizeof(number));
        line("named %s = %llu (%zu bytes)", names[i], (unsigned long long)number, bytes);
    }
    /* FreeBSD's swap devices (struct xswdev: version, dev, flags, blocks, used, in pages), a node read by number only. */
    int mib[4];
    size_t levels = 3;
    if (sysctlnametomib("vm.swap_info", mib, &levels)) {
        line("named vm.swap_info: errno %d", errno);
        return;
    }
    for (mib[levels] = 0; mib[levels] < 4; mib[levels]++) {
        int device[8] = {0};
        size_t bytes = sizeof(device);
        if (sysctl(mib, (u_int)levels + 1, device, &bytes, NULL, 0)) {
            line("vm.swap_info.%d: errno %d", mib[levels], errno);
            break;
        }
        line("vm.swap_info.%d (%zu bytes): %d %d %d %d %d %d", mib[levels], bytes, device[0], device[1], device[2],
             device[3], device[4], device[5]);
    }
}

static void *records(int *mib, u_int levels, size_t *bytes)
{
    *bytes = 0;
    if (sysctl(mib, levels, NULL, bytes, NULL, 0) != 0 || !*bytes)
        return NULL;
    *bytes += *bytes / 4 + 4096;
    void *raw = malloc(*bytes);
    if (raw && sysctl(mib, levels, raw, bytes, NULL, 0) != 0) {
        free(raw);
        return NULL;
    }
    return raw;
}

static void processes(void)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PROC, 0};
    size_t bytes;
    char *raw = records(mib, 4, &bytes);
    size_t count = 0;

    if (!raw) {
        line("kern.proc failed, errno %d", errno);
        return;
    }
    /* The console's records are ki_structsize apart (1096 bytes, the SDK's are 1088); the fields read come first. */
    for (const char *cursor = raw; cursor + sizeof(int) <= raw + bytes; count++) {
        struct kinfo_proc record;
        const int size = ((const struct kinfo_proc *)cursor)->ki_structsize;
        if (size <= 0 || cursor + size > raw + bytes)
            break;
        memset(&record, 0, sizeof(record));
        memcpy(&record, cursor, (size_t)size < sizeof(record) ? (size_t)size : sizeof(record));
        cursor += size;
        uint64_t resident = UINT64_MAX, resident_mapped = UINT64_MAX, fmem = UINT64_MAX, fmem_mapped = UINT64_MAX;
        const int resident_status = sceKernelGetResidentCount(record.ki_pid, &resident, &resident_mapped);
        const int fmem_status = sceKernelGetResidentFmemCount(record.ki_pid, &fmem, &fmem_mapped);
        line("process pid=%d ppid=%d rss_pages=%ld swrss_pages=%ld vsize=%llu resident=%d/%#llx/%#llx "
             "fmem=%d/%#llx/%#llx name=%s", record.ki_pid, record.ki_ppid, (long)record.ki_rssize,
             (long)record.ki_swrss, (unsigned long long)record.ki_size, resident_status, (unsigned long long)resident,
             (unsigned long long)resident_mapped, fmem_status, (unsigned long long)fmem,
             (unsigned long long)fmem_mapped, record.ki_comm);
    }
    free(raw);
    line("processes done: %zu", count);
}

static void mappings(int pid)
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_VMMAP, pid};
    size_t bytes;
    char *raw = records(mib, 4, &bytes);
    long resident[16] = {0}, private_resident[16] = {0}, regions[16] = {0};
    unsigned long long span[16] = {0};
    size_t count = 0;

    if (!raw) {
        line("map %d: kern.proc.vmmap failed, errno %d", pid, errno);
        return;
    }
    for (const char *cursor = raw; cursor + sizeof(int) <= raw + bytes; count++) {
        struct kinfo_vmentry entry;
        const int size = ((const struct kinfo_vmentry *)cursor)->kve_structsize;
        if (size <= 0 || cursor + size > raw + bytes)
            break;
        memset(&entry, 0, sizeof(entry));
        memcpy(&entry, cursor, (size_t)size < sizeof(entry) ? (size_t)size : sizeof(entry));
        cursor += size;
        if (count < 3)
            line("map %d: record size %d (the SDK's %zu)", pid, size, sizeof(entry));
        const int kind = entry.kve_type >= 0 && entry.kve_type < 15 ? entry.kve_type : 15;
        resident[kind] += entry.kve_resident;
        private_resident[kind] += entry.kve_private_resident;
        regions[kind]++;
        span[kind] += entry.kve_end - entry.kve_start;
        if (entry.kve_resident >= 64 || (entry.kve_path[0] && entry.kve_resident))
            line("map %d: %#llx-%#llx type=%d prot=%#x flags=%#x resident=%d private=%d refs=%d shadows=%d path=%.120s",
                 pid, (unsigned long long)entry.kve_start, (unsigned long long)entry.kve_end, entry.kve_type,
                 entry.kve_protection, entry.kve_flags, entry.kve_resident, entry.kve_private_resident,
                 entry.kve_ref_count, entry.kve_shadow_count, entry.kve_path);
    }
    for (int kind = 0; kind < 16; kind++)
        if (regions[kind])
            line("map %d: type %d: %ld regions, %llu MiB of address space, %ld resident pages, %ld private", pid, kind,
                 regions[kind], span[kind] >> 20, resident[kind], private_resident[kind]);
    free(raw);
    line("map %d done: %zu records", pid, count);
}

static void *serve(void *opaque)
{
    pw_memory_helper_serve(*(int *)opaque);
    return NULL;
}

static void census(void)
{
    static int helper_end;
    int pair[2];
    pthread_t thread;
    struct pw_memory_census taken;

    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair)) {
        line("census: socketpair errno %d", errno);
        return;
    }
    helper_end = pair[1];
    pthread_create(&thread, NULL, serve, &helper_end);
    const int page = pw_memory_census_page(pair[0], 1);
    if (page < 0) {
        line("census: no page, errno %d", errno);
        return;
    }
    const volatile struct pw_memory_census *map = mmap(NULL, sizeof(*map), PROT_READ, MAP_SHARED, page, 0);
    if (map == MAP_FAILED) {
        line("census: map errno %d", errno);
        return;
    }
    for (int i = 0; i < 4; i++) {
        usleep(600000);
        const int status = pw_memory_census_read(map, &taken);
        line("census %d: read %d/%d system=%llu MiB processes=%u helpers=%llu MiB in %u largest=%llu MiB pid %d "
             "status=%d taken=%llu", i, status, status ? errno : 0, (unsigned long long)(taken.system >> 20),
             taken.processes, (unsigned long long)(taken.helpers >> 20), taken.helper_count,
             (unsigned long long)(taken.largest >> 20), taken.largest_pid, taken.status,
             (unsigned long long)taken.taken_ms);
    }
}

int main(int argc, char **argv)
{
    int map_pid = 0, take_census = 0;

    /* websrv may pass the send's args as the whole argv, unsplit and with no program name first. */
    for (int i = 0; i < argc; i++) {
        const char *map = argv[i] ? strstr(argv[i], "map=") : NULL;
        if (map)
            map_pid = atoi(map + 4);
        if (argv[i] && strstr(argv[i], "census"))
            take_census = 1;
    }
    line("console-memory: pid %d, page size %d", getpid(), getpagesize());
    size_t available = 0;
    uint64_t configured = 0;
    const int available_status = sceKernelAvailableFlexibleMemorySize(&available);
    const int configured_status = sceKernelConfiguredFlexibleMemorySize(&configured);
    line("flexible: available %llu (status %#x), configured %llu (status %#x)", (unsigned long long)available,
         (unsigned)available_status, (unsigned long long)configured, (unsigned)configured_status);
    kernel_numbers();
    named_numbers();
    processes();
    if (map_pid)
        mappings(map_pid);
    if (take_census)
        census();
    line("console-memory done");
    return 0;
}
