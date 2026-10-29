/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_MEMORY_HELPER_H
#define PW_MEMORY_HELPER_H
/* The memory helper: a helper process that makes the shared memory the title would otherwise make, so none of it is
 * the title's (src/pw_shared_page.h says why). The title keeps one control socket to it (SOCK_SEQPACKET, one caller
 * at a time) and asks for two things:
 *
 *   alloc  an object of bytes, zero, its pages not yet allocated; its descriptor comes back.
 *   touch  grow an object (sent with the request) to `to` bytes if it is smaller, then write through every page of
 *          [from, to) without changing what is in it, so the pages are the helper's before the title writes them.
 *   dmem   direct memory of bytes from the application's pool, granted to two pids (the title's and a client's): its offset
 *          comes back, not a descriptor; any process granted maps it with sceKernelMapDirectMemory (native/pw_dmem_share.h).
 *          The helper owns it until it is freed or the helper ends, so it outlives every mapper. ENOSYS when built without
 *          PW_MEMORY_HELPER_DMEM (a host build).
 *   census the descriptor of a page the helper writes a census of the console's system memory into every half
 *          second (struct pw_memory_census), leaving out the process `from` (the title). The first request starts it.
 *
 * The helper ends when the title's end of the socket closes.
 *
 * Every request waits PW_MEMORY_REPLY_MS for its answer; the census, the title's first request, waits
 * PW_MEMORY_FIRST_REPLY_MS, because the helper answers it after collecting the section files a previous session left.
 * No answer in time is ETIMEDOUT, an answer that is not this request's (size, magic, kind) is EPROTO, and a helper
 * whose end of the socket is already closed (the request cannot be sent) is ENOTCONN. Each marks the socket dead: its late answer may still be in it, so nothing more is sent and every later call fails at once with
 * ENOTCONN. A status the helper reports never marks it. Before, every title request waited in recvmsg with no deadline
 * under the title's memory lock, inside the GPU broker's and the direct-section table's locks, so one stalled helper
 * wedged the broker, every helper's pager and then the server (2026-10-14; record removed from the tree).
 * A dead socket must not be closed while anything the helper made may be mapped: the helper ends at the close and the
 * kernel frees its direct memory under the processes that map it (native/pw_dmem_share.h). */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The memory the console's system services share, which every helper draws on and the title does not: what each
 * process holds of it (sceKernelGetResidentFmemCount, the first figure of SceShellCore's FMEM klog lines), summed over
 * every process but the title (the first figure of its "VM Stats: RSS" lines). About 245 MiB with nothing running. Past
 * about 850 MiB the console swaps: a .NET (MonoGame) game's helpers reached 857 MiB and settled near 810 (2026-10-06),
 * and a launcher's stayed between 865 and 970 MiB until the console stopped answering (2026-10-07; records removed from
 * the tree, in git history). The kernel
 * publishes no other figure to watch: its free-page and swap counters are not there (console-memory-2026-10-07-05). */
#define PW_MEMORY_CENSUS_MAGIC 0x504d4343u   /* "PMCC" */
struct pw_memory_census {
    uint32_t magic;          /* PW_MEMORY_CENSUS_MAGIC once a census is in */
    uint32_t sequence;       /* odd while one is being written */
    uint64_t taken_ms;       /* when, on CLOCK_MONOTONIC */
    uint64_t system;         /* bytes: every process's, the title's left out */
    uint64_t helpers;        /* bytes: the helpers' (the processes the helper's parent started, the helper among them) */
    uint64_t largest;        /* bytes: the largest helper's */
    int32_t largest_pid;
    uint32_t processes;      /* processes counted */
    uint32_t helper_count;
    int32_t status;          /* 0, or the errno of the census that failed (the figures are the last good one's) */
    uint32_t page_tables;    /* the system's CPU page tables (sceKernelGetPageTableStats as given; 0 when it did not answer) */
    uint32_t page_tables_free;
    uint64_t helpers_mapped; /* bytes: the helpers' mapped figure (the FMEM pair's second), their address space; logged only */
    uint64_t payloads;       /* bytes: the user's payloads' (pw_memory_is_payload), for the title's startup tip */
    uint32_t payload_count;
};

/* A payload: not started by SceSysCore, which starts Sony's processes and titles, and not named as Sony's services and
 * our helpers are ("Sce..."). The base's FMEM lines: elfldr.elf, kstuff.elf, shadowmountplus.elf, websrv.elf,
 * ftpsrv.elf, klogsrv.elf, nanodns.elf, pldmgr.elf, payload. A first rule took only the helpers' loader's children and
 * init's, and read 7 MiB in 2 processes of the base's ~90 (re3-steam-base-2026-10-28). syscore 0: not found, no payload. */
static inline int pw_memory_is_payload(int ppid, int syscore, const char *name)
{
    return syscore > 0 && ppid != syscore && ppid > 0 && name && name[0] &&
           !(name[0] == 'S' && name[1] == 'c' && name[2] == 'e');
}

/* The figure the title's guard acts on: the resident census, never the helpers' mapped figure, which is their address
 * space (memplan-gate-2026-10-28-01: 48 GiB "committed" against 1,163 MiB resident ended every game). */
static inline uint64_t pw_memory_census_guarded(const struct pw_memory_census *census) { return census->system; }

/* The helper: answer requests on control until the other end closes. Returns how many were answered. */
unsigned long pw_memory_helper_serve(int control);

/* The deadlines above, in milliseconds. 5 s is under a helper pager's 10 s wait for a page (pw_wine_process.c); it is
 * not measured yet: the title logs each kind's slowest answer to calibrate it. */
enum { PW_MEMORY_REPLY_MS = 5000, PW_MEMORY_FIRST_REPLY_MS = 30000 };
/* 1 when a request on control was not answered in time or out of turn (see above), else 0. */
int pw_memory_dead(int control);
/* Tests: a shorter deadline than PW_MEMORY_REPLY_MS for every request but the census; 0 restores it. */
void pw_memory_set_reply_ms(int milliseconds);
/* The helper sleeps milliseconds (at most 60000) before it answers: the deadline's test and the console's stall probe.
 * 0, or -1 with errno (EINVAL over 60000, not sent). */
int pw_memory_stall(int control, uint32_t milliseconds);

/* Every call below may also fail with ETIMEDOUT, EPROTO or ENOTCONN (see above). */
/* The title: an object of bytes, or -1 (errno set). */
int pw_memory_alloc(int control, uint64_t bytes);
/* Shared maps/alias coherence/growth, but no byte I/O, private map or shrink
 * contract. Native helper tries physical backing, then ordinary backing on
 * refusal. Broker/transports use this; Wine sections keep pw_memory_alloc. */
int pw_memory_alloc_shared(int control, uint64_t bytes);
/* The title: shared direct memory (see above): 0 with *offset, or -1 with errno. pid_b 0 grants only pid_a. */
int pw_memory_dmem_alloc(int control, uint64_t bytes, int pid_a, int pid_b, int64_t *offset);
/* The title: grant a range the helper owns to one more pid (bytes under 4 GiB). 0, or -1 (errno set). */
int pw_memory_dmem_grant(int control, int64_t offset, uint64_t bytes, int pid);
/* The title: give a range back, once nothing maps it any more. 0, or -1 (errno set). */
int pw_memory_dmem_free(int control, int64_t offset, uint64_t bytes);
/* The title: 0, or -1 (errno set). fd stays the caller's. */
int pw_memory_touch(int control, int fd, uint64_t from, uint64_t to);
/* The title: the census page's descriptor (map it PROT_READ, MAP_SHARED, sizeof(struct pw_memory_census) bytes), or -1
 * (errno set: ENOSYS from a helper built without the census, as a host build is). */
int pw_memory_census_page(int control, int title_pid);
/* Anyone with the page mapped: copy a consistent census out of it. 0, or -1 with errno EAGAIN when none is in yet or
 * the helper kept writing. */
int pw_memory_census_read(const volatile struct pw_memory_census *page, struct pw_memory_census *census);
/* The helper's writer, for tests: write census into page as the helper's thread does. */
void pw_memory_census_write(volatile struct pw_memory_census *page, const struct pw_memory_census *census);

#ifdef __cplusplus
}
#endif
#endif
