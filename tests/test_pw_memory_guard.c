/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_memory_guard.h"
#include "../src/pw_memory_helper.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define MIB(n) ((uint64_t)(n) << 20)

/* Feed `count` censuses of `mib` half a second apart from *now; the first verdict that is not FINE, and when. */
static enum pw_memory_verdict feed(struct pw_memory_guard *guard, uint64_t *now, unsigned count, unsigned mib,
                                   uint64_t *when)
{
    enum pw_memory_verdict verdict = PW_MEMORY_FINE;
    for (unsigned i = 0; i < count; ++i) {
        *now += 500;
        const enum pw_memory_verdict got = pw_memory_guard_sample(guard, MIB(mib), *now);
        if (got != PW_MEMORY_FINE && verdict == PW_MEMORY_FINE) {
            verdict = got;
            if (when) *when = *now;
        }
    }
    return verdict;
}

int main(void)
{
    struct pw_memory_guard guard;
    uint64_t now, when = 0;

    /* No limit, no guard. */
    pw_memory_guard_init(&guard, 0, 0);
    now = 1000;
    assert(feed(&guard, &now, 200, 2000, NULL) == PW_MEMORY_FINE);

    /* A .NET (MonoGame) game's run (2026-10-06; record removed from the tree): about 500 MiB, 852-857 while it loads, then
     * 800-825 as it plays. Never asked to close. */
    pw_memory_guard_init(&guard, PW_MEMORY_GUARD_LIMIT_MIB, 0);
    now = 1000;
    assert(feed(&guard, &now, 60, 500, NULL) == PW_MEMORY_FINE);
    assert(feed(&guard, &now, 20, 857, NULL) == PW_MEMORY_FINE);
    for (int minute = 0; minute < 10; ++minute)
        assert(feed(&guard, &now, 120, 810 + (minute % 3) * 7, NULL) == PW_MEMORY_FINE);
    assert(pw_memory_guard_pressure_ms(&guard) == 0);

    /* A load that passes the limit for 15 s and comes back is left alone. */
    assert(feed(&guard, &now, 30, 900, NULL) == PW_MEMORY_FINE);
    assert(pw_memory_guard_pressure_ms(&guard) == 15000);
    assert(feed(&guard, &now, 60, 820, NULL) == PW_MEMORY_FINE);
    assert(pw_memory_guard_pressure_ms(&guard) == 0);

    /* A launcher's run (2026-10-07; record removed from the tree): 706, then between 865 and 948 for minutes. Asked to close 20 s into it,
     * the dips below the limit not counting; never ended, as it never stood past 940 for 3 s. */
    pw_memory_guard_init(&guard, PW_MEMORY_GUARD_LIMIT_MIB, 0);
    now = 1000;
    /* It reached its band slowly (8 MiB a second: 64 MiB in the steep window, under its 120). */
    for (unsigned mib = 706; mib < 865; mib += 4) assert(feed(&guard, &now, 1, mib, NULL) == PW_MEMORY_FINE);
    const uint64_t onset = now;
    static const unsigned launcher[] = {915, 878, 919, 872, 867, 865, 915, 871, 938, 948, 893, 880, 877, 945, 922, 923};
    enum pw_memory_verdict verdict = PW_MEMORY_FINE;
    for (unsigned round = 0; round < 4 && verdict == PW_MEMORY_FINE; ++round)
        for (unsigned i = 0; i < sizeof(launcher) / sizeof(*launcher) && verdict == PW_MEMORY_FINE; ++i)
            verdict = feed(&guard, &now, 2, launcher[i], &when);
    assert(verdict == PW_MEMORY_CLOSE);
    assert(when - onset >= 20000 && when - onset <= 26000);
    /* Said once. */
    assert(feed(&guard, &now, 120, 930, NULL) == PW_MEMORY_FINE);

    /* Past the end mark: a moment is not enough, 3 s is, and it is said once even after a close. */
    assert(feed(&guard, &now, 5, 945, NULL) == PW_MEMORY_FINE);   /* 2.5 s */
    assert(feed(&guard, &now, 1, 930, NULL) == PW_MEMORY_FINE);
    assert(feed(&guard, &now, 6, 945, NULL) == PW_MEMORY_FINE);   /* 2.5 s since it went past */
    assert(feed(&guard, &now, 1, 945, &when) == PW_MEMORY_END);   /* 3 s */
    assert(!guard.steep);   /* the end mark, and its log says so */
    assert(feed(&guard, &now, 60, 990, NULL) == PW_MEMORY_FINE);

    /* A steep climb is ended before it is asked to close, and then not asked. */
    pw_memory_guard_init(&guard, PW_MEMORY_GUARD_LIMIT_MIB, 0);
    now = 1000;
    assert(feed(&guard, &now, 10, 600, NULL) == PW_MEMORY_FINE);
    assert(feed(&guard, &now, 7, 960, NULL) == PW_MEMORY_END);
    assert(guard.steep);
    assert(feed(&guard, &now, 120, 960, NULL) == PW_MEMORY_FINE);

    /* A launcher's 2026-10-11 run (record removed from the tree): 627 MiB steady, then a climb of about 25 MiB a second; the kernel's limit is about 989
     * MiB. Ended on the way up, well before it, and not before the climb is steep and near the limit. */
    pw_memory_guard_init(&guard, PW_MEMORY_GUARD_LIMIT_MIB, 0);
    now = 1000;
    assert(feed(&guard, &now, 80, 627, NULL) == PW_MEMORY_FINE);
    {
        unsigned ended_at = 0;
        for (unsigned mib = 640; mib <= 1030 && !ended_at; mib += 12)
            if (feed(&guard, &now, 1, mib, NULL) == PW_MEMORY_END) ended_at = mib;
        assert(ended_at >= 860 && ended_at <= 930); /* the limit less 10 MiB, and far short of the 989 */
    }
    assert(feed(&guard, &now, 20, 1000, NULL) == PW_MEMORY_FINE);   /* said once */
    /* The same band reached slowly (a load that takes a minute) is left to the other rules. */
    pw_memory_guard_init(&guard, PW_MEMORY_GUARD_LIMIT_MIB, 0);
    now = 1000;
    for (unsigned mib = 500; mib <= 900; mib += 2) assert(feed(&guard, &now, 1, mib, NULL) == PW_MEMORY_FINE);
    /* A run that sets its own end mark goes past the limit as it chooses. */
    pw_memory_guard_init(&guard, 400, 2000);
    now = 1000;
    assert(feed(&guard, &now, 20, 300, NULL) == PW_MEMORY_FINE);
    assert(feed(&guard, &now, 4, 700, NULL) != PW_MEMORY_END);

    /* A census that stops coming in counts for one interval, not for the gap; an old or repeated one is ignored. */
    pw_memory_guard_init(&guard, PW_MEMORY_GUARD_LIMIT_MIB, 0);
    now = 1000;
    assert(pw_memory_guard_sample(&guard, MIB(900), now) == PW_MEMORY_FINE);
    now += 15000;
    assert(pw_memory_guard_sample(&guard, MIB(900), now) == PW_MEMORY_FINE);
    assert(pw_memory_guard_pressure_ms(&guard) == 1000);
    assert(pw_memory_guard_sample(&guard, MIB(900), now) == PW_MEMORY_FINE);
    assert(pw_memory_guard_sample(&guard, MIB(900), now - 500) == PW_MEMORY_FINE);
    assert(pw_memory_guard_pressure_ms(&guard) == 1000);

    /* A lower limit and a far end mark, as the probe run sets them: well past the limit, only asked to close. */
    pw_memory_guard_init(&guard, 400, 2000);
    now = 1000;
    assert(feed(&guard, &now, 40, 399, NULL) == PW_MEMORY_FINE);
    assert(feed(&guard, &now, 41, 650, &when) == PW_MEMORY_CLOSE);
    assert(feed(&guard, &now, 120, 1990, NULL) == PW_MEMORY_FINE);
    assert(feed(&guard, &now, 7, 2000, NULL) == PW_MEMORY_END);

    /* The desktop session ends one process and re-arms: nothing more is said until it is re-armed, then the next
     * process past the end mark for the same time is acted on too, with the same marks. */
    assert(feed(&guard, &now, 20, 2000, NULL) == PW_MEMORY_FINE);
    const uint64_t limit = guard.limit, end = guard.end;
    pw_memory_guard_rearm(&guard);
    assert(guard.limit == limit && guard.end == end && !guard.ended && !guard.closed && !guard.count);
    assert(feed(&guard, &now, 4, 2000, NULL) == PW_MEMORY_FINE);
    assert(feed(&guard, &now, 10, 2000, NULL) == PW_MEMORY_END);

    /* The marks of the memory plan (src/pw_memory_guard.h): the generation from the direct pool, the firmware INSTALLED.
     * A Pro (12,928 MiB) has its own on every firmware; a standard PS5 or Slim (11,712) from 12.00 on, and before it, or
     * when the firmware is not known; a pool not known takes the Pro's, the lowest. */
    const uint64_t pro_pool = (uint64_t)12928 << 20, standard_pool = (uint64_t)11712 << 20;
    assert(pw_console_kind_of(pro_pool) == PW_CONSOLE_PRO && pw_console_kind_of(standard_pool) == PW_CONSOLE_STANDARD);
    assert(pw_console_kind_of(0) == PW_CONSOLE_PRO);
    struct pw_memory_marks marks = pw_memory_marks_for(PW_CONSOLE_PRO, 0x10010001u);
    assert(marks.warn_mib == 850 && marks.stop_mib == 950);
    marks = pw_memory_marks_for(PW_CONSOLE_PRO, 0x13600000u);
    assert(marks.warn_mib == 850 && marks.stop_mib == 950);
    marks = pw_memory_marks_for(PW_CONSOLE_STANDARD, 0x13600000u);
    assert(marks.warn_mib == 1750 && marks.stop_mib == 0);
    marks = pw_memory_marks_for(PW_CONSOLE_STANDARD, 0x12000000u);
    assert(marks.warn_mib == 1750 && marks.stop_mib == 0);
    for (unsigned firmware = 0x05500000u; firmware <= 0x11600000u; firmware += 0x00100000u) {   /* 5.50 to 11.60, 6.xx-9.xx untested */
        marks = pw_memory_marks_for(PW_CONSOLE_STANDARD, firmware);
        assert(marks.warn_mib == 1650 && marks.stop_mib == 0);
    }
    marks = pw_memory_marks_for(PW_CONSOLE_STANDARD, 0);                 /* firmware not known */
    assert(marks.warn_mib == 1650 && marks.stop_mib == 0);
    /* Spoofed: an enabler reports 99.99 or 11.59, but the marks take the installed word the title passes, so a 13.60
     * console that reports 11.59 keeps the 12.00 marks, and one that reports 99.99 on 10.01 keeps the earlier ones. The
     * title never hands the reported word here (native/proton_wine.cpp, pw_firmware_installed). */
    assert(pw_memory_marks_for(PW_CONSOLE_STANDARD, 0x13600000u).warn_mib == 1750);
    assert(pw_memory_marks_for(PW_CONSOLE_STANDARD, 0x10010001u).warn_mib == 1650);
    /* WARN under STOP everywhere; the swap points measured sit between them or under WARN's 16 MiB. */
    assert(pw_memory_marks_for(PW_CONSOLE_PRO, 0).warn_mib < pw_memory_marks_for(PW_CONSOLE_PRO, 0).stop_mib);
    assert(pw_memory_guard_default_end_mib(950) == 970 && pw_memory_guard_default_end_mib(990) == 1010);
    assert(pw_memory_guard_default_end_mib(2200) == 2270 && pw_memory_guard_default_end_mib(2100) == 2170);

    /* The plateau trigger: our base's ceiling run, resident flat at 1,800-1,846 under a WARN of 1,750. Under WARN it
     * never holds; a flat line with no frame gap never stops; one gap over 1 s within 60 s at the plateau does; a fall out
     * of the band starts the hold again; a climb that keeps rising is not a plateau. */
    struct pw_plateau plateau = {0, 0, 0};
    uint64_t t = 1000;
    for (int i = 0; i < 400; ++i, t += 500) assert(!pw_plateau_sample(&plateau, 1700, t, 5000, 1750));
    for (int i = 0; i < 400; ++i, t += 500) assert(!pw_plateau_sample(&plateau, 1800 + (i % 3) * 15, t, 200, 1750));
    memset(&plateau, 0, sizeof(plateau));
    int fired = 0;
    for (int i = 0; i < 130 && !fired; ++i, t += 500) fired = pw_plateau_sample(&plateau, 1825 + (i % 2) * 20, t, i == 30 ? 1400 : 100, 1750);
    assert(fired && t - plateau.since_ms >= 60000 && t - plateau.since_ms <= 61000);
    memset(&plateau, 0, sizeof(plateau));
    for (int i = 0; i < 100; ++i, t += 500) assert(!pw_plateau_sample(&plateau, 1825, t, 1400, 1750));    /* 50 s */
    assert(!pw_plateau_sample(&plateau, 1780, t, 1400, 1750));                                            /* out of the band */
    for (int i = 0; i < 100; ++i, t += 500) assert(!pw_plateau_sample(&plateau, 1790, t, 1400, 1750));    /* 50 s again */
    memset(&plateau, 0, sizeof(plateau));
    for (int i = 0; i < 400; ++i, t += 500) assert(!pw_plateau_sample(&plateau, 1750 + i * 40, t, 1400, 1750)); /* climbing */
    /* The Pro's knee at ~869 under its 850 WARN, held with gaps: the trigger, where the 950 mark is never reached. */
    memset(&plateau, 0, sizeof(plateau));
    fired = 0;
    for (int i = 0; i < 130 && !fired; ++i, t += 500) fired = pw_plateau_sample(&plateau, 869, t, 2000, 850);
    assert(fired);

    /* Today's failure, kept as a check: a census with 48 GiB mapped and 1,163 MiB resident (memplan-gate-2026-10-28-01)
     * never stops a game, with no resident mark or one above its resident, through ten minutes of samples. */
    struct pw_memory_census huge = {0};
    huge.system = MIB(1163);
    huge.helpers = MIB(600);
    huge.helpers_mapped = MIB(48236);
    assert(pw_memory_census_guarded(&huge) == MIB(1163));
    for (unsigned mark = 0; mark <= 2200; mark += 2200) {
        pw_memory_guard_init(&guard, mark, 0);
        now = 1000;
        for (int i = 0; i < 1200; ++i) {
            now += 500;
            assert(pw_memory_guard_sample(&guard, pw_memory_census_guarded(&huge), now) == PW_MEMORY_FINE);
        }
    }

    /* The slowdown: the ceiling test's 51 ms, then 187-249 ms; a fast first start and a 3x of it under 150 ms is noise. */
    assert(!pw_memory_start_slowed(51, 51) && !pw_memory_start_slowed(51, 150) && pw_memory_start_slowed(51, 187));
    assert(pw_memory_start_slowed(51, 249) && !pw_memory_start_slowed(0, 900) && !pw_memory_start_slowed(30, 120));

    /* The last resort: the signed-in client window of 2026-10-24 (about 725 MiB of kernel RSS, census 743-862) is left alone, a
     * climb to the end mark is ended, and the steep rule stays armed under it. */
    pw_memory_guard_init(&guard, 990, pw_memory_guard_default_end_mib(990));
    assert(guard.limit == MIB(990) && guard.end == MIB(1010));
    now = 1000;
    assert(feed(&guard, &now, 20, 250, NULL) == PW_MEMORY_FINE);
    assert(feed(&guard, &now, 90, 880, NULL) == PW_MEMORY_FINE);        /* a window that swaps but is under the last resort */
    assert(pw_memory_guard_pressure_ms(&guard) == 0);
    assert(feed(&guard, &now, 4, 1015, NULL) == PW_MEMORY_END);        /* a jump of 135 MiB to the end mark: the steep rule */
    for (unsigned idle = 731; idle <= 1223; idle += 164) {
        pw_memory_guard_init(&guard, 1800, 0);
        assert(guard.limit == MIB(1800) && guard.end == MIB(1870));
        now = 1000;
        assert(feed(&guard, &now, 20, idle, NULL) == PW_MEMORY_FINE);
        assert(feed(&guard, &now, 8, idle + 250, NULL) == PW_MEMORY_FINE);
        assert(feed(&guard, &now, 60, idle + 314 < 1537 ? idle + 314 : 1537, NULL) == PW_MEMORY_FINE);
        assert(feed(&guard, &now, 40, idle, NULL) == PW_MEMORY_FINE);
    }
    assert(feed(&guard, &now, 20, 2000, NULL) == PW_MEMORY_END);

    /* The page tables, read every 100 ms (time in ms). A slow climb as on owner-test-base-2026-10-28 (Steam's browser,
     * 7216, 7533, 8163 used of 8192 over minutes): closed at 768 free, ended at 512, each said once. */
    struct pw_page_table_guard tables;
    memset(&tables, 0, sizeof(tables));
    uint64_t pt = 1000;
    assert(pw_page_table_sample(&tables, 0, 0, pt) == PW_MEMORY_FINE);                        /* not known */
    assert(pw_page_table_sample(&tables, 8192, 8192 - 4570, pt += 10000) == PW_MEMORY_FINE);
    assert(pw_page_table_sample(&tables, 8192, 8192 - 7216, pt += 10000) == PW_MEMORY_FINE);
    assert(pw_page_table_sample(&tables, 8192, 8192 - 7533, pt += 10000) == PW_MEMORY_CLOSE);
    assert(pw_page_table_sample(&tables, 8192, 8192 - 7600, pt += 10000) == PW_MEMORY_FINE);  /* said once */
    assert(pw_page_table_sample(&tables, 8192, 8192 - 7690, pt += 10000) == PW_MEMORY_END);   /* 502 free */
    assert(pw_page_table_sample(&tables, 8192, 8192 - 8100, pt += 10000) == PW_MEMORY_FINE);
    assert(pw_page_table_sample(&tables, 8192, 8192 - 5908, pt += 10000) == PW_MEMORY_FINE);  /* back: armed again */
    assert(pw_page_table_sample(&tables, 9216, 100, pt += 10000) == PW_MEMORY_END);           /* the Pro's pool */
    assert(pw_page_table_sample(&tables, 9216, 100, pt += 10000) == PW_MEMORY_FINE);

    /* The owner's RE3 start with Steam (re3-steam-base-2026-10-28, the 250 ms sampler): 7016, 7031, 7028, 7121, 7254,
     * 7385, 7509, 7619 used of 8192, then out within 4 s. The steep rule ends it at 7385 (807 free, 369 within a second),
     * where the old marks closed at 736 free and ended after the pool was out. */
    memset(&tables, 0, sizeof(tables));
    const uint32_t re3[] = {7016, 7031, 7028, 7121, 7254, 7385, 7509, 7619};
    enum pw_memory_verdict first = PW_MEMORY_FINE;
    unsigned at = 0;
    for (unsigned k = 0; k < sizeof(re3) / sizeof(re3[0]) && first == PW_MEMORY_FINE; ++k) {
        pt += 250;
        first = pw_page_table_sample(&tables, 8192, 8192 - re3[k], pt);
        at = re3[k];
    }
    assert(first == PW_MEMORY_END && tables.steep && at == 7385);
    /* Steam idle at 6,800 used (1,392 free, under the steep band) with small moves never acts. */
    memset(&tables, 0, sizeof(tables));
    for (int k = 0; k < 600; ++k) assert(pw_page_table_sample(&tables, 8192, 1392 - (k % 5) * 10, pt += 100) == PW_MEMORY_FINE);
    /* gpu2m-pro-gate-2026-10-29-01 on a fresh Pro: the grouped session's 4 GiB probe took the pool from 3,345 free at its
     * start to 1,526, the last 300 within 2 s. That is 1,226 short of 512 at its pace: left to the marks. */
    memset(&tables, 0, sizeof(tables));
    assert(pw_page_table_sample(&tables, 9216, 3345, pt += 100) == PW_MEMORY_FINE);
    pt += 60000;
    for (int k = 0; k <= 20; ++k) assert(pw_page_table_sample(&tables, 9216, 1826 - k * 15, pt += 100) == PW_MEMORY_FINE);
    assert(!tables.steep && !tables.closed);
    /* Its pace kept up to the end mark's reach is ended there: at 806 free after the same 300 (812 or fewer), before the close at 768. */
    for (int k = 1; k <= 47; ++k) assert(pw_page_table_sample(&tables, 9216, 1526 - k * 15, pt += 100) == PW_MEMORY_FINE);
    assert(pw_page_table_sample(&tables, 9216, 1526 - 48 * 15, pt += 100) == PW_MEMORY_END && tables.steep);
    /* A fall of 300 spread over more than 2 s is not steep: 100 every second, from 1,500 to 800 free, is fine. */
    memset(&tables, 0, sizeof(tables));
    for (int k = 0; k <= 7; ++k) assert(pw_page_table_sample(&tables, 8192, 1500 - k * 100, pt += 1000) == PW_MEMORY_FINE);
    /* The Pro's garbage (9436 free of 9216; VM Stats' used count wrapped to 4294967019): unknown, never acted on. */
    assert(!pw_page_table_known(9216, 9436) && !pw_page_table_known(0, 0) && !pw_page_table_known(9216, 4294967019u));
    assert(pw_page_table_known(9216, 9216) && pw_page_table_known(8192, 0));
    memset(&tables, 0, sizeof(tables));
    assert(pw_page_table_sample(&tables, 9216, 9436, pt += 100) == PW_MEMORY_FINE);
    assert(pw_page_table_sample(&tables, 9216, 4294967019u, pt += 100) == PW_MEMORY_FINE);
    assert(pw_page_table_sample(&tables, 9216, 100, pt += 100) == PW_MEMORY_END);

    puts("pw_memory_guard: ok");
    return 0;
}
