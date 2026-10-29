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
#define PW_MEMORY_GUARD_LAST_RESORT_END_MARGIN_MIB 20u
/* The old firmware-only marks (the notice at 800/1700, the limit at 990/1800) and the measurements behind them are in git
 * history before the memory plan; pw_memory_marks_for replaces them. */
#define PW_MEMORY_GUARD_FIRMWARE_MIN 0x12u   /* the version word's high byte, as BCD: 12.00 and later */
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

/* The system's page tables, a pool apart from its memory: every helper process takes about 140 of them, more as it maps
 * more, and when the pool is out the kernel says "system page table exhausted" and a new process fails to start (EXECFAIL)
 * or a mapping with EIO (errno 5). The base PS5 (13.x, owner-test-base-2026-10-28) held 4570 of 8192 used at rest; Steam's
 * browser in many processes took them to 8192 at a census of only 1732 MiB, under the memory limit. So the guard asks the
 * game to close once 768 or fewer are free, and ends it at 512 or fewer, or sooner on a fast fall (below). */
#define PW_PAGE_TABLE_CLOSE_FREE 768u
/* The owner's RE3 start with Steam on the base (re3-steam-base-2026-10-28): the pool fell from 7,016 to 8,188 used in
 * about 6 s, up to 250 a quarter second; the close at 736 free came 4 s before it was out, and the end at 256, read half
 * a second apart, came after one "system page table exhausted". So the end is at 512, and while 1,536 or fewer are free
 * the game is ended once the last 2 s of fall, repeated, would take the pool to 512 (807 free after a fall of 369 there).
 * A bounded fall is left to the marks: gpu2m-pro-gate-2026-10-29-01's 4 GiB probe fell 300 within 2 s to 1,526 free on a
 * fresh Pro and was ended by the first rule (a fixed fall of 300), well before either mark. The title reads every 100 ms. */
#define PW_PAGE_TABLE_END_FREE 512u
#define PW_PAGE_TABLE_STEEP_FREE 1536u
#define PW_PAGE_TABLE_STEEP_MS 2000u
struct pw_page_table_guard {
    int closed, ended, steep;      /* each verdict said once until more than PW_PAGE_TABLE_STEEP_FREE are free again */
    uint64_t times[32];            /* the recent readings within PW_PAGE_TABLE_STEEP_MS, oldest first */
    uint32_t frees[32];
    unsigned count;
};
/* A census's page tables. Not known, so nothing to do: a total of 0, or more free than the total (the Pro at 10.01 read
 * 9436 free of 9216 while SceShellCore's VM Stats printed "page table CPU 4294967019/9216", a used count that went
 * negative, guided-setup's ceiling run of 2026-10-28). */
static inline int pw_page_table_known(uint32_t total, uint32_t free_tables) { return total && free_tables <= total; }
enum pw_memory_verdict pw_page_table_sample(struct pw_page_table_guard *guard, uint32_t total, uint32_t free_tables,
                                            uint64_t now_ms);

/* The marks of a console (the memory plan of 2026-10-28, docs/PLAN.md "Memory plan"), keyed on the console's generation
 * and its INSTALLED firmware (pw_firmware_installed; an enabler spoofs the reported word, volunteers read 99.99 and 11.59):
 * WARN, a notice on the resident census where the console starts to swap, and STOP, the clean close back to the launcher,
 * on the same census. On a standard PS5 resident memory flattens at the swap point (1,800-1,846 MiB while the ceiling test
 * filled 4 GiB), so its STOP comes from the slowdown (pw_memory_start_slowed) and the page-table net; a helper's mapped
 * figure is its address space, not memory asked of the kernel (memplan-gate-2026-10-28-01 read 48 GiB), and no figure
 * on the console counts what has been swapped.
 * The swap points measured (docs/TESTER_MEMORY_RESULTS.md, the ceiling test): a standard PS5 or Slim from 12.00 at
 * 1,790-1,886 MiB, before 12.00 at 1,698-1,818 (5.50's ~1,300 was a test's false stop), a Pro at 869 (10.01), 882 (9.60)
 * and 926-969 (13.60); our Pro froze at 1,026. The stops for a standard PS5 are first marks, to be set by the owner's
 * run that plays past WARN. Firmware 6.xx-9.xx is untested: it takes the marks
 * of before 12.00, as does a firmware not known. */
#define PW_CONSOLE_PRO_DIRECT_MIB 12288u   /* the application's direct pool: 12,928 MiB on a Pro, 11,712 on the others */
enum pw_console_kind { PW_CONSOLE_STANDARD, PW_CONSOLE_PRO };
struct pw_memory_marks { unsigned warn_mib, stop_mib; };   /* stop_mib 0: no resident mark, the triggers only */
/* The generation from sceKernelGetDirectMemorySize's bytes: a Pro (CFI-7xxx) has the larger pool. A size not known (0)
 * takes the Pro's marks, the lowest. */
enum pw_console_kind pw_console_kind_of(uint64_t direct_bytes);
struct pw_memory_marks pw_memory_marks_for(enum pw_console_kind kind, unsigned firmware);
/* STOP's third trigger, on every console: resident memory held within PW_PLATEAU_BAND_MIB of the session's plateau (the
 * highest it reached at WARN or past it) for PW_PLATEAU_HOLD_MS, while a frame gap of more than PW_PLATEAU_GAP_MS was
 * seen in that time. Resident memory flattens where the console swaps: a standard PS5 at 1,800-1,846 MiB, our Pro at
 * about 869 MiB in two runs, so a number above the knee may never be reached. A fall out of the band, or below WARN,
 * starts the hold again. */
#define PW_PLATEAU_BAND_MIB 32u
#define PW_PLATEAU_HOLD_MS 60000u
#define PW_PLATEAU_GAP_MS 1000u
struct pw_plateau { uint64_t peak_mib, since_ms; int gap_seen; };
/* 1 once the hold is met (and again each sample after); frame_gap_ms 0 when no frame is known. */
int pw_plateau_sample(struct pw_plateau *plateau, uint64_t resident_mib, uint64_t now_ms, uint64_t frame_gap_ms,
                      unsigned warn_mib);

/* STOP's second trigger: a helper start more than 3 times slower than the session's first, and 150 ms or more. The
 * ceiling test's starts took about 51 ms until 2.3 GiB had been swapped out, then 187-249 ms (our base, 2026-10-28). The
 * title asks it only at WARN or past it. */
int pw_memory_start_slowed(uint64_t first_ms, uint64_t last_ms);

/* limit_mib 0 makes a guard that never acts; end_mib 0 puts the end mark PW_MEMORY_GUARD_END_MARGIN_MIB past the
 * limit. */
void pw_memory_guard_init(struct pw_memory_guard *guard, unsigned limit_mib, unsigned end_mib);
/* A census of system bytes taken at now_ms (the census's own time; one not newer than the last is ignored). */
enum pw_memory_verdict pw_memory_guard_sample(struct pw_memory_guard *guard, uint64_t system, uint64_t now_ms);
/* The end mark that goes with a limit: 20 MiB past one under 1,100 MiB (a Pro's, close to its freeze), 70 past the others. */
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
