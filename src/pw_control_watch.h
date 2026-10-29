/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_CONTROL_WATCH_H
#define PW_CONTROL_WATCH_H
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include "../wine/ps5/pw_wine_process.h"

/* Observe lifetime only; another thread owns every control packet. Requesting
 * POLLIN here makes queued signals immediately wake an observer that cannot
 * consume them. A timeout then provides no pacing. Hangup/error are reported
 * by poll even with events=0, including when unread packets remain queued.
 * Returns poll's status and exposes revents for terminal diagnostics. */
static inline int pw_control_watch(int fd,int milliseconds,short *events)
{
    struct pollfd owner={fd,0,0};
    int result=poll(&owner,1,milliseconds);
    *events=owner.revents;
    return result;
}

/* The title's pid from the bootstrap's NAME=VALUE block (WINE_PS5_TITLE_PID, which the title sets for itself and its
 * helpers inherit, wine/ps5/pw_wine_inherit.h), for the loader's watch on the title's own exit: 0 when absent or not a
 * plain decimal number. The block's entries each end in a zero; the list ends with an empty one or at size. */
static inline int pw_control_title_pid(const char *block,size_t size)
{
    static const char name[]="WINE_PS5_TITLE_PID=";
    const char *end=block+size;
    for(const char *entry=block;entry<end && *entry;) {
        size_t length=0;
        while(entry+length<end && entry[length])++length;
        if(length>sizeof(name)-1 && !strncmp(entry,name,sizeof(name)-1)) {
            long value=0;
            for(size_t i=sizeof(name)-1;i<length;++i) {
                if(entry[i]<'0' || entry[i]>'9' || value>99999999L)return 0;
                value=value*10+(entry[i]-'0');
            }
            return (int)value;
        }
        entry+=length+1;
    }
    return 0;
}

/* The helper's backstop (native/proton_wine_child.c's control loop): how long after ntdll stamps a doom reason
 * (wine/ps5/pw_wine_process.h) the loader ends the process without Wine, by reason - 1. GPU, ENGINE, TEST, STALL: 5 s.
 * In one 2026-10-15 run the helper was gone 1-2 s after the broker client's 15 s give-up by the flight clock (6.6 s or
 * less by the mixer's); in another (records removed from the tree; in git history) the give-up came at about klog second 20050 and JSCd aborted at about 20067, so
 * 5 s ends the helper at about 20055 (inferred from the timeline, not shown). EXIT: 20 s, room for a normal exit's
 * write-behind flush and DLL detach (6.6 s or less so far), which a stuck exit never finishes. STALL's 5 s leave
 * room for the stall watch's two thread listings, 2 s apart (pw_control_stalled, below). */
static const long long pw_control_doom_grace_ms[PW_CHILD_DOOMS] = {5000, 20000, 5000, 5000, 5000};
/* The stall watch's rule (native/proton_wine_child.c, once a second on the loader's log thread, no Wine lock): Wine's
 * virtual mutex or the direct-memory engine's lock held PW_CONTROL_STALL_MS or more in one hold (each one's held time
 * starts at its holder's acquisition: pw_wine_perf.h, pw_wine_dmem.h) stamps STALL, once a helper, and the loader ends
 * the helper 5 s later: 8 to 9 s into the hold. The virtual mutex's half leaves out two holds that are not stalls:
 * - the pager's ask (vlock_doing PW_CHILD_PAGER_ASK, wine/ps5/pw_wine_process.h), which waits up to 10 s for the
 *   title by design and is retried after; the memory-stall case keeps the title 5 s from its answer;
 * - every hold once EXIT is stamped (exit_stamp, PW_CHILD_DOOM_EXIT's): after process_exiting Wine's unlock no longer
 *   reaches the record (pw_wine_perf.h), so a hold the exit cut short reads as held for good; EXIT's 20 s bound it.
 * shortcut: a look in the few milliseconds between an ask's answer and the release still counts the wait; count the
 * hold without its asks in the record if a run ever stamps STALL there.
 * Measured: the virtual mutex's longest clean hold was 157 ms (longest_ms, a 469 s run of 2026-10-16, record removed
 * from the tree; 25 ms in gate-hang-ab-2026-10-16-01). The engine lock keeps no longest hold: no look saw it held 2 s in either (the 469 s run's
 * 128 stall lines are waits, the engine held 1 ms at most at them; the gate printed none), so its clean holds stayed
 * under 3 s, a margin not measured below that. In six runs of a Unity game of 2026-10-16 one thread of the earlier emulator experiment
 * held both in NtProtectVirtualMemory until the helper died (in sceKernelMprotect of the guest's direct memory where
 * the record names the call; records removed from the tree, in git history), the last stall lines at 14.9 s to 62.5 s;
 * the Shell UI lost its heartbeat meanwhile, SceShellCore force-aborted JSCd after 31 s in one of them, and all
 * recovered once the helper was gone. This contains the stall; its cause is open (docs/PLAN.md, "Kernel mprotect
 * stall"). The owner whose hold reached it (a kernel thread id), or 0. */
enum { PW_CONTROL_STALL_MS = 3000 };
static inline uint64_t pw_control_stalled(uint64_t vlock_owner, uint64_t vlock_held_us, const char *vlock_doing,
                                          int64_t exit_stamp, uint64_t dmem_owner, uint64_t dmem_held_us)
{
    const int bounded = exit_stamp || (vlock_doing && !strcmp(vlock_doing, PW_CHILD_PAGER_ASK));
    if (vlock_owner && !bounded && vlock_held_us >= PW_CONTROL_STALL_MS * 1000ull) return vlock_owner;
    return dmem_owner && dmem_held_us >= PW_CONTROL_STALL_MS * 1000ull ? dmem_owner : 0;
}
enum { PW_CONTROL_DOOM_SEEN = 1, PW_CONTROL_DOOM_DUE = 2 };
typedef struct PwControlDoom {
    unsigned seen;          /* reasons already reported, bit reason - 1 */
    unsigned due_reason;    /* whose deadline due_ms is */
    long long due_ms;       /* the earliest deadline so far, 0 for none */
} PwControlDoom;
/* One look at the stamps (CLOCK_MONOTONIC ms, 0: not stamped) at now_ms. PW_CONTROL_DOOM_SEEN with *reason for a reason
 * not reported before (call again: one per call); then PW_CONTROL_DOOM_DUE with *reason once now_ms reaches the
 * earliest deadline any stamp set; else 0. A later reason never moves the deadline later. Start from a zeroed watch. */
static inline int pw_control_doom_step(PwControlDoom *watch, const int64_t *stamps, long long now_ms, unsigned *reason)
{
    for (unsigned i = 0; i < PW_CHILD_DOOMS; ++i) {
        if (!stamps[i]) continue;
        const long long due = stamps[i] + pw_control_doom_grace_ms[i];
        if (!watch->due_ms || due < watch->due_ms) { watch->due_ms = due; watch->due_reason = i + 1; }
    }
    for (unsigned i = 0; i < PW_CHILD_DOOMS; ++i)
        if (stamps[i] && !(watch->seen & 1u << i)) { watch->seen |= 1u << i; *reason = i + 1; return PW_CONTROL_DOOM_SEEN; }
    if (watch->due_ms && now_ms >= watch->due_ms) { *reason = watch->due_reason; return PW_CONTROL_DOOM_DUE; }
    return 0;
}
#endif
