/* SPDX-License-Identifier: GPL-3.0-or-later
 * Windows 4 KiB code/data protections sharing a 16 KiB host page, and code
 * beside data in different host pages. Exercise actual writes and execution. */
#include <windows.h>
#include <winternl.h>

static volatile unsigned image_data;
/* The verdict line's marker: the default run's, or a case's own (--truth, --speed, --rearm). */
static const char *marker = "PS5_PROTON_VM_PROTECT_PROBE";
static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}
static void fail(const char *why)
{
    say(marker); say(" FAIL: "); say(why); say("\n"); ExitProcess(1);
}

/* A write tracker (an emulator's GPU cache and page manager): 4 KiB pages made read-only or no-access, each first
 * touch reported as an access violation whose handler opens that one page. The console's host page is 16 KiB, and Wine
 * mapped it with the most any of its four pages allowed: once one page was opened, the others' writes went unreported
 * and the GPU drew stale vertices (a Unity game's text under an emulator, 2026-10-14; record removed from the tree). */
static volatile LONG tracker_faults, tracker_seen;  /* reports, and which of the 16 pages were reported */
static const char *decimal(char *out, unsigned value)
{
    char *p = out + 15;
    *p = 0;
    do *--p = (char)('0' + value % 10); while (value /= 10);
    return p;
}
static unsigned char *tracker_base;
static void tracker_fail(const char *what, const char *why)
{
    say(marker); say(" FAIL: "); say(what); say(": "); say(why); say("\n"); ExitProcess(1);
}
static LONG CALLBACK tracker_handler(EXCEPTION_POINTERS *e)
{
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    unsigned char *at;
    DWORD old;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    at = (unsigned char *)r->ExceptionInformation[1];
    if (!tracker_base || at < tracker_base || at >= tracker_base + 65536) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&tracker_faults);
    InterlockedOr(&tracker_seen, 1L << ((at - tracker_base) / 4096));
    if (!VirtualProtect((void *)((ULONG_PTR)at & ~(ULONG_PTR)4095), 4096,
                        r->ExceptionInformation[0] == 1 ? PAGE_READWRITE : PAGE_READONLY, &old))
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
static unsigned elapsed_us(LARGE_INTEGER t0)
{
    LARGE_INTEGER t1, hz;
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&hz);
    return (unsigned)((t1.QuadPart - t0.QuadPart) * 1000000 / hz.QuadPart);
}
/* 64 KiB at base (64 KiB aligned): every 4 KiB page reported before its first write lands and only once, each 16 KiB's
 * first read reported; with no handler taking the reports, stores into a writable page beside read-only ones land and
 * leave those read-only; a file read lands in such a page. Gives the microseconds of 16 KiB of stores with a tracker
 * and of 4 KiB without one. */
static void tracker_check(unsigned char *base, const char *what, unsigned *tracked_us, unsigned *untracked_us)
{
    LARGE_INTEGER t0;
    char path[MAX_PATH], dir[MAX_PATH];
    unsigned char pattern[4096];
    DWORD old, done;
    HANDLE file;
    LONG before;
    unsigned page, i;

    tracker_base = base;
    if (!VirtualProtect(base, 65536, PAGE_READONLY, &old)) tracker_fail(what, "read-only protection");
    before = tracker_faults; tracker_seen = 0;
    for (page = 0; page < 16; page++) {
        volatile unsigned char *p = base + page * 4096;
        p[0] = (unsigned char)page;
        if (!(tracker_seen & (1L << page)))
            tracker_fail(what, "a write to a read-only 4 KiB page beside a writable one was not reported");
        p[2048] = 0x5a; p[4095] = (unsigned char)~page;
    }
    if (tracker_faults != before + 16) tracker_fail(what, "a page was reported more than once");
    if (!VirtualProtect(base, 65536, PAGE_NOACCESS, &old)) tracker_fail(what, "no-access protection");
    before = tracker_faults;
    for (page = 0; page < 16; page += 4) {
        volatile unsigned char *p = base + page * 4096;
        if (p[0] != (unsigned char)page || p[4095] != (unsigned char)~page) tracker_fail(what, "contents after reopening");
        if (tracker_faults != before + (LONG)page / 4 + 1) tracker_fail(what, "a read of a no-access page was not reported");
    }
    /* A tracker's cost: 16 KiB of 8-byte stores into read-only pages. */
    if (!VirtualProtect(base, 65536, PAGE_READONLY, &old)) tracker_fail(what, "read-only again");
    before = tracker_faults;
    QueryPerformanceCounter(&t0);
    for (i = 0; i < 2048; i++) ((volatile ULONGLONG *)base)[i] = i;
    *tracked_us = elapsed_us(t0);
    if (tracker_faults != before + 4) tracker_fail(what, "16 KiB of stores were not reported once a page");
    /* No handler takes the reports: one page writable among read-only ones, its stores land, the others stay closed. */
    if (!VirtualProtect(base, 65536, PAGE_READONLY, &old) || !VirtualProtect(base, 4096, PAGE_READWRITE, &old))
        tracker_fail(what, "mixed protection");
    tracker_base = NULL;
    QueryPerformanceCounter(&t0);
    for (i = 0; i < 512; i++) ((volatile ULONGLONG *)base)[i] = ((ULONGLONG)7 << 32) | i;
    *untracked_us = elapsed_us(t0);
    tracker_base = base;
    for (i = 0; i < 512; i++)
        if (((volatile ULONGLONG *)base)[i] != (((ULONGLONG)7 << 32) | i)) tracker_fail(what, "stores into the writable page");
    before = tracker_faults;
    if (((volatile ULONGLONG *)base)[512] != 512 || ((volatile ULONGLONG *)base)[1023] != 1023 || tracker_faults != before)
        tracker_fail(what, "reads of the read-only neighbours");
    /* A file read lands in the writable page and nowhere else. */
    for (i = 0; i < sizeof(pattern); i++) pattern[i] = (unsigned char)(i * 7 + 3);
    if (!GetTempPathA(sizeof(dir), dir) || !GetTempFileNameA(dir, "vmp", 0, path)) tracker_fail(what, "temporary file");
    file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
    if (file == INVALID_HANDLE_VALUE || !WriteFile(file, pattern, sizeof(pattern), &done, NULL) || done != sizeof(pattern) ||
        SetFilePointer(file, 0, NULL, FILE_BEGIN) != 0) tracker_fail(what, "temporary file contents");
    tracker_base = NULL;
    if (!ReadFile(file, base, 4096, &done, NULL) || done != 4096) tracker_fail(what, "a file read into a writable page beside read-only ones");
    tracker_base = base;
    CloseHandle(file);
    for (i = 0; i < sizeof(pattern); i++) if (base[i] != pattern[i]) tracker_fail(what, "file read contents");
    if (((volatile ULONGLONG *)base)[512] != 512 || tracker_faults != before)
        tracker_fail(what, "the file read touched a neighbour");
    tracker_seen = 0;
    ((volatile unsigned char *)base)[4096 * 2] = 9;
    if (!(tracker_seen & 4)) tracker_fail(what, "a neighbour stayed read-only after the file read");
    /* An emulator's two watches in one host page: page 1 no-access (GPU-written memory watched for any
     * access), pages 2-3 read-only (memory watched for writes), page 0 open. Stores into page 0 report nothing: a write
     * report of the no-access page made the emulator upload stale bytes over what the GPU drew (a Unity game's menu glyphs as
     * solid blocks; reports off, 2026-10-15, drew them right; record removed from the tree). In the second host page (4-7). */
    if (!VirtualProtect(base + 16384, 16384, PAGE_READONLY, &old) ||
        !VirtualProtect(base + 16384, 4096, PAGE_READWRITE, &old) ||
        !VirtualProtect(base + 16384 + 4096, 4096, PAGE_NOACCESS, &old))
        tracker_fail(what, "mixed watches");
    tracker_seen = 0;
    for (i = 0; i < 512; i++) ((volatile ULONGLONG *)(base + 16384))[i] = ((ULONGLONG)9 << 32) | i;
    if (tracker_seen) tracker_fail(what, "a store into a writable page beside a no-access one reported a page");
    for (i = 0; i < 512; i++)
        if (((volatile ULONGLONG *)(base + 16384))[i] != (((ULONGLONG)9 << 32) | i))
            tracker_fail(what, "stores beside a no-access page");
    tracker_base = NULL;
    if (!VirtualProtect(base, 65536, PAGE_READWRITE, &old)) tracker_fail(what, "reset");
}

/* Stepped writes (Wine patches 1122, 1134): a write into a writable 4 KiB page whose 16 KiB host page holds read-only
 * pages too runs through the 4 KiB MMU on the console in a helper, or as one instruction under the trap flag (a title,
 * WoW64, WINE_PS5_MMU4K=0); with 4 KiB pages (the PC) it never faults, so the PC is the oracle. A case that does not
 * end must not leave the runner to stop the probe (its stop escalates to the server's terminate, the sequence that
 * powered the console off): the watchdog ends it. */
#ifdef _WIN64
static volatile LONG case_steps, case_tf_armed, case_open_on_fault;
static unsigned char *case_low, *case_high, *case_fault;
#endif
static void start(const char *what)
{
    say("PS5_PROTON_VM_PROTECT_PROBE case: "); say(what); say("\n");
}
static int asked(const char *argument)
{
    const char *line = GetCommandLineA();
    for (; *line; line++) {
        unsigned i = 0;
        while (argument[i] && line[i] == argument[i]) i++;
        if (!argument[i]) return 1;
    }
    return 0;
}
static DWORD WINAPI watchdog(void *unused)
{
    (void)unused;
    Sleep(60000);
    say(marker); say(" FAIL: watchdog: the probe did not end within 60 s\n");
    ExitProcess(2);
    return 0;
}
#ifdef _WIN64
static LONG CALLBACK case_handler(EXCEPTION_POINTERS *e)
{
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    unsigned char *at;
    DWORD old;
    if (r->ExceptionCode == EXCEPTION_SINGLE_STEP && case_tf_armed) {
        InterlockedIncrement(&case_steps);
        e->ContextRecord->EFlags &= ~0x100;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters != 2) return EXCEPTION_CONTINUE_SEARCH;
    at = (unsigned char *)r->ExceptionInformation[1];
    if (at < case_low || at >= case_high) return EXCEPTION_CONTINUE_SEARCH;
    if (!case_open_on_fault) fail("an access violation on a page its protection lets the program write");
    if (!case_fault) case_fault = at;
    if (!VirtualProtect((void *)((ULONG_PTR)at & ~(ULONG_PTR)4095), 4096, PAGE_READWRITE, &old))
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
/* 16 KiB at base: read-only but for the 4 KiB pages in writable (bit i: page i). */
static void mixed(unsigned char *base, unsigned writable)
{
    DWORD old;
    unsigned page;
    if (!VirtualProtect(base, 16384, PAGE_READONLY, &old)) fail("mixed protection");
    for (page = 0; page < 4; page++)
        if (writable >> page & 1 && !VirtualProtect(base + page * 4096, 4096, PAGE_READWRITE, &old))
            fail("mixed protection");
}
static void rep_movsb(void *to, const void *from, SIZE_T bytes)
{
    __asm__ volatile("rep movsb" : "+D"(to), "+S"(from), "+c"(bytes) : : "memory");
}
/* V5 (docs/MMU4K.md section 12): two threads store 100,000 times each into their own writable page of one host page
 * while a third makes a read-only page of it read-only again every 100 us; an access violation fails the probe. */
enum { CONTEND_STORES = 100000 };
static unsigned char *contended;
static DWORD WINAPI contend_store(void *arg)
{
    volatile ULONGLONG *at = (volatile ULONGLONG *)(contended + (ULONG_PTR)arg * 4096);
    unsigned i;
    for (i = 0; i < CONTEND_STORES; i++) at[i % 512] = ((ULONGLONG)(ULONG_PTR)arg << 32) | i;
    return 0;
}
static volatile LONG contend_done;
static DWORD WINAPI contend_protect(void *unused)
{
    LARGE_INTEGER t0, t1, hz;
    DWORD old;
    (void)unused;
    QueryPerformanceFrequency(&hz);
    while (!contend_done) {
        if (!VirtualProtect(contended + 3 * 4096, 4096, PAGE_READONLY, &old)) fail("contention: protection");
        QueryPerformanceCounter(&t0);
        do QueryPerformanceCounter(&t1);
        while (!contend_done && (t1.QuadPart - t0.QuadPart) * 10000 < hz.QuadPart);
    }
    return 0;
}
/* Every stepping case, on 64 KiB at base; contention also runs two stepping threads in one host page while a third
 * makes a read-only page of it read-only again and again (a step's page closed under it before it ran). */
static void stepped_cases(unsigned char *base, int contention, unsigned *rep_us)
{
    static unsigned char from[8192];
    LARGE_INTEGER t0;
    MEMORY_BASIC_INFORMATION info;
    DWORD old;
    unsigned i;
    for (i = 0; i < sizeof(from); i++) from[i] = (unsigned char)(i * 13 + 5);
    case_low = base; case_high = base + 65536;
    if (!AddVectoredExceptionHandler(1, case_handler)) fail("case handler");

    start("rep movsb of 4096 bytes beside read-only pages");
    mixed(base, 1);
    QueryPerformanceCounter(&t0);
    rep_movsb(base, from, 4096);
    *rep_us = elapsed_us(t0);
    for (i = 0; i < 4096; i++) if (base[i] != from[i]) fail("rep: contents");
    if (!VirtualQuery(base + 4096, &info, sizeof(info)) || info.Protect != PAGE_READONLY || info.RegionSize != 12288)
        fail("rep: the read-only pages beside");
    /* Fixed before any console run: 4096 steps at 86 us are 352 ms (vmp-tracker-2026-10-14-03); 1 s is 2.8 times. */
    if (*rep_us > 1000000) fail("rep: 4096 stepped bytes took more than 1 s");

    start("rep movsb from a writable page into a read-only one");
    mixed(base, 1);
    case_open_on_fault = 1; case_fault = NULL;
    rep_movsb(base + 2048, from, 4096);
    case_open_on_fault = 0;
    if (case_fault != base + 4096) fail("rep: the access violation was not at the read-only page's first byte");
    for (i = 0; i < 4096; i++) if (base[2048 + i] != from[i]) fail("rep across pages: contents");

    start("the program's own trap flag over a stepped write");
    mixed(base, 1);
    case_steps = 0; case_tf_armed = 1;
    __asm__ volatile("pushfq\n\torq $0x100, (%%rsp)\n\tpopfq\n\tmovb $7, (%0)\n\tnop"
                     : : "r"(base + 100) : "memory", "cc");
    case_tf_armed = 0;
    if (case_steps != 1 || base[100] != 7) fail("trap flag: the program's single step was not given exactly once");

    start("a write across two host pages beside read-only pages");
    mixed(base, 8);
    mixed(base + 16384, 1);
    __asm__ volatile("movq %1, (%0)" : : "r"(base + 16384 - 4), "r"(0x1122334455667788ull) : "memory");
    if (*(volatile ULONGLONG *)(base + 16384 - 4) != 0x1122334455667788ull) fail("across host pages: contents");

    if (contention) {
        HANDLE threads[3];
        start("contention: two threads storing 100000 times in one host page while a third protects it every 100 us");
        contended = base + 32768;
        mixed(contended, 3);
        contend_done = 0;
        threads[2] = CreateThread(NULL, 0, contend_protect, NULL, 0, NULL);
        threads[0] = CreateThread(NULL, 0, contend_store, (void *)0, 0, NULL);
        threads[1] = CreateThread(NULL, 0, contend_store, (void *)1, 0, NULL);
        if (!threads[0] || !threads[1] || !threads[2]) fail("contention: threads");
        WaitForMultipleObjects(2, threads, TRUE, INFINITE);
        contend_done = 1;
        WaitForSingleObject(threads[2], INFINITE);
        for (i = 0; i < 2; i++)
            if (((volatile ULONGLONG *)(contended + i * 4096))[(CONTEND_STORES - 1) % 512] !=
                (((ULONGLONG)i << 32) | (CONTEND_STORES - 1)))
                fail("contention: a store did not land");
    }
    case_low = case_high = NULL;
    if (!VirtualProtect(base, 65536, PAGE_READWRITE, &old)) fail("stepped cases: reset");
}
#endif

/* The 4 KiB MMU (Wine patch 1134, docs/MMU4K.md section 12): on the console a store into a writable 4 KiB page whose
 * 16 KiB host page holds read-only pages runs through the MMU in a helper; a title, a WoW64 thread and
 * WINE_PS5_MMU4K=0 keep 1122's step. The PC's 4 KiB pages need neither, so the PC under Wine is the oracle: every case
 * holds there. Each mode prints one verdict line, PS5_PROTON_VM_PROTECT_<TRUTH|SPEED|REARM> PASS or FAIL. */
#ifdef _WIN64
# define CONTEXT_IP(c) ((c)->Rip)
# define CONTEXT_DI(c) ((c)->Rdi)
# define CONTEXT_AX(c) ((c)->Rax)
#else
# define CONTEXT_IP(c) ((c)->Eip)
# define CONTEXT_DI(c) ((c)->Edi)
# define CONTEXT_AX(c) ((c)->Eax)
#endif
/* V2 truth: the store being made (its RIP, address register, value register), where it must fault (0: nowhere), and
 * what the 64 KiB under test held before it. The handler checks that a store faults at the store, before any of its
 * bytes land, and opens that one page, as a write tracker does. */
static volatile unsigned char *truth_base;
static unsigned char truth_old[65536];
static volatile ULONG_PTR truth_rip, truth_dest, truth_value, truth_fault;
static volatile LONG truth_faults[16];
static const char *volatile truth_wrong;
static LONG CALLBACK truth_handler(EXCEPTION_POINTERS *e)
{
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    const CONTEXT *c = e->ContextRecord;
    const char *wrong = NULL;
    ULONG_PTR at;
    DWORD old;
    unsigned i;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 || !truth_base)
        return EXCEPTION_CONTINUE_SEARCH;
    at = r->ExceptionInformation[1];
    if (at < (ULONG_PTR)truth_base || at - (ULONG_PTR)truth_base >= 65536) return EXCEPTION_CONTINUE_SEARCH;
    if (r->ExceptionInformation[0] != 1) wrong = "an access violation that is not a write";
    else if (!truth_fault) wrong = "a store into a page its protection lets the program write faulted";
    else if (at != truth_fault) wrong = "the access violation is not at the store's first protected byte";
    else if (CONTEXT_IP(c) != truth_rip) wrong = "the access violation's instruction pointer is not the store";
    else if (CONTEXT_DI(c) != truth_dest || CONTEXT_AX(c) != truth_value) wrong = "the registers are not the store's";
    else
        for (i = 0; i < 65536; i++)
            if (truth_base[i] != truth_old[i]) {
                wrong = "the store landed before its access violation";
                break;
            }
    if (wrong && !truth_wrong) truth_wrong = wrong;
    InterlockedIncrement(&truth_faults[(at - (ULONG_PTR)truth_base) / 4096]);
    truth_fault = 0;   /* the store runs again and lands */
    return VirtualProtect((void *)(at & ~(ULONG_PTR)4095), 4096, PAGE_READWRITE, &old) ? EXCEPTION_CONTINUE_EXECUTION
                                                                                      : EXCEPTION_CONTINUE_SEARCH;
}
/* One store of size bytes (1, 2, 4, 8 in 64-bit code, 16, 32) at at, from bytes; fault: where it must fault, or 0. */
static void truth_store(volatile unsigned char *at, unsigned size, const unsigned char *bytes, ULONG_PTR fault)
{
    ULONG_PTR value = 0;
    unsigned i;
    for (i = 0; i < size && i < sizeof(value); i++) value |= (ULONG_PTR)bytes[i] << (8 * i);
    truth_dest = (ULONG_PTR)at;
    truth_value = value;
    truth_fault = fault;
#ifdef _WIN64
# define TRUTH_STORE(before, store) __asm__ volatile(before "leaq 1f(%%rip), %%rcx\n\tmovq %%rcx, %0\n1:\t" store \
                                                     : "=m"(truth_rip) : "D"(at), "a"(value), "S"(bytes) \
                                                     : "rcx", "xmm0", "memory")
#else
# define TRUTH_STORE(before, store) __asm__ volatile(before "movl $1f, %%ecx\n\tmovl %%ecx, %0\n1:\t" store \
                                                     : "=m"(truth_rip) : "D"(at), "a"(value), "S"(bytes) \
                                                     : "ecx", "xmm0", "memory")
#endif
    switch (size) {
    case 1: TRUTH_STORE("", "movb %b2, (%1)"); break;
    case 2: TRUTH_STORE("", "movw %w2, (%1)"); break;
    case 4: TRUTH_STORE("", "movl %k2, (%1)"); break;
#ifdef _WIN64
    case 8: TRUTH_STORE("", "movq %q2, (%1)"); break;
#endif
    case 16: TRUTH_STORE("movdqu (%3), %%xmm0\n\t", "movdqu %%xmm0, (%1)"); break;
    default: TRUTH_STORE("vmovdqu (%3), %%ymm0\n\t", "vmovdqu %%ymm0, (%1)\n\tvzeroupper"); break;
    }
    if (truth_fault && !truth_wrong) truth_wrong = "a store into a read-only page did not fault";
    for (i = 0; i < size; i++) ((volatile unsigned char *)truth_old)[at - truth_base + i] = bytes[i];
}
static void truth_verdict(const char *what)
{
    unsigned i;
    if (truth_wrong) fail(truth_wrong);
    for (i = 0; i < 65536; i++)
        if (truth_base[i] != truth_old[i]) fail(what);
}
#ifdef _WIN64
static const unsigned truth_sizes[] = { 1, 2, 4, 8, 16, 32 };
#else
static const unsigned truth_sizes[] = { 1, 2, 4, 16, 32 };
#endif
/* One round of V2: all 16 pages read-only, then stores of every size into the listed pages, the first store into each
 * at its own address: each listed page faults once, at its first store, and no other page ever. */
static void truth_round(volatile unsigned char *base, const unsigned *pages, unsigned count, unsigned round)
{
    unsigned char bytes[32];
    DWORD old;
    unsigned p, k, i, listed;
    for (i = 0; i < 16; i++) truth_faults[i] = 0;
    if (!VirtualProtect((void *)base, 65536, PAGE_READONLY, &old)) fail("truth: read-only protection");
    for (p = 0; p < count; p++)
        for (k = 0; k < sizeof(truth_sizes) / sizeof(truth_sizes[0]); k++) {
            const unsigned size = truth_sizes[k];
            volatile unsigned char *at = base + pages[p] * 4096 + (k * 613 + pages[p] * 37 + round * 101) % (4096 - size);
            for (i = 0; i < size; i++) bytes[i] = (unsigned char)(round * 59 + p * 13 + k * 7 + i);
            truth_store(at, size, bytes, k ? 0 : (ULONG_PTR)at);
        }
    for (i = 0; i < 16; i++) {
        for (listed = p = 0; p < count; p++) listed |= pages[p] == i;
        if (truth_faults[i] != (LONG)listed)
            fail(listed ? "truth: a written page did not fault exactly once" : "truth: a page never written faulted");
    }
    truth_verdict("truth: memory after the stores");
}
/* V2 and V7 and, in 64-bit code, V1 and V3. */
static void truth_cases(volatile unsigned char *base)
{
    static const unsigned first[] = { 0, 2, 3, 5, 6, 9, 12, 15 }, second[] = { 1, 4, 7, 8, 10, 13 };
    unsigned char bytes[32];
    DWORD old;
    unsigned i, k;
    if (!AddVectoredExceptionHandler(1, truth_handler)) fail("truth handler");
    for (i = 0; i < 65536; i++) base[i] = truth_old[i] = (unsigned char)(i * 7 + 1);
    truth_base = base;
    start("V2 truth: every store into a read-only 4 KiB page faults at the store before it lands, once a page");
    truth_round(base, first, sizeof(first) / sizeof(first[0]), 0);
    truth_round(base, second, sizeof(second) / sizeof(second[0]), 1);   /* re-armed pages fault again */
    start("V2 truth: a writable page beside a no-access one and two read-only ones takes stores with no fault");
    if (!VirtualProtect((void *)base, 16384, PAGE_READONLY, &old) || !VirtualProtect((void *)base, 4096, PAGE_READWRITE, &old) ||
        !VirtualProtect((void *)(base + 4096), 4096, PAGE_NOACCESS, &old))
        fail("truth: mixed watches");
    for (k = 0; k < sizeof(truth_sizes) / sizeof(truth_sizes[0]); k++) {
        for (i = 0; i < truth_sizes[k]; i++) bytes[i] = (unsigned char)(k * 31 + i);
        truth_store(base + 4096 - truth_sizes[k] - k, truth_sizes[k], bytes, 0);
    }
    for (i = 0; i < 1024; i++) {
        bytes[0] = (unsigned char)(i * 3);
        truth_store(base + i, 1, bytes, 0);
    }
    if (!VirtualProtect((void *)(base + 4096), 4096, PAGE_READONLY, &old)) fail("truth: no-access page read back");
    truth_verdict("truth: memory beside the no-access page");
#ifdef _WIN64
    start("V3 straddle: a store across a writable and a read-only page faults at the read-only page's first byte");
    if (!VirtualProtect((void *)base, 65536, PAGE_READONLY, &old) || !VirtualProtect((void *)base, 4096, PAGE_READWRITE, &old) ||
        !VirtualProtect((void *)(base + 3 * 4096), 4096, PAGE_READWRITE, &old))
        fail("straddle: protection");
    for (k = 1; k < sizeof(truth_sizes) / sizeof(truth_sizes[0]); k++) {
        const unsigned size = truth_sizes[k], splits[3] = { 1, size / 2, size - 1 };
        unsigned s;
        for (s = 0; s < 3; s++) {
            for (i = 0; i < size; i++) bytes[i] = (unsigned char)(k * 41 + s * 5 + i);
            /* inside one host page (pages 0 and 1), and across two (pages 3 and 4) */
            if (!VirtualProtect((void *)(base + 4096), 4096, PAGE_READONLY, &old) ||
                !VirtualProtect((void *)(base + 16384), 4096, PAGE_READONLY, &old))
                fail("straddle: read-only again");
            truth_store(base + 4096 - splits[s], size, bytes, (ULONG_PTR)(base + 4096));
            truth_store(base + 16384 - splits[s], size, bytes, (ULONG_PTR)(base + 16384));
        }
    }
    truth_verdict("straddle: memory after the stores");
#endif
    /* V7 cost: a writable page whose host page holds only no-access pages beside it is mapped writable, so its stores are
     * plain memory. Patch 2075; before it each store faulted and ran as one stepped instruction, and Mono, whose hazard
     * table sits on such a host page, took 40 s to start a .NET game (2026-10-24). Read-only and guard pages
     * beside a writable one still make it fault (V2). 200,000 stores take well under a millisecond as memory, and seconds
     * when each is stepped; the bound is 100 ms. */
    start("V7 cost: stores into a writable page beside only no-access pages are not stepped one by one");
    {
        LARGE_INTEGER frequency, begin, end;
        volatile ULONG *words = (volatile ULONG *)base;
        unsigned n;
        truth_base = NULL;
        if (!VirtualProtect((void *)base, 16384, PAGE_NOACCESS, &old) || !VirtualProtect((void *)base, 4096, PAGE_READWRITE, &old))
            fail("cost: protection");
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&begin);
        for (n = 0; n < 200000; n++) words[n & 1023] = n;
        QueryPerformanceCounter(&end);
        if ((end.QuadPart - begin.QuadPart) * 1000 > frequency.QuadPart * 100)
            fail("cost: 200,000 stores beside no-access pages took over 100 ms: each was stepped");
    }
    truth_base = NULL;
    if (!VirtualProtect((void *)base, 65536, PAGE_READWRITE, &old)) fail("truth: reset");
}

#ifdef _WIN64
/* V1 signals: a #UD and a #DE raised by instructions a run executes (each after a store that starts a run on the
 * console) reach the program's handler at their own instruction with the registers as they were, and ymm0-15 come
 * back intact after both. */
static volatile ULONG_PTR signal_ud, signal_de;
static volatile LONG signal_seen;
static const char *volatile signal_wrong;
static unsigned char signal_vectors[1024 + 72] __attribute__((aligned(32)));   /* ymm0-15 in, out, then rbx, r8-r15 */
static int signal_constants(const CONTEXT *c)
{
    return c->R9 == 0x0909090909090909ull && c->R10 == 0x0a0a0a0a0a0a0a0aull && c->R11 == 0x0b0b0b0b0b0b0b0bull &&
           c->R12 == 0x0c0c0c0c0c0c0c0cull && c->R13 == 0x0d0d0d0d0d0d0d0dull && c->R14 == 0x0e0e0e0e0e0e0e0eull &&
           c->R15 == 0x0f0f0f0f0f0f0f0full && c->Rax == 7 && c->Rcx == 0 && c->Rdx == 0;
}
static LONG CALLBACK signal_handler(EXCEPTION_POINTERS *e)
{
    CONTEXT *c = e->ContextRecord;
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ILLEGAL_INSTRUCTION && c->Rip == signal_ud) {
        if (!signal_constants(c) || c->Rbx != 0x1111111111111112ull || c->R8 != 0x0808080808080808ull)
            signal_wrong = "#UD: the registers are not the program's";
        signal_seen |= 1;
    } else if (code == EXCEPTION_INT_DIVIDE_BY_ZERO && c->Rip == signal_de) {
        if (!signal_constants(c) || c->Rbx != 0x1111111111111112ull || c->R8 != 0x0808080808080809ull)
            signal_wrong = "#DE: the registers are not the program's";
        signal_seen |= 2;
    } else
        return EXCEPTION_CONTINUE_SEARCH;
    c->Rip += 3;   /* lock add %ebx, %eax and div %rcx are three bytes each */
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void signal_case(unsigned char *base)
{
    void *handler;
    unsigned i;
    start("V1 signals: #UD and #DE in a run reach the program at the instruction, registers and ymm0-15 intact");
    for (i = 0; i < 512; i++) ((volatile unsigned char *)signal_vectors)[i] = (unsigned char)(i * 11 + 3);
    mixed(base, 1);
    if (!(handler = AddVectoredExceptionHandler(1, signal_handler))) fail("signals: handler");
    __asm__ volatile(
        "leaq 2f(%%rip), %%rax\n\tmovq %%rax, %[ud]\n\t"
        "leaq 3f(%%rip), %%rax\n\tmovq %%rax, %[de]\n\t"
        "vmovdqu 0(%%rsi), %%ymm0\n\tvmovdqu 32(%%rsi), %%ymm1\n\tvmovdqu 64(%%rsi), %%ymm2\n\t"
        "vmovdqu 96(%%rsi), %%ymm3\n\tvmovdqu 128(%%rsi), %%ymm4\n\tvmovdqu 160(%%rsi), %%ymm5\n\t"
        "vmovdqu 192(%%rsi), %%ymm6\n\tvmovdqu 224(%%rsi), %%ymm7\n\tvmovdqu 256(%%rsi), %%ymm8\n\t"
        "vmovdqu 288(%%rsi), %%ymm9\n\tvmovdqu 320(%%rsi), %%ymm10\n\tvmovdqu 352(%%rsi), %%ymm11\n\t"
        "vmovdqu 384(%%rsi), %%ymm12\n\tvmovdqu 416(%%rsi), %%ymm13\n\tvmovdqu 448(%%rsi), %%ymm14\n\t"
        "vmovdqu 480(%%rsi), %%ymm15\n\t"
        "movabsq $0x1111111111111111, %%rbx\n\tmovabsq $0x0808080808080808, %%r8\n\t"
        "movabsq $0x0909090909090909, %%r9\n\tmovabsq $0x0a0a0a0a0a0a0a0a, %%r10\n\t"
        "movabsq $0x0b0b0b0b0b0b0b0b, %%r11\n\tmovabsq $0x0c0c0c0c0c0c0c0c, %%r12\n\t"
        "movabsq $0x0d0d0d0d0d0d0d0d, %%r13\n\tmovabsq $0x0e0e0e0e0e0e0e0e, %%r14\n\t"
        "movabsq $0x0f0f0f0f0f0f0f0f, %%r15\n\t"
        "xorl %%ecx, %%ecx\n\txorl %%edx, %%edx\n\tmovl $7, %%eax\n\t"
        "movq %%rax, (%%rdi)\n\t"          /* into the writable page: on the console a run starts here */
        "addq $1, %%rbx\n"
        "2:\t.byte 0xf0, 0x01, 0xd8\n\t"   /* lock add %ebx, %eax: #UD */
        "movq %%rax, 8(%%rdi)\n\t"         /* the next run */
        "addq $1, %%r8\n"
        "3:\tdivq %%rcx\n\t"               /* #DE */
        "vmovdqu %%ymm0, 512(%%rsi)\n\tvmovdqu %%ymm1, 544(%%rsi)\n\tvmovdqu %%ymm2, 576(%%rsi)\n\t"
        "vmovdqu %%ymm3, 608(%%rsi)\n\tvmovdqu %%ymm4, 640(%%rsi)\n\tvmovdqu %%ymm5, 672(%%rsi)\n\t"
        "vmovdqu %%ymm6, 704(%%rsi)\n\tvmovdqu %%ymm7, 736(%%rsi)\n\tvmovdqu %%ymm8, 768(%%rsi)\n\t"
        "vmovdqu %%ymm9, 800(%%rsi)\n\tvmovdqu %%ymm10, 832(%%rsi)\n\tvmovdqu %%ymm11, 864(%%rsi)\n\t"
        "vmovdqu %%ymm12, 896(%%rsi)\n\tvmovdqu %%ymm13, 928(%%rsi)\n\tvmovdqu %%ymm14, 960(%%rsi)\n\t"
        "vmovdqu %%ymm15, 992(%%rsi)\n\tvzeroupper\n\t"
        "movq %%rbx, 1024(%%rsi)\n\tmovq %%r8, 1032(%%rsi)\n\tmovq %%r9, 1040(%%rsi)\n\tmovq %%r10, 1048(%%rsi)\n\t"
        "movq %%r11, 1056(%%rsi)\n\tmovq %%r12, 1064(%%rsi)\n\tmovq %%r13, 1072(%%rsi)\n\tmovq %%r14, 1080(%%rsi)\n\t"
        "movq %%r15, 1088(%%rsi)\n\t"
        : [ud] "=m"(signal_ud), [de] "=m"(signal_de)
        : "S"(signal_vectors), "D"(base)
        : "rax", "rbx", "rcx", "rdx", "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15", "xmm0", "xmm1", "xmm2",
          "xmm3", "xmm4", "xmm5", "xmm6", "xmm7", "xmm8", "xmm9", "xmm10", "xmm11", "xmm12", "xmm13", "xmm14", "xmm15",
          "memory", "cc");
    RemoveVectoredExceptionHandler(handler);
    if (signal_wrong) fail(signal_wrong);
    if (signal_seen != 3) fail("signals: the #UD or the #DE did not reach the program's handler");
    for (i = 0; i < 512; i++)
        if (((volatile unsigned char *)signal_vectors)[512 + i] != ((volatile unsigned char *)signal_vectors)[i])
            fail("signals: ymm0-15 changed");
    {
        const volatile ULONGLONG *gpr = (const volatile ULONGLONG *)(signal_vectors + 1024);
        if (gpr[0] != 0x1111111111111112ull || gpr[1] != 0x0808080808080809ull) fail("signals: rbx or r8 after");
        for (i = 2; i < 9; i++)
            if (gpr[i] != 0x0101010101010101ull * (7 + i)) fail("signals: r9-r15 after");
    }
    if (*(volatile ULONGLONG *)base != 7 || *(volatile ULONGLONG *)(base + 8) != 7) fail("signals: the stores");
    mixed(base, 15);
}

/* V4 speed: stores into a writable page beside three read-only ones, the best of three, in microseconds. */
static unsigned speed_us(unsigned char *base, unsigned which)
{
    static unsigned char from[4096];
    unsigned best = ~0u, round, i;
    for (round = 0; round < 3; round++) {
        LARGE_INTEGER t0;
        unsigned us;
        mixed(base, 1);
        QueryPerformanceCounter(&t0);
        switch (which) {
        case 0: for (i = 0; i < 1024; i++) ((volatile unsigned *)base)[i] = i; break;
        case 1: for (i = 0; i < 256; i++) __asm__ volatile("movdqu %%xmm0, (%0)" : : "r"(base + 16 * i) : "memory"); break;
        case 2:
            for (i = 0; i < 128; i++) __asm__ volatile("vmovdqu %%ymm0, (%0)" : : "r"(base + 32 * i) : "memory");
            __asm__ volatile("vzeroupper");
            break;
        case 3: rep_movsb(base, from, 4096); break;
        default: {
            void *to = base;
            SIZE_T count = 512;
            __asm__ volatile("rep stosq" : "+D"(to), "+c"(count) : "a"(0x5a5a5a5a5a5a5a5aull) : "memory");
        }
        }
        us = elapsed_us(t0);
        if (us < best) best = us;
    }
    return best;
}
static void speed_case(unsigned char *base)
{
    static const char *const what[] = { " us (bound 600), 256 SSE stores ", " us, 128 AVX stores ", " us, rep movsb of 4096 ",
                                        " us, rep stosq of 4096 ", " us (best of 3)" };
    char line[16], setting[8];
    unsigned us[5], i;
    const int stepped = GetEnvironmentVariableA("WINE_PS5_MMU4K", setting, sizeof(setting)) && setting[0] == '0';
    start("V4 speed: stores into a writable page beside three read-only ones");
    us[0] = speed_us(base, 0);
    for (i = 0; i < 1024; i++) if (((volatile unsigned *)base)[i] != i) fail("speed: the stores");
    for (i = 1; i < 5; i++) us[i] = speed_us(base, i);
    say(stepped || us[0] <= 600 ? "PS5_PROTON_VM_PROTECT_SPEED PASS: " : "PS5_PROTON_VM_PROTECT_SPEED FAIL: ");
    say("1024 4-byte stores beside read-only pages took ");
    for (i = 0; i < 5; i++) { say(decimal(line, us[i])); say(what[i]); }
    say(stepped ? "; WINE_PS5_MMU4K=0: stepped, the bound not applied\n" : "\n");
    ExitProcess(stepped || us[0] <= 600 ? 0 : 1);
}

/* V6 re-arm: one thread stores rising numbers into a writable page beside read-only ones; another makes it read-only,
 * reads it, waits and reads it again. A change after the protection returned, with no access violation in between,
 * is a store that went through a page the program had closed: 100,000 cycles, none. */
static volatile ULONGLONG *rearm_page;
static volatile LONG rearm_faults, rearm_handled, rearm_stop;
static LONG CALLBACK rearm_handler(EXCEPTION_POINTERS *e)
{
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    DWORD old;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 ||
        r->ExceptionInformation[1] - (ULONG_PTR)rearm_page >= 4096)
        return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&rearm_faults);
    if (!VirtualProtect((void *)rearm_page, 4096, PAGE_READWRITE, &old)) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&rearm_handled);
    return EXCEPTION_CONTINUE_EXECUTION;
}
static DWORD WINAPI rearm_writer(void *unused)
{
    ULONGLONG n = 0;
    (void)unused;
    while (!rearm_stop) rearm_page[0] = ++n;
    return 0;
}
static void rearm_case(unsigned char *base)
{
    LARGE_INTEGER t0, t1, hz;
    char line[16];
    unsigned cycle, changed = 0, violations = 0;
    HANDLE writer;
    DWORD old;
    start("V6 re-arm: a page made read-only takes no store until its access violation");
    mixed(base, 1);
    rearm_page = (volatile ULONGLONG *)base;
    if (!AddVectoredExceptionHandler(1, rearm_handler)) fail("re-arm: handler");
    if (!(writer = CreateThread(NULL, 0, rearm_writer, NULL, 0, NULL))) fail("re-arm: thread");
    QueryPerformanceFrequency(&hz);
    for (cycle = 0; cycle < 100000; cycle++) {
        ULONGLONG before, after;
        LONG faults;
        while (rearm_handled != rearm_faults) YieldProcessor();
        faults = rearm_faults;
        if (!VirtualProtect(base, 4096, PAGE_READONLY, &old)) fail("re-arm: protection");
        before = rearm_page[0];
        QueryPerformanceCounter(&t0);
        do QueryPerformanceCounter(&t1); while ((t1.QuadPart - t0.QuadPart) * 50000 < hz.QuadPart);   /* 20 us */
        after = rearm_page[0];
        if (after != before) {
            changed++;
            if (rearm_faults == faults) violations++;
        }
    }
    rearm_stop = 1;
    WaitForSingleObject(writer, INFINITE);
    say(violations ? "PS5_PROTON_VM_PROTECT_REARM FAIL: " : "PS5_PROTON_VM_PROTECT_REARM PASS: ");
    say(decimal(line, violations)); say(" stores through a closed page in 100000 cycles; the page changed in ");
    say(decimal(line, changed)); say(" cycles after an access violation\n");
    ExitProcess(violations ? 1 : 0);
}
#endif

/* Code made executable (Wine patch 1127). A console helper's kernel lets only JIT shared memory run: code becomes JIT
 * memory at its first execute, and every later protection change of it is one kernel call. These are the shapes
 * programs give their own code (E1-E7 of the JIT design); mov eax, imm32; ret is the same bytes in 32-bit and 64-bit
 * code. An access violation in a case's memory fails the probe: a fault the protection allows must never reach the
 * program (a conversion's read-only copy, or patch 0954's two-call window). */
static unsigned char *exec_low, *exec_high;
static volatile LONG exec_guards;
static LONG CALLBACK exec_handler(EXCEPTION_POINTERS *e)
{
    const EXCEPTION_RECORD *r = e->ExceptionRecord;
    unsigned char *at;
    if (r->ExceptionCode == STATUS_GUARD_PAGE_VIOLATION) {
        InterlockedIncrement(&exec_guards);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    at = (unsigned char *)r->ExceptionInformation[1];
    if (at >= exec_low && at < exec_high) fail("an access violation in code or data its protection allows");
    return EXCEPTION_CONTINUE_SEARCH;
}
static void emit(unsigned char *at, unsigned value)
{
    unsigned i;
    at[0] = 0xb8;
    for (i = 0; i < 4; i++) at[1 + i] = (unsigned char)(value >> (8 * i));
    at[5] = 0xc3;
    FlushInstructionCache(GetCurrentProcess(), at, 6);
}
static unsigned run(const unsigned char *at)
{
    return ((unsigned (*)(void))at)();
}
static void protect(void *at, SIZE_T bytes, DWORD protection, const char *why)
{
    DWORD old;
    if (!VirtualProtect(at, bytes, protection, &old)) fail(why);
}
static unsigned char e3_image[32768] = { 1 };  /* .data: holds one 16 KiB page of the image whose first 4 KiB become code */
static volatile LONG e3_done;
static LONG e3_writes;
static DWORD WINAPI e3_writer(void *at)
{
    LONG writes = 0;
    while (!e3_done) {
        InterlockedIncrement((volatile LONG *)at);
        writes++;
    }
    e3_writes = writes;
    return 0;
}
static unsigned char *e6_code;
static volatile LONG e6_done, e6_calls;
static DWORD WINAPI e6_run(void *unused)
{
    (void)unused;
    while (!e6_done) {
        if (run(e6_code) != 6) fail("E6: the code returned another value");
        InterlockedIncrement(&e6_calls);
    }
    return 0;
}
static void exec_cases(unsigned *rewrite_us)
{
    char path[MAX_PATH];
    unsigned char *code, *page;
    void *written[16];
    ULONG_PTR count;
    ULONG granularity;
    HANDLE threads[4];
    LARGE_INTEGER t0;
    MEMORY_BASIC_INFORMATION info;
    DWORD old;
    unsigned i;
    if (!AddVectoredExceptionHandler(1, exec_handler)) fail("exec handler");

    start("E1 code made executable, rewritten and run again, 1000 times");
    if (!(code = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) fail("E1: allocation");
    exec_low = code; exec_high = code + 65536;
    QueryPerformanceCounter(&t0);
    for (i = 0; i < 1000; i++) {
        protect(code, 4096, PAGE_READWRITE, "E1: read-write");
        emit(code, i);
        protect(code, 4096, PAGE_EXECUTE_READ, "E1: execute");
        if (run(code) != i) fail("E1: the rewritten code did not run");
    }
    *rewrite_us = elapsed_us(t0);
    if (!VirtualFree(code, 0, MEM_RELEASE)) fail("E1: release");

    start("E2 read-write-execute memory (Mono, Zink) written, run, rewritten and run");
    if (!(code = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE))) fail("E2: allocation");
    exec_low = code; exec_high = code + 65536;
    emit(code, 1);
    emit(code + 61440, 3);
    if (run(code) != 1 || run(code + 61440) != 3) fail("E2: the code did not run");
    emit(code, 2);
    if (run(code) != 2) fail("E2: the rewritten code did not run");
    if (!VirtualFree(code, 0, MEM_RELEASE)) fail("E2: release");

    start("E3 code beside data in one 16 KiB page of the image (vcruntime140_1): one thread writes the data, another runs the code");
    page = (unsigned char *)(((ULONG_PTR)e3_image + 16383) & ~(ULONG_PTR)16383);
    if (!VirtualQuery(page, &info, sizeof(info)) || info.Type != MEM_IMAGE) fail("E3: not an image page");
    exec_low = page; exec_high = page + 16384;
    emit(page, 77);
    if (!VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old)) fail("E3: execute");
    e3_done = 0;
    if (!(threads[0] = CreateThread(NULL, 0, e3_writer, page + 4096, 0, NULL))) fail("E3: thread");
    while (*(volatile LONG *)(page + 4096) < 1000) Sleep(0);
    for (i = 0; i < 1000; i++)
        if (run(page) != 77) fail("E3: the code did not run");
    e3_done = 1;
    WaitForSingleObject(threads[0], INFINITE);
    CloseHandle(threads[0]);
    if (*(volatile LONG *)(page + 4096) != e3_writes) fail("E3: a write to the data beside the code was lost");
    protect(page, 4096, old, "E3: restore");

#ifndef _WIN64
    {
        /* A 32-bit program without DEP: everything readable runs (Wine's force_exec_prot). 64-bit processes cannot. */
        NTSTATUS (WINAPI *set_information)(HANDLE, PROCESSINFOCLASS, void *, ULONG) =
            (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtSetInformationProcess");
        ULONG flags = MEM_EXECUTE_OPTION_ENABLE;
        char dir[MAX_PATH];
        unsigned char *view;
        HANDLE file, mapping;
        DWORD done;
        start("E4 execute enabled for the process: heap memory runs, and a file view still takes read-only protection");
        if (!set_information || set_information(GetCurrentProcess(), ProcessExecuteFlags, &flags, sizeof(flags)))
            fail("E4: execute for the process");
        if (!(code = HeapAlloc(GetProcessHeap(), 0, 64))) fail("E4: heap");
        emit(code, 4);
        if (run(code) != 4) fail("E4: heap code did not run");
        HeapFree(GetProcessHeap(), 0, code);
        if (!GetTempPathA(sizeof(dir), dir) || !GetTempFileNameA(dir, "vmx", 0, path)) fail("E4: temporary file");
        file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, NULL);
        if (file == INVALID_HANDLE_VALUE || !WriteFile(file, GetModuleHandleA(NULL), 4096, &done, NULL) || done != 4096)
            fail("E4: file");
        mapping = CreateFileMappingA(file, NULL, PAGE_READWRITE, 0, 0, NULL);
        view = mapping ? MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 0) : NULL;
        if (!view) fail("E4: file view");
        protect(view, 4096, PAGE_READONLY, "E4: a file view's read-only protection");
        if (view[0] != 'M' || view[1] != 'Z') fail("E4: file view contents");
        if (!UnmapViewOfFile(view) || !CloseHandle(mapping) || !CloseHandle(file)) fail("E4: file view release");
        flags = MEM_EXECUTE_OPTION_DISABLE;
        if (set_information(GetCurrentProcess(), ProcessExecuteFlags, &flags, sizeof(flags))) fail("E4: execute disabled again");
    }
#endif

    start("E5 an image loaded as a resource only: every page reads, nothing runs");
    {
        const unsigned char *image;
        const IMAGE_NT_HEADERS *nt;
        HMODULE resource;
        resource = GetModuleFileNameA(NULL, path, sizeof(path)) ?
                   LoadLibraryExA(path, NULL, LOAD_LIBRARY_AS_IMAGE_RESOURCE) : NULL;
        if (!resource) fail("E5: resource-only load");
        image = (const unsigned char *)((ULONG_PTR)resource & ~(ULONG_PTR)3);
        nt = (const IMAGE_NT_HEADERS *)(image + ((const IMAGE_DOS_HEADER *)image)->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || !VirtualQuery(image, &info, sizeof(info)) || info.Type != MEM_IMAGE)
            fail("E5: not an image");
        exec_low = (unsigned char *)image; exec_high = exec_low + nt->OptionalHeader.SizeOfImage;
        for (i = 0; i < nt->OptionalHeader.SizeOfImage; i += 4096) (void)((volatile const unsigned char *)image)[i];
        exec_low = exec_high = NULL;
        if (!FreeLibrary(resource)) fail("E5: release");
    }

    start("E6 four threads run code while a fifth changes its 4 KiB neighbour's protection 10000 times");
    if (!(e6_code = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) fail("E6: allocation");
    exec_low = e6_code; exec_high = e6_code + 65536;
    emit(e6_code, 6);
    protect(e6_code, 4096, PAGE_EXECUTE_READ, "E6: execute");
    for (i = 0; i < 4; i++)
        if (!(threads[i] = CreateThread(NULL, 0, e6_run, NULL, 0, NULL))) fail("E6: threads");
    while (e6_calls < 100) Sleep(0);
    for (i = 0; i < 10000; i++)
        protect(e6_code + 4096, 4096, i & 1 ? PAGE_READWRITE : PAGE_READONLY, "E6: the neighbour's protection");
    e6_done = 1;
    WaitForMultipleObjects(4, threads, TRUE, INFINITE);
    for (i = 0; i < 4; i++) CloseHandle(threads[i]);
    if (!VirtualFree(e6_code, 0, MEM_RELEASE)) fail("E6: release");

    start("E7 a guard page of data and of code raises once, and a write-watch page of code reports its writes");
    if (!(code = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE))) fail("E7: allocation");
    exec_low = code; exec_high = code + 65536;
    protect(code, 4096, PAGE_READWRITE | PAGE_GUARD, "E7: guard");
    exec_guards = 0;
    (void)((volatile unsigned char *)code)[0];
    (void)((volatile unsigned char *)code)[0];
    if (exec_guards != 1) fail("E7: a guard page of data did not raise exactly once");
    emit(code + 16384, 8);
    protect(code + 16384, 4096, PAGE_EXECUTE_READ | PAGE_GUARD, "E7: guard of code");
    exec_guards = 0;
    if (run(code + 16384) != 8 || run(code + 16384) != 8 || exec_guards != 1)
        fail("E7: a guard page of code did not raise exactly once");
    if (!VirtualFree(code, 0, MEM_RELEASE)) fail("E7: release");
    code = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_EXECUTE_READWRITE);
    if (!code) fail("E7: write-watch allocation");
    exec_low = code; exec_high = code + 65536;
    emit(code, 70);
    if (run(code) != 70 || ResetWriteWatch(code, 65536) || run(code) != 70) fail("E7: write-watch code did not run");
    count = 16;
    if (GetWriteWatch(0, code, 65536, written, &count, &granularity) || count) fail("E7: running code was reported as a write");
    emit(code, 71);
    count = 16;
    /* Write watch is kept per host page: on the console's 16 KiB pages one write reports its four 4 KiB pages
     * (upstream behaviour; over-reporting only makes a collector rescan more). */
    if (GetWriteWatch(0, code, 65536, written, &count, &granularity) || count < 1 || count > 16384 / 4096 ||
        written[0] != code)
        fail(count ? "E7: a write to code was reported outside its host page" : "E7: a write to code was not reported");
    if (run(code) != 71) fail("E7: rewritten write-watch code did not run");
    if (!VirtualFree(code, 0, MEM_RELEASE)) fail("E7: write-watch release");
    exec_low = exec_high = NULL;
}

void mainCRTStartup(void)
{
    unsigned char *base = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    DWORD old; MEMORY_BASIC_INFORMATION info;
    unsigned iteration, offset;
    if (!CreateThread(NULL, 0, watchdog, NULL, 0, NULL)) fail("watchdog");
    if (!base) fail("allocation");
    if (asked("--truth")) {
        marker = "PS5_PROTON_VM_PROTECT_TRUTH";
#ifdef _WIN64
        signal_case(base);
#endif
        truth_cases(base);
#ifdef _WIN64
        say("PS5_PROTON_VM_PROTECT_TRUTH PASS: V1 #UD and #DE after stores into a writable page beside read-only ones "
            "reached the handler with their registers and ymm0-15; V2 every store into a read-only 4 KiB page faulted at "
            "the store before it landed, once a page, never in an unwritten page, and none beside a no-access page; V3 "
            "30 stores across a writable and a read-only page faulted at the read-only page's first byte; V7 200,000 stores beside only "
            "no-access pages took under 100 ms\n");
#else
        say("PS5_PROTON_VM_PROTECT_TRUTH PASS: V2 every store into a read-only 4 KiB page faulted at the store before it "
            "landed, once a page, never in an unwritten page, and none beside a no-access page; V7 200,000 stores beside only no-access "
            "pages took under 100 ms (32-bit: no V1, V3)\n");
#endif
        ExitProcess(0);
    }
#ifdef _WIN64
    if (asked("--speed")) {
        marker = "PS5_PROTON_VM_PROTECT_SPEED";
        speed_case(base);
    }
    if (asked("--rearm")) {
        marker = "PS5_PROTON_VM_PROTECT_REARM";
        rearm_case(base);
    }
#endif
    for (iteration = 0; iteration < 32; ++iteration) {
        if (!VirtualProtect(base, 65536, PAGE_READWRITE, &old)) fail("reset");
        /* mov eax,42; ret -- synthetic x86-64 code generated by this test. */
        base[0] = 0xb8; base[1] = 42; base[2] = base[3] = base[4] = 0; base[5] = 0xc3;
        if (!VirtualProtect(base, 4096, PAGE_EXECUTE_READ, &old) ||
            !FlushInstructionCache(GetCurrentProcess(), base, 4096)) fail("executable protection");
        for (offset = 4096; offset < 65536; offset += 4096) {
            volatile unsigned char *data = base + offset;
            *data = (unsigned char)(iteration + offset/4096);
            if (*data != (unsigned char)(iteration + offset/4096)) fail("adjacent write");
            if (!VirtualQuery(base+offset, &info, sizeof(info)) || info.Protect != PAGE_READWRITE)
                fail("adjacent logical protection");
        }
        if (((unsigned (*)(void))base)() != 42) fail("code execution");
        image_data = iteration;
        if (image_data != iteration) fail("image data write");
    }
    if (!VirtualFree(base, 0, MEM_RELEASE)) fail("release");
    /* A reserved allocation is a different path from reserve+commit. Commit
     * only the metadata-sized 4 KiB page between inaccessible neighbors, as
     * general-purpose allocators do; recommit must also supply zeroed bytes. */
    base = VirtualAlloc(NULL, 2*1024*1024, MEM_RESERVE, PAGE_NOACCESS);
    if (!base) fail("sparse reservation");
    if (VirtualAlloc(base+8192, 4096, MEM_COMMIT, PAGE_READWRITE) != base+8192)
        fail("sparse neighbor commit");
    base[8192] = 0x7e; base[12287] = 0xe7;
    for (iteration = 0; iteration < 32; ++iteration) {
        volatile unsigned char *data = base+4096;
        if (VirtualAlloc(base+4096, 4096, MEM_COMMIT, PAGE_READWRITE) != base+4096)
            fail("sparse commit");
        if (*data || data[4095]) fail("recommit zero contents");
        *data = 0xa5; data[4095] = 0x5a;
        if (*data != 0xa5 || data[4095] != 0x5a) fail("sparse write");
        if (!VirtualQuery(base+4096, &info, sizeof(info)) || info.State != MEM_COMMIT ||
            info.Protect != PAGE_READWRITE) fail("sparse logical protection");
        if (!VirtualQuery(base, &info, sizeof(info)) || info.State != MEM_RESERVE ||
            !VirtualQuery(base+12288, &info, sizeof(info)) || info.State != MEM_RESERVE)
            fail("neighbor reservation preserved");
        if (!VirtualFree(base+4096, 4096, MEM_DECOMMIT)) fail("sparse decommit");
        if (base[8192] != 0x7e || base[12287] != 0xe7) fail("live neighbor contents preserved");
    }
    if (!VirtualFree(base, 0, MEM_RELEASE)) fail("sparse release");
    base = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!base) fail("spanning allocation");
    for (offset = 0; offset < 65536; offset += 4096) {
        base[offset] = 0x33; base[offset+4095] = 0xcc;
    }
    if (!VirtualFree(base+4096, 32768, MEM_DECOMMIT)) fail("spanning decommit");
    if (base[0] != 0x33 || base[4095] != 0xcc || base[36864] != 0x33 || base[40959] != 0xcc)
        fail("spanning neighbors preserved");
    if (VirtualAlloc(base+4096, 32768, MEM_COMMIT, PAGE_READWRITE) != base+4096)
        fail("spanning recommit");
    for (offset = 4096; offset < 36864; offset += 4096)
        if (base[offset] || base[offset+4095]) fail("spanning zero contents");
    if (!VirtualFree(base, 0, MEM_RELEASE)) fail("spanning release");
#ifdef _WIN64
    /* A large reservation committed a little at a time, as Chromium's allocator, .NET's region GC and V8 do:
     * 32 GiB - 64 KiB to find a 16 GiB-aligned 16 GiB pool, then the pool at that address. */
    {
        const SIZE_T pool = (SIZE_T)16 << 30, granule = 65536;
        unsigned char *search = VirtualAlloc(NULL, pool * 2 - granule, MEM_RESERVE, PAGE_NOACCESS), *aligned, *code;
        if (!search) fail("large reservation");
        aligned = (unsigned char *)(((ULONG_PTR)search + pool - 1) & ~(ULONG_PTR)(pool - 1));
        if (!VirtualFree(search, 0, MEM_RELEASE)) fail("large release");
        base = VirtualAlloc(aligned, pool, MEM_RESERVE, PAGE_NOACCESS);
        if (base != aligned) fail("aligned large reservation");
        for (offset = 0; offset < 4; ++offset) {
            unsigned char *at = base + (SIZE_T)offset * (pool / 4) + 8192;
            if (VirtualAlloc(at, 65536, MEM_COMMIT, PAGE_READWRITE) != at) fail("large commit");
            if (at[0] || at[65535]) fail("large commit zero contents");
            at[0] = 0x11; at[65535] = 0x22;
            if (at[0] != 0x11 || at[65535] != 0x22) fail("large write");
            if (!VirtualQuery(at - 8192, &info, sizeof(info)) || info.State != MEM_RESERVE)
                fail("large neighbor reservation preserved");
        }
        if (!VirtualFree(base + 8192, 65536, MEM_DECOMMIT)) fail("large decommit");
        if (VirtualAlloc(base + 8192, 65536, MEM_COMMIT, PAGE_READWRITE) != base + 8192) fail("large recommit");
        if (base[8192] || base[8192 + 65535]) fail("large recommit zero contents");
        if (base[pool / 4 + 8192] != 0x11 || base[pool / 4 + 8192 + 65535] != 0x22) fail("large neighbor contents kept");
        code = base + pool / 2 + 8192;
        code[0] = 0xb8; code[1] = 7; code[2] = code[3] = code[4] = 0; code[5] = 0xc3;
        if (!VirtualProtect(code, 4096, PAGE_EXECUTE_READ, &old) ||
            !FlushInstructionCache(GetCurrentProcess(), code, 4096)) fail("large executable protection");
        if (((unsigned (*)(void))code)() != 7) fail("large code execution");
        if (!VirtualFree(base, 0, MEM_RELEASE)) fail("large pool release");
    }
#endif
    {
        char line[16];
        unsigned private_us[2], section_us[2];
        HANDLE section;
        unsigned char *view;
        if (!AddVectoredExceptionHandler(1, tracker_handler)) fail("exception handler");
        base = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!base) fail("tracker allocation");
        start("write tracking in private memory");
        tracker_check(base, "private memory", &private_us[0], &private_us[1]);
        if (!VirtualFree(base, 0, MEM_RELEASE)) fail("tracker release");
        /* A large anonymous section, as an emulator's guest memory (on the console it lives in direct memory). */
        section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 256u << 20, NULL);
        view = section ? MapViewOfFile(section, FILE_MAP_ALL_ACCESS, 0, 0, 0) : NULL;
        if (!view) fail("tracker section");
        start("write tracking in a section view");
        tracker_check(view + (64u << 20), "section view", &section_us[0], &section_us[1]);
        if (!UnmapViewOfFile(view) || !CloseHandle(section)) fail("tracker section release");
        /* A view smaller than the host page, as wineboot maps KUSER_SHARED_DATA (0x720 bytes): the free pages beside it
         * in its 16 KiB host page must not make it read-only (usd-diag-2026-10-14-02). */
        section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 65536, NULL);
        view = section ? MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, 0x720) : NULL;
        if (!view) fail("small view");
        ((volatile ULONG *)view)[0x260 / 4] = 0x4a65;
        if (((volatile ULONG *)view)[0x260 / 4] != 0x4a65) fail("small view write");
        if (!UnmapViewOfFile(view) || !CloseHandle(section)) fail("small view release");
        say("PS5_PROTON_VM_PROTECT_PROBE tracker: each 4 KiB page's first write reported once, each 16 KiB's first read; "
            "16 KiB of stores into read-only pages took ");
        say(decimal(line, private_us[0])); say(" us (private), "); say(decimal(line, section_us[0]));
        say(" us (section); 4 KiB beside read-only pages with no handler took ");
        say(decimal(line, private_us[1])); say(" us (private), "); say(decimal(line, section_us[1])); say(" us (section)\n");
    }
#ifdef _WIN64
    {
        char line[16];
        unsigned rep_us;
        const int contention = asked("--contention");
        base = VirtualAlloc(NULL, 65536, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!base) fail("stepped cases allocation");
        stepped_cases(base, contention, &rep_us);
        if (!VirtualFree(base, 0, MEM_RELEASE)) fail("stepped cases release");
        say("PS5_PROTON_VM_PROTECT_PROBE stepped: rep movsb of 4096 bytes beside read-only pages took ");
        say(decimal(line, rep_us)); say(" us (bound 1000000)");
        say(contention ? "; contention ran\n" : "; contention not asked (--contention)\n");
    }
#endif
    {
        char line[16];
        unsigned rewrite_us;
        exec_cases(&rewrite_us);
        say("PS5_PROTON_VM_PROTECT_PROBE exec: E1-E7 passed; 1000 rewrites of a code page (read-write, write, execute, run) took ");
        say(decimal(line, rewrite_us)); say(" us\n");
    }
#ifdef _WIN64
    say("PS5_PROTON_VM_PROTECT_PROBE PASS: 32 code executions, 480 adjacent writes and queries, 32 sparse zero cycles, spanning decommit preserves neighbors, a 16 GiB pool committed 64 KiB at a time, 4 KiB write tracking in private memory and a section, a 0x720-byte view written, stepped rep movsb, an access violation at a read-only page's first byte, the program's own single step once, a write across two host pages, executable memory E1-E7\n");
#else
    say("PS5_PROTON_VM_PROTECT_PROBE PASS: 32 code executions, 480 adjacent writes and queries, 32 sparse zero cycles, spanning decommit preserves neighbors, 4 KiB write tracking in private memory and a section, a 0x720-byte view written, executable memory E1-E7 with execute enabled for the process (32-bit: no 16 GiB pool, no stepped writes)\n");
#endif
    ExitProcess(0);
}
