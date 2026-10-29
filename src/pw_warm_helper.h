/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PW_WARM_HELPER_H
#define PW_WARM_HELPER_H
/*
 * One helper started ahead of need (wine/ps5/pw_wine_process.c): carrier spawned through elfldr, rendezvous done, its
 * loader waiting for a bootstrap (native/proton_wine_child.c). A start takes it and sends the bootstrap at once; the
 * next one is made in the background. Everything a process decides (flags, CPU placement, GPU, environment, ntdll)
 * still happens after the bootstrap, so a warm start runs exactly what a cold one does.
 *
 * Why: from one installer custom action's end to the next helper's carrier exec took 0.75-1.1 s on the Pro, the
 * carrier to Wine's first module another 150-250 ms (2026-10-27, docs/PLAN.md). The spawn's part of that leaves the
 * path of the start.
 *
 * Rules, under the caller's lock (pw_wine_process.c's child_lock):
 *  - at most one parked helper, and one being made;
 *  - none once spawns ended (the title ends its game), and none with WINE_PS5_WARM_HELPER=0;
 *  - a parked helper waits PW_WARM_KEEP_MS after the last start, then is closed: none lingers past a burst of starts.
 * A parked helper's loader waits PW_WARM_BOOTSTRAP_MS for its bootstrap, longer than any keep.
 */
#include <string.h>

enum { PW_WARM_KEEP_MS = 30000, PW_WARM_BOOTSTRAP_MS = PW_WARM_KEEP_MS + 30000 };

typedef struct PwWarmHelper {
    int fd;                  /* the title's end of the parked helper's control pair; -1: none parked */
    int pid;                 /* the parked helper's pid; 0 with fd -1 */
    int making;              /* 1 while one is being made */
    long long last_start_ms; /* the last start, monotonic */
} PwWarmHelper;
#define PW_WARM_HELPER_INIT {-1, 0, 0, 0}

/* WINE_PS5_WARM_HELPER: on unless "0". */
static inline int pw_warm_enabled(const char *setting) { return !(setting && !strcmp(setting, "0")); }

/* Whether a start should have the next helper made now. */
static inline int pw_warm_should_make(const PwWarmHelper *warm, int enabled, int ended)
{
    return enabled && !ended && warm->fd < 0 && !warm->making;
}

/* 1 and the parked helper's fd and pid, which are the caller's now; 0 when none is parked. */
static inline int pw_warm_take(PwWarmHelper *warm, int *fd, int *pid)
{
    if (warm->fd < 0) return 0;
    *fd = warm->fd; *pid = warm->pid;
    warm->fd = -1; warm->pid = 0;
    return 1;
}

/* The one being made is ready: 1 when parked, 0 when the caller must close it (spawns ended, or one is parked). */
static inline int pw_warm_park(PwWarmHelper *warm, int fd, int pid, int ended)
{
    warm->making = 0;
    if (ended || warm->fd >= 0 || fd < 0 || pid <= 0) return 0;
    warm->fd = fd; warm->pid = pid;
    return 1;
}

/* How long the helper parked as pid may still wait, in ms; 0: close it now (kept long enough, or spawns ended).
 * -1: it is not parked any more (a start took it). */
static inline long long pw_warm_wait_ms(const PwWarmHelper *warm, int pid, long long now_ms, int ended)
{
    if (warm->fd < 0 || warm->pid != pid) return -1;
    if (ended) return 0;
    const long long left = warm->last_start_ms + PW_WARM_KEEP_MS - now_ms;
    return left > 0 ? left : 0;
}
#endif
