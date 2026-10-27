/* SPDX-License-Identifier: GPL-3.0-or-later
 * Directories opened relative to a directory handle while the server holds more than 4096 descriptors, the way .NET's
 * FileSystemEnumerator (Wine-Mono) lists a folder: the parent by CreateFileW (list access, backup semantics), each child by
 * NtCreateFile relative to the parent's handle. The console has no *at() calls; the in-process server opens a relative name
 * with fchdir(parent's descriptor), and pw_wine_cwd keeps a path for each descriptor number. Its table stopped at 4096, the
 * title's server held 8269 with Steam and a game running, and every relative open above that was refused: a .NET game's "Access
 * to the path ...\Content\Dialog\Fonts is denied" (2026-10-24; PW_CWD_MAX_FDS, wine/ps5/pw_wine_cwd.h).
 *
 * So: hold 4400 handles first (each is a descriptor in the title's server; parked ones keep their numbers), then open
 * C:\windows and list it, opening every subdirectory relative to the listing's handle, then system32 relative to C:\windows
 * and drivers relative to system32. PASS when every open succeeds; ends by itself. */
#include <windows.h>

typedef struct { USHORT Length, MaximumLength; PWSTR Buffer; } USTR;
typedef struct { ULONG Length; HANDLE RootDirectory; USTR *ObjectName; ULONG Attributes; PVOID Security, Quality; } OBJA;
typedef struct { LONG_PTR Status; ULONG_PTR Information; } IOSB;

enum { HELD = 4400 };

static LONG (WINAPI *NtCreateFile_)(HANDLE *, ACCESS_MASK, OBJA *, IOSB *, LARGE_INTEGER *, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
static LONG (WINAPI *NtClose_)(HANDLE);
static LONG (WINAPI *NtQueryDirectoryFile_)(HANDLE, HANDLE, PVOID, PVOID, IOSB *, PVOID, ULONG, ULONG, BOOLEAN, USTR *, BOOLEAN);
static int failures;

static void say(const char *text)
{
    DWORD written;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, lstrlenA(text), &written, NULL);
    OutputDebugStringA(text);
}

static void result(const char *what, ULONG_PTR value)
{
    static const char digits[] = "0123456789abcdef";
    char text[320], number[9];
    int i;
    for (i = 0; i < 8; i++) number[i] = digits[(value >> (28 - 4 * i)) & 15];
    number[8] = 0;
    lstrcpyA(text, "PS5_PROTON_NT_RELDIR_PROBE ");
    lstrcatA(text, what);
    lstrcatA(text, " -> ");
    lstrcatA(text, number);
    lstrcatA(text, "\n");
    say(text);
}

static LONG open_relative(HANDLE parent, const WCHAR *name, HANDLE *out)
{
    USTR string;
    OBJA attributes = { sizeof(attributes), parent, &string, 0x40 /* OBJ_CASE_INSENSITIVE */, NULL, NULL };
    IOSB io;
    string.Buffer = (PWSTR)name;
    string.Length = (USHORT)(lstrlenW(name) * 2);
    string.MaximumLength = string.Length + 2;
    return NtCreateFile_(out, FILE_LIST_DIRECTORY | SYNCHRONIZE, &attributes, &io, NULL, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         1 /* FILE_OPEN */, 0x20 /* SYNCHRONOUS_IO_NONALERT */ | 1 /* DIRECTORY_FILE */ | 0x4000 /* OPEN_FOR_BACKUP_INTENT */, NULL, 0);
}

static HANDLE open_listing(const WCHAR *path)
{
    return CreateFileW(path, FILE_LIST_DIRECTORY | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                       FILE_FLAG_BACKUP_SEMANTICS, NULL);
}

/* A relative open that must succeed: its status is printed, and a failure counted. */
static HANDLE must_open(HANDLE parent, const WCHAR *name, const char *what)
{
    HANDLE child = NULL;
    LONG status = open_relative(parent, name, &child);
    result(what, (ULONG_PTR)status);
    if (status) { failures++; return NULL; }
    return child;
}

void mainCRTStartup(void)
{
    static HANDLE held[HELD];
    static unsigned char buffer[8192];
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    HANDLE windows, system32, drivers;
    IOSB io;
    LONG listed = 0;
    BOOLEAN restart = TRUE;
    int count, entries = 0, subdirectories = 0, refused = 0;

    NtCreateFile_ = (void *)GetProcAddress(ntdll, "NtCreateFile");
    NtClose_ = (void *)GetProcAddress(ntdll, "NtClose");
    NtQueryDirectoryFile_ = (void *)GetProcAddress(ntdll, "NtQueryDirectoryFile");

    for (count = 0; count < HELD; count++) {
        held[count] = CreateFileW(L"C:\\windows\\system32\\kernel32.dll", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                  OPEN_EXISTING, 0, NULL);
        if (held[count] == INVALID_HANDLE_VALUE) break;
    }
    result("files held open first (of 4400)", (ULONG_PTR)count);
    if (count < HELD) { result("  last error", GetLastError()); failures++; }

    windows = open_listing(L"C:\\windows");
    result("C:\\windows by CreateFileW (handle; -1 is failure)", (ULONG_PTR)windows);
    if (windows == INVALID_HANDLE_VALUE) { say("PS5_PROTON_NT_RELDIR_PROBE FAIL: C:\\windows did not open\n"); ExitProcess(1); }

    /* As .NET's enumerator does: list on the parent's own handle (FileDirectoryInformation, class 1) and open each
     * subdirectory relative to that handle while the listing goes on. */
    while (!(listed = NtQueryDirectoryFile_(windows, NULL, NULL, NULL, &io, buffer, sizeof(buffer), 1, FALSE, NULL, restart))) {
        ULONG at = 0;
        restart = FALSE;
        for (;;) {
            const unsigned char *entry = buffer + at;
            const ULONG next = *(const ULONG *)entry, attributes = *(const ULONG *)(entry + 56), name_bytes = *(const ULONG *)(entry + 60);
            const WCHAR *entry_name = (const WCHAR *)(entry + 64);
            static WCHAR name[260];
            unsigned n = name_bytes / 2 < 259 ? name_bytes / 2 : 259, c;
            HANDLE child;
            for (c = 0; c < n; c++) name[c] = entry_name[c];
            name[n] = 0;
            entries++;
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) && !(n == 1 && name[0] == '.') && !(n == 2 && name[0] == '.' && name[1] == '.')) {
                LONG status = open_relative(windows, name, &child);
                subdirectories++;
                if (status) {
                    char label[300];
                    unsigned k;
                    lstrcpyA(label, "REFUSED during the listing: relative open of ");
                    for (k = 0; k < n && k < 200; k++) label[45 + k] = (char)name[k];
                    label[45 + k] = 0;
                    result(label, (ULONG_PTR)status);
                    refused++;
                } else NtClose_(child);
            }
            if (!next) break;
            at += next;
        }
    }
    result("listing entries seen", (ULONG_PTR)entries);
    result("subdirectories opened relative to the listing's handle", (ULONG_PTR)subdirectories);
    result("of them refused", (ULONG_PTR)refused);
    result("listing ended with (0x80000006 is no more files)", (ULONG_PTR)listed);
    failures += refused;
    if (listed != (LONG)0x80000006 || subdirectories < 2) failures++;

    system32 = must_open(windows, L"system32", "system32 relative to C:\\windows (status; 0 is success)");
    drivers = system32 ? must_open(system32, L"drivers", "drivers relative to system32 (status)") : NULL;
    if (drivers) NtClose_(drivers);
    if (system32) NtClose_(system32);
    NtClose_(windows);
    while (count > 0) CloseHandle(held[--count]);

    if (failures) {
        result("failures", (ULONG_PTR)failures);
        say("PS5_PROTON_NT_RELDIR_PROBE FAIL: a directory opened relative to a handle was refused\n");
        ExitProcess(1);
    }
    say("PS5_PROTON_NT_RELDIR_PROBE PASS: with 4400 files held, every subdirectory of C:\\windows opened relative to the listing's handle, "
        "and system32 and drivers relative to their parents\n");
    ExitProcess(0);
}
