/* SPDX-License-Identifier: GPL-3.0-or-later
 * Reserve large free Windows intervals, then load graphics/theme libraries.
 * The same asset-free PE compares late image placement on pinned Linux Wine
 * and PS5. Keep small holes and native mappings intact; release every span.
 */
#include <stdint.h>
#include <windows.h>

typedef void *(WINAPI *alloc2_fn)(HANDLE, void *, SIZE_T, DWORD, DWORD, void *, ULONG);
static void *reservations[8192];
static SIZE_T sizes[8192];

static void report(const char *stage, unsigned count, uint64_t bytes, DWORD error, int passed)
{
    char line[260];
    wsprintfA(line, "PS5_PROTON_LATE_LIBRARY_STAGE stage=%s spans=%u bytes=%I64u error=%lu passed=%d\n",
              stage, count, bytes, error, passed);
    OutputDebugStringA(line);
}

/* A reservation over 32 GiB is one range to a Windows program wherever the system cuts it: the kernel refuses one mapping over
 * 32 GiB (RLIMIT_DATA), so the runtime makes it in pieces of 16 GiB (patch 1231). Here: reserve 33 GiB at a free address, query
 * on both sides of the 16 GiB boundary, commit and write across it, decommit, query again, release the whole range. 1 when it
 * passed or when no free interval holds 33 GiB (said); 0 with the failing step named. */
static int boundary(void)
{
    const uint64_t big = (uint64_t)33 << 30, piece = (uint64_t)16 << 30;
    uint64_t cursor = UINT64_C(0x40000);
    char line[260];

    while (cursor < UINT64_C(0xfc00000000))
    {
        MEMORY_BASIC_INFORMATION info = {0};
        uint64_t base, end, start;
        if (!VirtualQuery((void *)(uintptr_t)cursor, &info, sizeof(info))) break;
        base = (uintptr_t)info.BaseAddress;
        end = base + info.RegionSize;
        if (end <= cursor) break;
        start = (base + 65535) & ~UINT64_C(65535);
        if (info.State == MEM_FREE && end > start && end - start >= big + 0x100000)
        {
            char *at = (char *)(uintptr_t)start, *edge = at + piece - 0x10000;
            const char *step = NULL;
            char *got = VirtualAlloc(at, (SIZE_T)big, MEM_RESERVE, PAGE_NOACCESS);
            MEMORY_BASIC_INFORMATION before = {0}, across = {0}, after = {0};
            if (got != at) step = "reserve";
            else if (!VirtualQuery(edge + 0x10000 - 0x1000, &before, sizeof(before)) || before.State != MEM_RESERVE ||
                     before.AllocationBase != at || before.RegionSize != big - piece + 0x1000) step = "query-before";
            else if (!VirtualAlloc(edge, 0x20000, MEM_COMMIT, PAGE_READWRITE)) step = "commit";
            else
            {
                edge[0] = (char)0xa5;
                edge[0x1ffff] = (char)0x5a;
                if ((unsigned char)edge[0] != 0xa5 || (unsigned char)edge[0x1ffff] != 0x5a) step = "write";
                else if (!VirtualQuery(edge, &across, sizeof(across)) || across.State != MEM_COMMIT || across.RegionSize != 0x20000 ||
                         across.AllocationBase != at) step = "query-across";
                else if (!VirtualFree(edge, 0x20000, MEM_DECOMMIT)) step = "decommit";
                else if (!VirtualQuery(edge, &after, sizeof(after)) || after.State != MEM_RESERVE || after.AllocationBase != at) step = "query-after";
            }
            if (!step && !VirtualFree(at, 0, MEM_RELEASE)) step = "release";
            else if (step && got == at) VirtualFree(at, 0, MEM_RELEASE);
            wsprintfA(line, "PS5_PROTON_LATE_LIBRARY_STAGE stage=boundary start=%I64x bytes=%I64u step=%s passed=%d error=%lu\n", start, big,
                      step ? step : "all", step == NULL, step ? GetLastError() : 0);
            OutputDebugStringA(line);
            return step == NULL;
        }
        cursor = end;
    }
    OutputDebugStringA("PS5_PROTON_LATE_LIBRARY_STAGE stage=boundary skipped=no-free-33-gib passed=1\n");
    return 1;
}

/* How much address space can be reserved at a free interval above what the runtime already holds, in 1 GiB pieces of a fixed
 * address, until the system refuses: the first sizeable free interval (a 16 GiB piece is refused at 0x7ef000000 on the standard
 * PS5 although RLIMIT_DATA is 32 GiB: a total, or a size?). Said, then every piece is released. */
static void grow(void)
{
    const uint64_t piece = (uint64_t)1 << 30;
    uint64_t cursor = UINT64_C(0x40000);
    char line[220];

    while (cursor < UINT64_C(0xfc00000000))
    {
        MEMORY_BASIC_INFORMATION info = {0};
        uint64_t base, end, start;
        if (!VirtualQuery((void *)(uintptr_t)cursor, &info, sizeof(info))) break;
        base = (uintptr_t)info.BaseAddress;
        end = base + info.RegionSize;
        if (end <= cursor) break;
        start = (base + 65535) & ~UINT64_C(65535);
        if (info.State == MEM_FREE && end > start && end - start >= ((uint64_t)8 << 30))
        {
            unsigned got = 0, limit = (unsigned)((end - start) >> 30);
            DWORD error = 0;
            if (limit > 64) limit = 64;
            for (; got < limit; ++got)
                if (!VirtualAlloc((void *)(uintptr_t)(start + got * piece), (SIZE_T)piece, MEM_RESERVE, PAGE_NOACCESS))
                {
                    error = GetLastError();
                    break;
                }
            wsprintfA(line, "PS5_PROTON_LATE_LIBRARY_STAGE stage=grow start=%I64x free_gib=%I64u reserved_gib=%u error=%lu\n", start,
                      (end - start) >> 30, got, error);
            OutputDebugStringA(line);
            for (unsigned i = 0; i < got; ++i) VirtualFree((void *)(uintptr_t)(start + i * piece), 0, MEM_RELEASE);
            return;
        }
        cursor = end;
    }
    OutputDebugStringA("PS5_PROTON_LATE_LIBRARY_STAGE stage=grow skipped=no-free-8-gib\n");
}

/* Where the system takes a fixed reservation just above the 31 GiB the runtime holds at start: 256 MiB at a time from 0x7ee340000
 * to 0x900000000 at the addresses VirtualQuery calls free, each released at once. The kernel refuses the first 16 GiB piece at
 * 0x7ef000000 (ENOMEM, with no mapping of its own in the range) where the same Pro with the kernel's module loader reserves from
 * 0x8012d0000 on: if the refusal stops at an address, something of the kernel's lies below it (alpha-3, item 1b). */
static void scan(void)
{
    const uint64_t step = (uint64_t)256 << 20;
    uint64_t at;
    unsigned refused = 0, taken = 0;
    uint64_t first_taken = 0, last_refused = 0;
    char line[220];

    for (at = UINT64_C(0x7ee340000); at < UINT64_C(0x900000000); at += step)
    {
        MEMORY_BASIC_INFORMATION info = {0};
        void *got;
        if (!VirtualQuery((void *)(uintptr_t)at, &info, sizeof(info)) || info.State != MEM_FREE || (uintptr_t)info.BaseAddress + info.RegionSize < at + step) continue;
        got = VirtualAlloc((void *)(uintptr_t)at, (SIZE_T)step, MEM_RESERVE, PAGE_NOACCESS);
        if (got)
        {
            if (!taken++) first_taken = at;
            VirtualFree(got, 0, MEM_RELEASE);
        }
        else
        {
            /* What exactly the system holds in a refused 256 MiB block: 16 MiB pieces, then 1 MiB pieces of the refused
             * ones, then 64 KiB pieces; every refused 64 KiB range is said (at most 16 lines). */
            static unsigned said;
            ++refused;
            last_refused = at;
            for (uint64_t big = at; big < at + step; big += (uint64_t)16 << 20)
            {
                void *piece = VirtualAlloc((void *)(uintptr_t)big, (SIZE_T)16 << 20, MEM_RESERVE, PAGE_NOACCESS);
                if (piece) { VirtualFree(piece, 0, MEM_RELEASE); continue; }
                for (uint64_t mid = big; mid < big + ((uint64_t)16 << 20); mid += (uint64_t)1 << 20)
                {
                    void *one_mib = VirtualAlloc((void *)(uintptr_t)mid, (SIZE_T)1 << 20, MEM_RESERVE, PAGE_NOACCESS);
                    if (one_mib) { VirtualFree(one_mib, 0, MEM_RELEASE); continue; }
                    for (uint64_t low = mid; low < mid + ((uint64_t)1 << 20); low += 0x10000)
                    {
                        void *tiny = VirtualAlloc((void *)(uintptr_t)low, 0x10000, MEM_RESERVE, PAGE_NOACCESS);
                        if (tiny) { VirtualFree(tiny, 0, MEM_RELEASE); continue; }
                        if (said++ < 16)
                        {
                            wsprintfA(line, "PS5_PROTON_LATE_LIBRARY_STAGE stage=held range=%I64x error=%lu\n", low, GetLastError());
                            OutputDebugStringA(line);
                        }
                    }
                }
            }
        }
    }
    {
        /* What the runtime itself says of the addresses the system holds, at the 2 GiB boundaries from 32 GiB up. */
        static const uint64_t probes[] = {UINT64_C(0x7fff00000), UINT64_C(0x800000000), UINT64_C(0x800100000), UINT64_C(0x880000000), UINT64_C(0x900000000)};
        for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i)
        {
            MEMORY_BASIC_INFORMATION info = {0};
            BOOL asked = VirtualQuery((void *)(uintptr_t)probes[i], &info, sizeof(info));
            wsprintfA(line, "PS5_PROTON_LATE_LIBRARY_STAGE stage=at addr=%I64x asked=%d base=%I64x alloc=%I64x size=%I64x state=%lx type=%lx protect=%lx\n", probes[i],
                      (int)asked, (uint64_t)(uintptr_t)info.BaseAddress, (uint64_t)(uintptr_t)info.AllocationBase, (uint64_t)info.RegionSize, info.State, info.Type,
                      info.Protect);
            OutputDebugStringA(line);
        }
    }
    {
        /* The system's held block at each 2 GiB boundary from 32 GiB to 64 GiB: how many 64 KiB steps from the boundary are
         * refused before the first accepted (0 = the boundary itself is free; 256 = the whole 16 MiB looked at). A boundary
         * that Wine already knows as occupied is skipped. */
        for (uint64_t boundary = UINT64_C(0x800000000); boundary < UINT64_C(0x1000000000); boundary += UINT64_C(0x80000000))
        {
            MEMORY_BASIC_INFORMATION info = {0};
            unsigned steps = 0;
            if (!VirtualQuery((void *)(uintptr_t)boundary, &info, sizeof(info)) || info.State != MEM_FREE) continue;
            for (; steps < 256; ++steps)
            {
                void *piece = VirtualAlloc((void *)(uintptr_t)(boundary + (uint64_t)steps * 0x10000), 0x10000, MEM_RESERVE, PAGE_NOACCESS);
                if (piece) { VirtualFree(piece, 0, MEM_RELEASE); break; }
            }
            wsprintfA(line, "PS5_PROTON_LATE_LIBRARY_STAGE stage=slot boundary=%I64x refused_64k_steps=%u\n", boundary, steps);
            OutputDebugStringA(line);
        }
    }
    wsprintfA(line, "PS5_PROTON_LATE_LIBRARY_STAGE stage=scan taken=%u first_taken=%I64x refused=%u last_refused=%I64x\n", taken, first_taken, refused, last_refused);
    OutputDebugStringA(line);
}

void mainCRTStartup(void)
{
    static const uint64_t bands[][2] = {
        {UINT64_C(0x40000), UINT64_C(0x1000000000)},
        {UINT64_C(0x1000000000), UINT64_C(0xfc00000000)}};
    alloc2_fn alloc = (alloc2_fn)GetProcAddress(GetModuleHandleA("kernelbase.dll"), "VirtualAlloc2");
    unsigned count = 0, released = 0, retries = 0;
    uint64_t reserved_bytes = 0;
    int passed = !!alloc;
    DWORD error = 0;
    passed = passed && boundary();
    /* The measuring stages reserve and release ranges the runtime then keeps (patch 1128), which changes what the reservations
     * that follow meet: they run only when asked (WINETEST_LATE_LIBRARY_DIAG=1), never in the gate. */
    {
        char diag[8] = "";
        if (GetEnvironmentVariableA("WINETEST_LATE_LIBRARY_DIAG", diag, sizeof(diag)) && diag[0] == '1')
        {
            grow();
            scan();
        }
    }
    for (unsigned band = 0; passed && band < 2; ++band)
    {
        uint64_t cursor = bands[band][0], limit = bands[band][1];
        while (cursor < limit)
        {
            MEMORY_BASIC_INFORMATION info = {0};
            if (!VirtualQuery((void *)(uintptr_t)cursor, &info, sizeof(info)))
            {
                error = GetLastError();
                passed = 0;
                break;
            }
            uint64_t base = (uintptr_t)info.BaseAddress;
            uint64_t end = base + info.RegionSize;
            if (end <= cursor || end < base)
            {
                passed = 0;
                break;
            }
            if (end > limit)
                end = limit;
            uint64_t start = base > bands[band][0] ? base : bands[band][0];
            start = (start + 65535) & ~UINT64_C(65535);
            uint64_t aligned_end = end & ~UINT64_C(65535);
            if (info.State == MEM_FREE && aligned_end > start && aligned_end - start > 0x1000000)
            {
                if (count == sizeof(reservations) / sizeof(reservations[0]))
                {
                    passed = 0;
                    break;
                }
                void *at = alloc(GetCurrentProcess(), (void *)(uintptr_t)start,
                                 (SIZE_T)(aligned_end - start), MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                 PAGE_NOACCESS, NULL, 0);
                if (!at && retries < 64 && (GetLastError() == ERROR_INVALID_ADDRESS || GetLastError() == ERROR_NOT_ENOUGH_MEMORY))
                {
                    /* A refusal of a range VirtualQuery called free: the runtime learns what the system holds in it from the
                     * refusal, and the same question asked again (as a program does) answers with that interval split. */
                    ++retries;
                    continue;
                }
                if (!at && GetLastError() == ERROR_NOT_ENOUGH_MEMORY && count)
                {
                    /* The console gives a process a finite budget of address space (the base PS5, 13.40, stopped after 31 GiB):
                     * what it allowed is what the libraries are loaded around. Said, and not a failure. */
                    char budget[160];
                    wsprintfA(budget, "PS5_PROTON_LATE_LIBRARY_STAGE stage=budget start=%I64x size=%I64x error=%lu\n", start,
                              aligned_end - start, GetLastError());
                    OutputDebugStringA(budget);
                    band = 2;
                    break;
                }
                if (at != (void *)(uintptr_t)start)
                {
                    char where[200];
                    error = GetLastError();
                    wsprintfA(where, "PS5_PROTON_LATE_LIBRARY_STAGE stage=reserve-refused start=%I64x size=%I64x query_base=%I64x query_size=%I64x error=%lu\n",
                              start, aligned_end - start, (uint64_t)(uintptr_t)info.BaseAddress, (uint64_t)info.RegionSize, error);
                    OutputDebugStringA(where);
                    {
                        /* The refusal is what the engine learns a mapping that is not Wine's from: asked again, the
                         * range that was reported free is split around it. */
                        MEMORY_BASIC_INFORMATION again = {0};
                        char note[200];
                        BOOL asked = VirtualQuery((void *)(uintptr_t)start, &again, sizeof(again));
                        wsprintfA(note, "PS5_PROTON_LATE_LIBRARY_STAGE stage=asked-again start=%I64x asked=%d base=%I64x size=%I64x state=%lx\n", start, (int)asked,
                                  (uint64_t)(uintptr_t)again.BaseAddress, (uint64_t)again.RegionSize, again.State);
                        OutputDebugStringA(note);
                    }
                    if (at)
                        VirtualFree(at, 0, MEM_RELEASE);
                    passed = 0;
                    break;
                }
                sizes[count] = (SIZE_T)(aligned_end - start);
                reservations[count++] = at;
                reserved_bytes += aligned_end - start;
            }
            cursor = end;
        }
    }
    passed = passed && count;
    report("reserve", count, reserved_bytes, error, passed);
    /* How much address space the console lets a Windows process reserve is a property of the console and of the runtime:
     * alpha-2's Pro reserved 6 spans (31 GiB) where alpha-1's reserved 17 (917 GiB), and the probe passed all the same, as
     * it passes on any stop at the budget. So the total is judged against a floor measured on each console (the installed
     * firmware, WINE_PS5_FIRMWARE), and a console without one fails and says so: measure it and add a line. */
    if (passed)
    {
        static const struct { const char *firmware; uint64_t floor_gib; } floors[] = {
            {"10.01", 900},   /* measured 917 GiB, 16 spans; PS5 Pro, the kernel's module loader */
            {"13.40", 900},   /* measured 917 GiB, 16 spans; standard PS5, the user-space module loader */
            {"13.60", 900},   /* measured 917 GiB, 17 spans; the same standard PS5 updated (fmem-gate-2026-10-25-01) */
        };
        char firmware[16] = "";
        uint64_t floor_gib = 0;
        int known = 0;
        char line[260];
        GetEnvironmentVariableA("WINE_PS5_FIRMWARE", firmware, sizeof(firmware));
        for (unsigned i = 0; i < sizeof(floors) / sizeof(floors[0]); ++i)
            if (!lstrcmpA(firmware, floors[i].firmware)) { floor_gib = floors[i].floor_gib; known = 1; }
        passed = known && reserved_bytes >= (floor_gib << 30);
        wsprintfA(line, "PS5_PROTON_LATE_LIBRARY_STAGE stage=floor firmware=%s reserved_gib=%I64u floor_gib=%I64u known=%d passed=%d\n",
                  firmware[0] ? firmware : "unknown", reserved_bytes >> 30, floor_gib, known, passed);
        OutputDebugStringA(line);
    }
    static const char *libraries[] = {"vulkan-1.dll", "uxtheme.dll"};
    for (unsigned i = 0; passed && i < sizeof(libraries) / sizeof(libraries[0]); ++i)
    {
        HMODULE library = LoadLibraryA(libraries[i]);
        error = library ? 0 : GetLastError();
        passed = library && FreeLibrary(library);
        report(libraries[i], count, reserved_bytes, error, passed);
    }
    for (unsigned i = 0; i < count; ++i)
        if (VirtualFree(reservations[i], 0, MEM_RELEASE))
            ++released;
    passed = passed && released == count;
    report("release", released, reserved_bytes, 0, passed);
    /* What VirtualQuery says of the released spans now: free on the pinned Linux Wine. On the console a mapping the
     * kernel holds inside one that is not Wine's is reported taken, never free. Recorded, not judged. */
    unsigned taken = 0;
    uint64_t taken_bytes = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        char *at = reservations[i], *end = at + sizes[i];
        int any = 0;
        while (at < end)
        {
            MEMORY_BASIC_INFORMATION info = {0};
            if (!VirtualQuery(at, &info, sizeof(info)) || !info.RegionSize)
                break;
            char *next = (char *)info.BaseAddress + info.RegionSize;
            if (info.State != MEM_FREE)
            {
                any = 1;
                taken_bytes += (uint64_t)((next < end ? next : end) - at);
            }
            at = next;
        }
        taken += any;
    }
    report("after-release", taken, taken_bytes, 0, passed);
    OutputDebugStringA(passed ? "PS5_PROTON_LATE_LIBRARY PASS\n" : "PS5_PROTON_LATE_LIBRARY FAIL\n");
    ExitProcess(passed ? 0 : 1);
}
