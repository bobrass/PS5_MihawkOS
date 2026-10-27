/* SPDX-License-Identifier: GPL-3.0-or-later
 * Byte-range locks as structured storage uses them, in a 32-bit (WoW64) and a 64-bit build of the same file. ole32's
 * storage32 test ran past 90 s on the console after storage32.c:3331 (test_direct_swmr, test_locking), where the PC
 * takes 2 s (2026-10-25, both builds). ole32 retries a contended lock for up to 20 s, so a lock that outlives the handle
 * that took it would cost that per open. The pinned Wine on the PC is the oracle.
 *
 *   lock        an exclusive lock at 0x7fffff00 (where storage locks) taken without waiting
 *   contended   a second handle cannot take it (ERROR_LOCK_VIOLATION)
 *   released    once the first handle is closed, the second takes it
 *   duplicate   a lock held through a handle that is closed while its duplicate is open (printed; the PC decides)
 *   shared      two handles take one shared lock
 *   storage-*   StgCreateDocfile, then StgOpenStorage in test_direct_swmr's modes, each under 2 s
 * Lines: PS5_PROTON_LOCK <step> PASS|FAIL|INFO <ms>: <detail>; then PS5_PROTON_LOCK PASS|FAIL. Ends by itself. */
#define COBJMACROS
#include <windows.h>
#include <ole2.h>

int __cdecl sprintf(char *out, const char *format, ...);   /* msvcrt's; its header trips -Wunused-parameter */

static int failures;

static void line(const char *step, int ok, DWORD ms, const char *detail)
{
    char text[400];
    DWORD written;
    if (ok == 0) failures++;
    sprintf(text, "PS5_PROTON_LOCK %s %s %lu: %s\n", step, ok < 0 ? "INFO" : ok ? "PASS" : "FAIL", (unsigned long)ms, detail);
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static HANDLE open_file(const char *path, DWORD disposition)
{
    return CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                       disposition, FILE_ATTRIBUTE_NORMAL, NULL);
}

static BOOL lock(HANDLE file, DWORD flags)
{
    OVERLAPPED at = {0};
    at.Offset = 0x7fffff00;
    return LockFileEx(file, flags | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at);
}

static void unlock(HANDLE file)
{
    OVERLAPPED at = {0};
    at.Offset = 0x7fffff00;
    UnlockFileEx(file, 0, 1, 0, &at);
}

static void storage_open(const char *step, const WCHAR *path, DWORD mode, int may_refuse)
{
    IStorage *storage = NULL;
    DWORD start = GetTickCount();
    HRESULT hr = StgOpenStorage(path, NULL, mode, NULL, 0, &storage);
    DWORD ms = GetTickCount() - start;
    char detail[100];
    sprintf(detail, "hr %08lx in %lu ms", (unsigned long)hr, (unsigned long)ms);
    line(step, ms < 2000 && (SUCCEEDED(hr) || (may_refuse && hr == STG_E_INVALIDFLAG)), ms, detail);
    if (storage) IStorage_Release(storage);
}

/* What a transacted open is made of, timed (INFO lines; the PC's times are the reference): it took ~50 ms on the
 * console against 1 ms on the PC (2026-10-25). */
static void costs(const char *dir)
{
    WCHAR wdir[MAX_PATH], name[MAX_PATH];
    char detail[200], path[MAX_PATH];
    IStorage *scratch = NULL;
    HANDLE file;
    OVERLAPPED at;
    DWORD start, i, ms[6], written;
    BYTE block[512] = {0};

    MultiByteToWideChar(CP_ACP, 0, dir, -1, wdir, MAX_PATH);
    start = GetTickCount();
    for (i = 0; i < 20; i++) {
        GetTempFileNameW(wdir, L"PWL", 0, name);
        DeleteFileW(name);
    }
    ms[0] = GetTickCount() - start;
    start = GetTickCount();
    for (i = 0; i < 20; i++) {
        GetTempFileNameW(wdir, L"PWL", 0, name);
        file = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                           FILE_FLAG_RANDOM_ACCESS | FILE_FLAG_DELETE_ON_CLOSE, NULL);
        WriteFile(file, block, sizeof(block), &written, NULL);
        CloseHandle(file);
    }
    ms[1] = GetTickCount() - start;
    start = GetTickCount();
    for (i = 0; i < 20; i++) {
        scratch = NULL;
        StgCreateDocfile(NULL, STGM_CREATE | STGM_READWRITE | STGM_SHARE_EXCLUSIVE | STGM_DELETEONRELEASE, 0, &scratch);
        if (scratch) IStorage_Release(scratch);
    }
    ms[2] = GetTickCount() - start;
    sprintf(path, "%spwlock.cost", dir);
    file = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, CREATE_ALWAYS, 0, NULL);
    start = GetTickCount();
    for (i = 0; i < 100; i++) {
        memset(&at, 0, sizeof(at));
        at.Offset = 0x7fffff00 + i;
        LockFileEx(file, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &at);
        UnlockFileEx(file, 0, 1, 0, &at);
    }
    ms[3] = GetTickCount() - start;
    start = GetTickCount();
    for (i = 0; i < 100; i++) {
        memset(&at, 0, sizeof(at));
        at.Offset = 512 * (i % 8);
        WriteFile(file, block, sizeof(block), NULL, &at);
        ReadFile(file, block, sizeof(block), NULL, &at);
    }
    ms[4] = GetTickCount() - start;
    start = GetTickCount();
    for (i = 0; i < 20; i++) FlushFileBuffers(file);
    ms[5] = GetTickCount() - start;
    CloseHandle(file);
    DeleteFileA(path);
    sprintf(detail, "20 GetTempFileName %lu ms; 20 temp delete-on-close files %lu ms; 20 scratch storages %lu ms; "
            "100 lock+unlock %lu ms; 100 write+read 512 B %lu ms; 20 flushes %lu ms", (unsigned long)ms[0],
            (unsigned long)ms[1], (unsigned long)ms[2], (unsigned long)ms[3], (unsigned long)ms[4], (unsigned long)ms[5]);
    line("costs", -1, ms[2], detail);
}

/* The calls a create-and-delete is made of, each timed alone over 20 files (microseconds in all). */
static void split(const char *dir)
{
    LARGE_INTEGER frequency, a, b;
    LONGLONG ticks[7] = {0};   /* integers: the probe has no floating-point runtime */
    char path[MAX_PATH], moved[MAX_PATH], detail[300];
    HANDLE files[20];
    int i;

    QueryPerformanceFrequency(&frequency);
#define TIMED(slot, call) do { QueryPerformanceCounter(&a); call; QueryPerformanceCounter(&b); \
        ticks[slot] += b.QuadPart - a.QuadPart; } while (0)
    for (i = 0; i < 20; i++) {
        sprintf(path, "%spwsplit%02d.tmp", dir, i);
        TIMED(0, files[i] = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL));
        TIMED(1, CloseHandle(files[i]));
        sprintf(moved, "%spwsplit%02d.mov", dir, i);
        TIMED(2, MoveFileExA(path, moved, 0));
        TIMED(3, DeleteFileA(moved));
        sprintf(path, "%spwsplitdir%02d", dir, i);
        TIMED(4, CreateDirectoryA(path, NULL));
        TIMED(5, RemoveDirectoryA(path));
        sprintf(path, "%spwsplit-missing%02d.tmp", dir, i);
        TIMED(6, GetFileAttributesA(path));
    }
#undef TIMED
#define US(slot) ((unsigned long)(ticks[slot] * 1000000 / frequency.QuadPart / 20))
    sprintf(detail, "per call, us: create %lu, close %lu, rename %lu, delete %lu, mkdir %lu, rmdir %lu, missing-name lookup %lu",
            US(0), US(1), US(2), US(3), US(4), US(5), US(6));
    line("split", -1, (DWORD)((US(0) + US(3)) / 1000), detail);
#undef US
}

void mainCRTStartup(void)
{
    char dir[MAX_PATH], path[MAX_PATH], detail[200];
    WCHAR wpath[MAX_PATH];
    HANDLE first, second, third, copy, fourth;
    IStorage *storage = NULL;
    DWORD start, error;
    BOOL ok;
    HRESULT hr;

    GetTempPathA(MAX_PATH, dir);
    sprintf(path, "%spwlock.bin", dir);
    first = open_file(path, CREATE_ALWAYS);
    second = open_file(path, OPEN_EXISTING);
    line("open", first != INVALID_HANDLE_VALUE && second != INVALID_HANDLE_VALUE, 0, path);

    line("lock", lock(first, LOCKFILE_EXCLUSIVE_LOCK), 0, "first handle, exclusive");
    ok = lock(second, LOCKFILE_EXCLUSIVE_LOCK);
    error = GetLastError();
    sprintf(detail, "second handle: %s, error %lu", ok ? "taken" : "refused", (unsigned long)error);
    line("contended", !ok && error == ERROR_LOCK_VIOLATION, 0, detail);
    if (ok) unlock(second);

    CloseHandle(first);
    start = GetTickCount();
    ok = lock(second, LOCKFILE_EXCLUSIVE_LOCK);
    error = GetLastError();
    sprintf(detail, "after the first handle closed: %s, error %lu", ok ? "taken" : "refused", (unsigned long)error);
    line("released", ok, GetTickCount() - start, detail);
    if (ok) unlock(second);

    third = open_file(path, OPEN_EXISTING);
    lock(third, LOCKFILE_EXCLUSIVE_LOCK);
    DuplicateHandle(GetCurrentProcess(), third, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
    CloseHandle(third);
    ok = lock(second, LOCKFILE_EXCLUSIVE_LOCK);
    error = GetLastError();
    sprintf(detail, "taken through a handle closed while its duplicate is open: %s, error %lu", ok ? "taken" : "refused",
            (unsigned long)error);
    line("duplicate", -1, 0, detail);
    if (ok) unlock(second);
    CloseHandle(copy);
    ok = lock(second, LOCKFILE_EXCLUSIVE_LOCK);
    sprintf(detail, "after the duplicate closed too: %s", ok ? "taken" : "refused");
    line("duplicate-closed", ok, 0, detail);
    if (ok) unlock(second);

    fourth = open_file(path, OPEN_EXISTING);
    ok = lock(second, 0) && lock(fourth, 0);
    line("shared", ok, 0, ok ? "both handles hold it" : "refused");
    unlock(second);
    unlock(fourth);
    CloseHandle(fourth);
    CloseHandle(second);
    DeleteFileA(path);

    CoInitialize(NULL);
    sprintf(path, "%spwlock.stg", dir);
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    start = GetTickCount();
    hr = StgCreateDocfile(wpath, STGM_CREATE | STGM_READWRITE | STGM_SHARE_DENY_WRITE | STGM_TRANSACTED, 0, &storage);
    sprintf(detail, "hr %08lx", (unsigned long)hr);
    line("storage-create", hr == S_OK && GetTickCount() - start < 2000, GetTickCount() - start, detail);
    if (storage) IStorage_Release(storage);
    storage_open("storage-swmr-read", wpath, STGM_DIRECT_SWMR | STGM_READ | STGM_SHARE_DENY_NONE, 1);
    storage_open("storage-swmr-write", wpath, STGM_DIRECT_SWMR | STGM_READWRITE | STGM_SHARE_DENY_WRITE, 0);
    storage_open("storage-transacted", wpath, STGM_READ | STGM_SHARE_DENY_NONE | STGM_TRANSACTED, 0);
    storage = NULL;
    hr = StgOpenStorage(wpath, NULL, STGM_READ | STGM_SHARE_DENY_NONE | STGM_TRANSACTED, NULL, 0, &storage);
    storage_open("storage-beside-open", wpath, STGM_READ | STGM_SHARE_DENY_NONE | STGM_TRANSACTED, 0);
    if (storage) IStorage_Release(storage);
    DeleteFileA(path);
    costs(dir);
    split(dir);

    {
        /* run-console's verdict: the marker, then PASS or FAIL */
        DWORD written;
        const char *verdict = failures ? "PS5_PROTON_LOCK FAIL: a check failed\n" : "PS5_PROTON_LOCK PASS: every check held\n";
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), verdict, lstrlenA(verdict), &written, NULL);
        OutputDebugStringA(verdict);
    }
    ExitProcess(failures ? 1 : 0);
}
