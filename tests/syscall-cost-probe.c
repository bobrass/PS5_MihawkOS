/* SPDX-License-Identifier: GPL-3.0-or-later
 * What one system call costs a Windows program on the console, in a 32-bit (WoW64) and a 64-bit build of the same file:
 * a call that does almost nothing (NtYieldExecution), an event made and closed (NtCreateEvent + NtClose), and an event
 * set and waited on while signalled. A .NET game's start-up thread (32-bit) spent its sampled time inside NtCreateEvent,
 * NtClose and waits (2026-10-24). Prints microseconds per operation; ends by itself. */
#include <windows.h>

static LONG (WINAPI *NtYieldExecution_)(void);
/* Floating point without a C runtime: the linker asks for this marker. */
int _fltused = 1;

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void report(const char *what, LONGLONG ticks, LONGLONG frequency, int count)
{
    char text[200], number[24];
    /* In double: a 32-bit build has no 64-bit division without a runtime. */
    const unsigned long nanos = (unsigned long)((double)ticks * 1e9 / (double)frequency / (double)count);
    unsigned long whole = nanos / 1000, part = nanos % 1000;
    int i = 0, j;
    char digits[24];
    do { digits[i++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    for (j = 0; j < i; j++) number[j] = digits[i - 1 - j];
    number[i++] = '.';
    number[i++] = (char)('0' + part / 100);
    number[i++] = (char)('0' + part / 10 % 10);
    number[i] = 0;
    lstrcpyA(text, sizeof(void *) == 4 ? "PS5_PROTON_SYSCALL_COST 32-bit " : "PS5_PROTON_SYSCALL_COST 64-bit ");
    lstrcatA(text, what);
    lstrcatA(text, ": ");
    lstrcatA(text, number);
    lstrcatA(text, " us each\n");
    say(text);
}

void mainCRTStartup(void)
{
    LARGE_INTEGER frequency, start, end;
    HANDLE event;
    int i;
    const int count = 20000;

    NtYieldExecution_ = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtYieldExecution");
    QueryPerformanceFrequency(&frequency);

    QueryPerformanceCounter(&start);
    for (i = 0; i < count; i++) NtYieldExecution_();
    QueryPerformanceCounter(&end);
    report("NtYieldExecution", end.QuadPart - start.QuadPart, frequency.QuadPart, count);

    QueryPerformanceCounter(&start);
    for (i = 0; i < count; i++) { event = CreateEventA(NULL, TRUE, FALSE, NULL); CloseHandle(event); }
    QueryPerformanceCounter(&end);
    report("CreateEvent + CloseHandle", end.QuadPart - start.QuadPart, frequency.QuadPart, count);

    event = CreateEventA(NULL, TRUE, FALSE, NULL);
    QueryPerformanceCounter(&start);
    for (i = 0; i < count; i++) { SetEvent(event); WaitForSingleObject(event, 0); ResetEvent(event); }
    QueryPerformanceCounter(&end);
    report("SetEvent + WaitForSingleObject + ResetEvent", end.QuadPart - start.QuadPart, frequency.QuadPart, count);
    CloseHandle(event);

    QueryPerformanceCounter(&start);
    for (i = 0; i < count; i++) GetTickCount();
    QueryPerformanceCounter(&end);
    report("GetTickCount (no system call)", end.QuadPart - start.QuadPart, frequency.QuadPart, count);
    say("PS5_PROTON_SYSCALL_COST PASS: reported\n");
    ExitProcess(0);
}
