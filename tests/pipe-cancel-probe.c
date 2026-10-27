/* SPDX-License-Identifier: GPL-3.0-or-later
 * A pending read on a pipe, cancelled, and the pipe used again: in a 32-bit (WoW64) and a 64-bit build of the same file.
 * ntdll's pipe test on the console (2026-10-25, PE64): after NtCancelIoFileEx cancelled a pending NtReadFile, the next
 * read on the same handle answered STATUS_OBJECT_TYPE_MISMATCH (c0000024, pipe.c:1405) and in the next mode a write
 * failed with ERROR_NO_DATA (232, pipe.c:1190), where the pinned Linux Wine passes. Steam's and launchers' IPC cancel
 * overlapped pipe reads all the time. The pinned Wine on the PC is the oracle: this passes there.
 *
 * For a byte-mode and a message-mode pipe pair (overlapped server end, overlapped client end), ROUNDS times:
 *   1. a read is left pending; CancelIoEx cancels it; the read ends with ERROR_OPERATION_ABORTED;
 *   2. two reads pend on one OVERLAPPED; NtCancelIoFileEx cancels both (the test's "duplicate iosb");
 *   2b. as ntdll's test does: NtReadFile with an APC routine and an event, cancelled by NtCancelIoFileEx, the APC run by
 *       an alertable sleep, then NtReadFile with the APC again, twice on one IO_STATUS_BLOCK, cancelled again;
 *   3. the other end writes 2 bytes; a new read on the same handle gets them; then the other direction;
 *   4. the handles still answer GetNamedPipeInfo.
 * PASS when every step holds; ends by itself. */
#include <windows.h>
#include <winternl.h>

enum { ROUNDS = 20 };

static LONG (WINAPI *NtCancelIoFileEx_)(HANDLE, IO_STATUS_BLOCK *, IO_STATUS_BLOCK *);
static LONG (WINAPI *NtReadFile_)(HANDLE, HANDLE, PIO_APC_ROUTINE, void *, IO_STATUS_BLOCK *, void *, ULONG, LARGE_INTEGER *, ULONG *);
static volatile LONG apcs;

static void WINAPI apc(void *context, IO_STATUS_BLOCK *io, ULONG reserved)
{
    (void)io; (void)reserved;
    InterlockedIncrement((LONG *)context);
}
static int failures, checks;

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void hex(char *out, ULONG_PTR value)
{
    static const char digits[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 8; i++) out[i] = digits[(value >> (28 - 4 * i)) & 15];
    out[8] = 0;
}

static void check(int ok, const char *mode, int round, const char *what, ULONG_PTR value)
{
    char text[300], number[9], r[4];
    checks++;
    if (ok) return;
    failures++;
    if (failures > 30) return;
    hex(number, value);
    r[0] = (char)('0' + round / 10); r[1] = (char)('0' + round % 10); r[2] = 0;
    lstrcpyA(text, "PS5_PROTON_PIPE_CANCEL ");
    lstrcatA(text, mode);
    lstrcatA(text, " round ");
    lstrcatA(text, r);
    lstrcatA(text, ": ");
    lstrcatA(text, what);
    lstrcatA(text, " -> ");
    lstrcatA(text, number);
    lstrcatA(text, "\n");
    say(text);
}

/* A read on handle that must be left pending. */
static BOOL pend(HANDLE handle, OVERLAPPED *ov, char *buffer)
{
    DWORD got;
    ResetEvent(ov->hEvent);
    return !ReadFile(handle, buffer, 2, &got, ov) && GetLastError() == ERROR_IO_PENDING;
}

/* write 2 bytes on one end, read them on the other with a fresh overlapped read */
static void exchange(HANDLE from, HANDLE to, const char *mode, int round, const char *what)
{
    OVERLAPPED ov = { 0 }, wov = { 0 };
    char buffer[4] = { 0 };
    DWORD got = 0, wrote = 0;
    BOOL done;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    wov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    done = ReadFile(to, buffer, 2, &got, &ov);
    check(done || GetLastError() == ERROR_IO_PENDING, mode, round, what, GetLastError());
    done = WriteFile(from, "pw", 2, &wrote, &wov) || (GetLastError() == ERROR_IO_PENDING && GetOverlappedResult(from, &wov, &wrote, TRUE));
    check(done && wrote == 2, mode, round, "write after the cancel (last error)", GetLastError());
    done = GetOverlappedResult(to, &ov, &got, TRUE);
    check(done && got == 2 && buffer[0] == 'p' && buffer[1] == 'w', mode, round, "read after the cancel got the bytes (got)", got);
    CloseHandle(ov.hEvent);
    CloseHandle(wov.hEvent);
}

static void pair(DWORD type, const char *mode)
{
    static const WCHAR name[] = L"\\\\.\\pipe\\pw-pipe-cancel-probe";
    HANDLE server, client, event = CreateEventW(NULL, TRUE, FALSE, NULL);
    OVERLAPPED ov = { 0 };
    IO_STATUS_BLOCK cancel;
    char buffer[4];
    DWORD got, flags;
    int round;

    server = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, type | (type == PIPE_TYPE_MESSAGE ? PIPE_READMODE_MESSAGE : 0),
                              1, 4096, 4096, 0, NULL);
    client = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    check(server != INVALID_HANDLE_VALUE && client != INVALID_HANDLE_VALUE, mode, 0, "pipe pair made (last error)", GetLastError());
    if (server == INVALID_HANDLE_VALUE || client == INVALID_HANDLE_VALUE) return;
    if (type == PIPE_TYPE_MESSAGE) { DWORD m = PIPE_READMODE_MESSAGE; SetNamedPipeHandleState(client, &m, NULL, NULL); }
    ov.hEvent = event;
    for (round = 0; round < ROUNDS; round++) {
        /* 1: CancelIoEx on one pending read */
        check(pend(server, &ov, buffer), mode, round, "server read pends (last error)", GetLastError());
        check(CancelIoEx(server, &ov), mode, round, "CancelIoEx (last error)", GetLastError());
        check(!GetOverlappedResult(server, &ov, &got, TRUE) && GetLastError() == ERROR_OPERATION_ABORTED, mode, round,
              "cancelled read ends aborted (last error)", GetLastError());
        /* 2: two reads on one OVERLAPPED, cancelled by NtCancelIoFileEx */
        if (NtCancelIoFileEx_) {
            LONG status;
            check(pend(server, &ov, buffer), mode, round, "first of two reads pends", GetLastError());
            check(pend(server, &ov, buffer), mode, round, "second of two reads pends (c0000024 was the bug)", GetLastError());
            status = NtCancelIoFileEx_(server, (IO_STATUS_BLOCK *)&ov, &cancel);
            check(!status, mode, round, "NtCancelIoFileEx (status)", (ULONG_PTR)status);
            SleepEx(1, TRUE);
            check(WaitForSingleObject(event, 1000) == WAIT_OBJECT_0, mode, round, "event signalled after the cancel", 0);
            check(ov.Internal == 0xc0000120 /* STATUS_CANCELLED */, mode, round, "status after the cancel", ov.Internal);
        }
        /* 2b: NtReadFile with an APC, as ntdll's pipe test (pipe.c:1380-1424) */
        if (NtCancelIoFileEx_ && NtReadFile_) {
            IO_STATUS_BLOCK io, io2;
            LONG status;
            int k;
            for (k = 1; k <= 2; k++) {
                int n;
                apcs = 0;
                ResetEvent(event);
                for (n = 0; n < k; n++) {
                    io.Status = (LONG)0xdeadbabe;
                    status = NtReadFile_(server, event, (PIO_APC_ROUTINE)apc, (void *)&apcs, &io, buffer, 2, NULL, NULL);
                    check(status == 0x103 /* STATUS_PENDING */, mode, round, k == 1 ? "NtReadFile with an APC pends (status)"
                          : "NtReadFile with an APC after a cancelled one pends (c0000024 was pipe.c:1405)", (ULONG_PTR)status);
                }
                status = NtCancelIoFileEx_(server, &io, &io2);
                check(!status, mode, round, "NtCancelIoFileEx of the APC read (status)", (ULONG_PTR)status);
                Sleep(1);
                check(io.Status == (LONG)0xc0000120, mode, round, "APC read's status after the cancel", (ULONG_PTR)io.Status);
                check(WaitForSingleObject(event, 0) == WAIT_OBJECT_0, mode, round, "APC read's event after the cancel", 0);
                SleepEx(1, TRUE);
                check(apcs == k, mode, round, "APCs run by the alertable sleep (count)", (ULONG_PTR)apcs);
            }
        }
        /* 3: the pipe still works, both ways */
        exchange(client, server, mode, round, "server reads again (last error)");
        exchange(server, client, mode, round, "client reads (last error)");
        /* 4: still a pipe */
        check(GetNamedPipeInfo(server, &flags, NULL, NULL, NULL), mode, round, "server is still a pipe (last error)", GetLastError());
        check(GetNamedPipeInfo(client, &flags, NULL, NULL, NULL), mode, round, "client is still a pipe (last error)", GetLastError());
    }
    CloseHandle(client);
    CloseHandle(server);
    CloseHandle(event);
}

void mainCRTStartup(void)
{
    char text[200], number[9];
    NtCancelIoFileEx_ = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtCancelIoFileEx");
    NtReadFile_ = (void *)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtReadFile");
    pair(PIPE_TYPE_BYTE, "byte");
    pair(PIPE_TYPE_MESSAGE, "message");
    hex(number, (ULONG_PTR)checks);
    lstrcpyA(text, failures ? "PS5_PROTON_PIPE_CANCEL FAIL: checks 0x" : "PS5_PROTON_PIPE_CANCEL PASS: checks 0x");
    lstrcatA(text, number);
    lstrcatA(text, failures ? ", some failed\n" : ": a cancelled pipe read leaves the pipe working, in both modes, 20 rounds each\n");
    say(text);
    ExitProcess(failures ? 1 : 0);
}
