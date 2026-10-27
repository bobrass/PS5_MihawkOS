/* SPDX-License-Identifier: GPL-3.0-or-later
 * What a program finds at addresses tests and programs use as "invalid": VirtualQuery of each, in a 32-bit (WoW64) and
 * a 64-bit build of the same file. kernel32's heap test crashed on the console in GlobalSize(0xdeadbee0), which on the
 * PC only finds that the pointer is not its heap's (2026-10-25, both builds). Prints state, protection, type, base and
 * size per address; ends by itself. */
#include <windows.h>

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void hex(char *out, ULONG_PTR value)
{
    static const char digits[] = "0123456789abcdef";
    int i, n = sizeof(void *) * 2;
    for (i = 0; i < n; i++) out[i] = digits[(value >> (4 * (n - 1 - i))) & 15];
    out[n] = 0;
}

void mainCRTStartup(void)
{
    static const ULONG_PTR addresses[] = { 0xdeadbee0, 0xdeadbeef, 0xbaadf00d, 0xcccccccc, 0x400000, 0x10000, 0x7ffe0000, 0x80000000, 0xc0000000, 0xfeedface };
    unsigned i;
    for (i = 0; i < sizeof(addresses) / sizeof(addresses[0]); i++) {
        MEMORY_BASIC_INFORMATION info;
        char line[300], a[24], b[24], c[24], d[24], e[24], f[24];
        if (!VirtualQuery((void *)addresses[i], &info, sizeof(info))) { hex(a, addresses[i]); lstrcpyA(line, "PS5_PROTON_ADDRESS "); lstrcatA(line, a); lstrcatA(line, " query failed\n"); say(line); continue; }
        hex(a, addresses[i]); hex(b, info.State); hex(c, info.Protect); hex(d, info.Type); hex(e, (ULONG_PTR)info.AllocationBase); hex(f, info.RegionSize);
        lstrcpyA(line, "PS5_PROTON_ADDRESS "); lstrcatA(line, a);
        lstrcatA(line, " state "); lstrcatA(line, b); lstrcatA(line, " protect "); lstrcatA(line, c); lstrcatA(line, " type "); lstrcatA(line, d);
        lstrcatA(line, " allocation "); lstrcatA(line, e); lstrcatA(line, " region "); lstrcatA(line, f); lstrcatA(line, "\n");
        say(line);
    }
    say(IsBadReadPtr((void *)0xdeadbee0, 16) ? "PS5_PROTON_ADDRESS 0xdeadbee0 not readable\n" : "PS5_PROTON_ADDRESS 0xdeadbee0 READABLE\n");
    say("PS5_PROTON_ADDRESS PASS: listed\n");
    ExitProcess(0);
}
