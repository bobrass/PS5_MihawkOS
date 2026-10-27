/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The title's guard over the console's system memory, which every helper draws on and the title does not
 * (src/pw_memory_helper.h has the census it reads). Past about 850 MiB the console swaps. A game whose helpers keep
 * it there swaps it until it stops answering: a launcher's held it between 865 and 970 MiB for about three minutes, and
 * the console had to be restarted (2026-10-07; record removed from the tree, in git history). A game that only passes
 * through is fine: a .NET (MonoGame) game's reached 857 MiB while it loaded, then settled near 810 MiB and played (2026-10-06).
 *
 * So the guard asks the game to close as a user would once the census has stood at the limit (870 MiB unless the
 * title says otherwise) for 20 of the last 30 s, and to be ended at once when it stands 70 MiB past it for 3 s. Each
 * is said once. It reads time from the samples, so a census that stops coming in decides nothing.
 *
 * And a climb that is too steep for either: the console's own limit for the system's resident memory is about
 * 989 MiB ("RSS x / 989" in SceShellCore's VM Stats line), and past it the console swaps and stops answering.
 * 2026-10-11 (record removed from the tree; in git history): a launcher's GPU process went from 60 to 331 MiB and the system from 670 to 919 MiB
 * (the census) in 10 s, and the kernel read 929 then 1026 MiB, the console froze and had to be restarted: the 3 s
 * past 940 MiB came too late. So a census that is within 10 MiB of the limit or past it, and has risen by 120 MiB in
 * the last 8 s, ends the game at once. A slow arrival in the same band (the launcher's 865 to 948 MiB for minutes, the
 * .NET game's 857) is not touched. It applies to the default end mark only: a run that sets its own end mark has
 * chosen to go past the limit.
 *
 * The census counts a shared object in every process that maps it (K14, docs/PLAN.md), so the helpers' sync device
 * (8.6 MiB, since 2026-10-17) adds 8-9 MiB to each helper that uses it: 214 against 153 MiB for one idle desktop's
 * helpers (the Pro, 2026-10-20). Counting it once is not safe: a build that did let a launcher's census reach
 * about 1000 MiB, and SceShellCore's VM Stats read RSS up to 1081 MiB with swap-out going from 38 to 139 MiB, where
 * the census as it is ended the launcher at 872-900 MiB with swap-out flat (the Pro, 2026-10-20; records kept off the tree: they name the launcher).
 * On the Pro (10.01) the raw census is what tracks the console's pressure.
 * Firmware 13 is different (K19, the standard PS5 at 13.40, 2026-10-21): 1.5 GiB of extra mappings of one shared
 * object moved nothing, where 640 MiB of distinct pages made the kernel take 56 MiB from the system's processes. There
 * the title leaves the helpers' shared code copies beyond the first out of the census (native/proton_wine.cpp,
 * wine/ps5/pw_wine_codeshare.h); the sync device's region is still counted per mapping.
 */
#ifndef PW_MEMORY_GUARD_H
#define PW_MEMORY_GUARD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PW_MEMORY_GUARD_LIMIT_MIB 870u      /* the earlier default (to 2026-10-24): the scenario tests of 2026-10-06/11 are fitted to it */
/* The default now (2026-10-24, the owner's decision): a last resort, not a policy. The old build ran Steam and another launcher's game with no
 * guard; the guard's 870 ended a signed-in client window that swaps from about 700 MiB of RSS but runs (2026-10-24: RSS 725 of the
 * kernel's ~1039 at the signed-in peak, swap-out 64 to 113 MiB). 990 sits 49 MiB under the kernel's limit as the freeze it exists
 * for (1026 of 989 on 2026-10-11) is a few seconds of climb at 15-36 MiB/s, so the end mark is 20 MiB past it and the steep rule
 * (a rise of 120 MiB in 8 s to within 10 MiB of the limit) stays armed. A game that wants less says so with PROTON_SYSTEM_MEMORY_LIMIT. */
#define PW_MEMORY_GUARD_LAST_RESORT_MIB 990u
#define PW_MEMORY_GUARD_LAST_RESORT_END_MARGIN_MIB 20u
/* Where the Pro's kernel starts to pay for real memory (pro-charge-distinct-2026-10-24-05: other processes trimmed from printed RSS
 * 540, swap-out 63 to 126 MiB at RSS 824; one object mapped again and again moved nothing up to 1175 MiB per-mapping, so only physical
 * pages count). The census counts a shared code copy once on every console now, so it reads about what the kernel acts on. At this census
 * the title only says so, once a session, with a notice; nothing is closed or ended below the last resort. */
#define PW_MEMORY_GUARD_SWAP_NOTICE_MIB 800u
/* The notice's mark from firmware 12.00 on (pw_memory_guard_swap_notice_mib). The kernel there gives no swap figure to
 * watch (13.60, 2026-10-26: vm.stats.vm.* and vm.swap_info answer ENOENT; vm.swap_total and vm.swap_reserved are sizes),
 * so it is where the standard PS5's swap-out grew: flat at a census of 1631 MiB (359 to 364 MiB over a sign-in run),
 * 255 MiB with Steam at 1700, 302 MiB at 1865. The console's own processes hold 731-1223 MiB before Wine starts, so 800
 * said it in every session (the Direct3D 12 game peaked at 917 MiB with swap-out flat at 334-336 MiB). */
#define PW_MEMORY_GUARD_SWAP_NOTICE_13_MIB 1700u
/* The limit on firmware 12.00 and later, measured on the base PS5 (13.40, 2026-10-20) and checked on a second 13.x
 * console. The census there is not the Pro's: with nothing of Wine's running it read 731 to 1223 MiB at the start of
 * launches on one console, 87 processes each time (the user's own payloads and services; about 245 MiB on the Pro),
 * and a Wine start is ten helpers holding 270-430 MiB, so the census went to 1237-1537 MiB in every gate launch. The
 * console took it: its own resident total reached 1447 MiB over a whole gate (1453 on the second) with swap-out at
 * 13.4 to 19.5 MiB throughout, so its swap does not begin where the Pro's does. A mark from the first census cannot
 * follow it (marks of +200, +300, +400 over it, and one that grew with it, ended the helpers of every launch:
 * base-desktop-01, -02, base-gate-01, -02, -03); the mark is the console generation's. The Pro (firmware 10.01) keeps
 * PW_MEMORY_GUARD_LIMIT_MIB. Steam needs more (standard 13.40, 2026-10-21): with the guard off its corrected census
 * peaked at 1700 MiB (1717 raw, 96 MiB of shared copies beyond the first left out) while the console held 1760-1856
 * MiB RSS with 255 MiB swap, drew 1074 frames and closed cleanly; at 1600 the guard ended a webhelper each time Steam
 * restarted it (startcount 0..5). 1800 keeps that below the close rule, and the steep band guards a runaway climb. */
#define PW_MEMORY_GUARD_FIRMWARE_MIN 0x12u   /* the version word's high byte, as BCD: 12.00 and later */
#define PW_MEMORY_GUARD_LIMIT_13_MIB 1800u
#define PW_MEMORY_GUARD_END_MARGIN_MIB 70u   /* how far past it a game is ended at once */
#define PW_MEMORY_GUARD_WINDOW_MS 30000u     /* the window the limit is counted over */
#define PW_MEMORY_GUARD_CLOSE_MS 20000u      /* how long at the limit, within it, asks the game to close */
#define PW_MEMORY_GUARD_END_MS 3000u         /* how long past the end mark ends it */
#define PW_MEMORY_GUARD_STEEP_RISE_MIB 120u  /* a rise this big within the steep window, at the limit or near it... */
#define PW_MEMORY_GUARD_STEEP_WINDOW_MS 8000u
#define PW_MEMORY_GUARD_STEEP_NEAR_MIB 10u   /* ...counts from this far below the limit, and ends the game at once */

enum pw_memory_verdict {
    PW_MEMORY_FINE,     /* nothing to do */
    PW_MEMORY_CLOSE,    /* ask the game to close (said once) */
    PW_MEMORY_END,      /* end it now (said once) */
};

struct pw_memory_guard {
    uint64_t limit, end;               /* bytes; limit 0 is no guard */
    uint64_t last_ms;                  /* the previous sample's time, 0 before the first */
    uint64_t end_since_ms;             /* when the census went past the end mark and stayed, 0 when it is not */
    uint32_t spans[64];                /* the time each recent sample stood at the limit, ms, oldest first */
    uint64_t times[64];                /* and when each was taken */
    unsigned count;                    /* how many of them are kept */
    int closed, ended;                 /* each verdict is said once */
    int steep;                         /* the END came from the steep-climb rule, not the end mark (its log names it) */
    uint64_t rise_times[32];           /* recent censuses for the steep-climb rule */
    uint64_t rise_bytes[32];
    unsigned rise_count;
};

/* limit_mib 0 makes a guard that never acts; end_mib 0 puts the end mark PW_MEMORY_GUARD_END_MARGIN_MIB past the
 * limit. */
void pw_memory_guard_init(struct pw_memory_guard *guard, unsigned limit_mib, unsigned end_mib);
/* A census of system bytes taken at now_ms (the census's own time; one not newer than the last is ignored). */
enum pw_memory_verdict pw_memory_guard_sample(struct pw_memory_guard *guard, uint64_t system, uint64_t now_ms);
/* The default limit of a console, MiB, from the INSTALLED firmware's version word (pw_firmware_installed, src/pw_firmware.h;
 * never sceKernelGetSystemSwVersion, which an enabler spoofs: the Pro said 12.09 and ran with the 12.00-and-later limit
 * through alpha-2): the Pro's for an earlier firmware than 12.00 or none known (0), PW_MEMORY_GUARD_LIMIT_13_MIB from 12.00
 * on. The end mark's default is PW_MEMORY_GUARD_END_MARGIN_MIB past it. */
unsigned pw_memory_guard_default_limit_mib(unsigned firmware);
/* The census at which the title says the console starts to swap, MiB, from the same firmware word: PW_MEMORY_GUARD_SWAP_NOTICE_MIB
 * before 12.00 or none known, PW_MEMORY_GUARD_SWAP_NOTICE_13_MIB from 12.00 on. */
unsigned pw_memory_guard_swap_notice_mib(unsigned firmware);
/* The end mark that goes with a default limit: 20 MiB past the last-resort limit, 70 past the others. */
unsigned pw_memory_guard_default_end_mib(unsigned limit_mib);
/* After the guard acted on one process in the desktop session (the title ends the process that holds the memory, not
 * the session): it starts over, so the next process that holds the memory is acted on too, after the same times. */
void pw_memory_guard_rearm(struct pw_memory_guard *guard);
/* How long the census stood at the limit within the window, ms, as of the last sample. */
uint64_t pw_memory_guard_pressure_ms(const struct pw_memory_guard *guard);

#ifdef __cplusplus
}
#endif

#endif
