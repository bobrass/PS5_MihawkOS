/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../src/pw_memory_guard.h"
#include <assert.h>
#include <stdio.h>

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

    /* Firmware 12.00 and later has its own limit, whatever the first census was (the base PS5's read 731 to 1223 MiB at
     * the start of launches, a Wine start took ten helpers and the census to 1237-1537 MiB: all of it is under the marks,
     * and a climb past the end mark still ends the game). The Pro's firmware 10.01, and none known, keep theirs. */
    assert(pw_memory_guard_default_limit_mib(0x13400002u) == 1800 && pw_memory_guard_default_limit_mib(0x12000000u) == 1800);
    assert(pw_memory_guard_default_limit_mib(0x10010001u) == 990 && pw_memory_guard_default_limit_mib(0x11000000u) == 990);
    assert(pw_memory_guard_default_limit_mib(0) == 990);
    /* The soft notice sits under the last resort and above where an idle console reads (165-270 MiB). */
    assert(PW_MEMORY_GUARD_SWAP_NOTICE_MIB == 800 && PW_MEMORY_GUARD_SWAP_NOTICE_MIB < PW_MEMORY_GUARD_LAST_RESORT_MIB);
    assert(pw_memory_guard_swap_notice_mib(0x10010001u) == 800 && pw_memory_guard_swap_notice_mib(0) == 800);
    assert(pw_memory_guard_swap_notice_mib(0x13600000u) == 1700 && pw_memory_guard_swap_notice_mib(0x12000000u) == 1700);
    /* said before the guard acts on either generation */
    assert(pw_memory_guard_swap_notice_mib(0x13600000u) < pw_memory_guard_default_limit_mib(0x13600000u));
    assert(pw_memory_guard_swap_notice_mib(0x10010001u) < pw_memory_guard_default_limit_mib(0x10010001u));
    assert(pw_memory_guard_default_end_mib(990) == 1010 && pw_memory_guard_default_end_mib(1800) == 1870 && pw_memory_guard_default_end_mib(870) == 940);
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
        pw_memory_guard_init(&guard, pw_memory_guard_default_limit_mib(0x13400002u), 0);
        assert(guard.limit == MIB(1800) && guard.end == MIB(1870));
        now = 1000;
        assert(feed(&guard, &now, 20, idle, NULL) == PW_MEMORY_FINE);
        assert(feed(&guard, &now, 8, idle + 250, NULL) == PW_MEMORY_FINE);
        assert(feed(&guard, &now, 60, idle + 314 < 1537 ? idle + 314 : 1537, NULL) == PW_MEMORY_FINE);
        assert(feed(&guard, &now, 40, idle, NULL) == PW_MEMORY_FINE);
    }
    assert(feed(&guard, &now, 20, 2000, NULL) == PW_MEMORY_END);

    puts("pw_memory_guard: ok");
    return 0;
}
