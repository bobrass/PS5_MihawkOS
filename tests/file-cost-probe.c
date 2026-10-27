/* SPDX-License-Identifier: GPL-3.0-or-later
 * What a game's file calls cost on the console, in a 32-bit (WoW64) and a 64-bit build of the same file, run in a helper
 * as games are (PROTON_LAYOUT=launcher). A 2D game's loading thread spent 80% of its samples in NtReadFile,
 * NtQueryAttributesFile, NtCreateFile and NtClose, about 13 get_handle_fd server calls for every file it opened
 * (isaac-load-sample-2026-10-25-01): its 70 s load is these calls. Prints microseconds per operation against a file
 * of the prefix (kernel32.dll); ends by itself. On a PC each is a microsecond or less. */
#include <windows.h>

int _fltused = 1;

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void report(const char *what, LONGLONG ticks, LONGLONG frequency, int count)
{
    char text[200], number[24], digits[24];
    const unsigned long nanos = (unsigned long)((double)ticks * 1e9 / (double)frequency / (double)count);
    unsigned long whole = nanos / 1000, part = nanos % 1000;
    int i = 0, j;
    do { digits[i++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    for (j = 0; j < i; j++) number[j] = digits[i - 1 - j];
    number[i++] = '.';
    number[i++] = (char)('0' + part / 100);
    number[i++] = (char)('0' + part / 10 % 10);
    number[i] = 0;
    lstrcpyA(text, sizeof(void *) == 4 ? "PS5_PROTON_FILE_COST 32-bit " : "PS5_PROTON_FILE_COST 64-bit ");
    lstrcatA(text, what);
    lstrcatA(text, ": ");
    lstrcatA(text, number);
    lstrcatA(text, " us each\n");
    say(text);
}

static HANDLE open_file(const WCHAR *path)
{
    return CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
}

/* --read PATH: how fast a large file reads, cold then warm, in 4 KiB pieces (msvcrt's buffer, what that game reads with) and
 * in 1 MiB pieces: the 2D game read 1.43 GB of archives at 35 us a 4 KiB read, about 110 MB/s
 * (isaac-load-fix3-2026-10-25-01), where a cached 4 KiB read costs 7 us. */
static void read_speed(const WCHAR *path)
{
    static char big[1 << 20];
    static const struct { const char *what; DWORD size; } passes[] = {
        { "4 KiB reads, first pass", 4096 }, { "4 KiB reads, second pass", 4096 }, { "1 MiB reads, third pass", 1 << 20 } };
    LARGE_INTEGER frequency, start, end;
    unsigned int p;

    QueryPerformanceFrequency(&frequency);
    for (p = 0; p < sizeof(passes) / sizeof(passes[0]); p++) {
        HANDLE file = open_file(path);
        double total = 0, seconds;
        DWORD got;
        char text[160], number[24];
        int i = 0;
        unsigned long rate;
        if (file == INVALID_HANDLE_VALUE) { say("PS5_PROTON_FILE_COST FAIL: the file did not open\n"); ExitProcess(1); }
        QueryPerformanceCounter(&start);
        while (ReadFile(file, big, passes[p].size, &got, NULL) && got) total += got;
        QueryPerformanceCounter(&end);
        CloseHandle(file);
        seconds = (double)(end.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
        rate = (unsigned long)(total / 1048576.0 / (seconds > 0 ? seconds : 1));
        do { number[i++] = (char)('0' + rate % 10); rate /= 10; } while (rate);
        number[i] = 0;
        for (got = 0; got < (DWORD)i / 2; got++) { char c = number[got]; number[got] = number[i - 1 - got]; number[i - 1 - got] = c; }
        lstrcpyA(text, "PS5_PROTON_FILE_COST read ");
        lstrcatA(text, passes[p].what);
        lstrcatA(text, ": ");
        lstrcatA(text, number);
        lstrcatA(text, " MB/s\n");
        say(text);
    }
    say("PS5_PROTON_FILE_COST PASS: reported\n");
    ExitProcess(0);
}

void mainCRTStartup(void)
{
    static const WCHAR path[] = L"C:\\windows\\system32\\kernel32.dll";
    static char buffer[4096];
    LARGE_INTEGER frequency, start, end;
    HANDLE file;
    DWORD got;
    int i, failed = 0;

    {
        const WCHAR *line = GetCommandLineW(), *flag;
        for (flag = line; *flag; flag++)
            if (flag[0] == '-' && flag[1] == '-' && flag[2] == 'r' && flag[3] == 'e' && flag[4] == 'a' && flag[5] == 'd' && flag[6] == ' ')
                read_speed(flag + 7);
    }
    QueryPerformanceFrequency(&frequency);
    file = open_file(path);
    if (file == INVALID_HANDLE_VALUE) { say("PS5_PROTON_FILE_COST FAIL: kernel32.dll did not open\n"); ExitProcess(1); }

    /* Sequential 4 KiB reads on one handle, back to the start at the end of the file (msvcrt's fread buffer size) */
    QueryPerformanceCounter(&start);
    for (i = 0; i < 20000; i++) {
        if (!ReadFile(file, buffer, sizeof(buffer), &got, NULL)) failed++;
        if (got < sizeof(buffer)) SetFilePointer(file, 0, NULL, FILE_BEGIN);
    }
    QueryPerformanceCounter(&end);
    report("ReadFile 4 KiB", end.QuadPart - start.QuadPart, frequency.QuadPart, 20000);

    /* A seek and a read, as an archive's reader does for each entry */
    QueryPerformanceCounter(&start);
    for (i = 0; i < 20000; i++) {
        SetFilePointer(file, (i * 7919) % 200000, NULL, FILE_BEGIN);
        if (!ReadFile(file, buffer, 512, &got, NULL)) failed++;
    }
    QueryPerformanceCounter(&end);
    report("SetFilePointer + ReadFile 512 B", end.QuadPart - start.QuadPart, frequency.QuadPart, 20000);
    CloseHandle(file);

    /* A file opened, read once and closed */
    QueryPerformanceCounter(&start);
    for (i = 0; i < 2000; i++) {
        file = open_file(path);
        if (file == INVALID_HANDLE_VALUE) { failed++; continue; }
        if (!ReadFile(file, buffer, sizeof(buffer), &got, NULL)) failed++;
        CloseHandle(file);
    }
    QueryPerformanceCounter(&end);
    report("CreateFile + ReadFile 4 KiB + CloseHandle", end.QuadPart - start.QuadPart, frequency.QuadPart, 2000);

    QueryPerformanceCounter(&start);
    for (i = 0; i < 5000; i++) if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) failed++;
    QueryPerformanceCounter(&end);
    report("GetFileAttributes (a file there)", end.QuadPart - start.QuadPart, frequency.QuadPart, 5000);

    QueryPerformanceCounter(&start);
    for (i = 0; i < 5000; i++) if (GetFileAttributesW(L"C:\\windows\\system32\\no-such-file.txt") != INVALID_FILE_ATTRIBUTES) failed++;
    QueryPerformanceCounter(&end);
    report("GetFileAttributes (no such file)", end.QuadPart - start.QuadPart, frequency.QuadPart, 5000);

    /* The 2D game's pattern: a name in another case than the disk's, below a directory that does not exist
     * (mods\<mod>\resources\music\...), so every component is looked up one at a time */
    QueryPerformanceCounter(&start);
    for (i = 0; i < 5000; i++)
        if (GetFileAttributesW(L"C:\\WINDOWS\\SYSTEM32\\DRIVERS\\ETC\\NO-SUCH-DIR\\MUSIC\\file.ogg") != INVALID_FILE_ATTRIBUTES) failed++;
    QueryPerformanceCounter(&end);
    report("GetFileAttributes (other case, missing directory)", end.QuadPart - start.QuadPart, frequency.QuadPart, 5000);

    if (failed) {
        say("PS5_PROTON_FILE_COST FAIL: a call failed\n");
        ExitProcess(1);
    }
    say("PS5_PROTON_FILE_COST PASS: reported\n");
    ExitProcess(0);
}
